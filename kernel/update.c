#include <kernel/update.h>
#include <kernel/package.h>
#include <kernel/pit.h>
#include <string.h>

static struct update_component components[UPDATE_MAX_COMPONENTS];
static struct update_status status;
static uint32_t next_transaction_id = 1;

static void copy_text(char *dst, int cap, const char *src) {
    int i = 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void update_manager_init(void) {
    memset(components, 0, sizeof(components));
    memset(&status, 0, sizeof(status));
    status.state = UPDATE_IDLE;
}

int update_begin(void) {
    if (status.state != UPDATE_IDLE && status.state != UPDATE_APPLIED && status.state != UPDATE_FAILED) return 0;
    memset(components, 0, sizeof(components));
    status.state = UPDATE_STAGED;
    status.transaction_id = next_transaction_id++;
    status.component_count = 0;
    status.validated_count = 0;
    status.rollback_available = 0;
    status.staged_tick = pit_ticks();
    return 1;
}

int update_stage_component(const char *name, const char *version, const void *payload, uint32_t size, int required) {
    if (status.state != UPDATE_STAGED || !name || !version || !payload || size == 0 || status.component_count >= UPDATE_MAX_COMPONENTS) return 0;
    struct update_component *c = &components[status.component_count++];
    memset(c, 0, sizeof(*c));
    copy_text(c->name, sizeof(c->name), name);
    copy_text(c->version, sizeof(c->version), version);
    c->size = size;
    c->checksum = package_checksum(payload, size);
    c->required = required ? 1 : 0;
    return 1;
}

int update_validate(void) {
    if (status.state != UPDATE_STAGED || status.component_count == 0) return 0;
    status.validated_count = 0;
    for (int i = 0; i < status.component_count; i++) {
        components[i].validated = components[i].checksum != 0 && components[i].size > 0;
        if (components[i].validated) status.validated_count++;
        else if (components[i].required) {
            status.state = UPDATE_FAILED;
            return 0;
        }
    }
    status.state = UPDATE_VALIDATED;
    return 1;
}

int update_apply(void) {
    if (status.state != UPDATE_VALIDATED || status.validated_count != status.component_count) return 0;
    status.rollback_available = 1;
    status.state = UPDATE_APPLIED;
    return 1;
}

int update_request_rollback(void) {
    if (!status.rollback_available) return 0;
    status.state = UPDATE_ROLLBACK_PENDING;
    return 1;
}

int update_commit_boot_success(void) {
    if (status.state != UPDATE_APPLIED) return 0;
    status.rollback_available = 0;
    status.state = UPDATE_IDLE;
    return 1;
}

void update_status_get(struct update_status *out) {
    if (out) *out = status;
}

int update_component_snapshot(int index, struct update_component *out) {
    if (!out || index < 0 || index >= status.component_count) return 0;
    *out = components[index];
    return 1;
}
