#ifndef KERNEL_PACKAGE_H
#define KERNEL_PACKAGE_H

#include <stdint.h>

#define PACKAGE_MAX 32
#define PACKAGE_NAME 32
#define PACKAGE_VERSION 16
#define PACKAGE_ENTRY 32
#define PACKAGE_CAPS 8

struct package_manifest {
    char name[PACKAGE_NAME];
    char version[PACKAGE_VERSION];
    char entry[PACKAGE_ENTRY];
    uint32_t requested_caps;
    uint32_t payload_size;
    uint32_t checksum;
    uint32_t installed_tick;
    int enabled;
    int trusted;
};

void package_manager_init(void);
int package_install(const struct package_manifest *manifest, const void *payload, uint32_t payload_size);
int package_remove(const char *name);
int package_enable(const char *name, int enabled);
int package_find(const char *name);
int package_count(void);
int package_snapshot(int index, struct package_manifest *out);
uint32_t package_checksum(const void *data, uint32_t len);
int package_validate(const struct package_manifest *manifest, const void *payload, uint32_t payload_size);

#endif
