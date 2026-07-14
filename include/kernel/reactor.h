#ifndef KERNEL_REACTOR_H
#define KERNEL_REACTOR_H

#include <stdint.h>

#define REACTOR_MAX_WATCHES 64
#define REACTOR_QUEUE_DEPTH 64

enum reactor_source { REACTOR_TIMER, REACTOR_IPC, REACTOR_SIGNAL, REACTOR_SERVICE, REACTOR_OBJECT };

struct reactor_event {
    uint32_t id;
    enum reactor_source source;
    int owner_pid;
    uint32_t key;
    uint32_t value;
    uint32_t timestamp;
};

void reactor_init(void);
int reactor_watch(int owner_pid, enum reactor_source source, uint32_t key, uint32_t interval_ticks);
int reactor_unwatch(int watch_id);
int reactor_emit(enum reactor_source source, uint32_t key, uint32_t value);
int reactor_poll(int owner_pid, struct reactor_event *out);
void reactor_tick(void);
uint32_t reactor_event_count(void);
uint32_t reactor_drop_count(void);

#endif
