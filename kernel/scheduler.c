#include <kernel/scheduler.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <kernel/tss.h>
#include <kernel/usermode.h>

extern void switch_task(uint32_t *old_esp_store, uint32_t new_esp);
extern void task_trampoline(void);

static struct task tasks[MAX_TASKS];
static int task_count = 0;
static struct task *current_task = NULL;
static int scheduling_enabled = 0;

/* Every user task's trampoline lands here (no arguments -- state is read
 * back off the now-current task), which then drops to ring 3. */
static void user_task_shim(void) {
    struct task *t = current_task;
    uint32_t user_stack_top = (uint32_t)(t->user_stack_base + TASK_STACK_SIZE);
    enter_usermode(t->user_entry, user_stack_top);
}

static struct task *task_alloc(void) {
    if (task_count >= MAX_TASKS) return NULL;
    struct task *t = &tasks[task_count];
    t->pid = task_count;
    t->stack_base = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    t->user_stack_base = NULL;
    t->user_entry = NULL;
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

    prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;

    if (next->stack_base) {
        tss_set_kernel_stack((uint32_t)(next->stack_base + TASK_STACK_SIZE));
    }

    switch_task(&prev->esp, next->esp);
}

int scheduler_task_count(void) {
    return task_count;
}

struct task *scheduler_current(void) {
    return current_task;
}

void task_exited(void) {
    current_task->state = TASK_TERMINATED;
    serial_printf("scheduler: task pid=%d exited\n", current_task->pid);
    for (;;) {
        schedule();
        __asm__ volatile ("hlt");
    }
}
