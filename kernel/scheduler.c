#include <kernel/scheduler.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <kernel/tss.h>
#include <kernel/usermode.h>
#include <kernel/paging.h>
#include <kernel/fpu.h>
#include <kernel/apic.h>
#include <kernel/spinlock.h>
#include <kernel/pit.h>
#include <string.h>

extern void switch_task(uint32_t *old_esp_store, uint32_t new_esp, void *old_fpu, void *new_fpu);
extern void task_trampoline(void);

static struct task tasks[MAX_TASKS];
static int task_count;
static struct task *current_task_cpu[SCHED_MAX_CPUS];
static int scheduling_enabled;
static uint32_t total_context_switches;
static spinlock_t sched_lock = SPINLOCK_INIT;

static void copy_name(char *dst, const char *src) {
    int i = 0;
    if (!src || !src[0]) src = "task";
    while (src[i] && i < TASK_NAME_LEN - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void task_queue(struct task *t) {
    t->next = tasks[0].next;
    tasks[0].next = t;
}

static uint32_t build_initial_stack(uint8_t *stack_base, void (*entry)(void)) {
    uint32_t *stack_top = (uint32_t *)(stack_base + TASK_STACK_SIZE);
    *(--stack_top) = (uint32_t)entry;
    *(--stack_top) = (uint32_t)task_trampoline;
    *(--stack_top) = 0;
    *(--stack_top) = 0;
    *(--stack_top) = 0;
    *(--stack_top) = 0;
    return (uint32_t)stack_top;
}

static void user_task_shim(void) {
    struct task *t = current_task_cpu[apic_cpu_index()];
    uint32_t user_stack_top = t->page_dir_phys
        ? t->user_stack_top
        : (uint32_t)(t->user_stack_base + TASK_STACK_SIZE);
    enter_usermode(t->user_entry, user_stack_top);
}

static struct task *task_alloc(const char *name, enum task_kind kind) {
    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) return NULL;

    uint8_t fpu_clean[FPU_STATE_SIZE];
    fpu_get_clean_state(fpu_clean);

    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    if (task_count >= MAX_TASKS) {
        lock_release_irqrestore(&sched_lock, flags);
        kfree(stack);
        return NULL;
    }

    struct task *t = &tasks[task_count];
    memset(t, 0, sizeof(*t));
    t->pid = task_count;
    t->stack_base = stack;
    t->cpu_affinity = SCHED_CPU_BSP;
    t->kind = kind;
    t->created_tick = pit_ticks();
    t->state = TASK_READY;
    copy_name(t->name, name);
    memcpy(t->fpu_state, fpu_clean, FPU_STATE_SIZE);
    task_count++;
    lock_release_irqrestore(&sched_lock, flags);
    return t;
}

void scheduler_init(void) {
    memset(tasks, 0, sizeof(tasks));
    memset(current_task_cpu, 0, sizeof(current_task_cpu));
    task_count = 1;
    scheduling_enabled = 0;
    total_context_switches = 0;

    tasks[0].pid = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].cpu_affinity = SCHED_CPU_BSP;
    tasks[0].kind = TASK_KIND_KERNEL;
    tasks[0].created_tick = pit_ticks();
    tasks[0].next = &tasks[0];
    copy_name(tasks[0].name, "kernel-main");
    fpu_get_clean_state(tasks[0].fpu_state);
    current_task_cpu[SCHED_CPU_BSP] = &tasks[0];
    serial_printf("scheduler: initialized boot task pid=0 name=%s\n", tasks[0].name);
}

void scheduler_init_ap(void) {
    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    if (task_count >= MAX_TASKS) {
        lock_release_irqrestore(&sched_lock, flags);
        serial_printf("scheduler: AP registration failed, task table full\n");
        return;
    }

    struct task *t = &tasks[task_count];
    memset(t, 0, sizeof(*t));
    t->pid = task_count;
    t->state = TASK_RUNNING;
    t->cpu_affinity = SCHED_CPU_AP;
    t->kind = TASK_KIND_KERNEL;
    t->created_tick = pit_ticks();
    copy_name(t->name, "ap-bootstrap");
    fpu_get_clean_state(t->fpu_state);
    task_count++;
    task_queue(t);
    current_task_cpu[SCHED_CPU_AP] = t;
    lock_release_irqrestore(&sched_lock, flags);
    serial_printf("scheduler: AP task registered pid=%d\n", t->pid);
}

void task_set_cpu_affinity(struct task *t, int cpu_affinity) {
    if (!t || cpu_affinity < 0 || cpu_affinity >= SCHED_MAX_CPUS) return;
    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    t->cpu_affinity = cpu_affinity;
    lock_release_irqrestore(&sched_lock, flags);
}

struct task *task_create_named(void (*entry)(void), const char *name) {
    struct task *t = task_alloc(name, TASK_KIND_KERNEL);
    if (!t) return NULL;
    t->esp = build_initial_stack(t->stack_base, entry);
    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    task_queue(t);
    lock_release_irqrestore(&sched_lock, flags);
    serial_printf("scheduler: created kernel task pid=%d name=%s\n", t->pid, t->name);
    return t;
}

struct task *task_create(void (*entry)(void)) {
    return task_create_named(entry, "kernel-task");
}

struct task *task_create_user_named(void (*entry)(void), const char *name) {
    struct task *t = task_alloc(name, TASK_KIND_USER);
    if (!t) return NULL;
    t->user_stack_base = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!t->user_stack_base) {
        t->state = TASK_TERMINATED;
        return NULL;
    }
    t->user_entry = entry;
    t->esp = build_initial_stack(t->stack_base, user_task_shim);
    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    task_queue(t);
    lock_release_irqrestore(&sched_lock, flags);
    serial_printf("scheduler: created user task pid=%d name=%s\n", t->pid, t->name);
    return t;
}

struct task *task_create_user(void (*entry)(void)) {
    return task_create_user_named(entry, "user-task");
}

struct task *task_create_user_isolated(void (*entry)(void), uint32_t page_dir_phys, uint32_t user_stack_top) {
    struct task *t = task_alloc("elf-process", TASK_KIND_ISOLATED);
    if (!t) return NULL;
    t->page_dir_phys = page_dir_phys;
    t->user_stack_top = user_stack_top;
    t->user_entry = entry;
    t->esp = build_initial_stack(t->stack_base, user_task_shim);
    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    task_queue(t);
    lock_release_irqrestore(&sched_lock, flags);
    serial_printf("scheduler: created isolated task pid=%d\n", t->pid);
    return t;
}

void scheduler_start(void) {
    scheduling_enabled = 1;
    serial_printf("scheduler: preemption enabled (%d tasks)\n", task_count);
}

void schedule(void) {
    if (!scheduling_enabled) return;
    uint32_t cpu = apic_cpu_index();
    if (cpu >= SCHED_MAX_CPUS) cpu = SCHED_CPU_BSP;

    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    struct task *prev = current_task_cpu[cpu];
    if (!prev) {
        lock_release_irqrestore(&sched_lock, flags);
        return;
    }

    prev->runtime_ticks++;
    struct task *next = prev->next;
    while (next != prev && (next->state == TASK_TERMINATED || next->cpu_affinity != (int)cpu)) {
        next = next->next;
    }
    if (next == prev) {
        lock_release_irqrestore(&sched_lock, flags);
        return;
    }

    if (prev->state != TASK_TERMINATED) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    next->switches++;
    total_context_switches++;
    current_task_cpu[cpu] = next;
    lock_release_irqrestore(&sched_lock, flags);

    if (next->stack_base) {
        tss_set_kernel_stack_cpu((int)cpu, (uint32_t)(next->stack_base + TASK_STACK_SIZE));
    }
    paging_switch_directory(next->page_dir_phys ? next->page_dir_phys : paging_kernel_directory_phys());
    switch_task(&prev->esp, next->esp, prev->fpu_state, next->fpu_state);
}

int scheduler_task_count(void) {
    return task_count;
}

struct task *scheduler_current(void) {
    uint32_t cpu = apic_cpu_index();
    if (cpu >= SCHED_MAX_CPUS) cpu = SCHED_CPU_BSP;
    return current_task_cpu[cpu];
}

enum task_state scheduler_task_state(int pid) {
    if (pid < 0 || pid >= task_count) return TASK_TERMINATED;
    return tasks[pid].state;
}

int scheduler_task_snapshot(int pid, struct scheduler_task_info *out) {
    if (!out || pid < 0) return 0;
    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    if (pid >= task_count) {
        lock_release_irqrestore(&sched_lock, flags);
        return 0;
    }
    struct task *t = &tasks[pid];
    out->pid = t->pid;
    out->state = t->state;
    out->kind = t->kind;
    out->cpu_affinity = t->cpu_affinity;
    out->created_tick = t->created_tick;
    out->runtime_ticks = t->runtime_ticks;
    out->switches = t->switches;
    out->private_page_directory = t->page_dir_phys;
    copy_name(out->name, t->name);
    lock_release_irqrestore(&sched_lock, flags);
    return 1;
}

int scheduler_kill(int pid) {
    if (pid <= 0) return 0;
    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    if (pid >= task_count || tasks[pid].state == TASK_TERMINATED) {
        lock_release_irqrestore(&sched_lock, flags);
        return 0;
    }
    for (int cpu = 0; cpu < SCHED_MAX_CPUS; cpu++) {
        if (current_task_cpu[cpu] == &tasks[pid]) {
            lock_release_irqrestore(&sched_lock, flags);
            return 0;
        }
    }
    tasks[pid].state = TASK_TERMINATED;
    lock_release_irqrestore(&sched_lock, flags);
    serial_printf("scheduler: task pid=%d killed\n", pid);
    return 1;
}

int scheduler_set_affinity(int pid, int cpu_affinity) {
    if (pid <= 0 || cpu_affinity < 0 || cpu_affinity >= SCHED_MAX_CPUS) return 0;
    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    if (pid >= task_count || tasks[pid].state == TASK_TERMINATED) {
        lock_release_irqrestore(&sched_lock, flags);
        return 0;
    }
    tasks[pid].cpu_affinity = cpu_affinity;
    lock_release_irqrestore(&sched_lock, flags);
    return 1;
}

uint32_t scheduler_context_switches(void) {
    return total_context_switches;
}

void task_exited(void) {
    uint32_t cpu = apic_cpu_index();
    if (cpu >= SCHED_MAX_CPUS) cpu = SCHED_CPU_BSP;
    struct task *self = current_task_cpu[cpu];

    uint32_t flags = lock_acquire_irqsave(&sched_lock);
    self->state = TASK_TERMINATED;
    lock_release_irqrestore(&sched_lock, flags);
    serial_printf("scheduler: task pid=%d name=%s exited\n", self->pid, self->name);

    if (self->page_dir_phys) {
        paging_free_isolated_directory(self->page_dir_phys);
        self->page_dir_phys = 0;
    }

    __asm__ volatile ("sti");
    for (;;) {
        schedule();
        __asm__ volatile ("hlt");
    }
}
