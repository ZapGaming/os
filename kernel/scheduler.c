#include <kernel/scheduler.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <kernel/tss.h>
#include <kernel/usermode.h>
#include <kernel/paging.h>
#include <kernel/fpu.h>

extern void switch_task(uint32_t *old_esp_store, uint32_t new_esp, void *old_fpu, void *new_fpu);
extern void task_trampoline(void);

static struct task tasks[MAX_TASKS];
static int task_count = 0;
static struct task *current_task = NULL;
static int scheduling_enabled = 0;

/* Every user task's trampoline lands here (no arguments -- state is read
 * back off the now-current task), which then drops to ring 3. An
 * isolated task (page_dir_phys != 0) already has its stack mapped at a
 * fixed virtual address by the caller that set it up (kernel/elf.c);
 * a legacy one gets a fresh top-of-kmalloc'd-stack address instead. */
static void user_task_shim(void) {
    struct task *t = current_task;
    uint32_t user_stack_top = t->page_dir_phys
                                   ? t->user_stack_top
                                   : (uint32_t)(t->user_stack_base + TASK_STACK_SIZE);
    enter_usermode(t->user_entry, user_stack_top);
}

static struct task *task_alloc(void) {
    if (task_count >= MAX_TASKS) return NULL;
    struct task *t = &tasks[task_count];
    t->pid = task_count;
    t->stack_base = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    t->user_stack_base = NULL;
    t->user_entry = NULL;
    t->user_stack_top = 0;
    t->page_dir_phys = 0;
    fpu_get_clean_state(t->fpu_state);
    t->state = TASK_READY;
    task_count++;
    return t;
}

static void task_queue(struct task *t) {
    t->next = current_task->next;
    current_task->next = t;
}

void scheduler_init(void) {
    /* task 0 represents the already-running boot context (kernel_main's
     * own stack) -- it has no separately-allocated stack of its own. */
    tasks[0].pid = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].next = &tasks[0];
    fpu_get_clean_state(tasks[0].fpu_state);
    current_task = &tasks[0];
    task_count = 1;

    serial_printf("scheduler: initialized, boot task is pid 0\n");
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
    task_queue(t);

    serial_printf("scheduler: created kernel task pid=%d\n", t->pid);
    return t;
}

struct task *task_create_user(void (*entry)(void)) {
    struct task *t = task_alloc();
    if (!t) return NULL;

    t->user_stack_base = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    t->user_entry = entry;
    t->esp = build_initial_stack(t->stack_base, user_task_shim);
    task_queue(t);

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
    task_queue(t);

    serial_printf("scheduler: created isolated user task pid=%d\n", t->pid);
    return t;
}

void scheduler_start(void) {
    scheduling_enabled = 1;
    serial_printf("scheduler: preemption enabled (%d tasks)\n", task_count);
}

void schedule(void) {
    if (!scheduling_enabled) return;

    struct task *prev = current_task;
    struct task *next = current_task->next;

    /* skip terminated tasks */
    while (next->state == TASK_TERMINATED && next != prev) {
        next = next->next;
    }
    if (next == prev) return; /* nothing else runnable */

    if (prev->state != TASK_TERMINATED) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;

    if (next->stack_base) {
        tss_set_kernel_stack((uint32_t)(next->stack_base + TASK_STACK_SIZE));
    }

    /* Reload CR3 for whichever directory `next` should run under --
     * always, even switching between two tasks that both use the
     * shared kernel directory, since that's a single cheap register
     * write and the alternative is remembering to never forget it on
     * the paths that DO need a real switch. This also flushes the TLB,
     * which two isolated tasks mapping the same virtual window to
     * different physical frames genuinely depends on. Must happen
     * before switch_task(): the moment it `ret`s, we're running as
     * `next`, using whatever CR3 is already loaded. */
    paging_switch_directory(next->page_dir_phys ? next->page_dir_phys : paging_kernel_directory_phys());

    switch_task(&prev->esp, next->esp, prev->fpu_state, next->fpu_state);
}

int scheduler_task_count(void) {
    return task_count;
}

struct task *scheduler_current(void) {
    return current_task;
}

enum task_state scheduler_task_state(int pid) {
    if (pid < 0 || pid >= task_count) return TASK_TERMINATED;
    return tasks[pid].state;
}

void task_exited(void) {
    current_task->state = TASK_TERMINATED;
    serial_printf("scheduler: task pid=%d exited\n", current_task->pid);

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
    if (current_task->page_dir_phys) {
        paging_free_isolated_directory(current_task->page_dir_phys);
        current_task->page_dir_phys = 0;
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
