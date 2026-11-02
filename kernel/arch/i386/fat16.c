#include <kernel/ata.h>
#include <kernel/conversions.h>
#include <kernel/fat16.h>
#include <kernel/heap.h>
#include <kernel/tty.h>
#include <stdio.h>
#include <string.h>

#define FAT16_PARTITION_TYPE_CHS 0x04
#define FAT16_PARTITION_TYPE_LBA 0x06
#define FAT16_PARTITION_TYPE_LBA_EXT 0x0E
#define FAT16_ROOT_ENTRIES_PER_SECTOR (ATA_SECTOR_SIZE / sizeof(fat16_dir_entry_t))
#define FAT16_LFN_ATTRIBUTE 0x0F
#define FAT16_LFN_MAX_LENGTH 255
#define FAT16_LFN_MAX_ENTRIES 20

typedef struct __attribute__((packed)) {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attrib;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster_low;
    uint16_t name3[2];
} fat16_lfn_entry_t;

typedef struct {
    uint16_t characters[FAT16_LFN_MAX_ENTRIES * 13];
    uint8_t expected_order;
    uint8_t checksum;
    int valid;
} fat16_lfn_state_t;

typedef struct __attribute__((packed)) {
    uint8_t boot_indicator;
    uint8_t start_chs[3];
    uint8_t partition_type;
    uint8_t end_chs[3];
    uint32_t start_lba;
    uint32_t sector_count;
} partition_entry_t;

static fat16_bpb_t bpb;
static uint32_t disk_sector_count;
static uint32_t partition_start_lba;
static uint32_t partition_sector_count;
static uint32_t volume_sector_count;
static uint32_t fat_start_sector;
static uint32_t root_start_sector;
static uint32_t data_start_sector;
static uint32_t max_cluster;
static int mounted;

static int read_root_entry(uint32_t index, fat16_dir_entry_t* entry,
                           uint8_t* sector_buffer);

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

static int volume_read_sector(uint32_t sector, uint8_t* buffer) {
    if (sector >= volume_sector_count ||
        sector >= partition_sector_count ||
        partition_start_lba > UINT32_MAX - sector) {
        return 0;
    }
    return ata_read_sector(partition_start_lba + sector, buffer);
}

static int volume_write_sector(uint32_t sector, const uint8_t* buffer) {
    if (sector >= volume_sector_count ||
        sector >= partition_sector_count ||
        partition_start_lba > UINT32_MAX - sector) {
        return 0;
    }
    return ata_write_sector(partition_start_lba + sector, buffer);
}

static int is_fat16_partition_type(uint8_t type) {
    return type == FAT16_PARTITION_TYPE_CHS ||
        type == FAT16_PARTITION_TYPE_LBA ||
        type == FAT16_PARTITION_TYPE_LBA_EXT;
}

static int try_mount(uint32_t start_lba, uint32_t sector_count,
                     const uint8_t* boot_sector) {
    fat16_bpb_t candidate;
    memcpy(&candidate, boot_sector, sizeof(candidate));

    uint16_t signature;
    memcpy(&signature, boot_sector + 510, sizeof(signature));
    if (signature != 0xAA55 || candidate.bytes_per_sector != ATA_SECTOR_SIZE ||
        !candidate.sectors_per_cluster ||
        (candidate.sectors_per_cluster & (candidate.sectors_per_cluster - 1)) ||
        !candidate.reserved_sectors || !candidate.num_fats ||
        !candidate.root_dir_entries || !candidate.sectors_per_fat) {
        return 0;
    }

    uint32_t total_sectors = candidate.total_sectors_short
        ? candidate.total_sectors_short
        : candidate.total_sectors_large;
    if (!total_sectors || total_sectors > sector_count ||
        start_lba > disk_sector_count ||
        total_sectors > disk_sector_count - start_lba) {
        return 0;
    }

    uint32_t candidate_root_sectors =
        ((uint32_t)candidate.root_dir_entries * sizeof(fat16_dir_entry_t)
         + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    uint64_t candidate_root_start =
        (uint64_t)candidate.reserved_sectors +
        (uint64_t)candidate.num_fats * candidate.sectors_per_fat;
    uint64_t candidate_data_start =
        candidate_root_start + candidate_root_sectors;
    if (candidate_data_start >= total_sectors ||
        candidate_root_start > UINT32_MAX ||
        candidate_data_start > UINT32_MAX) {
        return 0;
    }

    uint32_t cluster_count =
        (total_sectors - (uint32_t)candidate_data_start) /
        candidate.sectors_per_cluster;
    if (cluster_count < 4085 || cluster_count >= 65525) {
        return 0;
    }

    uint64_t required_fat_bytes =
        ((uint64_t)cluster_count + 2) * sizeof(uint16_t);
    uint64_t fat_bytes =
        (uint64_t)candidate.sectors_per_fat * ATA_SECTOR_SIZE;
    if (required_fat_bytes > fat_bytes) {
        return 0;
    }

    bpb = candidate;
    partition_start_lba = start_lba;
    partition_sector_count = sector_count;
    volume_sector_count = total_sectors;
    fat_start_sector = candidate.reserved_sectors;
    root_start_sector = (uint32_t)candidate_root_start;
    data_start_sector = (uint32_t)candidate_data_start;
    max_cluster = cluster_count + 1;
    mounted = 1;
    return 1;
}

void fat16_init(void) {
    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t sector_count = 0;

    mounted = 0;
    disk_sector_count = 0;
    volume_sector_count = 0;
    if (!ata_init(&sector_count)) {
        printf("FAT16: ATA primary-master disk is unavailable or lacks LBA28.\n");
        return;
    }
    disk_sector_count = sector_count;

    if (!ata_read_sector(0, sector)) {
        printf("FAT16: Could not read the disk partition table.\n");
        return;
    }

    uint16_t signature;
    memcpy(&signature, sector + 510, sizeof(signature));
    if (signature == 0xAA55) {
        partition_entry_t* entries = (partition_entry_t*)(sector + 446);
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t start = entries[i].start_lba;
            uint32_t size = entries[i].sector_count;
            if (!is_fat16_partition_type(entries[i].partition_type) ||
                !size || start >= disk_sector_count ||
                size > disk_sector_count - start ||
                !ata_read_sector(start, sector)) {
                continue;
            }
            if (try_mount(start, size, sector)) {
                printf("FAT16: Mounted partition %s on ATA primary master.\n",
                       i == 0 ? "1" : i == 1 ? "2" : i == 2 ? "3" : "4");
                return;
            }
        }
    }

    // Keep existing superfloppy-format FAT16 images usable as whole-disk volumes.
    if (!ata_read_sector(0, sector)) {
        printf("FAT16: Could not reread the disk boot sector.\n");
        return;
    }
    if (try_mount(0, disk_sector_count, sector)) {
        printf("FAT16: Mounted whole-disk FAT16 volume on ATA primary master.\n");
        return;
    }

    printf("FAT16: No valid FAT16 primary partition found.\n");
}

static void format_name(const fat16_dir_entry_t* entry, char* out) {
    uint32_t p = 0;
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

static int valid_long_filename(const char* filename, size_t* length) {
    size_t size = 0;
    if (!filename || !*filename) {
        return 0;
    }

    while (filename[size]) {
        unsigned char c = (unsigned char)filename[size];
        if (c < 0x20 || c >= 0x7F || c == '"' || c == '*' ||
            c == '/' || c == ':' || c == '<' || c == '>' ||
            c == '?' || c == '\\' || c == '|') {
            return 0;
        }
        if (++size > FAT16_LFN_MAX_LENGTH) {
            return 0;
        }
    }
    if (filename[size - 1] == ' ' || filename[size - 1] == '.' ||
        (size == 1 && filename[0] == '.') ||
        (size == 2 && filename[0] == '.' && filename[1] == '.')) {
        return 0;
    }
    *length = size;
    return 1;
}

static void reset_lfn_state(fat16_lfn_state_t* state) {
    memset(state, 0, sizeof(*state));
}

static void store_lfn_part(fat16_lfn_state_t* state,
                           const fat16_lfn_entry_t* entry) {
    uint8_t order = entry->order & 0x1F;
    if (entry->order & 0x40) {
        reset_lfn_state(state);
        state->valid = order > 0 && order <= FAT16_LFN_MAX_ENTRIES;
        state->expected_order = order;
        state->checksum = entry->checksum;
    }

    if (!state->valid || !order || order != state->expected_order ||
        entry->attrib != FAT16_LFN_ATTRIBUTE || entry->type != 0 ||
        entry->first_cluster_low != 0 ||
        entry->checksum != state->checksum) {
        reset_lfn_state(state);
        return;
    }

    uint16_t* destination = state->characters + (order - 1) * 13;
    memcpy(destination, entry->name1, sizeof(entry->name1));
    memcpy(destination + 5, entry->name2, sizeof(entry->name2));
    memcpy(destination + 11, entry->name3, sizeof(entry->name3));
    state->expected_order--;
}

static int decode_lfn(const fat16_lfn_state_t* state,
                      const fat16_dir_entry_t* short_entry, char* name) {
    uint8_t checksum = 0;
    const uint8_t* short_name = (const uint8_t*)short_entry->name;
    for (size_t i = 0; i < 11; i++) {
        checksum = (uint8_t)(((checksum & 1) ? 0x80 : 0) +
                             (checksum >> 1) + short_name[i]);
    }
    if (!state->valid || state->expected_order ||
        checksum != state->checksum) {
        return 0;
    }

    size_t length = 0;
    size_t capacity = FAT16_LFN_MAX_ENTRIES * 13;
    while (length < capacity && length < FAT16_LFN_MAX_LENGTH) {
        uint16_t c = state->characters[length];
        if (c == 0 || c == 0xFFFF) {
            break;
        }
        if (c > 0x7F) {
            return 0;
        }
        name[length] = (char)c;
        length++;
    }
    if (!length || length > FAT16_LFN_MAX_LENGTH) {
        return 0;
    }
    name[length] = '\0';
    return 1;
}

static void copy_lfn_part(fat16_lfn_entry_t* entry, const char* filename,
                          size_t length, uint8_t order, uint8_t checksum) {
    uint16_t characters[13];
    size_t offset = (size_t)(order - 1) * 13;
    for (size_t i = 0; i < 13; i++) {
        size_t position = offset + i;
        if (position < length) {
            characters[i] = (uint8_t)filename[position];
        } else if (position == length) {
            characters[i] = 0;
        } else {
            characters[i] = 0xFFFF;
        }
    }

    memset(entry, 0, sizeof(*entry));
    entry->order = order;
    entry->attrib = FAT16_LFN_ATTRIBUTE;
    entry->checksum = checksum;
    memcpy(entry->name1, characters, sizeof(entry->name1));
    memcpy(entry->name2, characters + 5, sizeof(entry->name2));
    memcpy(entry->name3, characters + 11, sizeof(entry->name3));
}

static uint8_t short_name_checksum(const fat16_dir_entry_t* entry) {
    uint8_t checksum = 0;
    const uint8_t* name = (const uint8_t*)entry->name;
    for (size_t i = 0; i < 11; i++) {
        checksum = (uint8_t)(((checksum & 1) ? 0x80 : 0) +
                             (checksum >> 1) + name[i]);
    }
    return checksum;
}

static int write_root_entry(uint32_t index, const void* entry,
                            uint8_t* sector_buffer) {
    uint32_t sector = root_start_sector +
        index / FAT16_ROOT_ENTRIES_PER_SECTOR;
    uint32_t offset =
        (index % FAT16_ROOT_ENTRIES_PER_SECTOR) * sizeof(fat16_dir_entry_t);
    if (index >= bpb.root_dir_entries ||
        !volume_read_sector(sector, sector_buffer)) {
        return 0;
    }
    memcpy(sector_buffer + offset, entry, sizeof(fat16_dir_entry_t));
    return volume_write_sector(sector, sector_buffer);
}

static int make_fat_name(const char* src, char* name, char* ext) {
    uint32_t name_length = 0;
    uint32_t ext_length = 0;
    int in_extension = 0;

    memset(name, ' ', 8);
    memset(ext, ' ', 3);
    if (!src || !*src) {
        return 0;
    }

    for (; *src; src++) {
        char c = *src;
        if (c == '.' && !in_extension) {
            if (!name_length) {
                return 0;
            }
            in_extension = 1;
            continue;
        }
        if (c == '.') {
            return 0;
        }
        if (c <= ' ' || c >= 127 || c == '/' || c == '\\' ||
            c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            return 0;
        }

        if (in_extension) {
            if (ext_length == 3) {
                return 0;
            }
            if (c >= 'a' && c <= 'z') {
                c -= 'a' - 'A';
            }
            ext[ext_length++] = c;
        } else {
            if (name_length == 8) {
                return 0;
            }
            if (c >= 'a' && c <= 'z') {
                c -= 'a' - 'A';
            }
            name[name_length++] = c;
        }
    }

    return name_length != 0 && (!in_extension || ext_length != 0);
}

static char make_alias_character(char c) {
    if (c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
    }
    const char* allowed = "$%'-_@~`!(){}^#&";
    int allowed_character = 0;
    for (size_t i = 0; allowed[i]; i++) {
        if (allowed[i] == c) {
            allowed_character = 1;
            break;
        }
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        allowed_character) {
        return c;
    }
    return '_';
}

static int short_alias_exists(const char* name, const char* ext,
                              uint8_t* sector_buffer) {
    for (uint32_t i = 0; i < bpb.root_dir_entries; i++) {
        fat16_dir_entry_t entry;
        if (!read_root_entry(i, &entry, sector_buffer)) {
            return -1;
        }
        if (entry.name[0] == 0) {
            return 0;
        }
        if ((uint8_t)entry.name[0] == 0xE5 ||
            entry.attrib == FAT16_LFN_ATTRIBUTE || (entry.attrib & 0x08)) {
            continue;
        }
        if (!memcmp(entry.name, name, 8) &&
            !memcmp(entry.ext, ext, 3)) {
            return 1;
        }
    }
    return 0;
}

static int make_short_alias(const char* filename, size_t length,
                            char* name, char* ext, int* needs_lfn,
                            uint8_t* sector_buffer) {
    if (make_fat_name(filename, name, ext)) {
        *needs_lfn = 0;
        return 1;
    }

    *needs_lfn = 1;
    memset(name, ' ', 8);
    memset(ext, ' ', 3);

    size_t base_end = length;
    size_t extension_start = length;
    for (size_t i = 0; i < length; i++) {
        if (filename[i] == '.') {
            extension_start = i + 1;
        }
    }
    if (extension_start < length) {
        base_end = extension_start - 1;
        size_t ext_length = length - extension_start;
        for (size_t i = 0; i < ext_length && i < 3; i++) {
            ext[i] = make_alias_character(filename[extension_start + i]);
        }
    }

    char base[256];
    size_t base_length = 0;
    for (size_t i = 0; i < base_end; i++) {
        if (filename[i] != '.') {
            base[base_length++] = make_alias_character(filename[i]);
        }
    }
    if (!base_length) {
        base[base_length++] = '_';
    }

    for (uint32_t suffix = 1; suffix <= 999999; suffix++) {
        char digits[7];
        uint32_t value = suffix;
        size_t digit_count = 0;
        do {
            digits[digit_count++] = (char)('0' + value % 10);
            value /= 10;
        } while (value && digit_count < sizeof(digits));

        size_t prefix_length = 8 - digit_count - 1;
        if (prefix_length > base_length) {
            prefix_length = base_length;
        }
        memset(name, ' ', 8);
        memcpy(name, base, prefix_length);
        name[prefix_length] = '~';
        for (size_t i = 0; i < digit_count; i++) {
            name[prefix_length + 1 + i] = digits[digit_count - i - 1];
        }

        int exists = short_alias_exists(name, ext, sector_buffer);
        if (exists < 0) {
            return 0;
        }
        if (!exists) {
            return 1;
        }
    }
    return 0;
}

static int read_root_entry(uint32_t index, fat16_dir_entry_t* entry,
                           uint8_t* sector_buffer) {
    uint32_t sector = root_start_sector +
        index / FAT16_ROOT_ENTRIES_PER_SECTOR;
    uint32_t offset =
        (index % FAT16_ROOT_ENTRIES_PER_SECTOR) * sizeof(fat16_dir_entry_t);

    if (!volume_read_sector(sector, sector_buffer)) {
        return 0;
    }
    memcpy(entry, sector_buffer + offset, sizeof(*entry));
    return 1;
}

static int read_fat_entry(uint16_t cluster, uint16_t* value,
                          uint8_t* sector_buffer) {
    uint32_t byte_offset = (uint32_t)cluster * sizeof(uint16_t);
    uint32_t sector = fat_start_sector + byte_offset / ATA_SECTOR_SIZE;
    uint32_t offset = byte_offset % ATA_SECTOR_SIZE;
    if (sector >= fat_start_sector + bpb.sectors_per_fat ||
        !volume_read_sector(sector, sector_buffer)) {
        return 0;
    }
    memcpy(value, sector_buffer + offset, sizeof(*value));
    return 1;
}

static int write_fat_entry(uint16_t cluster, uint16_t value,
                           uint8_t* sector_buffer) {
    uint32_t byte_offset = (uint32_t)cluster * sizeof(uint16_t);
    uint32_t relative_sector = byte_offset / ATA_SECTOR_SIZE;
    uint32_t offset = byte_offset % ATA_SECTOR_SIZE;
    if (relative_sector >= bpb.sectors_per_fat) {
        return 0;
    }

    for (uint32_t copy = 0; copy < bpb.num_fats; copy++) {
        uint32_t sector = fat_start_sector +
            copy * bpb.sectors_per_fat + relative_sector;
        if (!volume_read_sector(sector, sector_buffer)) {
            return 0;
        }
        memcpy(sector_buffer + offset, &value, sizeof(value));
        if (!volume_write_sector(sector, sector_buffer)) {
            return 0;
        }
    }
    return 1;
}

static int release_clusters(const uint16_t* clusters, uint32_t count,
                            uint8_t* sector_buffer) {
    int success = 1;
    for (uint32_t i = 0; i < count; i++) {
        if (!write_fat_entry(clusters[i], 0, sector_buffer)) {
            success = 0;
        }
    }
    return success;
}

static int release_cluster_chain(uint16_t cluster, uint8_t* sector_buffer) {
    uint32_t traversed = 0;
    while (cluster >= 2 && cluster <= max_cluster &&
           traversed++ < max_cluster - 1) {
        uint16_t next;
        if (!read_fat_entry(cluster, &next, sector_buffer)) {
            return 0;
        }
        if (!write_fat_entry(cluster, 0, sector_buffer)) {
            return 0;
        }
        if (next >= 0xFFF8) {
            return 1;
        }
        if (next < 2 || next > max_cluster) {
            return 0;
        }
        cluster = next;
    }
    return cluster == 0;
}

static int find_file(const char* filename, fat16_dir_entry_t* found,
                     uint32_t* found_index, uint8_t* sector_buffer) {
    fat16_lfn_state_t lfn_state;
    reset_lfn_state(&lfn_state);
    for (uint32_t i = 0; i < bpb.root_dir_entries; i++) {
        fat16_dir_entry_t entry;
        if (!read_root_entry(i, &entry, sector_buffer)) {
            printf("FAT16: Could not read the root directory.\n");
            return -1;
        }
        if (entry.name[0] == 0) {
            return 0;
        }
        if ((uint8_t)entry.name[0] == 0xE5) {
            reset_lfn_state(&lfn_state);
            continue;
        }
        if (entry.attrib == FAT16_LFN_ATTRIBUTE) {
            store_lfn_part(&lfn_state, (fat16_lfn_entry_t*)&entry);
            continue;
        }
        if (entry.attrib & 0x08) {
            reset_lfn_state(&lfn_state);
            continue;
        }

        char short_name[16];
        char long_name[FAT16_LFN_MAX_LENGTH + 1];
        format_name(&entry, short_name);
        int has_long_name = decode_lfn(&lfn_state, &entry, long_name);
        reset_lfn_state(&lfn_state);
        if (filename_equal(short_name, filename) ||
            (has_long_name && filename_equal(long_name, filename))) {
            *found = entry;
            if (found_index) {
                *found_index = i;
            }
            return 1;
        }
    }
    return 0;
}

static int find_free_entries(uint32_t required, uint32_t* start_index,
                             uint8_t* sector_buffer) {
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    for (uint32_t i = 0; i < bpb.root_dir_entries; i++) {
        fat16_dir_entry_t entry;
        if (!read_root_entry(i, &entry, sector_buffer)) {
            return -1;
        }
        if (entry.name[0] == 0) {
            if (!run_length) {
                run_start = i;
            }
            run_length = bpb.root_dir_entries - run_start;
            if (run_length >= required) {
                *start_index = run_start;
                return 1;
            }
            return 0;
        }
        if ((uint8_t)entry.name[0] == 0xE5) {
            if (!run_length) {
                run_start = i;
            }
            if (++run_length >= required) {
                *start_index = run_start;
                return 1;
            }
        } else {
            run_length = 0;
        }
    }
    return 0;
}

static int write_long_name_entries(const char* filename, size_t length,
                                   uint32_t short_entry_index,
                                   const fat16_dir_entry_t* short_entry,
                                   uint8_t* sector_buffer) {
    uint32_t count = (length + 12) / 13;
    if (!count) {
        return 1;
    }

    uint8_t checksum = short_name_checksum(short_entry);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t order = (uint8_t)(count - i);
        fat16_lfn_entry_t entry;
        copy_lfn_part(&entry, filename, length, order, checksum);
        if (i == 0) {
            entry.order |= 0x40;
        }
        if (!write_root_entry(short_entry_index - count + i, &entry,
                              sector_buffer)) {
            return 0;
        }
    }
    return 1;
}

void fat16_touch(const char* filename) {
    uint8_t sector_buffer[ATA_SECTOR_SIZE];
    fat16_dir_entry_t entry;
    uint32_t filename_length;
    uint32_t existing_index;
    uint32_t free_start;
    int needs_lfn;
    int result;
    char name[8];
    char ext[3];

    if (!mounted) {
        printf("FAT16 volume is not mounted.\n");
        return;
    }
    if (!valid_long_filename(filename, &filename_length)) {
        printf("Usage: touch <filename> (ASCII, up to 255 characters)\n");
        return;
    }

    result = find_file(filename, &entry, &existing_index, sector_buffer);
    if (result < 0) {
        return;
    }
    if (result) {
        printf("FAT16: File %s already exists.\n", filename);
        return;
    }

    if (!make_short_alias(filename, filename_length, name, ext, &needs_lfn,
                          sector_buffer)) {
        printf("FAT16: Could not create a unique short filename alias.\n");
        return;
    }

    uint32_t lfn_count = needs_lfn ? (filename_length + 12) / 13 : 0;
    result = find_free_entries(lfn_count + 1, &free_start, sector_buffer);
    if (result < 0) {
        printf("FAT16: Could not read the root directory.\n");
        return;
    }
    if (!result) {
        printf("FAT16: Not enough consecutive root directory entries.\n");
        return;
    }

    memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, name, sizeof(name));
    memcpy(entry.ext, ext, sizeof(ext));
    entry.attrib = 0x20;
    uint32_t short_entry_index = free_start + lfn_count;
    if (!write_long_name_entries(filename, filename_length,
                                 short_entry_index, &entry, sector_buffer) ||
        !write_root_entry(short_entry_index, &entry, sector_buffer)) {
        printf("FAT16: Could not write the new root directory entry.\n");
        return;
    }

    printf("File %s created successfully.\n", filename);
}

void fat16_write(const char* filename, const char* data) {
    uint8_t sector_buffer[ATA_SECTOR_SIZE];
    fat16_dir_entry_t entry;
    uint32_t filename_length;
    uint32_t entry_index = 0;
    uint32_t free_start;
    uint32_t lfn_count = 0;
    int replacing;
    int needs_lfn = 0;
    int result;
    char name[8];
    char ext[3];

    if (!mounted) {
        printf("FAT16 volume is not mounted.\n");
        return;
    }
    if (!data || !valid_long_filename(filename, &filename_length)) {
        printf("Usage: write <filename> <data>\n");
        return;
    }

    result = find_file(filename, &entry, &entry_index, sector_buffer);
    if (result < 0) {
        return;
    }
    replacing = result;
    if (replacing) {
        if (entry.attrib & 0x10) {
            printf("FAT16: Cannot write to a directory.\n");
            return;
        }
    } else {
        if (!make_short_alias(filename, filename_length, name, ext,
                              &needs_lfn, sector_buffer)) {
            printf("FAT16: Could not create a unique short filename alias.\n");
            return;
        }
        lfn_count = needs_lfn ? (filename_length + 12) / 13 : 0;
        result = find_free_entries(lfn_count + 1, &free_start, sector_buffer);
        if (result < 0) {
            printf("FAT16: Could not read the root directory.\n");
            return;
        }
        if (!result) {
            printf("FAT16: Not enough consecutive root directory entries.\n");
            return;
        }
        entry_index = free_start + lfn_count;
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.name, name, sizeof(name));
        memcpy(entry.ext, ext, sizeof(ext));
        entry.attrib = 0x20;
    }

    uint32_t data_length = strlen(data);
    uint32_t cluster_size =
        (uint32_t)bpb.sectors_per_cluster * ATA_SECTOR_SIZE;
    uint32_t cluster_count =
        (data_length + (uint64_t)cluster_size - 1) / cluster_size;
    if (data_length > UINT32_MAX ||
        (uint64_t)cluster_count > max_cluster - 1) {
        printf("FAT16: File is too large for this volume.\n");
        return;
    }

    uint16_t old_first_cluster = entry.first_cluster_low;
    uint16_t* new_clusters = 0;
    if (cluster_count) {
        new_clusters = (uint16_t*)kmalloc(cluster_count * sizeof(uint16_t));
        if (!new_clusters) {
            printf("FAT16: Not enough memory to prepare the file write.\n");
            return;
        }

        uint32_t found_clusters = 0;
        for (uint32_t cluster = 2;
             cluster <= max_cluster && found_clusters < cluster_count;
             cluster++) {
            uint16_t value;
            if (!read_fat_entry((uint16_t)cluster, &value, sector_buffer)) {
                printf("FAT16: Could not read the allocation table.\n");
                kfree(new_clusters);
                return;
            }
            if (value == 0) {
                new_clusters[found_clusters++] = (uint16_t)cluster;
            }
        }
        if (found_clusters != cluster_count) {
            printf("FAT16: Not enough free space on the volume.\n");
            kfree(new_clusters);
            return;
        }
    }

    uint32_t written_clusters = 0;
    for (; written_clusters < cluster_count; written_clusters++) {
        uint16_t next = written_clusters + 1 < cluster_count
            ? new_clusters[written_clusters + 1]
            : 0xFFFF;
        if (!write_fat_entry(new_clusters[written_clusters], next,
                             sector_buffer)) {
            if (!release_clusters(new_clusters, written_clusters + 1,
                                  sector_buffer)) {
                printf("FAT16: Failed to roll back partial allocation.\n");
            }
            kfree(new_clusters);
            printf("FAT16: Could not update the allocation tables.\n");
            return;
        }
    }

    uint32_t remaining = data_length;
    uint32_t data_offset = 0;
    for (uint32_t i = 0; i < cluster_count; i++) {
        uint32_t first_sector = data_start_sector +
            ((uint32_t)new_clusters[i] - 2) * bpb.sectors_per_cluster;
        for (uint32_t s = 0; s < bpb.sectors_per_cluster; s++) {
            memset(sector_buffer, 0, sizeof(sector_buffer));
            uint32_t sector_bytes = remaining > ATA_SECTOR_SIZE
                ? ATA_SECTOR_SIZE
                : remaining;
            if (sector_bytes) {
                memcpy(sector_buffer, data + data_offset, sector_bytes);
            }
            if (!volume_write_sector(first_sector + s, sector_buffer)) {
                if (!release_clusters(new_clusters, cluster_count,
                                      sector_buffer)) {
                    printf("FAT16: Failed to roll back file allocation.\n");
                }
                kfree(new_clusters);
                printf("FAT16: Could not write file data.\n");
                return;
            }
            data_offset += sector_bytes;
            remaining -= sector_bytes;
        }
    }

    entry.first_cluster_low = cluster_count ? new_clusters[0] : 0;
    entry.file_size = data_length;
    if ((!replacing &&
         !write_long_name_entries(filename, filename_length, entry_index,
                                  &entry, sector_buffer)) ||
        !write_root_entry(entry_index, &entry, sector_buffer)) {
        if (!release_clusters(new_clusters, cluster_count, sector_buffer)) {
            printf("FAT16: Failed to roll back file allocation.\n");
        }
        kfree(new_clusters);
        printf("FAT16: Could not update the root directory.\n");
        return;
    }

    kfree(new_clusters);
    if (replacing && old_first_cluster >= 2 &&
        !release_cluster_chain(old_first_cluster, sector_buffer)) {
        printf("FAT16: File was written, but old clusters could not be freed.\n");
        return;
    }
    char size[16];
    inttostr(data_length, size);
    printf("Wrote %s (%s bytes).\n", filename, size);
}

void fat16_ls(void) {
    uint8_t sector_buffer[ATA_SECTOR_SIZE];
    fat16_lfn_state_t lfn_state;
    reset_lfn_state(&lfn_state);
    if (!mounted) {
        printf("FAT16 volume is not mounted.\n");
        return;
    }

    printf("Directory listing of FAT16 volume:\n");
    for (uint32_t i = 0; i < bpb.root_dir_entries; i++) {
        fat16_dir_entry_t entry;
        if (!read_root_entry(i, &entry, sector_buffer)) {
            printf("FAT16: Could not read the root directory.\n");
            return;
        }
        if (entry.name[0] == 0) {
            break;
        }
        if ((uint8_t)entry.name[0] == 0xE5) {
            reset_lfn_state(&lfn_state);
            continue;
        }
        if (entry.attrib == FAT16_LFN_ATTRIBUTE) {
            store_lfn_part(&lfn_state, (fat16_lfn_entry_t*)&entry);
            continue;
        }
        if (entry.attrib & 0x08) {
            reset_lfn_state(&lfn_state);
            continue;
        }

        char name[FAT16_LFN_MAX_LENGTH + 1];
        if (!decode_lfn(&lfn_state, &entry, name)) {
            format_name(&entry, name);
        }
        reset_lfn_state(&lfn_state);
        if (entry.attrib & 0x10) {
            printf("  [DIR]  %s\n", name);
        } else {
            char size[16];
            inttostr(entry.file_size, size);
            printf("  [FILE] %s (%s bytes)\n", name, size);
        }
    }
}

void fat16_cat(const char* filename) {
    uint8_t sector_buffer[ATA_SECTOR_SIZE];
    fat16_dir_entry_t entry;
    if (!mounted) {
        printf("FAT16 volume is not mounted.\n");
        return;
    }
    if (!filename || !*filename) {
        printf("Usage: cat <filename>\n");
        return;
    }

    int result = find_file(filename, &entry, 0, sector_buffer);
    if (result < 0) {
        return;
    }
    if (!result || (entry.attrib & 0x10)) {
        printf("File not found: %s\n", filename);
        return;
    }

    uint16_t cluster = entry.first_cluster_low;
    uint32_t bytes_left = entry.file_size;
    uint32_t cluster_limit = 0;
    while (bytes_left) {
        if (cluster < 2 || cluster > max_cluster ||
            ++cluster_limit > max_cluster - 1) {
            printf("\nFAT16: Invalid cluster chain in %s.\n", filename);
            return;
        }

        uint32_t cluster_sector = data_start_sector +
            ((uint32_t)cluster - 2) * bpb.sectors_per_cluster;
        for (uint32_t i = 0;
             i < bpb.sectors_per_cluster && bytes_left;
             i++) {
            if (!volume_read_sector(cluster_sector + i, sector_buffer)) {
                printf("\nFAT16: Could not read file data.\n");
                return;
            }
            uint32_t to_print = bytes_left > ATA_SECTOR_SIZE
                ? ATA_SECTOR_SIZE
                : bytes_left;
            for (uint32_t j = 0; j < to_print; j++) {
                terminal_putchar((char)sector_buffer[j]);
            }
            bytes_left -= to_print;
        }

        if (bytes_left) {
            uint16_t next_cluster;
            if (!read_fat_entry(cluster, &next_cluster, sector_buffer)) {
                printf("\nFAT16: Could not read the allocation table.\n");
                return;
            }
            if (next_cluster >= 0xFFF0 ||
                next_cluster < 2 || next_cluster > max_cluster) {
                printf("\nFAT16: Invalid cluster chain in %s.\n", filename);
                return;
            }
            cluster = next_cluster;
        }
    }
    printf("\n");
}
