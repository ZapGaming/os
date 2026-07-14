#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <stdint.h>

#define NOVA_MAX_WINDOWS 12
#define NOVA_WINDOW_TITLE 32

enum nova_window_flags {
    NOVA_WINDOW_VISIBLE   = 1 << 0,
    NOVA_WINDOW_MINIMIZED = 1 << 1,
    NOVA_WINDOW_MAXIMIZED = 1 << 2,
    NOVA_WINDOW_RESIZABLE = 1 << 3,
    NOVA_WINDOW_CLOSABLE  = 1 << 4,
    NOVA_WINDOW_PINNED    = 1 << 5
};

struct nova_window {
    int id;
    int app_id;
    int x, y, w, h;
    int restore_x, restore_y, restore_w, restore_h;
    int z;
    uint32_t flags;
    uint32_t accent;
    char title[NOVA_WINDOW_TITLE];
};

void nova_wm_init(int desktop_w, int desktop_h, int top_reserved, int bottom_reserved);
int nova_wm_open(int app_id, const char *title, int x, int y, int w, int h,
                 uint32_t accent, uint32_t flags);
void nova_wm_close(int id);
void nova_wm_minimize(int id);
void nova_wm_toggle_maximize(int id);
void nova_wm_focus(int id);
void nova_wm_begin_pointer(int mx, int my);
void nova_wm_move_pointer(int mx, int my);
void nova_wm_end_pointer(void);
int nova_wm_hit_test(int mx, int my);
int nova_wm_focused(void);
int nova_wm_count(void);
struct nova_window *nova_wm_get(int id);
struct nova_window *nova_wm_at_z(int order);

#endif
