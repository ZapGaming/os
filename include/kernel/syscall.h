#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#define SYS_EXIT  0
#define SYS_WRITE 1
#define SYS_YIELD 2

void syscall_init(void);
const char *syscall_last_message(void);
int syscall_message_count(void);

#endif
