#ifndef KERNEL_SCHEDULER_H
#define KERNEL_SCHEDULER_H

#include <stdint.h>
#include <kernel/fpu.h>

#define TASK_STACK_SIZE (16 * 1024)
#define MAX_TASKS 16

enum task_state { TASK_READY, TASK_RUNNING, TASK_TERMINATED };

struct task {
    int pid;
    uint32_t esp;
    uint8_t *stack_base;      /* kernel stack (also used as TSS.esp0 while this task runs) */
    uint8_t *user_stack_base; /* ring-3 stack, only set for legacy (non-isolated) user tasks */
    void (*user_entry)(void);
    uint32_t user_stack_top;  /* ring-3 ESP to start at; only used when page_dir_phys != 0 */
    uint32_t page_dir_phys;   /* 0 = shared/legacy kernel directory; else this task's own isolated one */
    uint8_t fpu_state[FPU_STATE_SIZE]; /* x87 FPU registers/control word, saved/restored every switch */
    enum task_state state;
    struct task *next;
};

void scheduler_init(void);
struct task *task_create(void (*entry)(void));
struct task *task_create_user(void (*entry)(void));

/* Like task_create_user(), but for code that must run genuinely
 * isolated from the kernel and every other task -- an ELF binary
 * loaded from disk, specifically (see kernel/elf.c). `page_dir_phys`
 * is a directory built by paging_new_isolated_directory() +
 * paging_map_user_page(), already holding a private mapping for
 * `entry` and for the stack ending at `user_stack_top`. Freed
 * automatically (via paging_free_isolated_directory()) once this task
 * exits. */
struct task *task_create_user_isolated(void (*entry)(void), uint32_t page_dir_phys, uint32_t user_stack_top);

void scheduler_start(void);
void schedule(void);
int scheduler_task_count(void);
struct task *scheduler_current(void);

/* Returns the task state for `pid`, or TASK_TERMINATED if no such pid
 * was ever created -- lets a long-lived kernel data structure (the
 * GUI's fullscreen-takeover mode, specifically) check whether the task
 * that asked for it is still alive without holding a dangling
 * struct task* across that task's exit and slot reuse... except task
 * slots are never actually reused today (see the README), so this is
 * mostly just "did pid N ever finish." */
enum task_state scheduler_task_state(int pid);

/* Marks the current task terminated and never returns -- used both for
 * a normal SYS_EXIT syscall and (see kernel/exceptions.c) to contain a
 * ring-3 task that just faulted, instead of halting the whole kernel. */
void task_exited(void);

#endif
