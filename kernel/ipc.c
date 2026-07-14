/* Named channel registry -- see include/kernel/ipc.h for the design
 * rationale (why this file is deliberately schedule()-free, and why
 * that's what makes it host-testable). Every function below is a
 * straight-line state-machine transition on `channels[]`; none of them
 * loop, block, or call anything outside this translation unit besides
 * memset/memcpy/strlen/strcmp/strncpy. */
#include <kernel/ipc.h>
#include <string.h>

struct ipc_message {
    uint8_t data[IPC_MSG_MAX];
    uint32_t len;
};

struct ipc_channel {
    int in_use;
    int refcount;
    char name[IPC_NAME_MAX];
    struct ipc_message queue[IPC_QUEUE_DEPTH];
    int head;  /* index of the oldest still-queued message */
    int count; /* number of messages currently queued (0..IPC_QUEUE_DEPTH) */
};

static struct ipc_channel channels[IPC_MAX_CHANNELS];

void ipc_init(void) {
    memset(channels, 0, sizeof(channels));
}

int ipc_open(const char *name) {
    if (!name) return -1;
    size_t len = strlen(name);
    if (len >= IPC_NAME_MAX) return -1;

    for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
        if (channels[i].in_use && strcmp(channels[i].name, name) == 0) {
            channels[i].refcount++;
            return i;
        }
    }
    for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
        if (!channels[i].in_use) {
            struct ipc_channel *c = &channels[i];
            memset(c, 0, sizeof(*c));
            c->in_use = 1;
            c->refcount = 1;
            strncpy(c->name, name, IPC_NAME_MAX - 1);
            c->name[IPC_NAME_MAX - 1] = 0;
            return i;
        }
    }
    return -1; /* every existing channel has a different name, and no free slot */
}

void ipc_close(int id) {
    if (id < 0 || id >= IPC_MAX_CHANNELS) return;
    struct ipc_channel *c = &channels[id];
    if (!c->in_use) return;
    c->refcount--;
    if (c->refcount <= 0) {
        memset(c, 0, sizeof(*c)); /* frees the slot; drops any still-queued messages */
    }
}

int ipc_try_send(int id, const void *buf, uint32_t len) {
    if (id < 0 || id >= IPC_MAX_CHANNELS) return -1;
    struct ipc_channel *c = &channels[id];
    if (!c->in_use) return -1;
    if (len > IPC_MSG_MAX) return -1;
    if (c->count >= IPC_QUEUE_DEPTH) return 0;

    int slot = (c->head + c->count) % IPC_QUEUE_DEPTH;
    memcpy(c->queue[slot].data, buf, len);
    c->queue[slot].len = len;
    c->count++;
    return 1;
}

int ipc_try_recv(int id, void *buf, uint32_t cap, uint32_t *out_len) {
    if (id < 0 || id >= IPC_MAX_CHANNELS) return -1;
    struct ipc_channel *c = &channels[id];
    if (!c->in_use) return -1;
    if (c->count == 0) return 0;

    struct ipc_message *m = &c->queue[c->head];
    uint32_t copy_len = m->len < cap ? m->len : cap;
    memcpy(buf, m->data, copy_len);
    if (out_len) *out_len = m->len;
    c->head = (c->head + 1) % IPC_QUEUE_DEPTH;
    c->count--;
    return 1;
}
