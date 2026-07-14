#include <kernel/signal.h>
#include <kernel/scheduler.h>
#include <kernel/capability.h>
#include <kernel/pit.h>
#include <string.h>

struct signal_queue {
    struct signal_info items[SIGNAL_QUEUE_DEPTH];
    int head;
    int tail;
    int count;
};

static struct signal_queue queues[SIGNAL_MAX_TASKS];
static uint32_t sent_count;
static uint32_t dropped_count;

void signal_init(void) {
    memset(queues, 0, sizeof(queues));
    sent_count = 0;
    dropped_count = 0;
}

int signal_send(int sender_pid, int target_pid, enum nova_signal signal, uint32_t value) {
    if (target_pid <= 0 || target_pid >= SIGNAL_MAX_TASKS || signal == NOVA_SIG_NONE) return 0;
    if (sender_pid != target_pid && !capability_has(sender_pid, CAP_PROCESS_KILL)) return 0;
    struct signal_queue *q = &queues[target_pid];
    if (q->count >= SIGNAL_QUEUE_DEPTH) { dropped_count++; return 0; }
    struct signal_info *info = &q->items[q->tail];
    info->signal = signal;
    info->sender_pid = sender_pid;
    info->value = value;
    info->timestamp = pit_ticks();
    q->tail = (q->tail + 1) % SIGNAL_QUEUE_DEPTH;
    q->count++;
    sent_count++;
    return 1;
}

int signal_receive(int target_pid, struct signal_info *out) {
    if (!out || target_pid < 0 || target_pid >= SIGNAL_MAX_TASKS) return 0;
    struct signal_queue *q = &queues[target_pid];
    if (!q->count) return 0;
    *out = q->items[q->head];
    q->head = (q->head + 1) % SIGNAL_QUEUE_DEPTH;
    q->count--;
    return 1;
}

int signal_pending(int target_pid) {
    if (target_pid < 0 || target_pid >= SIGNAL_MAX_TASKS) return 0;
    return queues[target_pid].count;
}

void signal_dispatch_scheduler(void) {
    for (int pid = 1; pid < scheduler_task_count(); pid++) {
        if (!queues[pid].count) continue;
        struct signal_info info;
        if (!signal_receive(pid, &info)) continue;
        if (info.signal == NOVA_SIG_TERM || info.signal == NOVA_SIG_KILL) scheduler_kill(pid);
        else if (info.signal == NOVA_SIG_STOP) scheduler_set_affinity(pid, SCHED_CPU_BSP);
        else if (info.signal == NOVA_SIG_CONT) scheduler_set_affinity(pid, SCHED_CPU_BSP);
    }
}

uint32_t signal_total_sent(void) { return sent_count; }
uint32_t signal_total_dropped(void) { return dropped_count; }
