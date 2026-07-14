#ifndef GUI_NOVA_APPS_H
#define GUI_NOVA_APPS_H

#include <stdint.h>

#define NOVA_APP_COUNT 9

enum nova_app_id {
    NOVA_APP_HOME = 0,
    NOVA_APP_TERMINAL = 1,
    NOVA_APP_FILES = 2,
    NOVA_APP_TASKS = 3,
    NOVA_APP_BROWSER = 4,
    NOVA_APP_EDITOR = 5,
    NOVA_APP_ACTIVITY = 6,
    NOVA_APP_SETTINGS = 7,
    NOVA_APP_ABOUT = 8
};

struct nova_app_descriptor {
    enum nova_app_id id;
    const char *name;
    const char *subtitle;
    const char *glyph;
    uint32_t accent;
    char shortcut;
};

const struct nova_app_descriptor *nova_app_get(enum nova_app_id id);
const struct nova_app_descriptor *nova_app_at(int index);
int nova_app_count(void);

#endif
