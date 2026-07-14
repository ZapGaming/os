#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#define SYS_EXIT         0
#define SYS_WRITE        1
#define SYS_YIELD        2
#define SYS_GET_TICKS    3
#define SYS_SLEEP        4
#define SYS_POLL_KEY     5
#define SYS_BLIT         6
#define SYS_GETPID       7
#define SYS_CAP_GET      8
#define SYS_IPC_REGISTER 9
#define SYS_IPC_SEND     10
#define SYS_IPC_RECEIVE  11
#define SYS_IPC_PENDING  12
#define SYS_SYSFS_READ   13

/* SYS_IPC_SEND ABI:
 *   ebx = receiver pid
 *   ecx = message type
 *   edx = payload pointer
 *   esi = payload length
 * SYS_IPC_RECEIVE:
 *   ebx = pointer to struct ipc_message
 * SYS_SYSFS_READ:
 *   ebx = path pointer
 *   ecx = output buffer pointer
 *   edx = output capacity
 */

#define DOOM_BLIT_W 320
#define DOOM_BLIT_H 200

void syscall_init(void);
const char *syscall_last_message(void);
int syscall_message_count(void);

#endif
