#ifndef KERNEL_UPDATE_H
#define KERNEL_UPDATE_H

#include <stdint.h>

#define UPDATE_MAX_COMPONENTS 16
#define UPDATE_NAME 32
#define UPDATE_VERSION 16

enum update_state { UPDATE_IDLE, UPDATE_STAGED, UPDATE_VALIDATED, UPDATE_APPLIED, UPDATE_ROLLBACK_PENDING, UPDATE_FAILED };

struct update_component {
    char name[UPDATE_NAME];
    char version[UPDATE_VERSION];
    uint32_t checksum;
    uint32_t size;
    int required;
    int validated;
};

struct update_status {
    enum update_state state;
    uint32_t transaction_id;
    int component_count;
    int validated_count;
    int rollback_available;
    uint32_t staged_tick;
};

void update_manager_init(void);
int update_begin(void);
int update_stage_component(const char *name, const char *version, const void *payload, uint32_t size, int required);
int update_validate(void);
int update_apply(void);
int update_request_rollback(void);
int update_commit_boot_success(void);
void update_status_get(struct update_status *out);
int update_component_snapshot(int index, struct update_component *out);

#endif
