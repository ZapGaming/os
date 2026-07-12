#include <kernel/demo_user_task.h>
#include <kernel/syscall.h>

/* This function's code runs at CPL 3 (see enter_usermode.asm). It cannot
 * call serial_printf or any other kernel function directly -- `in`/`out`
 * and anything else privileged would GP-fault at ring 3. The only way out
 * is `int 0x80`. */

static inline int sys_write(const char *s) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_WRITE), "b"(s));
    return ret;
}

static inline void sys_yield(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_YIELD));
}

static inline void sys_exit(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT));
}

void demo_user_task_entry(void) {
    for (int i = 0; i < 8; i++) {
        sys_write("hello from ring 3 (user mode)!\n");
        sys_yield();
    }
    sys_exit();
}
