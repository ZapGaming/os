#ifndef KERNEL_SCHEDULER_H
#define KERNEL_SCHEDULER_H

#include <stdint.h>
#include <kernel/fpu.h>

#define TASK_STACK_SIZE (16 * 1024)
#define MAX_TASKS 16
#define SCHED_MAX_CPUS 2
#define SCHED_CPU_BSP  0
#define SCHED_CPU_AP   1
#define TASK_NAME_LEN 24

enum task_state { TASK_READY, TASK_RUNNING, TASK_TERMINATED };
enum task_kind { TASK_KIND_KERNEL, TASK_KIND_USER, TASK_KIND_ISOLATED };

struct task {
    int pid;
    uint32_t esp;
    uint8_t *stack_base;
    uint8_t *user_stack_base;
    void (*user_entry)(void);
    uint32_t user_stack_top;
    uint32_t page_dir_phys;
    uint8_t fpu_state[FPU_STATE_SIZE];
    enum task_state state;
    int cpu_affinity;
    enum task_kind kind;
    uint32_t created_tick;
    uint32_t runtime_ticks;
    uint32_t switches;
    char name[TASK_NAME_LEN];
    struct task *next;
};

struct scheduler_task_info {
    int pid;
    enum task_state state;
    enum task_kind kind;
    int cpu_affinity;
    uint32_t created_tick;
    uint32_t runtime_ticks;
    uint32_t switches;
    uint32_t private_page_directory;
    char name[TASK_NAME_LEN];
};

void scheduler_init(void);
void scheduler_init_ap(void);
void task_set_cpu_affinity(struct task *t, int cpu_affinity);
struct task *task_create(void (*entry)(void));
struct task *task_create_named(void (*entry)(void), const char *name);
struct task *task_create_user(void (*entry)(void));
struct task *task_create_user_named(void (*entry)(void), const char *name);
struct task *task_create_user_isolated(void (*entry)(void), uint32_t page_dir_phys, uint32_t user_stack_top);
void scheduler_start(void);
void schedule(void);
int scheduler_task_count(void);
struct task *scheduler_current(void);
enum task_state scheduler_task_state(int pid);
int scheduler_task_snapshot(int pid, struct scheduler_task_info *out);
int scheduler_kill(int pid);
int scheduler_set_affinity(int pid, int cpu_affinity);
uint32_t scheduler_context_switches(void);
void task_exited(void);

#endif
