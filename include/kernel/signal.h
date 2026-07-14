#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include <stdint.h>

#define SIGNAL_QUEUE_DEPTH 8
#define SIGNAL_MAX_TASKS 16

enum nova_signal {
    NOVA_SIG_NONE = 0,
    NOVA_SIG_TERM = 1,
    NOVA_SIG_KILL = 2,
    NOVA_SIG_STOP = 3,
    NOVA_SIG_CONT = 4,
    NOVA_SIG_USER1 = 5,
    NOVA_SIG_USER2 = 6
};

struct signal_info {
    enum nova_signal signal;
    int sender_pid;
    uint32_t value;
    uint32_t timestamp;
};

void signal_init(void);
int signal_send(int sender_pid, int target_pid, enum nova_signal signal, uint32_t value);
int signal_receive(int target_pid, struct signal_info *out);
int signal_pending(int target_pid);
void signal_dispatch_scheduler(void);
uint32_t signal_total_sent(void);
uint32_t signal_total_dropped(void);

#endif
