#include <kernel/syscall.h>
#include <kernel/idt.h>
#include <kernel/gdt.h>
#include <kernel/scheduler.h>
#include <kernel/serial.h>
#include <kernel/pit.h>
#include <kernel/ipc.h>
#include <drivers/keyboard.h>
#include <gui/compositor.h>
#include <net/http.h>
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
            terminal_route_output(scheduler_current()->pid, str);
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
        case SYS_IPC_OPEN:
            /* `ebx` is a pointer into the CALLING task's own address
             * space -- same "syscall trap doesn't touch CR3" reasoning
             * as SYS_WRITE/SYS_BLIT above, so it means exactly what the
             * caller thinks it means even for an isolated ELF task with
             * its own private directory. ipc_open() only ever reads it
             * (strlen/strcmp against the channel table), never keeps
             * the pointer itself past this call, so there's nothing
             * further to guard here. */
            regs->eax = (uint32_t)ipc_open((const char *)regs->ebx);
            break;
        case SYS_IPC_SEND: {
            /* Blocking send: retries the non-blocking ipc_try_send()
             * (kernel/ipc.c) via schedule() until the channel's queue
             * has room -- the exact same "loop schedule() from inside
             * the interrupt-gate handler" idiom SYS_SLEEP uses above,
             * just polling a queue slot instead of pit_ticks(). `ecx`
             * is, again, a pointer in the CALLER's own address space;
             * ipc_try_send() copies the bytes out of it into the
             * channel's queue immediately, so nothing here needs to
             * (or safely could) outlive this one handler invocation. */
            int r;
            while ((r = ipc_try_send((int)regs->ebx, (const void *)regs->ecx, regs->edx)) == 0) schedule();
            regs->eax = (r < 0) ? (uint32_t)-1 : 0;
            break;
        }
        case SYS_IPC_RECV: {
            /* Blocking receive: mirrors SYS_IPC_SEND, retrying
             * ipc_try_recv() until a message is queued. `ecx` is once
             * more the calling task's own pointer -- ipc_try_recv()
             * writes straight into it, so the bytes land in whichever
             * address space (kernel or a specific isolated user task's
             * own directory) is actually loaded right now, which is
             * always the caller's, for the same CR3-doesn't-change
             * reason as every other syscall here. */
            uint32_t out_len = 0;
            int r;
            while ((r = ipc_try_recv((int)regs->ebx, (void *)regs->ecx, regs->edx, &out_len)) == 0) schedule();
            regs->eax = (r < 0) ? (uint32_t)-1 : out_len;
            break;
        }
        case SYS_IPC_CLOSE:
            ipc_close((int)regs->ebx);
            regs->eax = 0;
            break;
        case SYS_WIN_OPEN:
            /* `ebx` is a pointer into the CALLING task's own address
             * space -- same reasoning as SYS_WRITE/SYS_BLIT/SYS_IPC_OPEN
             * above. gui_app_window_open() copies the title out of it
             * (a bounded strncpy) immediately, before this handler
             * returns, so there's nothing left to guard once it's back.
             * `esi` is the new 4th argument (style flags, see
             * WIN_FLAG_BORDERLESS in include/kernel/syscall.h) -- populated
             * by the `pusha` in kernel/isr_stubs.asm's isr_common_stub,
             * same as every other field struct registers already exposes. */
            regs->eax = (uint32_t)gui_app_window_open(scheduler_current()->pid,
                                                       (const char *)regs->ebx,
                                                       regs->ecx, regs->edx, regs->esi);
            break;
        case SYS_WIN_BLIT: {
            /* `ecx` is a pointer into the CALLING task's own address
             * space -- same reasoning as SYS_BLIT's pixel buffer above.
             * gui_app_window_blit() memcpy's it into the window's own
             * kernel-owned staging buffer synchronously, same as
             * gui_blit_fullscreen() does for the fullscreen case. */
            int r = gui_app_window_blit((int)regs->ebx, (const void *)regs->ecx);
            regs->eax = (r < 0) ? (uint32_t)-1 : 0;
            break;
        }
        case SYS_WIN_MOVE: {
            /* `ebx` = handle, `ecx`/`edx` = new x/y. Both cast to signed
             * int -- an app might legitimately want to move slightly
             * negative mid-bounce (see include/kernel/syscall.h's doc
             * comment), and `struct registers`' fields are all uint32_t,
             * so a caller's negative int arrives here as a large
             * unsigned value that has to be reinterpreted back to
             * signed, not clamped to 0. No pointer dereference at all in
             * this case, unlike most syscalls here. */
            int r = gui_app_window_move((int)regs->ebx, (int)regs->ecx, (int)regs->edx);
            regs->eax = (r < 0) ? (uint32_t)-1 : 0;
            break;
        }
        case SYS_HTTP_REQUEST: {
            /* `ebx` is a pointer to a `struct zos_http_request` living in
             * the CALLING task's own address space -- safe to
             * dereference directly for the same "syscall trap doesn't
             * change CR3" reason as every other pointer-argument syscall
             * here. Because CR3 doesn't change for the whole duration of
             * this handler, the struct's own pointer FIELDS (host/
             * method/path/extra_headers/body/response_buf/
             * content_type_buf) are ALSO still pointers into that same
             * still-loaded address space, so they're safe to dereference
             * too -- not just the outer struct pointer itself.
             *
             * host/method/path are bounds-copied into small fixed
             * kernel-side stack buffers before use (same sizing net/
             * http.c's own http_request() uses internally for
             * cur_host/cur_path) -- a non-NUL-terminated or absurdly
             * long string from a misbehaving caller just gets truncated,
             * never overruns anything. `extra_headers`/`body`/
             * `response_buf`/`content_type_buf` are passed straight
             * through to http_request(), which only ever reads
             * extra_headers/body and only ever writes up to the given
             * *_cap bytes into the two output buffers -- no additional
             * copying needed here.
             *
             * This is a BLOCKING syscall: http_request()'s own receive
             * loop already yields (pit_sleep()) while waiting on the
             * network, exactly like SYS_SLEEP/SYS_IPC_SEND/SYS_IPC_RECV
             * above already block by yielding from inside their handler
             * -- nothing new about that here, just a longer-running
             * example of the same idiom. */
            struct zos_http_request *req = (struct zos_http_request *)regs->ebx;

            char host[128], method[16], path[512];
            strncpy(host, req->host ? req->host : "", sizeof(host) - 1); host[sizeof(host) - 1] = 0;
            strncpy(method, req->method ? req->method : "GET", sizeof(method) - 1); method[sizeof(method) - 1] = 0;
            strncpy(path, req->path ? req->path : "/", sizeof(path) - 1); path[sizeof(path) - 1] = 0;

            uint16_t port = (uint16_t)req->port;
            if (port == 0) port = req->use_tls ? 443 : 80;

            /* MUST re-enable interrupts before calling into http_request()
             * below. `int 0x80` is an interrupt gate (see syscall_init()'s
             * idt_set_gate(0x80, ..., 0xEE) -- type 0xE = interrupt gate),
             * which clears EFLAGS.IF the instant the CPU vectors in here,
             * and switch_task() (kernel/switch_task.asm) never saves or
             * restores EFLAGS across a task switch -- it's a single,
             * genuinely global CPU register in this design, not per-task
             * state. http_request()'s receive loop waits via net/http.c's
             * pit_sleep(), which busy-waits on a bare `hlt` (not a
             * schedule()-loop like SYS_SLEEP/SYS_IPC_SEND/SYS_IPC_RECV
             * above) -- and `hlt` executed with IF=0 can NEVER be woken by
             * a maskable interrupt (the timer tick included), only by
             * NMI/SMI/reset. Without this `sti`, the very first pit_sleep()
             * inside http_request() halts the entire (single-core) system
             * forever -- confirmed by an actual hang during this feature's
             * own verification pass (EFL showed IF clear, CPU parked in
             * `hlt`, pit_ticks() frozen). This is the exact same class of
             * bug scheduler.c's task_exited() already documents and fixes
             * for its own schedule()-loop -- see its comment -- just newly
             * hit here because this is the first syscall whose blocking
             * wait is `hlt`-based rather than schedule()-based (a
             * schedule()-based wait can still make progress with IF
             * momentarily 0, since OTHER tasks' own ordinary interrupt
             * returns keep flipping the shared IF bit back to 1; a bare
             * `hlt` has no such escape hatch since nothing else can run at
             * all while this one CPU is parked). */
            __asm__ volatile ("sti");

            int ok = http_request(req->use_tls, host, port, method, path,
                                   req->extra_headers, req->body, req->body_len,
                                   &req->status_out, req->response_buf, req->response_cap,
                                   &req->response_len_out, req->content_type_buf, req->content_type_cap);
            regs->eax = ok ? 0 : (uint32_t)-1;
            break;
        }
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
