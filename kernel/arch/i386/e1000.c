#include <kernel/e1000.h>
#include <kernel/io.h>
#include <kernel/paging.h>
#include <kernel/pci.h>
#include <kernel/pmm.h>
#include <string.h>

#define E1000_MMIO_BASE 0x3F100000U
#define E1000_MMIO_SIZE 0x20000U
#define E1000_RING_SIZE 8
#define E1000_BUFFER_SIZE 2048
#define E1000_WAIT_LIMIT 1000000U
#define E1000_DMA_LIMIT 0x02000000U

#define E1000_REG_CTRL 0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_EERD 0x0014
#define E1000_REG_IMC 0x00D8
#define E1000_REG_RCTL 0x0100
#define E1000_REG_TCTL 0x0400
#define E1000_REG_TIPG 0x0410
#define E1000_REG_RDBAL 0x2800
#define E1000_REG_RDBAH 0x2804
#define E1000_REG_RDLEN 0x2808
#define E1000_REG_RDH 0x2810
#define E1000_REG_RDT 0x2818
#define E1000_REG_TDBAL 0x3800
#define E1000_REG_TDBAH 0x3804
#define E1000_REG_TDLEN 0x3808
#define E1000_REG_TDH 0x3810
#define E1000_REG_TDT 0x3818
#define E1000_REG_RAL 0x5400
#define E1000_REG_RAH 0x5404
#define E1000_REG_ICR 0x00C0

#define E1000_CTRL_RST (1U << 26)
#define E1000_RCTL_EN (1U << 1)
#define E1000_RCTL_BAM (1U << 15)
#define E1000_RCTL_SECRC (1U << 26)
#define E1000_TCTL_EN (1U << 1)
#define E1000_TCTL_PSP (1U << 3)
#define E1000_TX_CMD_EOP (1U << 0)
#define E1000_TX_CMD_IFCS (1U << 1)
#define E1000_TX_CMD_RS (1U << 3)
#define E1000_DESC_DONE (1U << 0)

typedef struct __attribute__((packed)) {
    uint64_t address;
    volatile uint16_t length;
    uint16_t checksum;
    volatile uint8_t status;
    volatile uint8_t errors;
    uint16_t special;
} e1000_rx_descriptor_t;

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    volatile uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} e1000_tx_descriptor_t;

static volatile uint32_t* registers;
static e1000_rx_descriptor_t* rx_ring;
static e1000_tx_descriptor_t* tx_ring;
static uint8_t* rx_buffers[E1000_RING_SIZE];
static uint8_t* tx_buffers[E1000_RING_SIZE];
static uint8_t mac_address[6];
static uint32_t rx_head;
static uint32_t tx_tail;
static int available;

static volatile uint32_t* e1000_register(uint32_t offset) {
    return (volatile uint32_t*)((volatile uint8_t*)registers + offset);
}

static void e1000_free_dma(void) {
    for (uint32_t i = 0; i < E1000_RING_SIZE; i++) {
        if (rx_buffers[i]) {
            pmm_free_block(rx_buffers[i]);
            rx_buffers[i] = 0;
        }
        if (tx_buffers[i]) {
            pmm_free_block(tx_buffers[i]);
            tx_buffers[i] = 0;
        }
    }
    if (rx_ring) {
        pmm_free_block(rx_ring);
        rx_ring = 0;
    }
    if (tx_ring) {
        pmm_free_block(tx_ring);
        tx_ring = 0;
    }
}

static void* e1000_allocate_dma_page(void) {
    void* page = pmm_alloc_block();
    if (page && (uint32_t)page >= E1000_DMA_LIMIT) {
        pmm_free_block(page);
        return 0;
    }
    return page;
}

static int e1000_read_mac_from_eeprom(void) {
    uint16_t words[3];
    for (uint32_t i = 0; i < 3; i++) {
        *e1000_register(E1000_REG_EERD) = (i << 8) | 1;
        uint32_t result = 0;
        for (uint32_t wait = 0; wait < E1000_WAIT_LIMIT; wait++) {
            result = *e1000_register(E1000_REG_EERD);
            if (result & (1U << 4)) {
                break;
            }
        }
        if (!(result & (1U << 4))) {
            return 0;
        }
        words[i] = (uint16_t)(result >> 16);
    }
    for (uint32_t i = 0; i < 3; i++) {
        mac_address[i * 2] = (uint8_t)words[i];
        mac_address[i * 2 + 1] = (uint8_t)(words[i] >> 8);
    }
    return 1;
}

static int e1000_valid_mac(void) {
    uint8_t any = 0;
    uint8_t all_ones = 0xFF;
    for (uint32_t i = 0; i < sizeof(mac_address); i++) {
        any |= mac_address[i];
        all_ones &= mac_address[i];
    }
    return any && all_ones != 0xFF && !(mac_address[0] & 1);
}

int e1000_init(void) {
    available = 0;
    registers = 0;
    memset(rx_buffers, 0, sizeof(rx_buffers));
    memset(tx_buffers, 0, sizeof(tx_buffers));
    rx_ring = 0;
    tx_ring = 0;

    const pci_device_t* controller = 0;
    for (uint32_t i = 0; i < pci_device_count(); i++) {
        const pci_device_t* candidate = pci_device_at(i);
        if (candidate && candidate->vendor_id == 0x8086 &&
            (candidate->device_id == 0x100E ||
             candidate->device_id == 0x10D3)) {
            controller = candidate;
            break;
        }
    }
    if (!controller) {
        return 0;
    }

    uint32_t bar = pci_read_config32(
        controller->bus, controller->device, controller->function, 0x10);
    if ((bar & 1) || !bar || bar == 0xFFFFFFFF) {
        return 0;
    }
    uint32_t bar_type = (bar >> 1) & 3;
    uint32_t high = bar_type == 2 ? pci_read_config32(
        controller->bus, controller->device, controller->function, 0x14) : 0;
    uint32_t physical_base = bar & 0xFFFFFFF0;
    if (high || (bar_type != 0 && bar_type != 2) ||
        !paging_map_kernel_mmio(E1000_MMIO_BASE, physical_base,
                                E1000_MMIO_SIZE)) {
        return 0;
    }
    registers = (volatile uint32_t*)E1000_MMIO_BASE;

    uint32_t command = pci_read_config32(
        controller->bus, controller->device, controller->function, 0x04);
    pci_write_config16(
        controller->bus, controller->device, controller->function, 0x04,
        (uint16_t)((command & 0xFFFF) | 0x0006));

    *e1000_register(E1000_REG_CTRL) |= E1000_CTRL_RST;
    for (uint32_t wait = 0; wait < E1000_WAIT_LIMIT; wait++) {
        if (!(*e1000_register(E1000_REG_CTRL) & E1000_CTRL_RST)) {
            break;
        }
    }
    if (*e1000_register(E1000_REG_CTRL) & E1000_CTRL_RST) {
        registers = 0;
        return 0;
    }

    *e1000_register(E1000_REG_IMC) = 0xFFFFFFFF;
    (void)*e1000_register(E1000_REG_ICR);
    uint32_t ral = *e1000_register(E1000_REG_RAL);
    uint32_t rah = *e1000_register(E1000_REG_RAH);
    for (uint32_t i = 0; i < 4; i++) {
        mac_address[i] = (uint8_t)(ral >> (i * 8));
    }
    mac_address[4] = (uint8_t)rah;
    mac_address[5] = (uint8_t)(rah >> 8);
    if (!e1000_valid_mac() && !e1000_read_mac_from_eeprom()) {
        registers = 0;
        return 0;
    }
    if (!e1000_valid_mac()) {
        registers = 0;
        return 0;
    }

    rx_ring = e1000_allocate_dma_page();
    tx_ring = e1000_allocate_dma_page();
    if (!rx_ring || !tx_ring) {
        e1000_free_dma();
        registers = 0;
        return 0;
    }
    memset(rx_ring, 0, PMM_BLOCK_SIZE);
    memset(tx_ring, 0, PMM_BLOCK_SIZE);
    for (uint32_t i = 0; i < E1000_RING_SIZE; i++) {
        rx_buffers[i] = e1000_allocate_dma_page();
        tx_buffers[i] = e1000_allocate_dma_page();
        if (!rx_buffers[i] || !tx_buffers[i]) {
            e1000_free_dma();
            registers = 0;
            return 0;
        }
        rx_ring[i].address = (uint32_t)rx_buffers[i];
        rx_ring[i].status = 0;
        tx_ring[i].address = (uint32_t)tx_buffers[i];
        tx_ring[i].status = E1000_DESC_DONE;
    }

    *e1000_register(E1000_REG_RDBAL) = (uint32_t)rx_ring;
    *e1000_register(E1000_REG_RDBAH) = 0;
    *e1000_register(E1000_REG_RDLEN) =
        E1000_RING_SIZE * sizeof(*rx_ring);
    *e1000_register(E1000_REG_RDH) = 0;
    *e1000_register(E1000_REG_RDT) = E1000_RING_SIZE - 1;
    *e1000_register(E1000_REG_TDBAL) = (uint32_t)tx_ring;
    *e1000_register(E1000_REG_TDBAH) = 0;
    *e1000_register(E1000_REG_TDLEN) =
        E1000_RING_SIZE * sizeof(*tx_ring);
    *e1000_register(E1000_REG_TDH) = 0;
    *e1000_register(E1000_REG_TDT) = 0;
    *e1000_register(E1000_REG_TIPG) = 10 | (8U << 10) | (6U << 20);
    asm volatile("" : : : "memory");
    *e1000_register(E1000_REG_RCTL) =
        E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC;
    *e1000_register(E1000_REG_TCTL) =
        E1000_TCTL_EN | E1000_TCTL_PSP | (0x10U << 4) | (0x40U << 12);
    rx_head = 0;
    tx_tail = 0;
    (void)*e1000_register(E1000_REG_STATUS);
    available = 1;
    return 1;
}

int e1000_is_available(void) {
    return available;
}

const uint8_t* e1000_mac_address(void) {
    return available ? mac_address : 0;
}

int e1000_send(const void* frame, uint16_t length) {
    if (!available || !frame || !length ||
        length > E1000_BUFFER_SIZE || length < 14) {
        return 0;
    }
    e1000_tx_descriptor_t* descriptor = &tx_ring[tx_tail];
    for (uint32_t wait = 0; wait < E1000_WAIT_LIMIT; wait++) {
        if (descriptor->status & E1000_DESC_DONE) {
            break;
        }
    }
    if (!(descriptor->status & E1000_DESC_DONE)) {
        return 0;
    }

    memcpy(tx_buffers[tx_tail], frame, length);
    if (length < 60) {
        memset(tx_buffers[tx_tail] + length, 0, 60 - length);
        length = 60;
    }
    descriptor->length = length;
    descriptor->checksum_offset = 0;
    descriptor->command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS |
        E1000_TX_CMD_RS;
    descriptor->status = 0;
    descriptor->checksum_start = 0;
    descriptor->special = 0;
    tx_tail = (tx_tail + 1) % E1000_RING_SIZE;
    asm volatile("" : : : "memory");
    *e1000_register(E1000_REG_TDT) = tx_tail;

    for (uint32_t wait = 0; wait < E1000_WAIT_LIMIT; wait++) {
        if (descriptor->status & E1000_DESC_DONE) {
            return 1;
        }
    }
    return 0;
}

int e1000_receive(void* frame, uint16_t capacity) {
    if (!available || !frame || !capacity) {
        return 0;
    }
    e1000_rx_descriptor_t* descriptor = &rx_ring[rx_head];
    if (!(descriptor->status & E1000_DESC_DONE)) {
        return 0;
    }

    uint16_t length = descriptor->length;
    int valid = !descriptor->errors && length && length <= capacity;
    if (valid) {
        memcpy(frame, rx_buffers[rx_head], length);
    }
    descriptor->status = 0;
    descriptor->length = 0;
    asm volatile("" : : : "memory");
    *e1000_register(E1000_REG_RDT) = rx_head;
    rx_head = (rx_head + 1) % E1000_RING_SIZE;
    return valid ? length : -1;
}
