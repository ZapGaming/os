#include <kernel/registry.h>
#include <string.h>

struct registry_tx_change {
    int used;
    int deleted;
    struct registry_entry entry;
};

struct registry_tx {
    int used;
    struct registry_tx_change changes[REGISTRY_MAX_KEYS];
};

static struct registry_entry entries[REGISTRY_MAX_KEYS];
static int entry_count;
static struct registry_tx transactions[REGISTRY_TX_MAX];
static uint32_t generation;

static void copy_text(char *dst, int cap, const char *src) {
    int i = 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int find_key(const char *key) {
    for (int i = 0; i < entry_count; i++) if (strcmp(entries[i].key, key) == 0) return i;
    return -1;
}

void registry_init(void) {
    memset(entries, 0, sizeof(entries));
    memset(transactions, 0, sizeof(transactions));
    entry_count = 0;
    generation = 1;
}

int registry_begin(void) {
    for (int i = 0; i < REGISTRY_TX_MAX; i++) {
        if (transactions[i].used) continue;
        memset(&transactions[i], 0, sizeof(transactions[i]));
        transactions[i].used = 1;
        return i;
    }
    return -1;
}

static struct registry_tx_change *tx_slot(int tx, const char *key) {
    if (tx < 0 || tx >= REGISTRY_TX_MAX || !transactions[tx].used) return 0;
    for (int i = 0; i < REGISTRY_MAX_KEYS; i++) {
        if (transactions[tx].changes[i].used && strcmp(transactions[tx].changes[i].entry.key, key) == 0)
            return &transactions[tx].changes[i];
    }
    for (int i = 0; i < REGISTRY_MAX_KEYS; i++) {
        if (!transactions[tx].changes[i].used) {
            transactions[tx].changes[i].used = 1;
            copy_text(transactions[tx].changes[i].entry.key, REGISTRY_KEY_MAX, key);
            return &transactions[tx].changes[i];
        }
    }
    return 0;
}

int registry_set(int tx, const char *key, enum registry_value_type type, const char *value) {
    if (!key || !key[0] || !value) return 0;
    struct registry_tx_change *c = tx_slot(tx, key);
    if (!c) return 0;
    c->deleted = 0;
    c->entry.type = type;
    copy_text(c->entry.value, REGISTRY_VALUE_MAX, value);
    return 1;
}

int registry_delete(int tx, const char *key) {
    struct registry_tx_change *c = tx_slot(tx, key);
    if (!c) return 0;
    c->deleted = 1;
    return 1;
}

int registry_commit(int tx) {
    if (tx < 0 || tx >= REGISTRY_TX_MAX || !transactions[tx].used) return 0;
    generation++;
    for (int i = 0; i < REGISTRY_MAX_KEYS; i++) {
        struct registry_tx_change *c = &transactions[tx].changes[i];
        if (!c->used) continue;
        int idx = find_key(c->entry.key);
        if (c->deleted) {
            if (idx >= 0) {
                for (int j = idx; j < entry_count - 1; j++) entries[j] = entries[j + 1];
                entry_count--;
            }
            continue;
        }
        if (idx < 0) {
            if (entry_count >= REGISTRY_MAX_KEYS) continue;
            idx = entry_count++;
        }
        entries[idx] = c->entry;
        entries[idx].version = generation;
    }
    memset(&transactions[tx], 0, sizeof(transactions[tx]));
    return 1;
}

int registry_rollback(int tx) {
    if (tx < 0 || tx >= REGISTRY_TX_MAX || !transactions[tx].used) return 0;
    memset(&transactions[tx], 0, sizeof(transactions[tx]));
    return 1;
}

int registry_get(const char *key, struct registry_entry *out) {
    int idx = find_key(key);
    if (idx < 0 || !out) return 0;
    *out = entries[idx];
    return 1;
}

int registry_snapshot(int index, struct registry_entry *out) {
    if (!out || index < 0 || index >= entry_count) return 0;
    *out = entries[index];
    return 1;
}

int registry_count(void) { return entry_count; }
uint32_t registry_generation(void) { return generation; }
