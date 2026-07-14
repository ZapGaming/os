#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_YIELD     2
#define SYS_GET_TICKS 3 /* returns pit_ticks() (100Hz) in eax */
#define SYS_SLEEP     4 /* ebx = milliseconds; yields until they've passed */
#define SYS_POLL_KEY  5 /* returns -1 if no event pending, else (pressed<<8)|scancode in eax */
#define SYS_BLIT      6 /* ebx = pointer to a caller-owned DOOM_BLIT_W*DOOM_BLIT_H uint32_t 0xRRGGBB buffer */

/* Named IPC channels (kernel/ipc.c) -- the first syscalls to use `ecx`
 * and `edx` (previously-unused-but-architecturally-free general-purpose
 * arg slots in `struct registers`, see include/kernel/idt.h) alongside
 * `ebx`, since ipc_send/ipc_recv each need three values (an id plus a
 * buffer pointer plus a length) where every syscall before this one
 * needed at most one. SYS_IPC_SEND/SYS_IPC_RECV both BLOCK (retry via
 * schedule() from inside the handler, exactly like SYS_SLEEP above)
 * until they can make progress -- see kernel/syscall.c. */
#define SYS_IPC_OPEN  7  /* ebx = pointer to a NUL-terminated channel name; returns channel id (>=0), or -1, in eax */
#define SYS_IPC_SEND  8  /* ebx = channel id, ecx = pointer to buf, edx = len; blocks until queued; returns 0, or -1 for an invalid id/oversized len, in eax */
#define SYS_IPC_RECV  9  /* ebx = channel id, ecx = pointer to buf, edx = cap; blocks until a message arrives; returns the message's actual length (may exceed cap -- truncated, not an error), or -1 for an invalid id, in eax */
#define SYS_IPC_CLOSE 10 /* ebx = channel id; always returns 0 in eax (invalid/already-closed ids are a silent no-op) */

/* Fixed resolution SYS_BLIT always copies -- matches the original DOOM's
 * internal resolution. Not user-configurable: the syscall has no way to
 * validate an arbitrary caller-supplied size against what's actually
 * mapped in their address space, so it only ever touches exactly this
 * many bytes (see kernel/syscall.c). */
#define DOOM_BLIT_W 320
#define DOOM_BLIT_H 200

void syscall_init(void);
const char *syscall_last_message(void);
int syscall_message_count(void);

#endif
