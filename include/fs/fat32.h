#ifndef FS_FAT32_H
#define FS_FAT32_H

#include <stdint.h>

#define FAT32_MAX_NAME 13 /* "FILENAME.EXT\0" */

struct fat_dirent_info {
    char name[FAT32_MAX_NAME];
    int is_dir;
    uint32_t size;
    uint32_t cluster;
};

/* Reads the boot sector from the disk `ata_read_sectors` talks to and
 * computes the layout (FAT location, data region, root cluster). Returns
 * 1 on success, 0 if there's no disk or it's not a FAT32 volume. */
int fat32_init(void);
int fat32_is_mounted(void);

uint32_t fat32_root_cluster(void);

/* Lists up to `max_entries` entries of the directory at `dir_cluster`
 * (deleted/volume-label/long-filename entries are skipped). Returns the
 * number of entries written. */
int fat32_list_dir(uint32_t dir_cluster, struct fat_dirent_info *out, int max_entries);

/* Reads up to `max_len` bytes of the file starting at `cluster` into buf.
 * Returns the number of bytes actually read. */
uint32_t fat32_read_file(uint32_t cluster, uint32_t file_size, void *buf, uint32_t max_len);

/* Creates (or overwrites, if it already exists) an 8.3-named file in
 * `dir_cluster` with the given contents. Returns 1 on success. */
int fat32_write_file(uint32_t dir_cluster, const char *name_8_3, const void *data, uint32_t len);

#endif
