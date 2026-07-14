#ifndef KERNEL_SERVICE_H
#define KERNEL_SERVICE_H

#include <stdint.h>

#define SERVICE_MAX 24
#define SERVICE_NAME 24
#define SERVICE_DEPENDENCIES 6

enum service_state { SERVICE_STOPPED, SERVICE_STARTING, SERVICE_RUNNING, SERVICE_DEGRADED, SERVICE_FAILED };
enum service_restart_policy { SERVICE_RESTART_NEVER, SERVICE_RESTART_ON_FAILURE, SERVICE_RESTART_ALWAYS };

struct service_descriptor {
    char name[SERVICE_NAME];
    char ipc_name[SERVICE_NAME];
    int pid;
    enum service_state state;
    enum service_restart_policy restart_policy;
    uint32_t starts;
    uint32_t failures;
    uint32_t last_heartbeat;
    int dependency_count;
    char dependencies[SERVICE_DEPENDENCIES][SERVICE_NAME];
};

void service_manager_init(void);
int service_register(const char *name, const char *ipc_name, enum service_restart_policy policy);
int service_add_dependency(const char *name, const char *dependency);
int service_start(const char *name, int pid);
int service_stop(const char *name);
int service_heartbeat(const char *name);
void service_poll(void);
int service_count(void);
int service_snapshot(int index, struct service_descriptor *out);
int service_find(const char *name);

#endif
