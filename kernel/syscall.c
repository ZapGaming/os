#include <kernel/syscall.h>
#include <kernel/idt.h>
#include <kernel/gdt.h>
#include <kernel/scheduler.h>
#include <kernel/serial.h>
#include <kernel/pit.h>
#include <drivers/keyboard.h>
#include <gui/compositor.h>
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
        case SYS_GET_TICKS:
            regs->eax = pit_ticks();
            break;
        case SYS_SLEEP: {
            uint32_t ticks = regs->ebx / 10;
            if (regs->ebx % 10) ticks++;
            uint32_t target = pit_ticks() + ticks;
            while (pit_ticks() < target) schedule();
            regs->eax = 0;
            break;
        }
        case SYS_POLL_KEY: {
            uint8_t scancode, pressed;
            if (keyboard_poll_event(&scancode, &pressed)) {
                regs->eax = ((uint32_t)pressed << 8) | scancode;
            } else {
                regs->eax = (uint32_t)-1;
            }
            break;
        }
        case SYS_BLIT:
            /* `ebx` is a pointer into the CALLING task's own address
             * space -- safe to dereference here specifically because a
             * syscall trap doesn't change CR3, so the isolated task's
             * own directory (with its own private mapping for this
             * buffer) is still what's loaded for the duration of this
             * handler. gui_blit_fullscreen() copies it into a kernel-
             * owned staging buffer immediately, since the GUI's own
             * redraw pass runs later, as a different task, under a
             * different (or the same shared) directory where this
             * pointer wouldn't mean the same thing -- or anything at
             * all. Like SYS_WRITE's string pointer, there's no
             * validation that the whole DOOM_BLIT_W*H range is
             * actually mapped in the caller's own directory before
             * reading it; a program passing a bad pointer here can
             * still fault (safely contained -- see kernel/exceptions.c
             * -- just this task, not the kernel). */
            gui_blit_fullscreen((const uint32_t *)regs->ebx, scheduler_current()->pid);
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
