#include <gui/nova_apps.h>

static const struct nova_app_descriptor apps[NOVA_APP_COUNT] = {
    { NOVA_APP_HOME, "Home", "Session overview and quick actions", "::", 0x4DE2F6, '1' },
    { NOVA_APP_TERMINAL, "Terminal", "Native shell and ELF process output", ">_", 0xB985FF, '2' },
    { NOVA_APP_FILES, "Files", "Browse the mounted FAT32 workspace", "[]", 0x6E8BFF, '3' },
    { NOVA_APP_TASKS, "Tasks", "Inspect scheduler and process state", "%%", 0x6DE7B4, '4' },
    { NOVA_APP_ACTIVITY, "Activity", "Kernel event stream and notifications", "~~", 0xFFD166, '5' },
    { NOVA_APP_SETTINGS, "Settings", "Theme, motion, density and performance", "##", 0xFF8FAB, '6' },
    { NOVA_APP_ABOUT, "System", "Architecture and build capabilities", "<>", 0xFF9F6E, '7' }
};

const struct nova_app_descriptor *nova_app_get(enum nova_app_id id) {
    for (int i = 0; i < NOVA_APP_COUNT; i++) {
        if (apps[i].id == id) return &apps[i];
    }
    return &apps[0];
}

const struct nova_app_descriptor *nova_app_at(int index) {
    if (index < 0 || index >= NOVA_APP_COUNT) return 0;
    return &apps[index];
}

int nova_app_count(void) {
    return NOVA_APP_COUNT;
}
