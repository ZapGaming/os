#include <kernel/syscall.h>
#include <kernel/idt.h>
#include <kernel/gdt.h>
#include <kernel/scheduler.h>
#include <kernel/serial.h>
#include <string.h>

extern void isr128(void);
extern void task_exited(void); /* marks current task terminated, never returns */

#define MSG_BUF_SIZE 128
static char last_message[MSG_BUF_SIZE];
static int message_count = 0;

static void copy_bounded(char *dst, const char *src, size_t max) {
    size_t i = 0;
    for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
    while (i > 0 && (dst[i - 1] == '\n' || dst[i - 1] == '\r')) i--;
    dst[i] = 0;
}

/* NOTE: `ebx` here is a raw pointer straight from ring-3. Because every
 * task currently shares one identity-mapped address space, dereferencing
 * it is safe for this demo -- a real OS would validate it lives in that
 * process's own mapped memory before touching it. */
static void syscall_handler(struct registers *regs) {
    switch (regs->eax) {
        case SYS_WRITE: {
            const char *str = (const char *)regs->ebx;
            serial_printf("[pid %d syscall] %s", scheduler_current()->pid, str);
            copy_bounded(last_message, str, MSG_BUF_SIZE);
            message_count++;
            regs->eax = strlen(str);
            break;
        }
        case SYS_YIELD:
            schedule();
            regs->eax = 0;
            break;
        case SYS_EXIT:
            task_exited(); /* never returns */
            break;
        default:
            regs->eax = (uint32_t)-1;
            break;
    }
}

void syscall_init(void) {
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE); /* DPL=3 interrupt gate */
    register_interrupt_handler(0x80, syscall_handler);
    serial_printf("syscall: int 0x80 gate installed\n");
}

const char *syscall_last_message(void) {
    return last_message;
}

int syscall_message_count(void) {
    return message_count;
}
