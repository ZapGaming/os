#ifndef KERNEL_SESSION_H
#define KERNEL_SESSION_H

#include <stdint.h>

#define SESSION_MAX_USERS 8
#define SESSION_MAX_ACTIVE 8
#define SESSION_NAME 24
#define SESSION_TOKEN 32

enum session_role { SESSION_GUEST, SESSION_USER, SESSION_DEVELOPER, SESSION_ADMIN };

struct user_identity {
    int uid;
    char name[SESSION_NAME];
    enum session_role role;
    uint32_t capability_ceiling;
    int enabled;
};

struct user_session {
    int sid;
    int uid;
    int owner_pid;
    char token[SESSION_TOKEN];
    uint32_t created_tick;
    uint32_t last_activity;
    int active;
};

void session_manager_init(void);
int user_create(const char *name, enum session_role role, uint32_t capability_ceiling);
int user_disable(int uid);
int session_open(int uid, int owner_pid, struct user_session *out);
int session_validate(int sid, const char *token, struct user_session *out);
int session_touch(int sid);
int session_close(int sid);
int session_current_for_pid(int pid, struct user_session *out);
int user_snapshot(int index, struct user_identity *out);
int session_snapshot(int index, struct user_session *out);
int user_count(void);
int session_count(void);

#endif
