#include <kernel/vfs.h>
#include <kernel/sysfs.h>
#include <fs/fat32.h>
#include <string.h>

static struct vfs_mount_info mounts[VFS_MAX_MOUNTS];
static int mount_count;

static void copy_text(char *dst, int cap, const char *src) {
    int i = 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int match_mount(const char *path) {
    int best = -1, best_len = -1;
    for (int i = 0; i < mount_count; i++) {
        int n = (int)strlen(mounts[i].path);
        if (strncmp(path, mounts[i].path, n) == 0 && n > best_len) { best = i; best_len = n; }
    }
    return best;
}

void vfs_init(void) {
    mount_count = 0;
    vfs_mount("/", "fat32", 0);
    vfs_mount("/proc", "procfs", 1);
    vfs_mount("/sys", "sysfs", 1);
    vfs_mount("/dev", "devfs", 1);
}

int vfs_mount(const char *path, const char *type, int readonly) {
    if (!path || !type || mount_count >= VFS_MAX_MOUNTS) return 0;
    for (int i = 0; i < mount_count; i++) if (strcmp(mounts[i].path, path) == 0) return 0;
    struct vfs_mount_info *m = &mounts[mount_count++];
    memset(m, 0, sizeof(*m));
    copy_text(m->path, sizeof(m->path), path);
    copy_text(m->type, sizeof(m->type), type);
    m->readonly = readonly;
    return 1;
}

int vfs_unmount(const char *path) {
    if (!path || strcmp(path, "/") == 0) return 0;
    for (int i = 0; i < mount_count; i++) {
        if (strcmp(mounts[i].path, path) != 0) continue;
        for (int j = i; j < mount_count - 1; j++) mounts[j] = mounts[j + 1];
        mount_count--;
        return 1;
    }
    return 0;
}

int vfs_read(const char *path, void *out, uint32_t cap, uint32_t *out_len) {
    if (!path || !out || cap == 0) return 0;
    int mi = match_mount(path);
    if (mi < 0) return 0;
    struct vfs_mount_info *m = &mounts[mi];
    if (strcmp(m->type, "procfs") == 0 || strcmp(m->type, "sysfs") == 0 || strcmp(m->type, "devfs") == 0) {
        int got = sysfs_read(path, (char *)out, cap);
        if (got < 0) { m->errors++; return 0; }
        m->reads++;
        if (out_len) *out_len = (uint32_t)got;
        return 1;
    }
    if (!fat32_is_mounted()) { m->errors++; return 0; }
    const char *name = path[0] == '/' ? path + 1 : path;
    struct fat_dirent_info entries[VFS_MAX_ENTRIES];
    int count = fat32_list_dir(fat32_root_cluster(), entries, VFS_MAX_ENTRIES);
    for (int i = 0; i < count; i++) {
        if (!entries[i].is_dir && strcmp(entries[i].name, name) == 0) {
            uint32_t got = fat32_read_file(entries[i].cluster, entries[i].size, out, cap);
            m->reads++;
            if (out_len) *out_len = got;
            return 1;
        }
    }
    m->errors++;
    return 0;
}

int vfs_write(const char *path, const void *data, uint32_t len) {
    int mi = match_mount(path);
    if (mi < 0 || mounts[mi].readonly || !fat32_is_mounted()) return 0;
    const char *name = path[0] == '/' ? path + 1 : path;
    if (!fat32_write_file(fat32_root_cluster(), name, data, len)) { mounts[mi].errors++; return 0; }
    mounts[mi].writes++;
    return 1;
}

int vfs_list(const char *path, struct vfs_dirent *out, int cap) {
    if (!path || !out || cap <= 0) return 0;
    if (strcmp(path, "/proc") == 0 || strcmp(path, "/sys") == 0 || strcmp(path, "/dev") == 0 || strcmp(path, "/") == 0) {
        char buf[512];
        int n = sysfs_list(path, buf, sizeof(buf));
        int count = 0, start = 0;
        for (int i = 0; i <= n && count < cap; i++) {
            if (buf[i] != '\n' && buf[i] != 0) continue;
            int len = i - start;
            if (len > 0) {
                if (len >= VFS_NAME_MAX) len = VFS_NAME_MAX - 1;
                memcpy(out[count].name, buf + start, len);
                out[count].name[len] = 0;
                out[count].size = 0;
                out[count].flags = VFS_NODE_VIRTUAL | (out[count].name[len - 1] == '/' ? VFS_NODE_DIR : VFS_NODE_FILE);
                count++;
            }
            start = i + 1;
        }
        return count;
    }
    if (!fat32_is_mounted()) return 0;
    struct fat_dirent_info entries[VFS_MAX_ENTRIES];
    int count = fat32_list_dir(fat32_root_cluster(), entries, cap < VFS_MAX_ENTRIES ? cap : VFS_MAX_ENTRIES);
    for (int i = 0; i < count; i++) {
        copy_text(out[i].name, sizeof(out[i].name), entries[i].name);
        out[i].size = entries[i].size;
        out[i].flags = entries[i].is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    }
    return count;
}

int vfs_exists(const char *path) {
    if (sysfs_exists(path)) return 1;
    uint8_t b;
    uint32_t n;
    return vfs_read(path, &b, 1, &n);
}

int vfs_mount_count(void) { return mount_count; }
int vfs_mount_snapshot(int index, struct vfs_mount_info *out) {
    if (!out || index < 0 || index >= mount_count) return 0;
    *out = mounts[index];
    return 1;
}
