#ifndef KERNEL_SCHEDULER_H
#define KERNEL_SCHEDULER_H

#include <stdint.h>

#define TASK_STACK_SIZE (16 * 1024)
#define MAX_TASKS 16

enum task_state { TASK_READY, TASK_RUNNING, TASK_TERMINATED };

struct task {
    int pid;
    uint32_t esp;
    uint8_t *stack_base;      /* kernel stack (also used as TSS.esp0 while this task runs) */
    uint8_t *user_stack_base; /* ring-3 stack, only set for user tasks */
    void (*user_entry)(void);
    enum task_state state;
    struct task *next;
};

void scheduler_init(void);
struct task *task_create(void (*entry)(void));
struct task *task_create_user(void (*entry)(void));
void scheduler_start(void);
void schedule(void);
int scheduler_task_count(void);
struct task *scheduler_current(void);

#endif
