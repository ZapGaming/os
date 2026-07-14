#ifndef KERNEL_CAPABILITY_H
#define KERNEL_CAPABILITY_H

#include <stdint.h>

#define CAP_IPC_SEND      (1u << 0)
#define CAP_IPC_RECEIVE   (1u << 1)
#define CAP_PROCESS_QUERY (1u << 2)
#define CAP_PROCESS_KILL  (1u << 3)
#define CAP_AFFINITY      (1u << 4)
#define CAP_FILESYSTEM    (1u << 5)
#define CAP_NETWORK       (1u << 6)
#define CAP_AUDIO         (1u << 7)
#define CAP_POWER         (1u << 8)
#define CAP_SYSTEM_ADMIN  (1u << 31)

#define CAP_DEFAULT_KERNEL (CAP_IPC_SEND | CAP_IPC_RECEIVE | CAP_PROCESS_QUERY | \
                            CAP_PROCESS_KILL | CAP_AFFINITY | CAP_FILESYSTEM | \
                            CAP_NETWORK | CAP_AUDIO | CAP_POWER | CAP_SYSTEM_ADMIN)
#define CAP_DEFAULT_USER   (CAP_IPC_SEND | CAP_IPC_RECEIVE | CAP_PROCESS_QUERY | CAP_FILESYSTEM)
#define CAP_DEFAULT_ISOLATED (CAP_IPC_SEND | CAP_IPC_RECEIVE)

void capability_init(void);
void capability_register_process(int pid, uint32_t mask);
uint32_t capability_get(int pid);
int capability_has(int pid, uint32_t mask);
int capability_grant(int actor_pid, int target_pid, uint32_t mask);
int capability_revoke(int actor_pid, int target_pid, uint32_t mask);
const char *capability_name(uint32_t single_capability);

#endif
