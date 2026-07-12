#ifndef KERNEL_DEMO_USER_TASK_H
#define KERNEL_DEMO_USER_TASK_H

/* Runs entirely in ring 3 (CPL 3) -- it can only talk to the kernel
 * through `int 0x80` syscalls, never direct port I/O (that would fault). */
void demo_user_task_entry(void);

#endif
