#ifndef KERNEL_REGISTRY_H
#define KERNEL_REGISTRY_H

#include <stdint.h>

#define REGISTRY_MAX_KEYS 64
#define REGISTRY_KEY_MAX 48
#define REGISTRY_VALUE_MAX 96
#define REGISTRY_TX_MAX 8

enum registry_value_type { REG_STRING, REG_U32, REG_BOOL };

struct registry_entry {
    char key[REGISTRY_KEY_MAX];
    enum registry_value_type type;
    char value[REGISTRY_VALUE_MAX];
    uint32_t version;
};

void registry_init(void);
int registry_begin(void);
int registry_set(int tx, const char *key, enum registry_value_type type, const char *value);
int registry_delete(int tx, const char *key);
int registry_commit(int tx);
int registry_rollback(int tx);
int registry_get(const char *key, struct registry_entry *out);
int registry_snapshot(int index, struct registry_entry *out);
int registry_count(void);
uint32_t registry_generation(void);

#endif
