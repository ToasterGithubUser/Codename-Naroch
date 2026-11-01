#include <kernel/fat16.h>
#include <stdio.h>
#include <kernel/conversions.h>
#include <kernel/tty.h>
static uint32_t disk_start = 0;
static uint32_t disk_size = 0;
static fat16_bpb_t* bpb = 0;
static uint32_t fat_addr = 0;
static uint32_t root_dir_addr = 0;
static uint32_t data_area_addr = 0;
static uint32_t max_cluster = 0;
static int strncmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

static int filename_equal(const char* a, const char* b) {
    while (*a && *b && ascii_lower(*a) == ascii_lower(*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

void fat16_init(uint32_t mem_addr, uint32_t size) {
    disk_start = 0;
    disk_size = 0;
    bpb = 0;

    if (!mem_addr || size < 512) {
        printf("FAT16: Invalid or truncated volume.\n");
        return;
    }

    fat16_bpb_t* volume_bpb = (fat16_bpb_t*)mem_addr;
    if (strncmp(volume_bpb->fs_type, "FAT16", 5) != 0 ||
        *(uint16_t*)(mem_addr + 510) != 0xAA55 ||
        volume_bpb->bytes_per_sector == 0 ||
        volume_bpb->sectors_per_cluster == 0 ||
        volume_bpb->reserved_sectors == 0 ||
        volume_bpb->num_fats == 0 ||
        volume_bpb->root_dir_entries == 0 ||
        volume_bpb->sectors_per_fat == 0) {
        printf("Fat16 FS: Invalid filesystem signature!\n");
        return;
    }

    uint32_t total_sectors = volume_bpb->total_sectors_short
        ? volume_bpb->total_sectors_short
        : volume_bpb->total_sectors_large;
    uint32_t bytes_per_sector = volume_bpb->bytes_per_sector;
    uint32_t root_dir_sectors =
        ((uint32_t)volume_bpb->root_dir_entries * sizeof(fat16_dir_entry_t)
         + bytes_per_sector - 1) / bytes_per_sector;
    uint64_t volume_bytes = (uint64_t)total_sectors * bytes_per_sector;
    uint64_t fat_start_sector = volume_bpb->reserved_sectors;
    uint64_t root_start_sector = fat_start_sector
        + (uint64_t)volume_bpb->num_fats * volume_bpb->sectors_per_fat;
    uint64_t data_start_sector = root_start_sector + root_dir_sectors;

    if (!total_sectors || volume_bytes > size ||
        data_start_sector >= total_sectors ||
        volume_bpb->sectors_per_cluster > total_sectors) {
        printf("FAT16: Invalid volume geometry.\n");
        return;
    }

    uint32_t cluster_count =
        (total_sectors - (uint32_t)data_start_sector) /
        volume_bpb->sectors_per_cluster;
    if (cluster_count < 4085 || cluster_count >= 65525) {
        printf("FAT16: Volume is not FAT16.\n");
        return;
    }

    uint64_t required_fat_bytes = ((uint64_t)cluster_count + 2) * sizeof(uint16_t);
    if ((uint64_t)volume_bpb->sectors_per_fat * bytes_per_sector <
        required_fat_bytes) {
        printf("FAT16: FAT is too small for the data area.\n");
        return;
    }

    disk_start = mem_addr;
    disk_size = size;
    bpb = volume_bpb;
    fat_addr = mem_addr + (uint32_t)(fat_start_sector * bytes_per_sector);
    root_dir_addr = mem_addr + (uint32_t)(root_start_sector * bytes_per_sector);
    data_area_addr = mem_addr + (uint32_t)(data_start_sector * bytes_per_sector);
    max_cluster = cluster_count + 1;
}

static void format_name(fat16_dir_entry_t* entry, char* out) {
    int p = 0;
    int name_length = 8;
    int ext_length = 3;
    while (name_length > 0 && entry->name[name_length - 1] == ' ') {
        name_length--;
    }
    while (ext_length > 0 && entry->ext[ext_length - 1] == ' ') {
        ext_length--;
    }

    for (int i = 0; i < name_length; i++) {
        out[p++] = entry->name[i];
    }
    if (ext_length > 0 && !(entry->attrib & 0x10)) {
        out[p++] = '.';
        for (int i = 0; i < ext_length; i++) {
            out[p++] = entry->ext[i];
        }
    }
    out[p] = '\0';
}

void fat16_ls(void) {
    if (!disk_start || !bpb) {
        printf("FAT16 volume is not mounted.\n");
        return;
    }

    fat16_dir_entry_t* entries = (fat16_dir_entry_t*)root_dir_addr;
    printf("Directory listing of FAT16 volume:\n");

    for (int i = 0; i < bpb->root_dir_entries; i++) {
        if (entries[i].name[0] == 0x00) break;       // Список пуст
        if ((uint8_t)entries[i].name[0] == 0xE5) continue; // Файл удален
        if (entries[i].attrib & 0x08) continue;       // Метка тома
        if (entries[i].attrib == 0x0F) continue;      // Long filename entry

        char namebuf[16];
        format_name(&entries[i], namebuf);

        if (entries[i].attrib & 0x10) {
            printf("  [DIR]  %s\n", namebuf);
        } else {
            char size_str[16];
            inttostr(entries[i].file_size, size_str);
            printf("  [FILE] %s (%s bytes)\n", namebuf, size_str);
        }
    }
}

void fat16_cat(const char* filename) {
    if (!disk_start || !bpb) {
        printf("FAT16 volume is not mounted.\n");
        return;
    }

    if (!filename || !*filename) {
        printf("Usage: cat <filename>\n");
        return;
    }

    fat16_dir_entry_t* entries = (fat16_dir_entry_t*)root_dir_addr;

    for (int i = 0; i < bpb->root_dir_entries; i++) {
        if (entries[i].name[0] == 0x00) break;
        if ((uint8_t)entries[i].name[0] == 0xE5) continue;
        if (entries[i].attrib & (0x08 | 0x10)) continue;

        char namebuf[16];
        format_name(&entries[i], namebuf);

        if (filename_equal(namebuf, filename)) {
            uint16_t cluster = entries[i].first_cluster_low;
            uint32_t bytes_left = entries[i].file_size;
            uint32_t cluster_size = bpb->sectors_per_cluster * bpb->bytes_per_sector;
            uint16_t* fat_table = (uint16_t*)fat_addr;
            uint32_t cluster_limit = 0;

            while (bytes_left > 0) {
                if (cluster < 2 || cluster > max_cluster ||
                    ++cluster_limit > max_cluster - 1) {
                    printf("\nFAT16: Invalid cluster chain in %s.\n", filename);
                    return;
                }

                uint64_t cluster_offset = (uint64_t)data_area_addr +
                    (uint64_t)(cluster - 2) * cluster_size;
                uint32_t to_read = bytes_left > cluster_size
                    ? cluster_size
                    : bytes_left;

                if (cluster_offset + to_read > (uint64_t)disk_start + disk_size) {
                    printf("\nFAT16: File data is outside the volume.\n");
                    return;
                }

                const char* cluster_data = (const char*)(uint32_t)cluster_offset;
                for (uint32_t j = 0; j < to_read; j++) {
                    terminal_putchar(cluster_data[j]);
                }

                bytes_left -= to_read;
                if (!bytes_left) {
                    break;
                }

                if ((uint64_t)fat_addr + ((uint64_t)cluster + 1) * sizeof(uint16_t) >
                    (uint64_t)disk_start + disk_size) {
                    printf("\nFAT16: FAT entry is outside the volume.\n");
                    return;
                }
                uint16_t next_cluster = fat_table[cluster];
                if (next_cluster >= 0xFFF8 || next_cluster == 0xFFF7) {
                    printf("\nFAT16: File ended before its size.\n");
                    return;
                }
                cluster = next_cluster;
            }
            printf("\n");
            return;
        }
    }
    printf("File not found: %s\n", filename);
}
