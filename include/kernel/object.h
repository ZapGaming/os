#ifndef KERNEL_OBJECT_H
#define KERNEL_OBJECT_H

#include <stdint.h>

#define OBJECT_MAX 128
#define HANDLE_MAX_PER_PROCESS 32
#define OBJECT_NAME 24

#define RIGHT_READ   (1u << 0)
#define RIGHT_WRITE  (1u << 1)
#define RIGHT_SIGNAL (1u << 2)
#define RIGHT_DUP    (1u << 3)
#define RIGHT_ADMIN  (1u << 31)

enum object_type { OBJECT_NONE, OBJECT_PROCESS, OBJECT_SERVICE, OBJECT_FILE, OBJECT_EVENT, OBJECT_CHANNEL, OBJECT_TIMER, OBJECT_SESSION };

struct object_info {
    int id;
    enum object_type type;
    int owner_pid;
    uint32_t refs;
    uint32_t rights;
    char name[OBJECT_NAME];
};

void object_manager_init(void);
int object_create(enum object_type type, int owner_pid, const char *name, uint32_t rights);
int object_close(int object_id);
int handle_open(int pid, int object_id, uint32_t rights);
int handle_close(int pid, int handle);
int handle_duplicate(int source_pid, int handle, int target_pid, uint32_t rights);
int handle_resolve(int pid, int handle, uint32_t required_rights, struct object_info *out);
int object_snapshot(int index, struct object_info *out);
int object_count(void);

#endif
