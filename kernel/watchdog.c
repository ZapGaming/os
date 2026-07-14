#include <kernel/watchdog.h>
#include <kernel/pit.h>
#include <string.h>

static struct watchdog_channel channels[WATCHDOG_MAX_CHANNELS];
static int channel_count;
static struct recovery_record records[WATCHDOG_CRASH_LOG];
static int record_head;
static int record_count;
static int safe_mode;

static void copy_text(char *dst, int cap, const char *src) {
    int i = 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void watchdog_init(void) {
    memset(channels, 0, sizeof(channels));
    memset(records, 0, sizeof(records));
    channel_count = 0;
    record_head = 0;
    record_count = 0;
    safe_mode = 0;
}

int watchdog_register(const char *name, uint32_t timeout_ticks, int critical) {
    if (!name || timeout_ticks == 0 || channel_count >= WATCHDOG_MAX_CHANNELS) return 0;
    for (int i = 0; i < channel_count; i++) if (strcmp(channels[i].name, name) == 0) return 0;
    struct watchdog_channel *c = &channels[channel_count++];
    memset(c, 0, sizeof(*c));
    copy_text(c->name, sizeof(c->name), name);
    c->timeout_ticks = timeout_ticks;
    c->last_kick = pit_ticks();
    c->critical = critical ? 1 : 0;
    return 1;
}

int watchdog_kick(const char *name) {
    for (int i = 0; i < channel_count; i++) {
        if (strcmp(channels[i].name, name) != 0) continue;
        channels[i].last_kick = pit_ticks();
        channels[i].tripped = 0;
        return 1;
    }
    return 0;
}

void recovery_record_fault(const char *source, const char *reason, uint32_t code) {
    int slot = record_head;
    records[slot].tick = pit_ticks();
    records[slot].code = code;
    copy_text(records[slot].source, sizeof(records[slot].source), source);
    copy_text(records[slot].reason, sizeof(records[slot].reason), reason);
    record_head = (record_head + 1) % WATCHDOG_CRASH_LOG;
    if (record_count < WATCHDOG_CRASH_LOG) record_count++;
}

void watchdog_poll(void) {
    uint32_t now = pit_ticks();
    for (int i = 0; i < channel_count; i++) {
        struct watchdog_channel *c = &channels[i];
        if (c->tripped || now - c->last_kick <= c->timeout_ticks) continue;
        c->tripped = 1;
        c->failures++;
        recovery_record_fault(c->name, "watchdog timeout", c->failures);
        if (c->critical) safe_mode = 1;
    }
}

int watchdog_channel_count(void) { return channel_count; }
int watchdog_channel_snapshot(int index, struct watchdog_channel *out) {
    if (!out || index < 0 || index >= channel_count) return 0;
    *out = channels[index];
    return 1;
}

int recovery_record_count(void) { return record_count; }
int recovery_record_snapshot(int index, struct recovery_record *out) {
    if (!out || index < 0 || index >= record_count) return 0;
    int start = (record_head - record_count + WATCHDOG_CRASH_LOG) % WATCHDOG_CRASH_LOG;
    *out = records[(start + index) % WATCHDOG_CRASH_LOG];
    return 1;
}

int recovery_safe_mode(void) { return safe_mode; }
void recovery_set_safe_mode(int enabled) { safe_mode = enabled ? 1 : 0; }
