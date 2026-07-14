#include <kernel/nova.h>
#include <kernel/pit.h>
#include <kernel/spinlock.h>
#include <fs/fat32.h>
#include <string.h>

#define NOVA_SETTINGS_FILE "NOVA.CFG"

static struct nova_event events[NOVA_EVENT_CAP];
static int event_head;
static int event_total;

static struct nova_notice notices[NOVA_NOTICE_CAP];
static int notice_head;
static int notice_total;
static uint32_t next_notice_id = 1;

static struct nova_settings settings;
static struct nova_health health;
static spinlock_t nova_lock = SPINLOCK_INIT;

static void copy_text(char *dst, int cap, const char *src) {
    int i = 0;
    if (!src || cap <= 0) return;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int visible_notice_count_locked(void) {
    int count = 0;
    int limit = notice_total < NOVA_NOTICE_CAP ? notice_total : NOVA_NOTICE_CAP;
    for (int i = 0; i < limit; i++) {
        if (!notices[i].dismissed) count++;
    }
    return count;
}

void nova_init(void) {
    memset(events, 0, sizeof(events));
    memset(notices, 0, sizeof(notices));
    memset(&health, 0, sizeof(health));

    settings.accent = 0;
    settings.reduced_motion = 0;
    settings.compact_mode = 0;
    settings.performance_mode = 0;
    settings.show_seconds = 1;

    event_head = 0;
    event_total = 0;
    notice_head = 0;
    notice_total = 0;
    next_notice_id = 1;

    if (fat32_is_mounted()) nova_settings_load();
    nova_event_emit(NOVA_SUCCESS, "kernel", "Nova service layer initialized");
    nova_notice_post(NOVA_INFO, "Welcome to Nova", "The redesigned ZapOS session is ready.");
}

void nova_pulse(void) {
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    health.uptime_ticks = pit_ticks();
    health.heartbeat++;
    lock_release_irqrestore(&nova_lock, flags);
}

void nova_event_emit(uint8_t severity, const char *source, const char *text) {
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    struct nova_event *e = &events[event_head];
    e->tick = pit_ticks();
    e->severity = severity;
    copy_text(e->source, sizeof(e->source), source ? source : "system");
    copy_text(e->text, sizeof(e->text), text ? text : "");
    event_head = (event_head + 1) % NOVA_EVENT_CAP;
    event_total++;
    health.events_written++;
    health.last_activity_tick = e->tick;
    lock_release_irqrestore(&nova_lock, flags);
}

int nova_event_count(void) {
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    int count = event_total < NOVA_EVENT_CAP ? event_total : NOVA_EVENT_CAP;
    lock_release_irqrestore(&nova_lock, flags);
    return count;
}

int nova_event_read(int newest_index, struct nova_event *out) {
    if (!out || newest_index < 0) return 0;
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    int count = event_total < NOVA_EVENT_CAP ? event_total : NOVA_EVENT_CAP;
    if (newest_index >= count) {
        lock_release_irqrestore(&nova_lock, flags);
        return 0;
    }
    int index = event_head - 1 - newest_index;
    while (index < 0) index += NOVA_EVENT_CAP;
    *out = events[index];
    lock_release_irqrestore(&nova_lock, flags);
    return 1;
}

uint32_t nova_notice_post(uint8_t severity, const char *title, const char *text) {
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    struct nova_notice *n = &notices[notice_head];
    n->id = next_notice_id++;
    n->created_tick = pit_ticks();
    n->severity = severity;
    n->dismissed = 0;
    copy_text(n->title, sizeof(n->title), title ? title : "Notice");
    copy_text(n->text, sizeof(n->text), text ? text : "");
    notice_head = (notice_head + 1) % NOVA_NOTICE_CAP;
    notice_total++;
    health.notices_written++;
    health.last_activity_tick = n->created_tick;
    uint32_t id = n->id;
    lock_release_irqrestore(&nova_lock, flags);
    return id;
}

int nova_notice_count(void) {
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    int count = visible_notice_count_locked();
    lock_release_irqrestore(&nova_lock, flags);
    return count;
}

int nova_notice_read(int newest_index, struct nova_notice *out) {
    if (!out || newest_index < 0) return 0;
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    int found = 0;
    int seen = 0;
    int limit = notice_total < NOVA_NOTICE_CAP ? notice_total : NOVA_NOTICE_CAP;
    for (int step = 0; step < limit; step++) {
        int index = notice_head - 1 - step;
        while (index < 0) index += NOVA_NOTICE_CAP;
        if (notices[index].dismissed) continue;
        if (seen == newest_index) {
            *out = notices[index];
            found = 1;
            break;
        }
        seen++;
    }
    lock_release_irqrestore(&nova_lock, flags);
    return found;
}

void nova_notice_dismiss(uint32_t id) {
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    int limit = notice_total < NOVA_NOTICE_CAP ? notice_total : NOVA_NOTICE_CAP;
    for (int i = 0; i < limit; i++) {
        if (notices[i].id == id) notices[i].dismissed = 1;
    }
    lock_release_irqrestore(&nova_lock, flags);
}

void nova_notice_dismiss_all(void) {
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    int limit = notice_total < NOVA_NOTICE_CAP ? notice_total : NOVA_NOTICE_CAP;
    for (int i = 0; i < limit; i++) notices[i].dismissed = 1;
    lock_release_irqrestore(&nova_lock, flags);
}

void nova_settings_get(struct nova_settings *out) {
    if (!out) return;
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    *out = settings;
    lock_release_irqrestore(&nova_lock, flags);
}

void nova_settings_set(const struct nova_settings *next) {
    if (!next) return;
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    settings = *next;
    if (settings.accent > 3) settings.accent = 0;
    settings.reduced_motion = settings.reduced_motion ? 1 : 0;
    settings.compact_mode = settings.compact_mode ? 1 : 0;
    settings.performance_mode = settings.performance_mode ? 1 : 0;
    settings.show_seconds = settings.show_seconds ? 1 : 0;
    health.last_activity_tick = pit_ticks();
    lock_release_irqrestore(&nova_lock, flags);
    nova_event_emit(NOVA_INFO, "settings", "Nova preferences updated");
}

int nova_settings_load(void) {
    if (!fat32_is_mounted()) return 0;
    struct fat_dirent_info entries[32];
    int count = fat32_list_dir(fat32_root_cluster(), entries, 32);
    for (int i = 0; i < count; i++) {
        if (!entries[i].is_dir && strcmp(entries[i].name, NOVA_SETTINGS_FILE) == 0) {
            uint8_t bytes[8];
            uint32_t got = fat32_read_file(entries[i].cluster, entries[i].size, bytes, sizeof(bytes));
            if (got < 5) return 0;
            settings.accent = bytes[0] <= 3 ? bytes[0] : 0;
            settings.reduced_motion = bytes[1] ? 1 : 0;
            settings.compact_mode = bytes[2] ? 1 : 0;
            settings.performance_mode = bytes[3] ? 1 : 0;
            settings.show_seconds = bytes[4] ? 1 : 0;
            return 1;
        }
    }
    return 0;
}

int nova_settings_save(void) {
    if (!fat32_is_mounted()) return 0;
    uint8_t bytes[8];
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    bytes[0] = settings.accent;
    bytes[1] = settings.reduced_motion;
    bytes[2] = settings.compact_mode;
    bytes[3] = settings.performance_mode;
    bytes[4] = settings.show_seconds;
    bytes[5] = 'N';
    bytes[6] = 'V';
    bytes[7] = 1;
    lock_release_irqrestore(&nova_lock, flags);
    int ok = fat32_write_file(fat32_root_cluster(), NOVA_SETTINGS_FILE, bytes, sizeof(bytes));
    nova_event_emit(ok ? NOVA_SUCCESS : NOVA_ERROR, "settings", ok ? "Preferences saved to disk" : "Preference save failed");
    return ok;
}

void nova_health_get(struct nova_health *out) {
    if (!out) return;
    uint32_t flags = lock_acquire_irqsave(&nova_lock);
    health.uptime_ticks = pit_ticks();
    *out = health;
    lock_release_irqrestore(&nova_lock, flags);
}
