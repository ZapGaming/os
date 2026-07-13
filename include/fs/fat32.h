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
 * `dir_cluster` with the given contents -- allocates a new directory
 * entry (growing the directory by a cluster if every existing slot is
 * in use) when the name doesn't already exist. Returns 1 on success,
 * 0 if the disk is full. */
int fat32_write_file(uint32_t dir_cluster, const char *name_8_3, const void *data, uint32_t len);

/* Deletes a file or empty directory. Returns 1 on success, 0 if the
 * name doesn't exist or (for a directory) still has entries in it. */
int fat32_delete_file(uint32_t dir_cluster, const char *name_8_3);

/* Renames (same directory) or moves (different directory) a file or
 * directory, keeping its existing cluster chain -- cheap regardless of
 * size. Returns 1 on success, 0 if the source doesn't exist or the
 * destination name is already taken. */
int fat32_rename_file(uint32_t old_dir_cluster, const char *old_name_8_3,
                       uint32_t new_dir_cluster, const char *new_name_8_3);

/* Creates a new, empty subdirectory of `parent_cluster` (with standard
 * "."/".." entries -- see fat32_parent_cluster). Returns 1 on success,
 * 0 if the name already exists or the disk is full. */
int fat32_mkdir(uint32_t parent_cluster, const char *name_8_3);

/* Returns the parent of `dir_cluster` (reading its ".." entry), or the
 * root cluster for the root itself or a directory with no ".." entry. */
uint32_t fat32_parent_cluster(uint32_t dir_cluster);

#endif
