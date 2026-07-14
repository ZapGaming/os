#ifndef KERNEL_SYSFS_H
#define KERNEL_SYSFS_H

#include <stdint.h>

#define SYSFS_PATH_MAX 64
#define SYSFS_READ_MAX 1024

void sysfs_init(void);
int sysfs_read(const char *path, char *out, uint32_t cap);
int sysfs_exists(const char *path);
int sysfs_list(const char *path, char *out, uint32_t cap);

#endif
