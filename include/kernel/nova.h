#ifndef KERNEL_NOVA_H
#define KERNEL_NOVA_H

#include <stdint.h>

#define NOVA_EVENT_CAP 32
#define NOVA_EVENT_TEXT 72
#define NOVA_NOTICE_CAP 8
#define NOVA_NOTICE_TEXT 80

/* Severity is intentionally tiny and ABI-stable so both kernel services and
 * the compositor can emit/read events without depending on GUI headers. */
enum nova_severity {
    NOVA_INFO = 0,
    NOVA_SUCCESS = 1,
    NOVA_WARNING = 2,
    NOVA_ERROR = 3
};

struct nova_event {
    uint32_t tick;
    uint8_t severity;
    char source[16];
    char text[NOVA_EVENT_TEXT];
};

struct nova_notice {
    uint32_t id;
    uint32_t created_tick;
    uint8_t severity;
    uint8_t dismissed;
    char title[24];
    char text[NOVA_NOTICE_TEXT];
};

struct nova_settings {
    uint8_t accent;          /* 0 cyan, 1 violet, 2 green, 3 amber */
    uint8_t reduced_motion;
    uint8_t compact_mode;
    uint8_t performance_mode;
    uint8_t show_seconds;
};

struct nova_health {
    uint32_t uptime_ticks;
    uint32_t heartbeat;
    uint32_t events_written;
    uint32_t notices_written;
    uint32_t last_activity_tick;
};

void nova_init(void);
void nova_pulse(void);

void nova_event_emit(uint8_t severity, const char *source, const char *text);
int nova_event_count(void);
int nova_event_read(int newest_index, struct nova_event *out);

uint32_t nova_notice_post(uint8_t severity, const char *title, const char *text);
int nova_notice_count(void);
int nova_notice_read(int newest_index, struct nova_notice *out);
void nova_notice_dismiss(uint32_t id);
void nova_notice_dismiss_all(void);

void nova_settings_get(struct nova_settings *out);
void nova_settings_set(const struct nova_settings *settings);
int nova_settings_load(void);
int nova_settings_save(void);

void nova_health_get(struct nova_health *out);

#endif
