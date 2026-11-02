#ifndef _KERNEL_ATA_H
#define _KERNEL_ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

int ata_init(uint32_t* sector_count);
int ata_read_sector(uint32_t lba, void* buffer);
int ata_write_sector(uint32_t lba, const void* buffer);

#endif
