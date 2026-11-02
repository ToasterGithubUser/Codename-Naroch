#include <kernel/ata.h>
#include <kernel/io.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <string.h>

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_ALT_STATUS  0x3F6

#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_DF   0x20
#define ATA_STATUS_BSY  0x80
#define ATA_WAIT_LIMIT  1000000

#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA 0x0CFC
#define AHCI_MMIO_VIRTUAL_BASE 0x3F000000U
#define AHCI_MMIO_MAP_SIZE 0x2000U
#define AHCI_PORT_COUNT 32
#define AHCI_COMMAND_SLOTS 32
#define AHCI_COMMAND_TIMEOUT 10000000U
#define AHCI_GHC_AE (1U << 31)
#define AHCI_PORT_CMD_ST (1U << 0)
#define AHCI_PORT_CMD_FRE (1U << 4)
#define AHCI_PORT_CMD_FR (1U << 14)
#define AHCI_PORT_CMD_CR (1U << 15)
#define AHCI_PORT_TFD_ERR (1U << 0)
#define AHCI_PORT_TFD_BSY (1U << 7)
#define AHCI_PORT_TFD_DRQ (1U << 3)
#define AHCI_PORT_IS_TFES (1U << 30)

static uint32_t ata_sector_count;
static volatile uint32_t* ahci_hba;
static volatile uint32_t* ahci_port;
static uint32_t* ahci_command_list;
static uint32_t* ahci_received_fis;
static uint32_t* ahci_command_tables[2];
static uint32_t ahci_sector_count;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t physical_region_count;
    uint32_t transferred;
    uint32_t command_table_base;
    uint32_t command_table_base_upper;
    uint32_t reserved[4];
} ahci_command_header_t;

typedef struct __attribute__((packed)) {
    uint32_t data_base;
    uint32_t data_base_upper;
    uint32_t reserved;
    uint32_t byte_count_and_interrupt;
} ahci_prdt_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t command_fis[64];
    uint8_t atapi_command[16];
    uint8_t reserved[48];
    ahci_prdt_entry_t prdt;
} ahci_command_table_t;

static uint32_t pci_read_config(uint8_t bus, uint8_t device,
                                uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
        ((uint32_t)function << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write_config_word(uint8_t bus, uint8_t device,
                                  uint8_t function, uint8_t offset,
                                  uint16_t value) {
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
        ((uint32_t)function << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), value);
}

static int ahci_find_controller(uint32_t* abar) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t device = 0; device < 32; device++) {
            uint32_t identity = pci_read_config(
                (uint8_t)bus, (uint8_t)device, 0, 0);
            if ((identity & 0xFFFF) == 0xFFFF) {
                continue;
            }

            uint32_t header =
                pci_read_config((uint8_t)bus, (uint8_t)device, 0, 0x0C);
            uint32_t function_count = (header & 0x00800000U) ? 8 : 1;
            for (uint32_t function = 0; function < function_count;
                 function++) {
                uint32_t function_id = pci_read_config(
                    (uint8_t)bus, (uint8_t)device, (uint8_t)function, 0);
                if ((function_id & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                uint32_t class_code = pci_read_config(
                    (uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x08);
                if ((class_code >> 8 & 0xFFFFFF) != 0x010601) {
                    continue;
                }

                uint32_t bar =
                    pci_read_config((uint8_t)bus, (uint8_t)device,
                                    (uint8_t)function, 0x24);
                if ((bar & 1) || (bar & 6) == 4 ||
                    !(bar & 0xFFFFFFF0)) {
                    continue;
                }
                uint32_t command = pci_read_config(
                    (uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x04);
                pci_write_config_word(
                    (uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x04,
                    (uint16_t)((command & 0xFFFF) | 0x0006));
                *abar = bar & 0xFFFFFFF0;
                return 1;
            }
        }
    }
    return 0;
}

static void ahci_stop_port(volatile uint32_t* port) {
    port[0x18 / 4] &= ~AHCI_PORT_CMD_ST;
    for (uint32_t i = 0; i < AHCI_COMMAND_TIMEOUT; i++) {
        if (!(port[0x18 / 4] & AHCI_PORT_CMD_CR)) {
            break;
        }
    }
    port[0x18 / 4] &= ~AHCI_PORT_CMD_FRE;
    for (uint32_t i = 0; i < AHCI_COMMAND_TIMEOUT; i++) {
        if (!(port[0x18 / 4] & AHCI_PORT_CMD_FR)) {
            break;
        }
    }
}

static void ahci_free_port_memory(void) {
    if (ahci_command_list) {
        pmm_free_block(ahci_command_list);
    }
    if (ahci_received_fis) {
        pmm_free_block(ahci_received_fis);
    }
    for (uint32_t i = 0; i < 2; i++) {
        if (ahci_command_tables[i]) {
            pmm_free_block(ahci_command_tables[i]);
        }
    }
    ahci_command_list = 0;
    ahci_received_fis = 0;
    ahci_command_tables[0] = 0;
    ahci_command_tables[1] = 0;
    ahci_port = 0;
}

static ahci_command_table_t* ahci_table_for(uint32_t slot) {
    uint32_t page_index = slot / 16;
    uint32_t table_index = slot % 16;
    return (ahci_command_table_t*)((uint8_t*)ahci_command_tables[page_index] +
                                   table_index * 256);
}

static int ahci_issue_command(uint8_t command, uint32_t lba,
                              const void* buffer, int write) {
    if (!ahci_port || !buffer || (uint32_t)buffer >= 0x02000000U ||
        (uint32_t)buffer + ATA_SECTOR_SIZE < (uint32_t)buffer ||
        (uint32_t)buffer + ATA_SECTOR_SIZE > 0x02000000U) {
        return 0;
    }

    uint32_t saved_flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(saved_flags) : : "memory");

    ahci_command_header_t* headers =
        (ahci_command_header_t*)ahci_command_list;
    if ((ahci_port[0x20 / 4] &
         (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) ||
        (ahci_port[0x38 / 4] & 1)) {
        if (saved_flags & 0x200) {
            asm volatile("sti");
        }
        return 0;
    }

    ahci_command_table_t* table = ahci_table_for(0);
    memset(table, 0, sizeof(*table));
    table->command_fis[0] = 0x27;
    table->command_fis[1] = 0x80;
    table->command_fis[2] = command;
    table->command_fis[4] = (uint8_t)lba;
    table->command_fis[5] = (uint8_t)(lba >> 8);
    table->command_fis[6] = (uint8_t)(lba >> 16);
    table->command_fis[7] = 0x40;
    table->command_fis[8] = (uint8_t)(lba >> 24);
    table->command_fis[9] = 0;
    table->command_fis[10] = 0;
    table->command_fis[12] = 1;
    table->prdt.data_base = (uint32_t)buffer;
    table->prdt.data_base_upper = 0;
    table->prdt.byte_count_and_interrupt = ATA_SECTOR_SIZE - 1;

    headers[0].flags = 5 | (write ? 1U << 6 : 0);
    headers[0].physical_region_count = 1;
    headers[0].transferred = 0;
    ahci_port[0x10 / 4] = 0xFFFFFFFF;
    ahci_port[0x30 / 4] = 0xFFFFFFFF;
    asm volatile("" : : : "memory");
    ahci_port[0x38 / 4] = 1;

    int success = 0;
    for (uint32_t i = 0; i < AHCI_COMMAND_TIMEOUT; i++) {
        if (ahci_port[0x10 / 4] & AHCI_PORT_IS_TFES) {
            break;
        }
        if (!(ahci_port[0x38 / 4] & 1)) {
            success = !(ahci_port[0x20 / 4] & AHCI_PORT_TFD_ERR);
            break;
        }
    }
    ahci_port[0x10 / 4] = 0xFFFFFFFF;
    ahci_port[0x30 / 4] = 0xFFFFFFFF;
    if (saved_flags & 0x200) {
        asm volatile("sti");
    }
    return success;
}

static int ahci_initialize(uint32_t* sector_count) {
    uint32_t abar;
    ahci_hba = 0;
    ahci_sector_count = 0;
    if (!sector_count || !ahci_find_controller(&abar)) {
        return 0;
    }

    uint32_t mmio_offset = abar & 0xFFF;
    if (!paging_map_kernel_mmio(AHCI_MMIO_VIRTUAL_BASE + mmio_offset,
                                abar, AHCI_MMIO_MAP_SIZE)) {
        return 0;
    }
    ahci_hba = (volatile uint32_t*)(AHCI_MMIO_VIRTUAL_BASE + mmio_offset);
    ahci_hba[0x04 / 4] |= AHCI_GHC_AE;
    uint32_t implemented_ports = ahci_hba[0x0C / 4];
    uint32_t port_count = (ahci_hba[0x00 / 4] & 0x1F) + 1;
    if (port_count > AHCI_PORT_COUNT) {
        port_count = AHCI_PORT_COUNT;
    }

    for (uint32_t i = 0; i < port_count; i++) {
        if (!(implemented_ports & (1U << i))) {
            continue;
        }
        volatile uint32_t* port =
            (volatile uint32_t*)((volatile uint8_t*)ahci_hba + 0x100 +
                                 i * 0x80);
        uint32_t sata_status = port[0x28 / 4];
        if ((sata_status & 0x0F) != 3 ||
            ((sata_status >> 8) & 0x0F) != 1 ||
            port[0x24 / 4] != 0x00000101) {
            continue;
        }

        ahci_stop_port(port);
        ahci_port = port;
        ahci_command_list = (uint32_t*)pmm_alloc_block();
        ahci_received_fis = (uint32_t*)pmm_alloc_block();
        ahci_command_tables[0] = (uint32_t*)pmm_alloc_block();
        ahci_command_tables[1] = (uint32_t*)pmm_alloc_block();
        if (!ahci_command_list || !ahci_received_fis ||
            !ahci_command_tables[0] || !ahci_command_tables[1] ||
            (uint32_t)ahci_command_list >= 0x02000000U ||
            (uint32_t)ahci_received_fis >= 0x02000000U ||
            (uint32_t)ahci_command_tables[0] >= 0x02000000U ||
            (uint32_t)ahci_command_tables[1] >= 0x02000000U) {
            if (ahci_command_list &&
                (uint32_t)ahci_command_list >= 0x02000000U) {
                pmm_free_block(ahci_command_list);
                ahci_command_list = 0;
            }
            if (ahci_received_fis &&
                (uint32_t)ahci_received_fis >= 0x02000000U) {
                pmm_free_block(ahci_received_fis);
                ahci_received_fis = 0;
            }
            if (ahci_command_tables[0] &&
                (uint32_t)ahci_command_tables[0] >= 0x02000000U) {
                pmm_free_block(ahci_command_tables[0]);
                ahci_command_tables[0] = 0;
            }
            if (ahci_command_tables[1] &&
                (uint32_t)ahci_command_tables[1] >= 0x02000000U) {
                pmm_free_block(ahci_command_tables[1]);
                ahci_command_tables[1] = 0;
            }
            ahci_free_port_memory();
            continue;
        }

        memset(ahci_command_list, 0, 4096);
        memset(ahci_received_fis, 0, 4096);
        memset(ahci_command_tables[0], 0, 4096);
        memset(ahci_command_tables[1], 0, 4096);
        ahci_command_header_t* headers =
            (ahci_command_header_t*)ahci_command_list;
        for (uint32_t slot = 0; slot < AHCI_COMMAND_SLOTS; slot++) {
            uint32_t table_page = slot < 16 ? 0 : 1;
            uint32_t table_offset = (slot % 16) * 256;
            headers[slot].command_table_base =
                (uint32_t)ahci_command_tables[table_page] + table_offset;
        }

        port[0x00 / 4] = (uint32_t)ahci_command_list;
        port[0x04 / 4] = 0;
        port[0x08 / 4] = (uint32_t)ahci_received_fis;
        port[0x0C / 4] = 0;
        port[0x10 / 4] = 0xFFFFFFFF;
        port[0x30 / 4] = 0xFFFFFFFF;
        port[0x18 / 4] |= AHCI_PORT_CMD_FRE;
        port[0x18 / 4] |= AHCI_PORT_CMD_ST;

        uint16_t identify[256];
        if (!ahci_issue_command(0xEC, 0, identify, 0)) {
            ahci_stop_port(port);
            ahci_free_port_memory();
            continue;
        }
        uint32_t sectors = (uint32_t)identify[60] |
            ((uint32_t)identify[61] << 16);
        if ((identify[83] & (1U << 10)) &&
            (identify[100] || identify[101])) {
            sectors = (uint32_t)identify[100] |
                ((uint32_t)identify[101] << 16);
        }
        if (!sectors) {
            ahci_stop_port(port);
            ahci_free_port_memory();
            continue;
        }
        ahci_sector_count = sectors;
        ata_sector_count = sectors;
        *sector_count = sectors;
        return 1;
    }

    ahci_hba = 0;
    return 0;
}

static void ata_delay_400ns(void) {
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
}

static int ata_wait(int require_data) {
    for (uint32_t i = 0; i < ATA_WAIT_LIMIT; i++) {
        uint8_t status = inb(ATA_STATUS);

        if (status == 0 || status == 0xFF) {
            return 0;
        }
        if (status & ATA_STATUS_BSY) {
            continue;
        }
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            (void)inb(ATA_ERROR);
            return 0;
        }
        if (!require_data || (status & ATA_STATUS_DRQ)) {
            return 1;
        }
    }

    return 0;
}

static int ata_select_sector(uint32_t lba, uint8_t command) {
    if (!ata_sector_count || lba >= ata_sector_count || lba > 0x0FFFFFFF) {
        return 0;
    }

    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay_400ns();
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, command);
    return ata_wait(1);
}

static int ata_pio_init(uint32_t* sector_count) {
    uint16_t identify[256];

    ata_sector_count = 0;
    if (!sector_count) {
        return 0;
    }

    outb(ATA_DRIVE, 0xA0);
    ata_delay_400ns();
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, 0xEC);

    uint8_t status = inb(ATA_STATUS);
    if (status == 0 || status == 0xFF) {
        return 0;
    }

    for (uint32_t i = 0; i < ATA_WAIT_LIMIT; i++) {
        status = inb(ATA_STATUS);
        if (status & ATA_STATUS_BSY) {
            continue;
        }
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            return 0;
        }

        if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0 ||
            !(status & ATA_STATUS_DRQ)) {
            return 0;
        }
        break;
    }

    if (!(status & ATA_STATUS_DRQ)) {
        return 0;
    }

    for (uint32_t i = 0; i < 256; i++) {
        identify[i] = inw(ATA_DATA);
    }

    if (!(identify[49] & (1U << 9))) {
        return 0;
    }

    ata_sector_count = (uint32_t)identify[60] |
        ((uint32_t)identify[61] << 16);
    if (!ata_sector_count) {
        return 0;
    }

    *sector_count = ata_sector_count;
    return 1;
}

static int ata_pio_read_sector(uint32_t lba, void* buffer) {
    if (!buffer || !ata_select_sector(lba, 0x20)) {
        return 0;
    }

    uint16_t* words = (uint16_t*)buffer;
    for (uint32_t i = 0; i < ATA_SECTOR_SIZE / sizeof(uint16_t); i++) {
        words[i] = inw(ATA_DATA);
    }

    ata_delay_400ns();
    return ata_wait(0);
}

static int ata_pio_write_sector(uint32_t lba, const void* buffer) {
    if (!buffer || !ata_select_sector(lba, 0x30)) {
        return 0;
    }

    const uint16_t* words = (const uint16_t*)buffer;
    for (uint32_t i = 0; i < ATA_SECTOR_SIZE / sizeof(uint16_t); i++) {
        outw(ATA_DATA, words[i]);
    }

    ata_delay_400ns();
    if (!ata_wait(0)) {
        return 0;
    }

    outb(ATA_COMMAND, 0xE7);
    return ata_wait(0);
}

int ata_init(uint32_t* sector_count) {
    if (ahci_initialize(sector_count)) {
        return 1;
    }
    return ata_pio_init(sector_count);
}

int ata_read_sector(uint32_t lba, void* buffer) {
    if (ahci_port) {
        return lba < ahci_sector_count &&
            ahci_issue_command(0x25, lba, buffer, 0);
    }
    return ata_pio_read_sector(lba, buffer);
}

int ata_write_sector(uint32_t lba, const void* buffer) {
    if (ahci_port) {
        return lba < ahci_sector_count &&
            ahci_issue_command(0x35, lba, buffer, 1);
    }
    return ata_pio_write_sector(lba, buffer);
}

const char* ata_backend_name(void) {
    return ahci_port ? "AHCI" : "ATA PIO";
}
