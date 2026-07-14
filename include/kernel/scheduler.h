#ifndef KERNEL_SCHEDULER_H
#define KERNEL_SCHEDULER_H

#include <stdint.h>
#include <kernel/fpu.h>

#define TASK_STACK_SIZE (16 * 1024)
#define MAX_TASKS 16

/* Capped at exactly 2 -- BSP + one AP -- matching kernel/smp.c only ever
 * waking one AP and kernel/tss.h's TSS_MAX_CPUS. Real multi-core
 * scheduling for more than one AP is explicit follow-on work, not
 * attempted by this pass (see this header's other new comments). */
#define SCHED_MAX_CPUS 2
#define SCHED_CPU_BSP  0
#define SCHED_CPU_AP   1

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
    /* Which logical CPU (SCHED_CPU_BSP or SCHED_CPU_AP) this task is
     * allowed to run on -- schedule() (kernel/scheduler.c) never picks
     * a task whose cpu_affinity doesn't match the CPU it's running on.
     * Every task_create*() call defaults a new task to SCHED_CPU_BSP
     * (preserving this kernel's pre-SMP-scheduling guarantee that
     * everything -- GUI, network, filesystem, ELF-loaded user code --
     * only ever runs on the BSP); the only way anything ever ends up
     * SCHED_CPU_AP is an explicit task_set_cpu_affinity() call, used in
     * exactly one place (kernel/kernel.c's bg_task re-pin) for exactly
     * one deliberately shared-state-free task in this pass. */
    int cpu_affinity;
    struct task *next;
};

void scheduler_init(void);

/* Registers the CALLING context itself -- right now, mid-call, on
 * whichever core this runs on -- as a real, schedulable struct task
 * pinned to SCHED_CPU_AP, and makes it that CPU's current task. This is
 * the AP-side mirror of what scheduler_init() already does for the BSP
 * (tasks[0] represents kernel_main()'s own already-running call stack,
 * never a freshly kmalloc'd one); the AP has no equivalent "task 0" of
 * its own until it calls this. Must be called exactly once, only by the
 * AP itself (kernel/apic.c's ap_main()), after scheduler_init() has
 * already run on the BSP, and before this CPU can possibly take the
 * timer interrupt that would call schedule() -- see ap_main() for the
 * exact ordering this depends on. */
void scheduler_init_ap(void);

/* Changes an already-created task's cpu_affinity after the fact, under
 * the same lock schedule() itself uses -- safe to call even while the
 * BSP's (or the AP's) own timer interrupt is concurrently calling
 * schedule() against the very same tasks[]/next chain this walks.
 * Used exactly once, by kernel/kernel.c, to re-pin the pre-existing
 * bg_task to the AP only if one actually came up (falls back to
 * leaving it on the BSP -- its default -- otherwise, which is exactly
 * this kernel's behavior before this pass). */
void task_set_cpu_affinity(struct task *t, int cpu_affinity);

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
