#include <kernel/scheduler.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <kernel/tss.h>
#include <kernel/usermode.h>
#include <kernel/paging.h>
#include <kernel/fpu.h>
#include <kernel/apic.h>
#include <kernel/spinlock.h>

extern void switch_task(uint32_t *old_esp_store, uint32_t new_esp, void *old_fpu, void *new_fpu);
extern void task_trampoline(void);

static struct task tasks[MAX_TASKS];
static int task_count = 0;

/* current_task_cpu[SCHED_CPU_BSP] is what used to be the single global
 * current_task; current_task_cpu[SCHED_CPU_AP] is this pass's addition,
 * indexed the same way apic_cpu_index() (kernel/apic.c) reports it.
 * apic_cpu_index() always returns SCHED_CPU_BSP on a single-CPU boot
 * (no AP ever exists to have index SCHED_CPU_AP), so every access below
 * degenerates to exactly the old single-`current_task` behavior in that
 * case -- current_task_cpu[SCHED_CPU_AP] simply stays NULL and unused
 * forever, never dereferenced, because nothing ever calls
 * apic_cpu_index() and gets SCHED_CPU_AP back unless kernel/apic.c's
 * apic_start_ap() actually succeeded. */
static struct task *current_task_cpu[SCHED_MAX_CPUS];

static int scheduling_enabled = 0;

/* The ONE lock this pass adds (kernel/spinlock.h) protects every access
 * to tasks[]/task_count/the task_queue's `next` chain/current_task_cpu[]
 * below -- the exact shared state that used to be safe-by-construction
 * only because a single core ever ran kernel code at once. Two cores'
 * timer interrupts can now call schedule() genuinely concurrently
 * (kernel/apic.c's ap_main() gives the AP its own Local APIC timer), and
 * task_set_cpu_affinity()/task_create*() can now also run concurrently
 * with either core's schedule() (kernel/kernel.c calls
 * task_set_cpu_affinity() after interrupts are already enabled and the
 * BSP's scheduler is already preempting) -- this single global lock
 * serializes all of it. With only 2 cores and at most a handful of
 * tasks, contention is negligible; see spinlock.h for why a plain
 * test-and-set busy-wait lock (not a ticket lock) is enough here. */
static spinlock_t sched_lock = SPINLOCK_INIT;

/* Every user task's trampoline lands here (no arguments -- state is read
 * back off the now-current task), which then drops to ring 3. An
 * isolated task (page_dir_phys != 0) already has its stack mapped at a
 * fixed virtual address by the caller that set it up (kernel/elf.c);
 * a legacy one gets a fresh top-of-kmalloc'd-stack address instead.
 * (User/isolated tasks are always SCHED_CPU_BSP-affine in this pass --
 * see task_create_user()/task_create_user_isolated() -- so in practice
 * this only ever runs on the BSP, but reads the per-CPU slot generically
 * rather than assuming that.) */
static void user_task_shim(void) {
    struct task *t = current_task_cpu[apic_cpu_index()];
    uint32_t user_stack_top = t->page_dir_phys
                                   ? t->user_stack_top
                                   : (uint32_t)(t->user_stack_base + TASK_STACK_SIZE);
    enter_usermode(t->user_entry, user_stack_top);
}

/* Anchors task_queue()'s insertion at tasks[0] rather than "whatever is
 * currently running", so that inserting a new task never depends on
 * which CPU (or which per-CPU current-task slot) happens to be calling
 * it -- tasks[0] (the BSP's boot task, set up in scheduler_init())
 * always exists and is always part of the one shared ring, on any boot
 * that ever reaches task creation at all. This is also exactly where
 * every pre-existing call site already landed anyway: every
 * task_create*() call in this codebase happens before the scheduler's
 * first-ever switch (scheduler_start()/sti in kernel/kernel.c), at which
 * point the old single global current_task was always still tasks[0] --
 * so this is a behavior-preserving change, not a new policy. Caller
 * must hold sched_lock. */
static void task_queue(struct task *t) {
    t->next = tasks[0].next;
    tasks[0].next = t;
}

static struct task *task_alloc(void) {
    /* kmalloc() is not SMP-safe (kernel/kheap.c has no lock of its own
     * -- explicitly out of scope for this pass), so this deliberately
     * runs OUTSIDE sched_lock, before taking it. That's safe only
     * because every task_create*() call in this codebase still runs
     * exclusively on the BSP's own thread of control (kernel_main,
     * whether before or after interrupts are enabled) -- nothing this
     * pass ever schedules onto the AP (see kernel/kernel.c) calls
     * kmalloc() itself, so there is no second, concurrent caller of
     * kmalloc() for this to race with. */
    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    uint8_t fpu_clean[FPU_STATE_SIZE];
    fpu_get_clean_state(fpu_clean);

    uint32_t eflags = lock_acquire_irqsave(&sched_lock);
    if (task_count >= MAX_TASKS) {
        lock_release_irqrestore(&sched_lock, eflags);
        return NULL;
    }
    struct task *t = &tasks[task_count];
    t->pid = task_count;
    t->stack_base = stack;
    t->user_stack_base = NULL;
    t->user_entry = NULL;
    t->user_stack_top = 0;
    t->page_dir_phys = 0;
    t->cpu_affinity = SCHED_CPU_BSP; /* default -- see struct task's doc comment */
    for (int i = 0; i < FPU_STATE_SIZE; i++) t->fpu_state[i] = fpu_clean[i];
    t->state = TASK_READY;
    task_count++;
    lock_release_irqrestore(&sched_lock, eflags);
    return t;
}

void scheduler_init(void) {
    /* task 0 represents the already-running boot context (kernel_main's
     * own stack) -- it has no separately-allocated stack of its own.
     * No lock needed here: this runs once, at boot, strictly before any
     * other CPU exists and before interrupts are even enabled. */
    tasks[0].pid = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].cpu_affinity = SCHED_CPU_BSP;
    tasks[0].next = &tasks[0];
    fpu_get_clean_state(tasks[0].fpu_state);
    current_task_cpu[SCHED_CPU_BSP] = &tasks[0];
    task_count = 1;

    serial_printf("scheduler: initialized, boot task is pid 0\n");
}

void scheduler_init_ap(void) {
    uint32_t eflags = lock_acquire_irqsave(&sched_lock);
    if (task_count >= MAX_TASKS) {
        lock_release_irqrestore(&sched_lock, eflags);
        serial_printf("scheduler: MAX_TASKS reached, AP could not register its own boot task -- AP stays un-scheduled\n");
        return;
    }
    struct task *t = &tasks[task_count];
    t->pid = task_count;
    t->stack_base = NULL; /* like tasks[0], this represents an already-running stack, not a fresh kmalloc'd one */
    t->user_stack_base = NULL;
    t->user_entry = NULL;
    t->user_stack_top = 0;
    t->page_dir_phys = 0;
    t->cpu_affinity = SCHED_CPU_AP;
    fpu_get_clean_state(t->fpu_state);
    t->state = TASK_RUNNING;
    task_count++;
    task_queue(t);
    current_task_cpu[SCHED_CPU_AP] = t;
    lock_release_irqrestore(&sched_lock, eflags);

    serial_printf("scheduler: AP registered its own running context as pid=%d (cpu_affinity=AP)\n", t->pid);
}

void task_set_cpu_affinity(struct task *t, int cpu_affinity) {
    uint32_t eflags = lock_acquire_irqsave(&sched_lock);
    t->cpu_affinity = cpu_affinity;
    lock_release_irqrestore(&sched_lock, eflags);
}

static uint32_t build_initial_stack(uint8_t *stack_base, void (*entry)(void)) {
    uint32_t *stack_top = (uint32_t *)(stack_base + TASK_STACK_SIZE);
    *(--stack_top) = (uint32_t)entry;            /* consumed by trampoline's `pop eax` */
    *(--stack_top) = (uint32_t)task_trampoline;   /* switch_task's `ret` target */
    *(--stack_top) = 0; /* ebx */
    *(--stack_top) = 0; /* esi */
    *(--stack_top) = 0; /* edi */
    *(--stack_top) = 0; /* ebp */
    return (uint32_t)stack_top;
}

struct task *task_create(void (*entry)(void)) {
    struct task *t = task_alloc();
    if (!t) return NULL;

    t->esp = build_initial_stack(t->stack_base, entry);
    uint32_t eflags = lock_acquire_irqsave(&sched_lock);
    task_queue(t);
    lock_release_irqrestore(&sched_lock, eflags);

    serial_printf("scheduler: created kernel task pid=%d\n", t->pid);
    return t;
}

struct task *task_create_user(void (*entry)(void)) {
    struct task *t = task_alloc();
    if (!t) return NULL;

    t->user_stack_base = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    t->user_entry = entry;
    t->esp = build_initial_stack(t->stack_base, user_task_shim);
    uint32_t eflags = lock_acquire_irqsave(&sched_lock);
    task_queue(t);
    lock_release_irqrestore(&sched_lock, eflags);

    serial_printf("scheduler: created user task pid=%d\n", t->pid);
    return t;
}

struct task *task_create_user_isolated(void (*entry)(void), uint32_t page_dir_phys, uint32_t user_stack_top) {
    struct task *t = task_alloc();
    if (!t) return NULL;

    t->page_dir_phys = page_dir_phys;
    t->user_stack_top = user_stack_top;
    t->user_entry = entry;
    t->esp = build_initial_stack(t->stack_base, user_task_shim);
    uint32_t eflags = lock_acquire_irqsave(&sched_lock);
    task_queue(t);
    lock_release_irqrestore(&sched_lock, eflags);

    serial_printf("scheduler: created isolated user task pid=%d\n", t->pid);
    return t;
}

void scheduler_start(void) {
    scheduling_enabled = 1;
    serial_printf("scheduler: preemption enabled (%d tasks)\n", task_count);
}

/* schedule() is now called from two different cores' timer interrupts
 * (the BSP's legacy PIT/IRQ0, and -- if kernel/apic.c brought one up --
 * the AP's own Local APIC timer), genuinely concurrently. The critical
 * section held under sched_lock is deliberately kept to just the
 * decision + bookkeeping (walking the shared `next` chain, and updating
 * prev/next state + current_task_cpu[cpu]) -- NOT the actual context
 * switch below it. That's safe because everything switch_task() and
 * paging_switch_directory() touch from here on (this core's own
 * register file/stack pointer/CR3, and -- via tss_set_kernel_stack_cpu()
 * -- this core's own TSS.esp0) is per-CPU state that only this core
 * ever writes; the only thing genuinely shared between cores is exactly
 * what the lock protects. */
void schedule(void) {
    if (!scheduling_enabled) return;

    uint32_t cpu = apic_cpu_index();

    uint32_t eflags = lock_acquire_irqsave(&sched_lock);

    struct task *prev = current_task_cpu[cpu];
    struct task *next = prev->next;

    /* skip terminated tasks, and tasks pinned to the OTHER cpu */
    while (next != prev && (next->state == TASK_TERMINATED || next->cpu_affinity != (int)cpu)) {
        next = next->next;
    }
    if (next == prev) {
        lock_release_irqrestore(&sched_lock, eflags);
        return; /* nothing else runnable on this CPU right now */
    }

    if (prev->state != TASK_TERMINATED) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task_cpu[cpu] = next;

    lock_release_irqrestore(&sched_lock, eflags);

    if (next->stack_base) {
        tss_set_kernel_stack_cpu((int)cpu, (uint32_t)(next->stack_base + TASK_STACK_SIZE));
    }

    /* Reload CR3 for whichever directory `next` should run under --
     * always, even switching between two tasks that both use the
     * shared kernel directory, since that's a single cheap register
     * write and the alternative is remembering to never forget it on
     * the paths that DO need a real switch. This also flushes the TLB,
     * which two isolated tasks mapping the same virtual window to
     * different physical frames genuinely depends on. Must happen
     * before switch_task(): the moment it `ret`s, we're running as
     * `next`, using whatever CR3 is already loaded. This is a per-CPU
     * register (CR3), so it's safe to write from either core with no
     * lock -- the shared page_directory it usually points at
     * (kernel/paging.c) is set up once at boot and never mutated
     * afterward, and every task the AP is ever pinned to in this pass
     * has page_dir_phys == 0 (the shared directory), never an isolated
     * one. */
    paging_switch_directory(next->page_dir_phys ? next->page_dir_phys : paging_kernel_directory_phys());

    switch_task(&prev->esp, next->esp, prev->fpu_state, next->fpu_state);
}

int scheduler_task_count(void) {
    return task_count;
}

struct task *scheduler_current(void) {
    return current_task_cpu[apic_cpu_index()];
}

enum task_state scheduler_task_state(int pid) {
    if (pid < 0 || pid >= task_count) return TASK_TERMINATED;
    return tasks[pid].state;
}

void task_exited(void) {
    struct task *self = current_task_cpu[apic_cpu_index()];
    self->state = TASK_TERMINATED;
    serial_printf("scheduler: task pid=%d exited\n", self->pid);

    /* A terminated task is skipped forever by schedule()'s round-robin
     * search, so its own esp/context is never switched back to -- the
     * loop below runs schedule() exactly once for real (the call that
     * actually leaves) and never reaches its own next iteration. That
     * makes right here, still running under this task's own CR3 one
     * last time, the only safe place to reclaim its isolated address
     * space: pmm_free_frame() only clears bookkeeping bits, it doesn't
     * unmap or zero anything, so the frames stay fully valid until the
     * moment CR3 actually changes inside schedule() below -- by which
     * point this task is permanently unreachable anyway. */
    if (self->page_dir_phys) {
        paging_free_isolated_directory(self->page_dir_phys);
        self->page_dir_phys = 0;
    }

    /* This never returns, so the interrupt gate that got us here (a
     * syscall, or the trampoline calling us after a task's entry point
     * returned) never reaches its own `iret` -- which is normally what
     * restores EFLAGS.IF. Without an explicit `sti`, this task's saved
     * context (and switch_task never touches EFLAGS) permanently carries
     * IF=0, so every time it's rescheduled it eventually `hlt`s with
     * interrupts disabled and the whole system freezes for good. */
    __asm__ volatile ("sti");
    for (;;) {
        schedule();
        __asm__ volatile ("hlt");
    }
}
