#include <kernel/session.h>
#include <kernel/pit.h>
#include <string.h>

static struct user_identity users[SESSION_MAX_USERS];
static struct user_session sessions[SESSION_MAX_ACTIVE];
static int users_count;
static int next_uid = 1;
static int next_sid = 1;
static uint32_t token_seed = 0x9E3779B9u;

static void copy_text(char *dst, int cap, const char *src) {
    int i = 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void make_token(char out[SESSION_TOKEN], int sid, int uid, int owner_pid) {
    static const char hex[] = "0123456789ABCDEF";
    uint32_t x = token_seed ^ (uint32_t)sid * 2654435761u ^ (uint32_t)uid * 2246822519u ^ (uint32_t)owner_pid ^ pit_ticks();
    for (int i = 0; i < SESSION_TOKEN - 1; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = hex[x & 0xF];
    }
    out[SESSION_TOKEN - 1] = 0;
    token_seed = x;
}

void session_manager_init(void) {
    memset(users, 0, sizeof(users));
    memset(sessions, 0, sizeof(sessions));
    users_count = 0;
    next_uid = 1;
    next_sid = 1;
    user_create("root", SESSION_ADMIN, 0xFFFFFFFFu);
    user_create("nova", SESSION_DEVELOPER, 0x7FFFFFFFu);
    user_create("guest", SESSION_GUEST, 0x00000007u);
}

int user_create(const char *name, enum session_role role, uint32_t capability_ceiling) {
    if (!name || !name[0] || users_count >= SESSION_MAX_USERS) return -1;
    for (int i = 0; i < users_count; i++) if (strcmp(users[i].name, name) == 0) return -1;
    struct user_identity *u = &users[users_count++];
    memset(u, 0, sizeof(*u));
    u->uid = next_uid++;
    copy_text(u->name, sizeof(u->name), name);
    u->role = role;
    u->capability_ceiling = capability_ceiling;
    u->enabled = 1;
    return u->uid;
}

int user_disable(int uid) {
    for (int i = 0; i < users_count; i++) {
        if (users[i].uid != uid) continue;
        users[i].enabled = 0;
        for (int j = 0; j < SESSION_MAX_ACTIVE; j++) if (sessions[j].active && sessions[j].uid == uid) sessions[j].active = 0;
        return 1;
    }
    return 0;
}

int session_open(int uid, int owner_pid, struct user_session *out) {
    int user_ok = 0;
    for (int i = 0; i < users_count; i++) if (users[i].uid == uid && users[i].enabled) user_ok = 1;
    if (!user_ok) return -1;
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (sessions[i].active) continue;
        struct user_session *s = &sessions[i];
        memset(s, 0, sizeof(*s));
        s->sid = next_sid++;
        s->uid = uid;
        s->owner_pid = owner_pid;
        s->created_tick = pit_ticks();
        s->last_activity = s->created_tick;
        s->active = 1;
        make_token(s->token, s->sid, uid, owner_pid);
        if (out) *out = *s;
        return s->sid;
    }
    return -1;
}

int session_validate(int sid, const char *token, struct user_session *out) {
    if (!token) return 0;
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (!sessions[i].active || sessions[i].sid != sid) continue;
        if (strcmp(sessions[i].token, token) != 0) return 0;
        sessions[i].last_activity = pit_ticks();
        if (out) *out = sessions[i];
        return 1;
    }
    return 0;
}

int session_touch(int sid) {
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (sessions[i].active && sessions[i].sid == sid) {
            sessions[i].last_activity = pit_ticks();
            return 1;
        }
    }
    return 0;
}

int session_close(int sid) {
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (sessions[i].active && sessions[i].sid == sid) {
            sessions[i].active = 0;
            return 1;
        }
    }
    return 0;
}

int session_current_for_pid(int pid, struct user_session *out) {
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (sessions[i].active && sessions[i].owner_pid == pid) {
            if (out) *out = sessions[i];
            return 1;
        }
    }
    return 0;
}

int user_snapshot(int index, struct user_identity *out) {
    if (!out || index < 0 || index >= users_count) return 0;
    *out = users[index];
    return 1;
}

int session_snapshot(int index, struct user_session *out) {
    if (!out || index < 0) return 0;
    int seen = 0;
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) if (sessions[i].active && seen++ == index) { *out = sessions[i]; return 1; }
    return 0;
}

int user_count(void) { return users_count; }
int session_count(void) { int n = 0; for (int i = 0; i < SESSION_MAX_ACTIVE; i++) if (sessions[i].active) n++; return n; }
