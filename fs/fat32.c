#include <fs/fat32.h>
#include <drivers/ata.h>
#include <kernel/serial.h>
#include <string.h>

struct fat32_bpb {
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved0[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} __attribute__((packed));

struct fat_dirent {
    uint8_t name[11];
    uint8_t attr;
    uint8_t nt_reserved;
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed));

#define ATTR_DIRECTORY 0x10
#define ATTR_VOLUME_ID 0x08
#define ATTR_LFN       0x0F
#define FAT_EOC_MIN    0x0FFFFFF8

static int mounted = 0;
static uint32_t fat_start_lba, data_start_lba, fat_size_32, root_cluster;
static uint32_t sectors_per_cluster, total_clusters, num_fats;

static uint32_t cluster_to_lba(uint32_t cluster) {
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

static uint32_t fat_read_entry(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_lba = fat_start_lba + fat_offset / ATA_SECTOR_SIZE;
    uint32_t ent_offset = fat_offset % ATA_SECTOR_SIZE;
    uint8_t sector[ATA_SECTOR_SIZE];
    ata_read_sectors(sector_lba, 1, sector);
    uint32_t val;
    memcpy(&val, sector + ent_offset, 4);
    return val & 0x0FFFFFFF;
}

static void fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_lba = fat_start_lba + fat_offset / ATA_SECTOR_SIZE;
    uint32_t ent_offset = fat_offset % ATA_SECTOR_SIZE;
    uint8_t sector[ATA_SECTOR_SIZE];
    ata_read_sectors(sector_lba, 1, sector);
    uint32_t existing;
    memcpy(&existing, sector + ent_offset, 4);
    uint32_t merged = (value & 0x0FFFFFFF) | (existing & 0xF0000000);
    memcpy(sector + ent_offset, &merged, 4);
    ata_write_sectors(sector_lba, 1, sector);
    if (num_fats > 1) {
        ata_write_sectors(sector_lba + fat_size_32, 1, sector);
    }
}

static uint32_t find_free_cluster(void) {
    for (uint32_t c = 2; c < total_clusters + 2; c++) {
        if (fat_read_entry(c) == 0) return c;
    }
    return 0;
}

static void free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < FAT_EOC_MIN) {
        uint32_t next = fat_read_entry(cluster);
        fat_write_entry(cluster, 0);
        cluster = next;
    }
}

static uint32_t alloc_chain(uint32_t count) {
    uint32_t first = 0, prev = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c = find_free_cluster();
        if (!c) { if (first) free_chain(first); return 0; }
        fat_write_entry(c, 0x0FFFFFFF);
        if (prev) fat_write_entry(prev, c);
        else first = c;
        prev = c;
    }
    return first;
}

int fat32_init(void) {
    uint8_t sector[ATA_SECTOR_SIZE];
    if (!ata_read_sectors(0, 1, sector)) {
        serial_printf("fat32: could not read boot sector\n");
        return 0;
    }

    struct fat32_bpb *bpb = (struct fat32_bpb *)sector;
    if (bpb->bytes_per_sector != ATA_SECTOR_SIZE || bpb->fat_size_32 == 0) {
        serial_printf("fat32: not a FAT32 volume (bps=%d fatsz32=%u)\n",
                      bpb->bytes_per_sector, bpb->fat_size_32);
        return 0;
    }

    fat_start_lba = bpb->reserved_sector_count;
    fat_size_32 = bpb->fat_size_32;
    num_fats = bpb->num_fats;
    data_start_lba = fat_start_lba + num_fats * fat_size_32;
    sectors_per_cluster = bpb->sectors_per_cluster;
    root_cluster = bpb->root_cluster;

    uint32_t data_sectors = bpb->total_sectors_32 - data_start_lba;
    total_clusters = data_sectors / sectors_per_cluster;

    mounted = 1;
    serial_printf("fat32: mounted, root_cluster=%u spc=%u clusters=%u\n",
                  root_cluster, sectors_per_cluster, total_clusters);
    return 1;
}

uint32_t fat32_root_cluster(void) {
    return root_cluster;
}

int fat32_is_mounted(void) {
    return mounted;
}

static void format_dirent_name(const uint8_t raw[11], char *out) {
    int i = 0, j = 0;
    while (i < 8 && raw[i] != ' ') out[j++] = (char)raw[i++];
    if (raw[8] != ' ') {
        out[j++] = '.';
        i = 8;
        while (i < 11 && raw[i] != ' ') out[j++] = (char)raw[i++];
    }
    out[j] = 0;
}

static void format_name_to_83(const char *name, uint8_t raw[11]) {
    memset(raw, ' ', 11);
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) raw[j++] = (uint8_t)name[i++];
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') {
        i++;
        j = 8;
        while (name[i] && j < 11) raw[j++] = (uint8_t)name[i++];
    }
}

int fat32_list_dir(uint32_t dir_cluster, struct fat_dirent_info *out, int max_entries) {
    if (!mounted) return 0;
    int count = 0;
    uint32_t cluster = dir_cluster;
    uint8_t sector[ATA_SECTOR_SIZE];

    while (cluster >= 2 && cluster < FAT_EOC_MIN && count < max_entries) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            ata_read_sectors(lba + s, 1, sector);
            for (int off = 0; off < ATA_SECTOR_SIZE; off += 32) {
                struct fat_dirent *de = (struct fat_dirent *)(sector + off);
                if (de->name[0] == 0x00) return count; /* end of directory */
                if (de->name[0] == 0xE5) continue;     /* deleted */
                if (de->attr == ATTR_LFN) continue;
                if (de->attr & ATTR_VOLUME_ID) continue;
                if (count >= max_entries) return count;

                format_dirent_name(de->name, out[count].name);
                out[count].is_dir = (de->attr & ATTR_DIRECTORY) != 0;
                out[count].size = de->file_size;
                out[count].cluster = ((uint32_t)de->cluster_high << 16) | de->cluster_low;
                count++;
            }
        }
        cluster = fat_read_entry(cluster);
    }
    return count;
}

uint32_t fat32_read_file(uint32_t cluster, uint32_t file_size, void *buf, uint32_t max_len) {
    if (!mounted) return 0;
    uint32_t total = file_size < max_len ? file_size : max_len;
    uint32_t read_so_far = 0;
    uint8_t *out = (uint8_t *)buf;
    uint8_t sector[ATA_SECTOR_SIZE];

    while (cluster >= 2 && cluster < FAT_EOC_MIN && read_so_far < total) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster && read_so_far < total; s++) {
            ata_read_sectors(lba + s, 1, sector);
            uint32_t chunk = total - read_so_far;
            if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
            memcpy(out + read_so_far, sector, chunk);
            read_so_far += chunk;
        }
        cluster = fat_read_entry(cluster);
    }
    return read_so_far;
}

static void write_chain(uint32_t cluster, const uint8_t *data, uint32_t len) {
    uint32_t written = 0;
    uint8_t sector[ATA_SECTOR_SIZE];

    while (cluster >= 2 && cluster < FAT_EOC_MIN && written < len) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster && written < len; s++) {
            uint32_t chunk = len - written;
            if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
            memset(sector, 0, ATA_SECTOR_SIZE);
            memcpy(sector, data + written, chunk);
            ata_write_sectors(lba + s, 1, sector);
            written += chunk;
        }
        cluster = fat_read_entry(cluster);
    }
}

/* Writes `de` (already positioned at raw_name) with a freshly allocated
 * cluster chain holding `data`/`len`, replacing whatever cluster/size it
 * had before (0 for a brand-new entry, or a real chain being overwritten
 * -- callers free the old chain first if there was one). */
static int write_dirent_contents(struct fat_dirent *de, const void *data, uint32_t len) {
    uint32_t cluster_bytes = sectors_per_cluster * ATA_SECTOR_SIZE;
    uint32_t clusters_needed = len ? (len + cluster_bytes - 1) / cluster_bytes : 0;
    uint32_t new_cluster = clusters_needed ? alloc_chain(clusters_needed) : 0;
    if (clusters_needed && !new_cluster) return 0; /* disk full */
    if (new_cluster) write_chain(new_cluster, (const uint8_t *)data, len);
    de->cluster_high = (uint16_t)(new_cluster >> 16);
    de->cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    de->file_size = len;
    return 1;
}

/* Overwrites the file if a directory entry with this name already
 * exists; otherwise creates a new entry (reusing a deleted/unused slot
 * in an existing directory cluster, or growing the directory by one
 * cluster if every slot in it is already in use) and writes it there.
 * Returns 1 on success, 0 if the disk is completely full. */
int fat32_write_file(uint32_t dir_cluster, const char *name_8_3, const void *data, uint32_t len) {
    if (!mounted) return 0;

    uint8_t raw_name[11];
    format_name_to_83(name_8_3, raw_name);

    uint32_t cluster = dir_cluster;
    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t last_cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT_EOC_MIN) {
        last_cluster = cluster;
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            ata_read_sectors(lba + s, 1, sector);
            for (int off = 0; off < ATA_SECTOR_SIZE; off += 32) {
                struct fat_dirent *de = (struct fat_dirent *)(sector + off);
                int is_free = (de->name[0] == 0x00 || de->name[0] == 0xE5);
                int is_match = !is_free && de->attr != ATTR_LFN && !(de->attr & ATTR_VOLUME_ID) &&
                               memcmp(de->name, raw_name, 11) == 0;
                if (!is_free && !is_match) continue;

                if (is_match) {
                    uint32_t old_cluster = ((uint32_t)de->cluster_high << 16) | de->cluster_low;
                    if (old_cluster >= 2) free_chain(old_cluster);
                } else {
                    memset(de, 0, sizeof(*de));
                    memcpy(de->name, raw_name, 11);
                }

                if (!write_dirent_contents(de, data, len)) return 0;
                ata_write_sectors(lba + s, 1, sector);
                return 1;
            }
        }
        cluster = fat_read_entry(cluster);
    }

    /* No name match and no free slot anywhere in the directory -- grow
     * it by one cluster and use the first slot there. */
    uint32_t new_dir_cluster = alloc_chain(1);
    if (!new_dir_cluster) return 0;
    memset(sector, 0, ATA_SECTOR_SIZE);
    uint32_t lba = cluster_to_lba(new_dir_cluster);
    for (uint32_t s = 0; s < sectors_per_cluster; s++) ata_write_sectors(lba + s, 1, sector);
    fat_write_entry(last_cluster, new_dir_cluster);

    ata_read_sectors(lba, 1, sector);
    struct fat_dirent *de = (struct fat_dirent *)sector;
    memcpy(de->name, raw_name, 11);
    if (!write_dirent_contents(de, data, len)) return 0;
    ata_write_sectors(lba, 1, sector);
    return 1;
}
