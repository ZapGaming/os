#include <kernel/syscall.h>
#include <kernel/idt.h>
#include <kernel/gdt.h>
#include <kernel/scheduler.h>
#include <kernel/serial.h>
#include <kernel/pit.h>
#include <kernel/ipc.h>
#include <kernel/capability.h>
#include <kernel/sysfs.h>
#include <drivers/keyboard.h>
#include <gui/compositor.h>
#include <string.h>

extern void isr128(void);
extern void task_exited(void);

#define MSG_BUF_SIZE 128
static char last_message[MSG_BUF_SIZE];
static int message_count = 0;

static void copy_bounded(char *dst, const char *src, size_t max) {
    size_t i = 0;
    if (!src || max == 0) return;
    for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
    while (i > 0 && (dst[i - 1] == '\n' || dst[i - 1] == '\r')) i--;
    dst[i] = 0;
}

static int current_pid(void) {
    struct task *t = scheduler_current();
    return t ? t->pid : 0;
}

static void syscall_handler(struct registers *regs) {
    int pid = current_pid();

    switch (regs->eax) {
        case SYS_WRITE: {
            const char *str = (const char *)regs->ebx;
            if (!str) { regs->eax = (uint32_t)-1; break; }
            serial_printf("[pid %d syscall] %s", pid, str);
            terminal_route_output(pid, str);
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
            if (keyboard_poll_event(&scancode, &pressed)) regs->eax = ((uint32_t)pressed << 8) | scancode;
            else regs->eax = (uint32_t)-1;
            break;
        }
        case SYS_BLIT:
            gui_blit_fullscreen((const uint32_t *)regs->ebx, pid);
            regs->eax = 0;
            break;
        case SYS_GETPID:
            regs->eax = (uint32_t)pid;
            break;
        case SYS_CAP_GET:
            regs->eax = capability_get(pid);
            break;
        case SYS_IPC_REGISTER: {
            const char *service = (const char *)regs->ebx;
            regs->eax = ipc_register(pid, service) ? 0u : (uint32_t)-1;
            break;
        }
        case SYS_IPC_SEND: {
            int receiver = (int)regs->ebx;
            uint32_t type = regs->ecx;
            const void *payload = (const void *)regs->edx;
            uint32_t length = regs->esi;
            regs->eax = ipc_send(pid, receiver, type, payload, length) ? 0u : (uint32_t)-1;
            break;
        }
        case SYS_IPC_RECEIVE: {
            struct ipc_message *out = (struct ipc_message *)regs->ebx;
            regs->eax = ipc_receive(pid, out) ? 0u : (uint32_t)-1;
            break;
        }
        case SYS_IPC_PENDING:
            regs->eax = (uint32_t)ipc_pending(pid);
            break;
        case SYS_SYSFS_READ: {
            const char *path = (const char *)regs->ebx;
            char *out = (char *)regs->ecx;
            uint32_t cap = regs->edx;
            if (!capability_has(pid, CAP_PROCESS_QUERY)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = (uint32_t)sysfs_read(path, out, cap);
            break;
        }
        case SYS_EXIT:
            ipc_unregister(pid);
            task_exited();
            break;
        default:
            regs->eax = (uint32_t)-1;
            break;
    }
}

void syscall_init(void) {
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);
    register_interrupt_handler(0x80, syscall_handler);
    serial_printf("syscall: int 0x80 gate installed with Nova IPC/sysfs ABI\n");
}

const char *syscall_last_message(void) {
    return last_message;
}

int syscall_message_count(void) {
    return message_count;
}
