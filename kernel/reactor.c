#include <kernel/reactor.h>
#include <kernel/pit.h>
#include <string.h>

struct reactor_watch_entry {
    int used;
    int id;
    int owner_pid;
    enum reactor_source source;
    uint32_t key;
    uint32_t interval_ticks;
    uint32_t next_fire;
};

static struct reactor_watch_entry watches[REACTOR_MAX_WATCHES];
static struct reactor_event queue[REACTOR_QUEUE_DEPTH];
static int q_head, q_tail, q_count;
static int next_watch_id = 1;
static uint32_t next_event_id = 1;
static uint32_t event_count_total;
static uint32_t drop_count_total;

void reactor_init(void) {
    memset(watches, 0, sizeof(watches));
    memset(queue, 0, sizeof(queue));
    q_head = q_tail = q_count = 0;
    next_watch_id = 1;
    next_event_id = 1;
    event_count_total = 0;
    drop_count_total = 0;
}

static int enqueue(int owner_pid, enum reactor_source source, uint32_t key, uint32_t value) {
    if (q_count >= REACTOR_QUEUE_DEPTH) {
        drop_count_total++;
        return 0;
    }
    struct reactor_event *e = &queue[q_tail];
    e->id = next_event_id++;
    e->source = source;
    e->owner_pid = owner_pid;
    e->key = key;
    e->value = value;
    e->timestamp = pit_ticks();
    q_tail = (q_tail + 1) % REACTOR_QUEUE_DEPTH;
    q_count++;
    event_count_total++;
    return 1;
}

int reactor_watch(int owner_pid, enum reactor_source source, uint32_t key, uint32_t interval_ticks) {
    for (int i = 0; i < REACTOR_MAX_WATCHES; i++) {
        if (watches[i].used) continue;
        watches[i].used = 1;
        watches[i].id = next_watch_id++;
        watches[i].owner_pid = owner_pid;
        watches[i].source = source;
        watches[i].key = key;
        watches[i].interval_ticks = interval_ticks;
        watches[i].next_fire = pit_ticks() + interval_ticks;
        return watches[i].id;
    }
    return -1;
}

int reactor_unwatch(int watch_id) {
    for (int i = 0; i < REACTOR_MAX_WATCHES; i++) {
        if (watches[i].used && watches[i].id == watch_id) {
            watches[i].used = 0;
            return 1;
        }
    }
    return 0;
}

int reactor_emit(enum reactor_source source, uint32_t key, uint32_t value) {
    int delivered = 0;
    for (int i = 0; i < REACTOR_MAX_WATCHES; i++) {
        struct reactor_watch_entry *w = &watches[i];
        if (!w->used || w->source != source || w->key != key) continue;
        delivered += enqueue(w->owner_pid, source, key, value);
    }
    return delivered;
}

int reactor_poll(int owner_pid, struct reactor_event *out) {
    if (!out || q_count == 0) return 0;
    int index = q_head;
    for (int n = 0; n < q_count; n++) {
        struct reactor_event *e = &queue[index];
        if (e->owner_pid == owner_pid) {
            *out = *e;
            int cur = index;
            while (cur != q_tail) {
                int next = (cur + 1) % REACTOR_QUEUE_DEPTH;
                if (next == q_tail) break;
                queue[cur] = queue[next];
                cur = next;
            }
            q_tail = (q_tail - 1 + REACTOR_QUEUE_DEPTH) % REACTOR_QUEUE_DEPTH;
            q_count--;
            return 1;
        }
        index = (index + 1) % REACTOR_QUEUE_DEPTH;
    }
    return 0;
}

void reactor_tick(void) {
    uint32_t now = pit_ticks();
    for (int i = 0; i < REACTOR_MAX_WATCHES; i++) {
        struct reactor_watch_entry *w = &watches[i];
        if (!w->used || w->source != REACTOR_TIMER || w->interval_ticks == 0) continue;
        if ((int32_t)(now - w->next_fire) >= 0) {
            enqueue(w->owner_pid, REACTOR_TIMER, w->key, now);
            w->next_fire = now + w->interval_ticks;
        }
    }
}

uint32_t reactor_event_count(void) { return event_count_total; }
uint32_t reactor_drop_count(void) { return drop_count_total; }
