#include <kernel/object.h>
#include <kernel/scheduler.h>
#include <string.h>

struct handle_entry { int used; int object_id; uint32_t rights; };
static struct object_info objects[OBJECT_MAX];
static uint8_t object_used[OBJECT_MAX];
static struct handle_entry handles[MAX_TASKS][HANDLE_MAX_PER_PROCESS];
static int next_object_id = 1;

static void copy_text(char *dst, const char *src) {
    int i = 0;
    while (src && src[i] && i < OBJECT_NAME - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static int object_index(int id) {
    for (int i = 0; i < OBJECT_MAX; i++) if (object_used[i] && objects[i].id == id) return i;
    return -1;
}
void object_manager_init(void) {
    memset(object_used, 0, sizeof(object_used));
    memset(handles, 0, sizeof(handles));
    next_object_id = 1;
}
int object_create(enum object_type type, int owner_pid, const char *name, uint32_t rights) {
    for (int i = 0; i < OBJECT_MAX; i++) {
        if (object_used[i]) continue;
        object_used[i] = 1;
        memset(&objects[i], 0, sizeof(objects[i]));
        objects[i].id = next_object_id++;
        objects[i].type = type;
        objects[i].owner_pid = owner_pid;
        objects[i].refs = 1;
        objects[i].rights = rights;
        copy_text(objects[i].name, name);
        return objects[i].id;
    }
    return -1;
}
int object_close(int object_id) {
    int idx = object_index(object_id);
    if (idx < 0) return 0;
    if (objects[idx].refs > 1) { objects[idx].refs--; return 1; }
    object_used[idx] = 0;
    return 1;
}
int handle_open(int pid, int object_id, uint32_t rights) {
    if (pid < 0 || pid >= MAX_TASKS) return -1;
    int idx = object_index(object_id);
    if (idx < 0 || (rights & objects[idx].rights) != rights) return -1;
    for (int h = 0; h < HANDLE_MAX_PER_PROCESS; h++) {
        if (handles[pid][h].used) continue;
        handles[pid][h].used = 1;
        handles[pid][h].object_id = object_id;
        handles[pid][h].rights = rights;
        objects[idx].refs++;
        return h;
    }
    return -1;
}
int handle_close(int pid, int handle) {
    if (pid < 0 || pid >= MAX_TASKS || handle < 0 || handle >= HANDLE_MAX_PER_PROCESS || !handles[pid][handle].used) return 0;
    int object_id = handles[pid][handle].object_id;
    handles[pid][handle].used = 0;
    return object_close(object_id);
}
int handle_duplicate(int source_pid, int handle, int target_pid, uint32_t rights) {
    struct object_info info;
    if (!handle_resolve(source_pid, handle, RIGHT_DUP, &info)) return -1;
    if ((rights & handles[source_pid][handle].rights) != rights) return -1;
    return handle_open(target_pid, info.id, rights);
}
int handle_resolve(int pid, int handle, uint32_t required_rights, struct object_info *out) {
    if (pid < 0 || pid >= MAX_TASKS || handle < 0 || handle >= HANDLE_MAX_PER_PROCESS) return 0;
    struct handle_entry *h = &handles[pid][handle];
    if (!h->used || (h->rights & required_rights) != required_rights) return 0;
    int idx = object_index(h->object_id);
    if (idx < 0) return 0;
    if (out) *out = objects[idx];
    return 1;
}
int object_snapshot(int index, struct object_info *out) {
    if (!out || index < 0) return 0;
    int seen = 0;
    for (int i = 0; i < OBJECT_MAX; i++) if (object_used[i] && seen++ == index) { *out = objects[i]; return 1; }
    return 0;
}
int object_count(void) { int n = 0; for (int i = 0; i < OBJECT_MAX; i++) if (object_used[i]) n++; return n; }
