#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <stdint.h>

#define IPC_MAX_ENDPOINTS 32
#define IPC_QUEUE_DEPTH 16
#define IPC_PAYLOAD_SIZE 96
#define IPC_SERVICE_NAME 24

struct ipc_message {
    uint32_t id;
    int sender_pid;
    int receiver_pid;
    uint32_t type;
    uint32_t length;
    uint32_t timestamp;
    uint8_t payload[IPC_PAYLOAD_SIZE];
};

struct ipc_endpoint_info {
    int pid;
    char service[IPC_SERVICE_NAME];
    uint32_t sent;
    uint32_t received;
    uint32_t dropped;
    int queued;
};

void ipc_init(void);
int ipc_register(int pid, const char *service_name);
int ipc_unregister(int pid);
int ipc_lookup(const char *service_name);
int ipc_send(int sender_pid, int receiver_pid, uint32_t type,
             const void *payload, uint32_t length);
int ipc_send_service(int sender_pid, const char *service_name, uint32_t type,
                     const void *payload, uint32_t length);
int ipc_receive(int receiver_pid, struct ipc_message *out);
int ipc_peek(int receiver_pid, struct ipc_message *out);
int ipc_pending(int receiver_pid);
int ipc_endpoint_snapshot(int index, struct ipc_endpoint_info *out);
int ipc_endpoint_count(void);
uint32_t ipc_total_messages(void);
uint32_t ipc_total_dropped(void);

#endif
