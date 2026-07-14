#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <stdint.h>

/* Named kernel-managed message channels ("pipes") -- the one primitive
 * that lets ANY two tasks exchange data, including two fully isolated
 * ELF-loaded user processes (separate page directories, see
 * kernel/elf.c) that otherwise share nothing at all. A channel is
 * identified by a short name, looked up by ipc_open() (dedup-by-name,
 * refcounted) rather than by a pid or handle either side has to already
 * know about the other to obtain -- exactly what lets two processes
 * that never see each other's pid rendezvous, the same way two Unix
 * processes rendezvous on a named FIFO path rather than a fd inherited
 * from a common ancestor.
 *
 * Deliberately just a data structure: every function here is a plain,
 * NON-BLOCKING state-machine transition on the static channel table
 * below, with NO call to schedule() anywhere in this file. That's what
 * keeps it fully host-testable in isolation (compile this file, unmodified,
 * against tiny stub headers on any host gcc -- no scheduler, no kmalloc,
 * no serial port to fake). The blocking retry-loop a real syscall needs
 * ("keep trying until there's room / a message shows up") lives one
 * layer up, in kernel/syscall.c's SYS_IPC_SEND/SYS_IPC_RECV handlers,
 * wrapped around the ipc_try_send()/ipc_try_recv() calls below --
 * mirroring exactly how SYS_SLEEP's blocking loop lives in syscall.c
 * around a non-blocking pit_ticks() check, not inside kernel/pit.c.
 *
 * Every channel, message slot and name is a fixed-size static array --
 * no kmalloc anywhere in this file -- so there's no interaction at all
 * with kmalloc-safety/scheduler-reentrancy concerns. Total static
 * footprint: IPC_MAX_CHANNELS * IPC_QUEUE_DEPTH * IPC_MSG_MAX bytes of
 * message storage (8 * 8 * 256 = 16KiB) plus small per-channel
 * bookkeeping -- fine to just always have around. */

#define IPC_MAX_CHANNELS 8
#define IPC_NAME_MAX     16  /* including the NUL terminator */
#define IPC_MSG_MAX       256 /* bytes per message */
#define IPC_QUEUE_DEPTH   8  /* messages per channel */

/* Clears the whole channel table. Call exactly once, early in
 * kernel_main() (before any task that might touch IPC can possibly
 * run) -- see kernel/kernel.c. */
void ipc_init(void);

/* Finds an existing in-use channel with this exact name and bumps its
 * refcount, or creates a new one (refcount=1, empty queue) if none
 * exists. Returns the channel id (0..IPC_MAX_CHANNELS-1) on success, -1
 * if `name` is NULL, too long (>= IPC_NAME_MAX including the NUL), or
 * there's no existing channel by that name AND no free slot left to
 * create one in. */
int ipc_open(const char *name);

/* Decrements the channel's refcount; once it reaches 0, frees the slot
 * (dropping any still-queued messages) so a later ipc_open() with a
 * different name can reuse it. Safe to call with an out-of-range id or
 * an id that isn't currently in use at all -- both are a silent no-op,
 * never a crash. */
void ipc_close(int id);

/* Non-blocking send. Copies exactly `len` bytes from `buf` into the
 * channel's queue as one message. Returns:
 *   1  -- queued.
 *   0  -- queue already has IPC_QUEUE_DEPTH messages pending; caller
 *         should retry later (this is the condition a blocking wrapper
 *         loops on).
 *  -1  -- invalid/unopened `id`, or `len` > IPC_MSG_MAX.
 * Never partially queues a message -- a given call either queues all
 * `len` bytes or none. */
int ipc_try_send(int id, const void *buf, uint32_t len);

/* Non-blocking receive, FIFO order (oldest-queued message first).
 * Returns:
 *   1  -- a message was dequeued; `*out_len` is set to that message's
 *         ACTUAL length as originally sent, which may be LARGER than
 *         `cap` -- only min(actual_len, cap) bytes are copied into
 *         `buf` in that case (truncated, not treated as an error; the
 *         caller can tell it happened by comparing `*out_len` to `cap`).
 *   0  -- queue is empty; caller should retry later (this is the
 *         condition a blocking wrapper loops on). `*out_len` untouched.
 *  -1  -- invalid/unopened `id`. `*out_len` untouched.
 */
int ipc_try_recv(int id, void *buf, uint32_t cap, uint32_t *out_len);

#endif
