#ifndef KERNEL_WATCHDOG_H
#define KERNEL_WATCHDOG_H

#include <stdint.h>

#define WATCHDOG_MAX_CHANNELS 16
#define WATCHDOG_NAME 24
#define WATCHDOG_CRASH_LOG 8

struct watchdog_channel {
    char name[WATCHDOG_NAME];
    uint32_t timeout_ticks;
    uint32_t last_kick;
    uint32_t failures;
    int critical;
    int tripped;
};

struct recovery_record {
    uint32_t tick;
    char source[WATCHDOG_NAME];
    char reason[64];
    uint32_t code;
};

void watchdog_init(void);
int watchdog_register(const char *name, uint32_t timeout_ticks, int critical);
int watchdog_kick(const char *name);
void watchdog_poll(void);
int watchdog_channel_count(void);
int watchdog_channel_snapshot(int index, struct watchdog_channel *out);
void recovery_record_fault(const char *source, const char *reason, uint32_t code);
int recovery_record_count(void);
int recovery_record_snapshot(int index, struct recovery_record *out);
int recovery_safe_mode(void);
void recovery_set_safe_mode(int enabled);

#endif
