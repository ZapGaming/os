#include <gui/compositor.h>
#include <gui/framebuffer.h>
#include <drivers/mouse.h>
#include <drivers/keyboard.h>
#include <kernel/pit.h>
#include <kernel/scheduler.h>
#include <kernel/demo.h>
#include <kernel/syscall.h>
#include <net/net.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <fs/fat32.h>
#include <string.h>

#define MAX_WINDOWS   7
#define TITLEBAR_H    28
#define TASKBAR_H     44
#define FM_ROW_H      16
#define FM_MAX_ENTRIES 24
#define FM_PREVIEW_MAX 2048

typedef struct {
    int x, y, w, h;
    char title[32];
    const char *body_line1;
    const char *body_line2;
    uint32_t accent;
    int is_process_monitor;
    int is_network;
    int is_file_manager;
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

/* File manager state -- a single instance, since there's only ever one
 * File Manager window. */
static uint32_t fm_current_dir = 0;
static struct fat_dirent_info fm_entries[FM_MAX_ENTRIES];
static int fm_entry_count = 0;
static int fm_viewing_file = 0;
static int fm_editing = 0;
static int fm_dirty = 0;
static char fm_preview_name[FAT32_MAX_NAME];
static char fm_preview_buf[FM_PREVIEW_MAX + 1];
static int fm_preview_len = 0;
static char fm_status_msg[32] = "";
static uint32_t fm_status_until = 0;

static void fm_refresh(void);

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
    win->is_network = 0;
    win->is_file_manager = 0;
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
               "Next up: process isolation + a filesystem",
               "See README.md for the plan", 0xE0954C);

    int pm = add_window(640, 420, 320, 190, "Process Monitor", NULL, NULL, 0xB05CE0);
    windows[pm].is_process_monitor = 1;

    int net = add_window(120, 460, 340, 190, "Network", NULL, NULL, 0x3ED0D8);
    windows[net].is_network = 1;

    if (fat32_is_mounted()) {
        int fm = add_window(480, 560, 380, 220, "File Manager", NULL, NULL, 0xF2C14E);
        windows[fm].is_file_manager = 1;
        fm_current_dir = fat32_root_cluster();
        fm_refresh();
    }
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

static void format_ip(uint32_t ip, char *buf) {
    char part[4];
    int i = 0;
    utoa((ip >> 24) & 0xFF, part); for (char *p = part; *p; p++) buf[i++] = *p;
    buf[i++] = '.';
    utoa((ip >> 16) & 0xFF, part); for (char *p = part; *p; p++) buf[i++] = *p;
    buf[i++] = '.';
    utoa((ip >> 8) & 0xFF, part); for (char *p = part; *p; p++) buf[i++] = *p;
    buf[i++] = '.';
    utoa(ip & 0xFF, part); for (char *p = part; *p; p++) buf[i++] = *p;
    buf[i] = 0;
}

static void draw_network(const gui_window_t *w) {
    int x = w->x + 14;
    int y = w->y + TITLEBAR_H + 14;
    char buf[20];

    if (!net_is_up()) {
        fb_draw_string(x, y, "no NIC detected", 0xE05252, 1);
        return;
    }

    const uint8_t *mac = net_get_mac();
    char macbuf[24];
    {
        static const char hex[] = "0123456789ABCDEF";
        int i = 0;
        for (int b = 0; b < 6; b++) {
            macbuf[i++] = hex[mac[b] >> 4];
            macbuf[i++] = hex[mac[b] & 0xF];
            if (b < 5) macbuf[i++] = ':';
        }
        macbuf[i] = 0;
    }
    fb_draw_string(x, y, "rtl8139  ", COL_MUTED, 1);
    fb_draw_string(x + fb_text_width("rtl8139  ", 1), y, macbuf, COL_TEXT, 1);

    format_ip(net_get_ip(), buf);
    fb_draw_string(x, y + 20, "ip      ", COL_MUTED, 1);
    fb_draw_string(x + fb_text_width("ip      ", 1), y + 20, buf, COL_TEXT, 1);

    format_ip(net_get_gateway_ip(), buf);
    uint8_t gw_mac[6];
    int resolved = arp_resolve(net_get_gateway_ip(), gw_mac);
    fb_draw_string(x, y + 40, "gateway ", COL_MUTED, 1);
    fb_draw_string(x + fb_text_width("gateway ", 1), y + 40, buf, COL_TEXT, 1);
    fb_draw_string(x + fb_text_width("gateway ", 1) + fb_text_width(buf, 1) + 8,
                   y + 40, resolved ? "(resolved)" : "(resolving...)",
                   resolved ? 0x8FE3A8 : 0xE0954C, 1);

    draw_labeled_uint(x, y + 68, "pings sent: ", (unsigned int)icmp_requests_sent(), COL_TEXT);
    draw_labeled_uint(x, y + 88, "replies received: ", (unsigned int)icmp_replies_received(), 0x8FE3A8);
    draw_labeled_uint(x, y + 108, "last rtt: ", icmp_last_rtt_ms(), COL_TEXT);
}

static void fm_refresh(void) {
    fm_entry_count = fat32_list_dir(fm_current_dir, fm_entries, FM_MAX_ENTRIES);
    fm_viewing_file = 0;
    fm_editing = 0;
    fm_dirty = 0;
}

static int fm_is_notes_txt(const char *name) {
    return strcmp(name, "NOTES.TXT") == 0;
}

/* Called when a row in the listing is clicked: navigate into directories,
 * open a read-only preview for other files, or an editable one for the
 * demo's NOTES.TXT (the only file fat32_write_file knows how to save). */
static void fm_open_entry(int index) {
    if (index < 0 || index >= fm_entry_count) return;
    struct fat_dirent_info *e = &fm_entries[index];

    if (e->is_dir) {
        if (strcmp(e->name, ".") == 0) return;
        fm_current_dir = (e->cluster < 2) ? fat32_root_cluster() : e->cluster;
        fm_refresh();
        return;
    }

    strcpy(fm_preview_name, e->name);
    fm_preview_len = fat32_read_file(e->cluster, e->size, fm_preview_buf, FM_PREVIEW_MAX);
    fm_preview_buf[fm_preview_len] = 0;
    fm_viewing_file = 1;
    fm_editing = fm_is_notes_txt(e->name);
    fm_dirty = 0;
}

static void fm_save_notes(void) {
    int ok = fat32_write_file(fm_current_dir, "NOTES.TXT", fm_preview_buf, fm_preview_len);
    strcpy(fm_status_msg, ok ? "Saved -- persists across reboot" : "Save failed");
    fm_status_until = pit_ticks() + 200;
    fm_dirty = 0;
}

/* Feeds typed characters into the open NOTES.TXT buffer; Enter saves.
 * Called from gui_run() only when the File Manager has it open for edit. */
static void fm_handle_key(char c) {
    if (!fm_editing) return;
    if (c == '\n' || c == '\r') {
        fm_save_notes();
    } else if (c == '\b') {
        if (fm_preview_len > 0) { fm_preview_len--; fm_dirty = 1; }
    } else if (c >= 32 && c < 127 && fm_preview_len < FM_PREVIEW_MAX) {
        fm_preview_buf[fm_preview_len++] = c;
        fm_dirty = 1;
    }
    fm_preview_buf[fm_preview_len] = 0;
}

static void draw_wrapped_text(int x, int y, int max_width, int max_rows, const char *text, uint32_t color) {
    int chars_per_line = max_width / 8;
    if (chars_per_line < 1) chars_per_line = 1;
    if (chars_per_line > 62) chars_per_line = 62;

    char line[64];
    int col = 0, row = 0, li = 0;
    for (const char *p = text; *p && row < max_rows; p++) {
        if (*p == '\n' || col >= chars_per_line) {
            line[li] = 0;
            fb_draw_string(x, y + row * FM_ROW_H, line, color, 1);
            li = 0; col = 0; row++;
            if (*p == '\n') continue;
        }
        line[li++] = *p;
        col++;
    }
    if (li > 0 && row < max_rows) {
        line[li] = 0;
        fb_draw_string(x, y + row * FM_ROW_H, line, color, 1);
    }
}

static void fm_handle_click(const gui_window_t *w, int my) {
    if (!fat32_is_mounted()) return;
    int rel_y = my - (w->y + TITLEBAR_H + 12);

    if (fm_viewing_file) {
        if (rel_y >= 0 && rel_y < FM_ROW_H) fm_viewing_file = 0; /* "<- back" row */
        return;
    }
    int row = (rel_y - 18) / FM_ROW_H;
    if (row >= 0) fm_open_entry(row);
}

static void draw_file_manager(const gui_window_t *w) {
    int x = w->x + 14;
    int y = w->y + TITLEBAR_H + 12;
    int content_w = w->w - 28;

    if (!fat32_is_mounted()) {
        fb_draw_string(x, y, "no disk/FAT32 detected", 0xE05252, 1);
        return;
    }

    if (fm_viewing_file) {
        fb_draw_string(x, y, "<- back to listing", 0x62D8FF, 1);
        fb_draw_string(x, y + 18, fm_preview_name, COL_TEXT, 1);
        if (fm_editing) {
            fb_draw_string(x, y + 34,
                           fm_dirty ? "editing -- press Enter to save" : "press Enter to save, Backspace to edit",
                           COL_MUTED, 1);
        }
        draw_wrapped_text(x, y + 52, content_w, (w->h - TITLEBAR_H - 70) / FM_ROW_H, fm_preview_buf, COL_TEXT);
        if (pit_ticks() < fm_status_until) {
            fb_draw_string(x, w->y + w->h - 18, fm_status_msg, 0x8FE3A8, 1);
        }
        return;
    }

    fb_draw_string(x, y, fm_current_dir == fat32_root_cluster() ? "/" : "(subfolder)", COL_MUTED, 1);
    int max_rows = (w->h - TITLEBAR_H - 30) / FM_ROW_H;
    for (int i = 0; i < fm_entry_count && i < max_rows; i++) {
        char line[40];
        if (fm_entries[i].is_dir) {
            strcpy(line, "[DIR] ");
            strcat(line, fm_entries[i].name);
        } else {
            char sizebuf[12];
            utoa(fm_entries[i].size, sizebuf);
            strcpy(line, fm_entries[i].name);
            strcat(line, "  ");
            strcat(line, sizebuf);
            strcat(line, "B");
        }
        fb_draw_string(x, y + 18 + i * FM_ROW_H, line, fm_entries[i].is_dir ? 0x62D8FF : COL_TEXT, 1);
    }
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
    if (w->is_network) draw_network(w);
    if (w->is_file_manager) draw_file_manager(w);
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
            int hit_titlebar = 0;
            for (int oi = window_count - 1; oi >= 0; oi--) {
                int wi = window_order[oi];
                gui_window_t *w = &windows[wi];
                if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + TITLEBAR_H) {
                    bring_to_front(oi);
                    dragging_window = wi;
                    drag_dx = mx - w->x;
                    drag_dy = my - w->y;
                    hit_titlebar = 1;
                    break;
                }
            }
            if (!hit_titlebar) {
                for (int oi = window_count - 1; oi >= 0; oi--) {
                    int wi = window_order[oi];
                    gui_window_t *w = &windows[wi];
                    if (mx >= w->x && mx < w->x + w->w && my >= w->y + TITLEBAR_H && my < w->y + w->h) {
                        bring_to_front(oi);
                        if (w->is_file_manager) fm_handle_click(w, my);
                        break;
                    }
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

        for (char c = keyboard_getchar(); c; c = keyboard_getchar()) {
            fm_handle_key(c);
        }

        draw_frame(mx, my);
        fb_swap_buffers();
        pit_sleep(16);
    }
}
