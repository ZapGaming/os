#include <kernel/capability.h>
#include <kernel/scheduler.h>
#include <kernel/spinlock.h>
#include <string.h>

static uint32_t process_caps[MAX_TASKS];
static uint8_t process_caps_explicit[MAX_TASKS];
static spinlock_t cap_lock = SPINLOCK_INIT;

void capability_init(void) {
    memset(process_caps, 0, sizeof(process_caps));
    memset(process_caps_explicit, 0, sizeof(process_caps_explicit));
    process_caps[0] = CAP_DEFAULT_KERNEL;
    process_caps_explicit[0] = 1;
}

void capability_register_process(int pid, uint32_t mask) {
    if (pid < 0 || pid >= MAX_TASKS) return;
    uint32_t flags = lock_acquire_irqsave(&cap_lock);
    process_caps[pid] = mask;
    process_caps_explicit[pid] = 1;
    lock_release_irqrestore(&cap_lock, flags);
}

uint32_t capability_get(int pid) {
    if (pid < 0 || pid >= MAX_TASKS) return 0;
    uint32_t flags = lock_acquire_irqsave(&cap_lock);
    uint32_t value = process_caps[pid];
    int explicit_value = process_caps_explicit[pid];
    lock_release_irqrestore(&cap_lock, flags);
    if (explicit_value) return value;

    struct scheduler_task_info info;
    if (!scheduler_task_snapshot(pid, &info)) return 0;
    if (info.kind == TASK_KIND_KERNEL) return CAP_DEFAULT_KERNEL;
    if (info.kind == TASK_KIND_USER) return CAP_DEFAULT_USER;
    return CAP_DEFAULT_ISOLATED;
}

int capability_has(int pid, uint32_t mask) {
    uint32_t value = capability_get(pid);
    return (value & CAP_SYSTEM_ADMIN) || ((value & mask) == mask);
}

int capability_grant(int actor_pid, int target_pid, uint32_t mask) {
    if (target_pid < 0 || target_pid >= MAX_TASKS) return 0;
    if (!capability_has(actor_pid, CAP_SYSTEM_ADMIN)) return 0;
    uint32_t flags = lock_acquire_irqsave(&cap_lock);
    if (!process_caps_explicit[target_pid]) {
        lock_release_irqrestore(&cap_lock, flags);
        process_caps[target_pid] = capability_get(target_pid);
        flags = lock_acquire_irqsave(&cap_lock);
        process_caps_explicit[target_pid] = 1;
    }
    process_caps[target_pid] |= mask;
    lock_release_irqrestore(&cap_lock, flags);
    return 1;
}

int capability_revoke(int actor_pid, int target_pid, uint32_t mask) {
    if (target_pid <= 0 || target_pid >= MAX_TASKS) return 0;
    if (!capability_has(actor_pid, CAP_SYSTEM_ADMIN)) return 0;
    uint32_t flags = lock_acquire_irqsave(&cap_lock);
    if (!process_caps_explicit[target_pid]) {
        lock_release_irqrestore(&cap_lock, flags);
        process_caps[target_pid] = capability_get(target_pid);
        flags = lock_acquire_irqsave(&cap_lock);
        process_caps_explicit[target_pid] = 1;
    }
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
