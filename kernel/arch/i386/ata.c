#include <kernel/ata.h>
#include <kernel/io.h>

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

static uint32_t ata_sector_count;

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

int ata_init(uint32_t* sector_count) {
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

int ata_read_sector(uint32_t lba, void* buffer) {
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

int ata_write_sector(uint32_t lba, const void* buffer) {
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
