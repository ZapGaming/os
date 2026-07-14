#include <gui/window.h>
#include <string.h>

static struct nova_window windows[NOVA_MAX_WINDOWS];
static int used[NOVA_MAX_WINDOWS];
static int next_id = 1;
static int focused_id = -1;
static int desktop_w, desktop_h, top_reserved, bottom_reserved;
static int drag_id = -1;
static int resize_id = -1;
static int drag_dx, drag_dy;

static void copy_title(char *dst, const char *src) {
    int i = 0;
    if (!src) src = "Window";
    while (src[i] && i < NOVA_WINDOW_TITLE - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int index_for_id(int id) {
    for (int i = 0; i < NOVA_MAX_WINDOWS; i++) if (used[i] && windows[i].id == id) return i;
    return -1;
}

static int highest_z(void) {
    int z = 0;
    for (int i = 0; i < NOVA_MAX_WINDOWS; i++) if (used[i] && windows[i].z > z) z = windows[i].z;
    return z;
}

static void clamp_window(struct nova_window *w) {
    if (w->w < 220) w->w = 220;
    if (w->h < 140) w->h = 140;
    if (w->w > desktop_w) w->w = desktop_w;
    if (w->h > desktop_h - top_reserved - bottom_reserved) w->h = desktop_h - top_reserved - bottom_reserved;
    if (w->x < 0) w->x = 0;
    if (w->y < top_reserved) w->y = top_reserved;
    if (w->x + w->w > desktop_w) w->x = desktop_w - w->w;
    if (w->y + w->h > desktop_h - bottom_reserved) w->y = desktop_h - bottom_reserved - w->h;
}

void nova_wm_init(int width, int height, int top, int bottom) {
    desktop_w = width;
    desktop_h = height;
    top_reserved = top;
    bottom_reserved = bottom;
    memset(used, 0, sizeof(used));
    next_id = 1;
    focused_id = -1;
    drag_id = -1;
    resize_id = -1;
}

int nova_wm_open(int app_id, const char *title, int x, int y, int w, int h,
                 uint32_t accent, uint32_t flags) {
    for (int i = 0; i < NOVA_MAX_WINDOWS; i++) {
        if (used[i]) continue;
        used[i] = 1;
        struct nova_window *win = &windows[i];
        memset(win, 0, sizeof(*win));
        win->id = next_id++;
        win->app_id = app_id;
        win->x = x; win->y = y; win->w = w; win->h = h;
        win->restore_x = x; win->restore_y = y; win->restore_w = w; win->restore_h = h;
        win->accent = accent;
        win->flags = flags | NOVA_WINDOW_VISIBLE;
        win->z = highest_z() + 1;
        copy_title(win->title, title);
        clamp_window(win);
        focused_id = win->id;
        return win->id;
    }
    return -1;
}

void nova_wm_close(int id) {
    int idx = index_for_id(id);
    if (idx < 0) return;
    used[idx] = 0;
    if (focused_id == id) focused_id = -1;
}

void nova_wm_minimize(int id) {
    struct nova_window *w = nova_wm_get(id);
    if (!w) return;
    w->flags ^= NOVA_WINDOW_MINIMIZED;
    if (w->flags & NOVA_WINDOW_MINIMIZED && focused_id == id) focused_id = -1;
    else nova_wm_focus(id);
}

void nova_wm_toggle_maximize(int id) {
    struct nova_window *w = nova_wm_get(id);
    if (!w) return;
    if (w->flags & NOVA_WINDOW_MAXIMIZED) {
        w->x = w->restore_x; w->y = w->restore_y; w->w = w->restore_w; w->h = w->restore_h;
        w->flags &= ~NOVA_WINDOW_MAXIMIZED;
    } else {
        w->restore_x = w->x; w->restore_y = w->y; w->restore_w = w->w; w->restore_h = w->h;
        w->x = 0; w->y = top_reserved; w->w = desktop_w; w->h = desktop_h - top_reserved - bottom_reserved;
        w->flags |= NOVA_WINDOW_MAXIMIZED;
    }
    nova_wm_focus(id);
}

void nova_wm_focus(int id) {
    struct nova_window *w = nova_wm_get(id);
    if (!w) return;
    if (w->flags & NOVA_WINDOW_MINIMIZED) w->flags &= ~NOVA_WINDOW_MINIMIZED;
    w->z = highest_z() + 1;
    focused_id = id;
}

int nova_wm_hit_test(int mx, int my) {
    int found = -1, best_z = -1;
    for (int i = 0; i < NOVA_MAX_WINDOWS; i++) {
        if (!used[i]) continue;
        struct nova_window *w = &windows[i];
        if (!(w->flags & NOVA_WINDOW_VISIBLE) || (w->flags & NOVA_WINDOW_MINIMIZED)) continue;
        if (mx >= w->x && my >= w->y && mx < w->x + w->w && my < w->y + w->h && w->z > best_z) {
            best_z = w->z;
            found = w->id;
        }
    }
    return found;
}

void nova_wm_begin_pointer(int mx, int my) {
    int id = nova_wm_hit_test(mx, my);
    if (id < 0) return;
    struct nova_window *w = nova_wm_get(id);
    nova_wm_focus(id);
    if ((w->flags & NOVA_WINDOW_RESIZABLE) && mx >= w->x + w->w - 18 && my >= w->y + w->h - 18) {
        resize_id = id;
        drag_dx = w->x + w->w - mx;
        drag_dy = w->y + w->h - my;
    } else if (my < w->y + 32 && !(w->flags & NOVA_WINDOW_MAXIMIZED)) {
        drag_id = id;
        drag_dx = mx - w->x;
        drag_dy = my - w->y;
    }
}

void nova_wm_move_pointer(int mx, int my) {
    if (drag_id >= 0) {
        struct nova_window *w = nova_wm_get(drag_id);
        if (!w) return;
        w->x = mx - drag_dx;
        w->y = my - drag_dy;
        clamp_window(w);
    } else if (resize_id >= 0) {
        struct nova_window *w = nova_wm_get(resize_id);
        if (!w) return;
        w->w = mx - w->x + drag_dx;
        w->h = my - w->y + drag_dy;
        clamp_window(w);
    }
}

void nova_wm_end_pointer(void) {
    drag_id = -1;
    resize_id = -1;
}

int nova_wm_focused(void) { return focused_id; }

int nova_wm_count(void) {
    int count = 0;
    for (int i = 0; i < NOVA_MAX_WINDOWS; i++) if (used[i]) count++;
    return count;
}

struct nova_window *nova_wm_get(int id) {
    int idx = index_for_id(id);
    return idx < 0 ? 0 : &windows[idx];
}

struct nova_window *nova_wm_at_z(int order) {
    int count = nova_wm_count();
    if (order < 0 || order >= count) return 0;
    int last_z = -1;
    struct nova_window *result = 0;
    for (int step = 0; step <= order; step++) {
        int best_z = 0x7FFFFFFF;
        result = 0;
        for (int i = 0; i < NOVA_MAX_WINDOWS; i++) {
            if (!used[i]) continue;
            if (windows[i].z > last_z && windows[i].z < best_z) {
                best_z = windows[i].z;
                result = &windows[i];
            }
        }
        if (!result) return 0;
        last_z = result->z;
    }
    return result;
}
