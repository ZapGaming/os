#include <kernel/service.h>
#include <kernel/ipc.h>
#include <kernel/pit.h>
#include <kernel/scheduler.h>
#include <string.h>

static struct service_descriptor services[SERVICE_MAX];
static int services_count;

static void copy_text(char *dst, const char *src) {
    int i = 0;
    while (src && src[i] && i < SERVICE_NAME - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int service_find(const char *name) {
    for (int i = 0; i < services_count; i++) if (strcmp(services[i].name, name) == 0) return i;
    return -1;
}

void service_manager_init(void) {
    memset(services, 0, sizeof(services));
    services_count = 0;
}

int service_register(const char *name, const char *ipc_name, enum service_restart_policy policy) {
    if (!name || service_find(name) >= 0 || services_count >= SERVICE_MAX) return 0;
    struct service_descriptor *s = &services[services_count++];
    memset(s, 0, sizeof(*s));
    copy_text(s->name, name);
    copy_text(s->ipc_name, ipc_name);
    s->pid = -1;
    s->state = SERVICE_STOPPED;
    s->restart_policy = policy;
    return 1;
}

int service_add_dependency(const char *name, const char *dependency) {
    int idx = service_find(name);
    if (idx < 0 || !dependency || services[idx].dependency_count >= SERVICE_DEPENDENCIES) return 0;
    copy_text(services[idx].dependencies[services[idx].dependency_count++], dependency);
    return 1;
}

static int dependencies_ready(struct service_descriptor *s) {
    for (int i = 0; i < s->dependency_count; i++) {
        int dep = service_find(s->dependencies[i]);
        if (dep < 0 || services[dep].state != SERVICE_RUNNING) return 0;
    }
    return 1;
}

int service_start(const char *name, int pid) {
    int idx = service_find(name);
    if (idx < 0 || pid < 0) return 0;
    struct service_descriptor *s = &services[idx];
    if (!dependencies_ready(s)) { s->state = SERVICE_DEGRADED; return 0; }
    s->state = SERVICE_STARTING;
    s->pid = pid;
    if (s->ipc_name[0]) ipc_register(pid, s->ipc_name);
    s->starts++;
    s->last_heartbeat = pit_ticks();
    s->state = SERVICE_RUNNING;
    return 1;
}

int service_stop(const char *name) {
    int idx = service_find(name);
    if (idx < 0) return 0;
    struct service_descriptor *s = &services[idx];
    if (s->pid >= 0) ipc_unregister(s->pid);
    s->pid = -1;
    s->state = SERVICE_STOPPED;
    return 1;
}

int service_heartbeat(const char *name) {
    int idx = service_find(name);
    if (idx < 0) return 0;
    services[idx].last_heartbeat = pit_ticks();
    if (services[idx].state == SERVICE_DEGRADED) services[idx].state = SERVICE_RUNNING;
    return 1;
}

void service_poll(void) {
    uint32_t now = pit_ticks();
    for (int i = 0; i < services_count; i++) {
        struct service_descriptor *s = &services[i];
        if (s->state != SERVICE_RUNNING || s->pid < 0) continue;
        if (scheduler_task_state(s->pid) == TASK_TERMINATED) {
            s->failures++;
            s->state = SERVICE_FAILED;
            s->pid = -1;
            continue;
        }
        if (now - s->last_heartbeat > 1000) s->state = SERVICE_DEGRADED;
    }
}

int service_count(void) { return services_count; }
int service_snapshot(int index, struct service_descriptor *out) {
    if (!out || index < 0 || index >= services_count) return 0;
    *out = services[index];
    return 1;
}
