#ifndef _KERNEL_FAT16_H
#define _KERNEL_FAT16_H

#include <stdint.h>
#include <stddef.h>

// BIOS Parameter Block для FAT16 (строго упакованный, 512 байт)
struct fat16_bpb {
    uint8_t  jmp[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;    // Обычно 512
    uint8_t  sectors_per_cluster;  // Сколько секторов в одном кластере
    uint16_t reserved_sectors;     // Зарезервированные сектора (включая этот)
    uint8_t  num_fats;             // Количество таблиц FAT (обычно 2)
    uint16_t root_dir_entries;     // Макс. количество файлов в корне (обычно 512)
    uint16_t total_sectors_short;
    uint8_t  media_type;
    uint16_t sectors_per_fat;      // Сколько секторов занимает одна таблица FAT
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_large;
    // Extended Boot Record
    uint8_t  drive_number;
    uint8_t  reserved;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];           // Должно быть "FAT16   "
} __attribute__((packed));
typedef struct fat16_bpb fat16_bpb_t;

// Запись о файле/папке в каталоге FAT16 (32 байта)
struct fat16_dir_entry {
    char     name[8];              // Имя (8 символов)
    char     ext[3];               // Расширение (3 символа)
    uint8_t  attrib;               // Атрибуты (0x10 - папка, 0x20 - файл)
    uint8_t  reserved;
    uint8_t  creation_time_ms;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;   // Всегда 0 для FAT16
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint16_t first_cluster_low;    // Номер первого кластера данных в таблице FAT
    uint32_t file_size;            // Размер файла в байтах
} __attribute__((packed));
typedef struct fat16_dir_entry fat16_dir_entry_t;

// Mount a FAT16 partition from the primary-master ATA disk.
void fat16_init(void);
int fat16_format(void);
void fat16_ls(void);
void fat16_cd(const char* path);
void fat16_mkdir(const char* name);
const char* fat16_get_cwd(void);
void fat16_cat(const char* filename);
void fat16_touch(const char* filename);
void fat16_write(const char* filename, const char* data);
int fat16_load_file(const char* path, void** contents, size_t* size);
int fat16_write_file(const char* path, const void* contents, size_t size);
int fat16_unlink(const char* path);
int fat16_rmdir(const char* path);
int fat16_is_mounted(void);

#endif
