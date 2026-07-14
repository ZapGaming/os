#include <kernel/ipc.h>
#include <kernel/capability.h>
#include <kernel/pit.h>
#include <kernel/spinlock.h>
#include <string.h>

struct ipc_endpoint {
    int used;
    int pid;
    char service[IPC_SERVICE_NAME];
    struct ipc_message queue[IPC_QUEUE_DEPTH];
    int head;
    int tail;
    int count;
    uint32_t sent;
    uint32_t received;
    uint32_t dropped;
};

static struct ipc_endpoint endpoints[IPC_MAX_ENDPOINTS];
static uint32_t next_message_id = 1;
static uint32_t total_messages;
static uint32_t total_dropped;
static spinlock_t ipc_lock = SPINLOCK_INIT;

static void copy_service(char *dst, const char *src) {
    int i = 0;
    if (!src) src = "";
    while (src[i] && i < IPC_SERVICE_NAME - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int endpoint_index_by_pid(int pid) {
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) if (endpoints[i].used && endpoints[i].pid == pid) return i;
    return -1;
}

static int endpoint_index_by_name(const char *name) {
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        if (endpoints[i].used && strcmp(endpoints[i].service, name) == 0) return i;
    }
    return -1;
}

void ipc_init(void) {
    memset(endpoints, 0, sizeof(endpoints));
    next_message_id = 1;
    total_messages = 0;
    total_dropped = 0;
}

int ipc_register(int pid, const char *service_name) {
    if (pid < 0 || !capability_has(pid, CAP_IPC_RECEIVE)) return 0;
    uint32_t flags = lock_acquire_irqsave(&ipc_lock);
    if (endpoint_index_by_pid(pid) >= 0 || (service_name && service_name[0] && endpoint_index_by_name(service_name) >= 0)) {
        lock_release_irqrestore(&ipc_lock, flags);
        return 0;
    }
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        if (endpoints[i].used) continue;
        memset(&endpoints[i], 0, sizeof(endpoints[i]));
        endpoints[i].used = 1;
        endpoints[i].pid = pid;
        copy_service(endpoints[i].service, service_name);
        lock_release_irqrestore(&ipc_lock, flags);
        return 1;
    }
    lock_release_irqrestore(&ipc_lock, flags);
    return 0;
}

int ipc_unregister(int pid) {
    uint32_t flags = lock_acquire_irqsave(&ipc_lock);
    int idx = endpoint_index_by_pid(pid);
    if (idx < 0) { lock_release_irqrestore(&ipc_lock, flags); return 0; }
    memset(&endpoints[idx], 0, sizeof(endpoints[idx]));
    lock_release_irqrestore(&ipc_lock, flags);
    return 1;
}

int ipc_lookup(const char *service_name) {
    uint32_t flags = lock_acquire_irqsave(&ipc_lock);
    int idx = endpoint_index_by_name(service_name);
    int pid = idx >= 0 ? endpoints[idx].pid : -1;
    lock_release_irqrestore(&ipc_lock, flags);
    return pid;
}

int ipc_send(int sender_pid, int receiver_pid, uint32_t type, const void *payload, uint32_t length) {
    if (!capability_has(sender_pid, CAP_IPC_SEND)) return 0;
    if (length > IPC_PAYLOAD_SIZE) length = IPC_PAYLOAD_SIZE;
    uint32_t flags = lock_acquire_irqsave(&ipc_lock);
    int target = endpoint_index_by_pid(receiver_pid);
    int source = endpoint_index_by_pid(sender_pid);
    if (target < 0) { lock_release_irqrestore(&ipc_lock, flags); return 0; }
    struct ipc_endpoint *ep = &endpoints[target];
    if (ep->count >= IPC_QUEUE_DEPTH) {
        ep->dropped++;
        total_dropped++;
        if (source >= 0) endpoints[source].dropped++;
        lock_release_irqrestore(&ipc_lock, flags);
        return 0;
    }
    struct ipc_message *m = &ep->queue[ep->tail];
    memset(m, 0, sizeof(*m));
    m->id = next_message_id++;
    m->sender_pid = sender_pid;
    m->receiver_pid = receiver_pid;
    m->type = type;
    m->length = length;
    m->timestamp = pit_ticks();
    if (payload && length) memcpy(m->payload, payload, length);
    ep->tail = (ep->tail + 1) % IPC_QUEUE_DEPTH;
    ep->count++;
    if (source >= 0) endpoints[source].sent++;
    total_messages++;
    lock_release_irqrestore(&ipc_lock, flags);
    return 1;
}

int ipc_send_service(int sender_pid, const char *service_name, uint32_t type, const void *payload, uint32_t length) {
    int pid = ipc_lookup(service_name);
    return pid >= 0 ? ipc_send(sender_pid, pid, type, payload, length) : 0;
}

int ipc_receive(int receiver_pid, struct ipc_message *out) {
    if (!out || !capability_has(receiver_pid, CAP_IPC_RECEIVE)) return 0;
    uint32_t flags = lock_acquire_irqsave(&ipc_lock);
    int idx = endpoint_index_by_pid(receiver_pid);
    if (idx < 0 || endpoints[idx].count == 0) { lock_release_irqrestore(&ipc_lock, flags); return 0; }
    struct ipc_endpoint *ep = &endpoints[idx];
    *out = ep->queue[ep->head];
    ep->head = (ep->head + 1) % IPC_QUEUE_DEPTH;
    ep->count--;
    ep->received++;
    lock_release_irqrestore(&ipc_lock, flags);
    return 1;
}

int ipc_peek(int receiver_pid, struct ipc_message *out) {
    if (!out || !capability_has(receiver_pid, CAP_IPC_RECEIVE)) return 0;
    uint32_t flags = lock_acquire_irqsave(&ipc_lock);
    int idx = endpoint_index_by_pid(receiver_pid);
    if (idx < 0 || endpoints[idx].count == 0) { lock_release_irqrestore(&ipc_lock, flags); return 0; }
    *out = endpoints[idx].queue[endpoints[idx].head];
    lock_release_irqrestore(&ipc_lock, flags);
    return 1;
}

int ipc_pending(int receiver_pid) {
    uint32_t flags = lock_acquire_irqsave(&ipc_lock);
    int idx = endpoint_index_by_pid(receiver_pid);
    int count = idx >= 0 ? endpoints[idx].count : 0;
    lock_release_irqrestore(&ipc_lock, flags);
    return count;
}

int ipc_endpoint_snapshot(int index, struct ipc_endpoint_info *out) {
    if (!out || index < 0) return 0;
    uint32_t flags = lock_acquire_irqsave(&ipc_lock);
    int seen = 0;
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        if (!endpoints[i].used) continue;
        if (seen++ != index) continue;
        out->pid = endpoints[i].pid;
        copy_service(out->service, endpoints[i].service);
        out->sent = endpoints[i].sent;
        out->received = endpoints[i].received;
        out->dropped = endpoints[i].dropped;
        out->queued = endpoints[i].count;
        lock_release_irqrestore(&ipc_lock, flags);
        return 1;
    }
    lock_release_irqrestore(&ipc_lock, flags);
    return 0;
}

int ipc_endpoint_count(void) {
    uint32_t flags = lock_acquire_irqsave(&ipc_lock);
    int count = 0;
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) if (endpoints[i].used) count++;
    lock_release_irqrestore(&ipc_lock, flags);
    return count;
}

uint32_t ipc_total_messages(void) { return total_messages; }
uint32_t ipc_total_dropped(void) { return total_dropped; }
