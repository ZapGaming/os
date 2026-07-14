#include <kernel/capability.h>
#include <kernel/scheduler.h>
#include <kernel/spinlock.h>
#include <string.h>

static uint32_t process_caps[MAX_TASKS];
static spinlock_t cap_lock = SPINLOCK_INIT;

void capability_init(void) {
    memset(process_caps, 0, sizeof(process_caps));
    process_caps[0] = CAP_DEFAULT_KERNEL;
}

void capability_register_process(int pid, uint32_t mask) {
    if (pid < 0 || pid >= MAX_TASKS) return;
    uint32_t flags = lock_acquire_irqsave(&cap_lock);
    process_caps[pid] = mask;
    lock_release_irqrestore(&cap_lock, flags);
}

uint32_t capability_get(int pid) {
    if (pid < 0 || pid >= MAX_TASKS) return 0;
    uint32_t flags = lock_acquire_irqsave(&cap_lock);
    uint32_t value = process_caps[pid];
    lock_release_irqrestore(&cap_lock, flags);
    return value;
}

int capability_has(int pid, uint32_t mask) {
    uint32_t value = capability_get(pid);
    return (value & CAP_SYSTEM_ADMIN) || ((value & mask) == mask);
}

int capability_grant(int actor_pid, int target_pid, uint32_t mask) {
    if (target_pid < 0 || target_pid >= MAX_TASKS) return 0;
    if (!capability_has(actor_pid, CAP_SYSTEM_ADMIN)) return 0;
    uint32_t flags = lock_acquire_irqsave(&cap_lock);
    process_caps[target_pid] |= mask;
    lock_release_irqrestore(&cap_lock, flags);
    return 1;
}

int capability_revoke(int actor_pid, int target_pid, uint32_t mask) {
    if (target_pid <= 0 || target_pid >= MAX_TASKS) return 0;
    if (!capability_has(actor_pid, CAP_SYSTEM_ADMIN)) return 0;
    uint32_t flags = lock_acquire_irqsave(&cap_lock);
    process_caps[target_pid] &= ~mask;
    lock_release_irqrestore(&cap_lock, flags);
    return 1;
}

const char *capability_name(uint32_t cap) {
    if (cap == CAP_IPC_SEND) return "ipc-send";
    if (cap == CAP_IPC_RECEIVE) return "ipc-receive";
    if (cap == CAP_PROCESS_QUERY) return "process-query";
    if (cap == CAP_PROCESS_KILL) return "process-kill";
    if (cap == CAP_AFFINITY) return "affinity";
    if (cap == CAP_FILESYSTEM) return "filesystem";
    if (cap == CAP_NETWORK) return "network";
    if (cap == CAP_AUDIO) return "audio";
    if (cap == CAP_POWER) return "power";
    if (cap == CAP_SYSTEM_ADMIN) return "system-admin";
    return "unknown";
}
