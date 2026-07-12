#include <gui/compositor.h>
#include <gui/framebuffer.h>
#include <drivers/mouse.h>
#include <drivers/keyboard.h>
#include <kernel/pit.h>
#include <kernel/scheduler.h>
#include <kernel/demo.h>
#include <kernel/syscall.h>
#include <string.h>

#define MAX_WINDOWS   5
#define TITLEBAR_H    28
#define TASKBAR_H     44

typedef struct {
    int x, y, w, h;
    char title[32];
    const char *body_line1;
    const char *body_line2;
    uint32_t accent;
    int is_process_monitor;
} gui_window_t;

static gui_window_t windows[MAX_WINDOWS];
static int window_order[MAX_WINDOWS];
static int window_count = 0;

static int dragging_window = -1;
static int drag_dx = 0, drag_dy = 0;
static int prev_left = 0;

static const uint32_t COL_BG_TOP    = 0x141B4D;
static const uint32_t COL_BG_BOTTOM = 0x05060F;
static const uint32_t COL_TEXT      = 0xF2F4FF;
static const uint32_t COL_MUTED     = 0xA8AFD6;

static int add_window(int x, int y, int w, int h, const char *title,
                       const char *l1, const char *l2, uint32_t accent) {
    int idx = window_count;
    gui_window_t *win = &windows[idx];
    win->x = x; win->y = y; win->w = w; win->h = h;
    strcpy(win->title, title);
    win->body_line1 = l1;
    win->body_line2 = l2;
    win->accent = accent;
    win->is_process_monitor = 0;
    window_order[window_count] = idx;
    window_count++;
    return idx;
}

static void bring_to_front(int order_pos) {
    int wi = window_order[order_pos];
    for (int i = order_pos; i < window_count - 1; i++) {
        window_order[i] = window_order[i + 1];
    }
    window_order[window_count - 1] = wi;
}

void gui_init(void) {
    add_window(120, 90, 340, 190, "About ZapOS",
               "A fully custom 32-bit OS kernel",
               "GUI + drivers written from scratch", 0x3E6FF0);
    add_window(560, 160, 300, 170, "System Monitor",
               "Kernel heap + paging: online",
               "PS/2 keyboard + mouse: online", 0x2FBF71);
    add_window(260, 340, 320, 150, "Roadmap",
               "Next up: networking + audio",
               "See README.md for the plan", 0xE0954C);

    int pm = add_window(640, 420, 320, 190, "Process Monitor", NULL, NULL, 0xB05CE0);
    windows[pm].is_process_monitor = 1;
}

static void utoa(unsigned int val, char *buf) {
    char tmp[12];
    int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

static void draw_clock(int x, int y) {
    unsigned int total_seconds = pit_ticks() / 100;
    unsigned int mm = (total_seconds / 60) % 60;
    unsigned int ss = total_seconds % 60;

    char buf[16];
    char mmbuf[4], ssbuf[4];
    utoa(mm, mmbuf);
    utoa(ss, ssbuf);

    int i = 0;
    if (mm < 10) buf[i++] = '0';
    for (char *p = mmbuf; *p; p++) buf[i++] = *p;
    buf[i++] = ':';
    if (ss < 10) buf[i++] = '0';
    for (char *p = ssbuf; *p; p++) buf[i++] = *p;
    buf[i] = 0;

    fb_draw_string(x, y, buf, COL_TEXT, 2);
}

static void draw_labeled_uint(int x, int y, const char *label, unsigned int val, uint32_t color) {
    char buf[16];
    utoa(val, buf);
    fb_draw_string(x, y, label, COL_MUTED, 1);
    fb_draw_string(x + fb_text_width(label, 1), y, buf, color, 1);
}

static void draw_process_monitor(const gui_window_t *w) {
    int x = w->x + 14;
    int y = w->y + TITLEBAR_H + 14;

    draw_labeled_uint(x, y, "tasks running: ", (unsigned int)scheduler_task_count(), COL_TEXT);
    draw_labeled_uint(x, y + 20, "bg kernel task ticks: ", get_bg_counter(), 0x8FE3A8);
    draw_labeled_uint(x, y + 40, "ring-3 syscalls seen: ", (unsigned int)syscall_message_count(), 0xE0954C);

    fb_draw_string(x, y + 68, "last message from ring 3:", COL_MUTED, 1);
    fb_draw_string(x, y + 86, syscall_last_message(), COL_TEXT, 1);
}

static void draw_shadow(int x, int y, int w, int h) {
    int offset = 8;
    for (int i = 0; i < offset; i++) {
        uint8_t alpha = 60 - i * (60 / offset);
        for (int px = x - i; px < x + w + i; px += 4) {
            fb_blend_pixel(px, y + h + i, 0x000000, alpha);
        }
        for (int py = y - i; py < y + h + i; py += 4) {
            fb_blend_pixel(x + w + i, py, 0x000000, alpha);
        }
    }
}

static void draw_window(const gui_window_t *w, int focused) {
    draw_shadow(w->x, w->y, w->w, w->h);

    /* title bar */
    uint32_t accent_dark = ((w->accent >> 1) & 0x7F7F7F);
    fb_fill_gradient_v(w->x, w->y, w->w, TITLEBAR_H,
                        focused ? w->accent : accent_dark,
                        focused ? accent_dark : (accent_dark >> 1) & 0x7F7F7F);

    fb_draw_string(w->x + 10, w->y + 10, w->title, 0xFFFFFF, 1);

    /* close button */
    int cbx = w->x + w->w - 20, cby = w->y + 8;
    fb_fill_rect(cbx, cby, 12, 12, 0xE05252);

    /* body */
    fb_fill_rect(w->x, w->y + TITLEBAR_H, w->w, w->h - TITLEBAR_H, 0x1B2040);
    fb_draw_rect(w->x, w->y, w->w, w->h, focused ? w->accent : accent_dark);

    if (w->body_line1) fb_draw_string(w->x + 14, w->y + TITLEBAR_H + 16, w->body_line1, COL_TEXT, 1);
    if (w->body_line2) fb_draw_string(w->x + 14, w->y + TITLEBAR_H + 34, w->body_line2, COL_MUTED, 1);
    if (w->is_process_monitor) draw_process_monitor(w);
}

static void draw_taskbar(void) {
    int y = fb_height() - TASKBAR_H;
    fb_fill_gradient_v(0, y, fb_width(), TASKBAR_H, 0x10132C, 0x05060F);
    fb_draw_line(0, y, fb_width(), y, 0x3A4270);

    /* start pill */
    fb_fill_rect(12, y + 8, 90, TASKBAR_H - 16, 0x3E6FF0);
    fb_draw_string(28, y + 16, "ZapOS", 0xFFFFFF, 1);

    draw_clock(fb_width() - 90, y + 12);
}

static void draw_cursor(int x, int y) {
    fb_fill_triangle(x, y, x, y + 16, x + 11, y + 12, 0x000000);
    fb_fill_triangle(x + 1, y + 2, x + 1, y + 13, x + 9, y + 11, 0xFFFFFF);
}

static void draw_frame(int mx, int my) {
    fb_fill_gradient_v(0, 0, fb_width(), fb_height(), COL_BG_TOP, COL_BG_BOTTOM);
    fb_draw_string(24, 20, "ZapOS", COL_TEXT, 3);
    fb_draw_string(24, 56, "a custom 32-bit OS", COL_MUTED, 1);

    for (int i = 0; i < window_count; i++) {
        int wi = window_order[i];
        draw_window(&windows[wi], i == window_count - 1);
    }

    draw_taskbar();
    draw_cursor(mx, my);
}

void gui_run(void) {
    for (;;) {
        int mx, my;
        uint8_t buttons;
        mouse_get_state(&mx, &my, &buttons);

        int left_down = (buttons & MOUSE_LEFT_BUTTON) != 0;
        int left_edge = left_down && !prev_left;

        if (left_edge) {
            for (int oi = window_count - 1; oi >= 0; oi--) {
                int wi = window_order[oi];
                gui_window_t *w = &windows[wi];
                if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + TITLEBAR_H) {
                    bring_to_front(oi);
                    dragging_window = wi;
                    drag_dx = mx - w->x;
                    drag_dy = my - w->y;
                    break;
                }
            }
        }
        if (!left_down) dragging_window = -1;

        if (dragging_window >= 0) {
            gui_window_t *w = &windows[dragging_window];
            w->x = mx - drag_dx;
            w->y = my - drag_dy;
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            if ((uint32_t)(w->x + w->w) > fb_width()) w->x = fb_width() - w->w;
            if ((uint32_t)(w->y + w->h) > fb_height() - TASKBAR_H) w->y = fb_height() - TASKBAR_H - w->h;
        }

        prev_left = left_down;

        while (keyboard_getchar()) { /* drain; no text widgets yet */ }

        draw_frame(mx, my);
        fb_swap_buffers();
        pit_sleep(16);
    }
}
