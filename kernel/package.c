#include <kernel/package.h>
#include <kernel/pit.h>
#include <kernel/vfs.h>
#include <string.h>

static struct package_manifest packages[PACKAGE_MAX];
static int packages_count;

uint32_t package_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < len; i++) { hash ^= p[i]; hash *= 16777619u; }
    return hash;
}

void package_manager_init(void) {
    memset(packages, 0, sizeof(packages));
    packages_count = 0;
}

int package_find(const char *name) {
    for (int i = 0; i < packages_count; i++) if (strcmp(packages[i].name, name) == 0) return i;
    return -1;
}

int package_validate(const struct package_manifest *m, const void *payload, uint32_t payload_size) {
    if (!m || !payload || !m->name[0] || !m->entry[0] || payload_size == 0) return 0;
    if (m->payload_size != payload_size) return 0;
    return m->checksum == package_checksum(payload, payload_size);
}

int package_install(const struct package_manifest *m, const void *payload, uint32_t payload_size) {
    if (!package_validate(m, payload, payload_size) || package_find(m->name) >= 0 || packages_count >= PACKAGE_MAX) return 0;
    char path[64] = "/PKG_";
    int pos = 5;
    for (int i = 0; m->name[i] && pos < 56; i++) path[pos++] = m->name[i];
    path[pos++] = '.'; path[pos++] = 'B'; path[pos++] = 'I'; path[pos++] = 'N'; path[pos] = 0;
    if (!vfs_write(path, payload, payload_size)) return 0;
    packages[packages_count] = *m;
    packages[packages_count].installed_tick = pit_ticks();
    packages[packages_count].enabled = 1;
    packages[packages_count].trusted = 1;
    packages_count++;
    return 1;
}

int package_remove(const char *name) {
    int idx = package_find(name);
    if (idx < 0) return 0;
    for (int i = idx; i < packages_count - 1; i++) packages[i] = packages[i + 1];
    packages_count--;
    return 1;
}

int package_enable(const char *name, int enabled) {
    int idx = package_find(name);
    if (idx < 0) return 0;
    packages[idx].enabled = enabled ? 1 : 0;
    return 1;
}

int package_count(void) { return packages_count; }
int package_snapshot(int index, struct package_manifest *out) {
    if (!out || index < 0 || index >= packages_count) return 0;
    *out = packages[index];
    return 1;
}
