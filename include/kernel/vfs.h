#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stdint.h>

#define VFS_PATH_MAX 128
#define VFS_NAME_MAX 32
#define VFS_MAX_MOUNTS 8
#define VFS_MAX_ENTRIES 64

#define VFS_NODE_FILE 1
#define VFS_NODE_DIR  2
#define VFS_NODE_VIRTUAL 4

struct vfs_dirent {
    char name[VFS_NAME_MAX];
    uint32_t size;
    uint32_t flags;
};

struct vfs_mount_info {
    char path[VFS_NAME_MAX];
    char type[VFS_NAME_MAX];
    int readonly;
    uint32_t reads;
    uint32_t writes;
    uint32_t errors;
};

void vfs_init(void);
int vfs_mount(const char *path, const char *type, int readonly);
int vfs_unmount(const char *path);
int vfs_read(const char *path, void *out, uint32_t cap, uint32_t *out_len);
int vfs_write(const char *path, const void *data, uint32_t len);
int vfs_list(const char *path, struct vfs_dirent *out, int cap);
int vfs_exists(const char *path);
int vfs_mount_count(void);
int vfs_mount_snapshot(int index, struct vfs_mount_info *out);

#endif
