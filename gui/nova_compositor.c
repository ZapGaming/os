#include <gui/compositor.h>
#include <gui/framebuffer.h>
#include <gui/shell.h>
#include <drivers/mouse.h>
#include <drivers/keyboard.h>
#include <kernel/pit.h>
#include <kernel/scheduler.h>
#include <fs/fat32.h>
#include <string.h>

#define DOOM_BLIT_W 320
#define DOOM_BLIT_H 200
#define PANEL_H 56
#define MAX_TERMINAL 6144
#define MAX_INPUT 128

static const uint32_t C_BG0 = 0x070A12;
static const uint32_t C_BG1 = 0x10172A;
static const uint32_t C_PANEL = 0x151D31;
static const uint32_t C_PANEL_2 = 0x1C2740;
static const uint32_t C_CARD = 0x111827;
static const uint32_t C_BORDER = 0x2B3854;
static const uint32_t C_TEXT = 0xF7F8FC;
static const uint32_t C_MUTED = 0x9EABC2;
static const uint32_t C_CYAN = 0x4DE2F6;
static const uint32_t C_BLUE = 0x6E8BFF;
static const uint32_t C_PURPLE = 0xB985FF;
static const uint32_t C_GREEN = 0x6DE7B4;
static const uint32_t C_RED = 0xFF6B85;

static uint32_t fullscreen_buffer[DOOM_BLIT_W * DOOM_BLIT_H];
static int fullscreen_active;
static int fullscreen_owner = -1;

static int active_view = 0;
static int launcher_open;
static int previous_left;
static int terminal_pid = -1;
static uint32_t terminal_cwd;
static char terminal_output[MAX_TERMINAL];
static int terminal_output_len;
static char terminal_input[MAX_INPUT];
static int terminal_input_len;

static int inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

static void append_text(const char *s) {
    if (!s) return;
    int n = (int)strlen(s);
    if (n <= 0) return;
    if (n >= MAX_TERMINAL) {
        s += n - (MAX_TERMINAL - 1);
        n = MAX_TERMINAL - 1;
    }
    if (terminal_output_len + n >= MAX_TERMINAL) {
        int drop = terminal_output_len + n - MAX_TERMINAL + 1;
        memmove(terminal_output, terminal_output + drop, terminal_output_len - drop);
        terminal_output_len -= drop;
    }
    memcpy(terminal_output + terminal_output_len, s, n);
    terminal_output_len += n;
    terminal_output[terminal_output_len] = 0;
}

void terminal_route_output(int pid, const char *s) {
    if (pid == terminal_pid) append_text(s);
}

void gui_blit_fullscreen(const uint32_t *pixels, int owner_pid) {
    if (!pixels) return;
    memcpy(fullscreen_buffer, pixels, sizeof(fullscreen_buffer));
    fullscreen_active = 1;
    fullscreen_owner = owner_pid;
}

static void draw_soft_rect(int x, int y, int w, int h, int radius, uint32_t color) {
    fb_fill_rounded_rect(x + 4, y + 5, w, h, radius, 0x03050A);
    fb_fill_rounded_rect(x, y, w, h, radius, color);
    fb_draw_rect(x, y, w, h, C_BORDER);
}

static void draw_orb(int cx, int cy, int r, uint32_t color) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (d <= r * r) {
                int alpha = 30 + ((r * r - d) * 130) / (r * r);
                fb_blend_pixel(cx + x, cy + y, color, (uint8_t)alpha);
            }
        }
    }
}

static void draw_wallpaper(void) {
    int w = (int)fb_width();
    int h = (int)fb_height();
    fb_fill_gradient_v(0, 0, w, h, C_BG1, C_BG0);
    draw_orb(w - 180, 130, 220, C_PURPLE);
    draw_orb(w / 3, h - 120, 260, C_BLUE);
    draw_orb(120, 120, 150, C_CYAN);

    for (int y = 0; y < h; y += 32)
        fb_draw_line(0, y, w, y, 0x111827);
    for (int x = 0; x < w; x += 32)
        fb_draw_line(x, 0, x, h, 0x0D1320);
}

static void draw_logo(int x, int y) {
    fb_fill_rounded_rect(x, y, 34, 34, 10, C_TEXT);
    fb_fill_triangle(x + 8, y + 24, x + 17, y + 7, x + 25, y + 24, C_BG0);
    fb_fill_triangle(x + 12, y + 22, x + 17, y + 13, x + 21, y + 22, C_CYAN);
}

static void draw_chip(int x, int y, const char *label, uint32_t accent) {
    int w = fb_text_width(label, 1) + 22;
    fb_fill_rounded_rect(x, y, w, 24, 12, C_PANEL_2);
    fb_fill_rounded_rect(x + 7, y + 8, 7, 7, 4, accent);
    fb_draw_string(x + 18, y + 8, label, C_TEXT, 1);
}

static void draw_topbar(void) {
    int w = (int)fb_width();
    fb_fill_rect(0, 0, w, PANEL_H, C_PANEL);
    fb_fill_rect(0, PANEL_H - 1, w, 1, C_BORDER);
    draw_logo(14, 11);
    fb_draw_string(58, 16, "ZapOS NOVA", C_TEXT, 2);
    fb_draw_string(58, 36, "32-bit. From scratch. Unreasonably alive.", C_MUTED, 1);

    draw_chip(w - 310, 16, "SMP", C_GREEN);
    draw_chip(w - 238, 16, fat32_is_mounted() ? "DISK" : "NO DISK", fat32_is_mounted() ? C_GREEN : C_RED);

    char clock[24];
    uint32_t total = pit_ticks() / 100;
    uint32_t sec = total % 60;
    uint32_t min = (total / 60) % 60;
    uint32_t hr = (total / 3600) % 24;
    clock[0] = '0' + (char)(hr / 10); clock[1] = '0' + (char)(hr % 10);
    clock[2] = ':';
    clock[3] = '0' + (char)(min / 10); clock[4] = '0' + (char)(min % 10);
    clock[5] = ':';
    clock[6] = '0' + (char)(sec / 10); clock[7] = '0' + (char)(sec % 10);
    clock[8] = 0;
    fb_draw_string(w - 88, 24, clock, C_TEXT, 1);
}

static void draw_sidebar_item(int index, int y, const char *symbol, const char *label) {
    int selected = active_view == index;
    if (selected) {
        fb_fill_rounded_rect(14, y, 196, 42, 12, C_PANEL_2);
        fb_fill_rounded_rect(14, y + 7, 4, 28, 2, index == 0 ? C_CYAN : index == 1 ? C_PURPLE : C_GREEN);
    }
    fb_draw_string(32, y + 13, symbol, selected ? C_TEXT : C_MUTED, 2);
    fb_draw_string(72, y + 17, label, selected ? C_TEXT : C_MUTED, 1);
}

static void draw_sidebar(void) {
    int h = (int)fb_height();
    fb_fill_rect(0, PANEL_H, 226, h - PANEL_H, 0x0D1320);
    fb_fill_rect(225, PANEL_H, 1, h - PANEL_H, C_BORDER);
    fb_draw_string(22, PANEL_H + 26, "WORKSPACE", C_MUTED, 1);
    draw_sidebar_item(0, PANEL_H + 52, "::", "Overview");
    draw_sidebar_item(1, PANEL_H + 102, ">_", "Terminal");
    draw_sidebar_item(2, PANEL_H + 152, "##", "System");

    fb_draw_string(22, h - 92, "BUILD", C_MUTED, 1);
    fb_draw_string(22, h - 70, "NOVA / EXPERIMENTAL", C_TEXT, 1);
    fb_draw_string(22, h - 48, "Ctrl-free computing", C_MUTED, 1);
}

static void draw_stat_card(int x, int y, int w, const char *label, const char *value, uint32_t accent) {
    draw_soft_rect(x, y, w, 104, 16, C_CARD);
    fb_fill_rounded_rect(x + 18, y + 18, 38, 38, 10, accent);
    fb_draw_string(x + 31, y + 31, "+", C_BG0, 2);
    fb_draw_string(x + 72, y + 22, label, C_MUTED, 1);
    fb_draw_string(x + 72, y + 48, value, C_TEXT, 2);
    fb_fill_rounded_rect(x + 18, y + 82, w - 36, 5, 3, C_PANEL_2);
    fb_fill_rounded_rect(x + 18, y + 82, (w - 36) * 3 / 4, 5, 3, accent);
}

static void draw_overview(void) {
    int w = (int)fb_width();
    int x = 258;
    int y = 86;
    int content_w = w - x - 30;
    fb_draw_string(x, y, "Your computer, without the historical baggage.", C_TEXT, 2);
    fb_draw_string(x, y + 30, "A compact native environment built directly on the hardware model.", C_MUTED, 1);

    int card_w = (content_w - 32) / 3;
    char tasks[12];
    int tc = scheduler_task_count();
    tasks[0] = '0' + (char)((tc / 10) % 10); tasks[1] = '0' + (char)(tc % 10); tasks[2] = 0;
    draw_stat_card(x, y + 72, card_w, "TASKS", tasks, C_CYAN);
    draw_stat_card(x + card_w + 16, y + 72, card_w, "ARCH", "x86", C_PURPLE);
    draw_stat_card(x + (card_w + 16) * 2, y + 72, card_w, "MODE", "RING 3", C_GREEN);

    int panel_y = y + 200;
    int left_w = content_w * 3 / 5;
    draw_soft_rect(x, panel_y, left_w, 270, 18, C_CARD);
    fb_draw_string(x + 22, panel_y + 20, "NOVA COMMAND CENTER", C_TEXT, 1);
    fb_draw_string(x + 22, panel_y + 52, "A new shell over a real kernel", C_TEXT, 2);
    fb_draw_string(x + 22, panel_y + 86, "No browser pretending to be a desktop.", C_MUTED, 1);
    fb_draw_string(x + 22, panel_y + 104, "No Linux hiding under the floorboards.", C_MUTED, 1);
    fb_draw_string(x + 22, panel_y + 122, "No design system assembled by committee.", C_MUTED, 1);

    fb_fill_rounded_rect(x + 22, panel_y + 162, 154, 42, 12, C_TEXT);
    fb_draw_string(x + 46, panel_y + 178, "OPEN TERMINAL", C_BG0, 1);
    fb_fill_rounded_rect(x + 188, panel_y + 162, 134, 42, 12, C_PANEL_2);
    fb_draw_string(x + 218, panel_y + 178, "SYSTEM", C_TEXT, 1);

    int right_x = x + left_w + 16;
    int right_w = content_w - left_w - 16;
    draw_soft_rect(right_x, panel_y, right_w, 270, 18, C_CARD);
    fb_draw_string(right_x + 20, panel_y + 20, "LIVE PULSE", C_TEXT, 1);
    for (int i = 0; i < 18; i++) {
        int bh = 18 + ((i * 17 + (int)(pit_ticks() / 5)) % 90);
        uint32_t c = i % 3 == 0 ? C_CYAN : i % 3 == 1 ? C_BLUE : C_PURPLE;
        fb_fill_rounded_rect(right_x + 18 + i * 10, panel_y + 220 - bh, 6, bh, 3, c);
    }
    fb_draw_string(right_x + 20, panel_y + 238, "scheduler activity", C_MUTED, 1);
}

static void draw_terminal_text(int x, int y, int w, int h) {
    int cols = w / 8;
    int rows = h / 14;
    if (cols < 1 || rows < 1) return;
    int total_lines = 1;
    for (int i = 0; i < terminal_output_len; i++) if (terminal_output[i] == '\n') total_lines++;
    int skip = total_lines > rows ? total_lines - rows : 0;
    int line = 0, col = 0, draw_row = 0;
    char buf[128];
    int bl = 0;
    for (int i = 0; i <= terminal_output_len; i++) {
        char c = i == terminal_output_len ? '\n' : terminal_output[i];
        if (c == '\n' || col >= cols || bl >= 126) {
            if (line >= skip && draw_row < rows) {
                buf[bl] = 0;
                fb_draw_string(x, y + draw_row * 14, buf, C_TEXT, 1);
                draw_row++;
            }
            line++; col = 0; bl = 0;
            if (c == '\n') continue;
        }
        if (c >= 32 && c < 127) { buf[bl++] = c; col++; }
    }
}

static void draw_terminal(void) {
    int x = 258, y = 84;
    int w = (int)fb_width() - x - 30;
    int h = (int)fb_height() - y - 30;
    draw_soft_rect(x, y, w, h, 18, 0x080C14);
    fb_fill_rounded_rect(x + 18, y + 17, 12, 12, 6, C_RED);
    fb_fill_rounded_rect(x + 38, y + 17, 12, 12, 6, 0xFFD166);
    fb_fill_rounded_rect(x + 58, y + 17, 12, 12, 6, C_GREEN);
    fb_draw_string(x + 92, y + 20, "nova://terminal", C_MUTED, 1);
    fb_fill_rect(x + 18, y + 45, w - 36, 1, C_BORDER);
    draw_terminal_text(x + 22, y + 60, w - 44, h - 112);
    fb_fill_rounded_rect(x + 18, y + h - 46, w - 36, 30, 10, C_PANEL);
    fb_draw_string(x + 30, y + h - 36, ">", C_CYAN, 1);
    fb_draw_string(x + 48, y + h - 36, terminal_input, C_TEXT, 1);
    if ((pit_ticks() / 40) % 2 == 0)
        fb_fill_rect(x + 48 + fb_text_width(terminal_input, 1), y + h - 37, 7, 10, C_CYAN);
}

static void draw_system(void) {
    int x = 258, y = 86;
    int w = (int)fb_width() - x - 30;
    fb_draw_string(x, y, "System architecture", C_TEXT, 2);
    fb_draw_string(x, y + 30, "What is actually running, minus marketing fog.", C_MUTED, 1);

    static const char *labels[] = {"BOOT", "KERNEL", "MEMORY", "GUI", "NETWORK", "RUNTIME"};
    static const char *values[] = {"GRUB / MULTIBOOT2", "32-BIT FREESTANDING", "PAGING + HEAP", "NOVA COMPOSITOR", "TCP/IP + TLS", "ELF + JS + WASM"};
    static const uint32_t accents[] = {0xFF9F6E, C_CYAN, C_BLUE, C_PURPLE, C_GREEN, 0xFFD166};
    for (int i = 0; i < 6; i++) {
        int row = i / 2, col = i % 2;
        int cw = (w - 16) / 2;
        int cx = x + col * (cw + 16);
        int cy = y + 76 + row * 112;
        draw_soft_rect(cx, cy, cw, 94, 16, C_CARD);
        fb_fill_rounded_rect(cx + 18, cy + 18, 8, 58, 4, accents[i]);
        fb_draw_string(cx + 44, cy + 22, labels[i], C_MUTED, 1);
        fb_draw_string(cx + 44, cy + 50, values[i], C_TEXT, 1);
    }
}

static void draw_launcher(void) {
    int w = (int)fb_width();
    int h = (int)fb_height();
    int lw = 520, lh = 300;
    int x = (w - lw) / 2, y = (h - lh) / 2;
    for (int yy = 0; yy < h; yy += 3)
        for (int xx = 0; xx < w; xx += 3)
            fb_blend_pixel(xx, yy, 0x000000, 90);
    draw_soft_rect(x, y, lw, lh, 22, C_PANEL);
    fb_draw_string(x + 28, y + 24, "COMMAND PALETTE", C_MUTED, 1);
    fb_draw_string(x + 28, y + 50, "Where should we go?", C_TEXT, 2);
    const char *names[] = {"Overview", "Terminal", "System"};
    const char *keys[] = {"1", "2", "3"};
    for (int i = 0; i < 3; i++) {
        int iy = y + 102 + i * 56;
        fb_fill_rounded_rect(x + 24, iy, lw - 48, 44, 12, i == active_view ? C_PANEL_2 : C_CARD);
        fb_fill_rounded_rect(x + 38, iy + 10, 24, 24, 7, i == 0 ? C_CYAN : i == 1 ? C_PURPLE : C_GREEN);
        fb_draw_string(x + 46, iy + 18, keys[i], C_BG0, 1);
        fb_draw_string(x + 82, iy + 17, names[i], C_TEXT, 1);
    }
}

static void draw_cursor(int x, int y) {
    fb_fill_triangle(x, y, x, y + 22, x + 15, y + 15, C_TEXT);
    fb_draw_line(x, y, x, y + 22, C_BG0);
    fb_draw_line(x, y, x + 15, y + 15, C_BG0);
}

static void execute_terminal(void) {
    append_text("\n> ");
    append_text(terminal_input);
    append_text("\n");
    terminal_pid = shell_execute(terminal_input, &terminal_cwd, append_text);
    terminal_input_len = 0;
    terminal_input[0] = 0;
}

static void handle_key(char c) {
    if (launcher_open) {
        if (c == '1' || c == '2' || c == '3') {
            active_view = c - '1';
            launcher_open = 0;
        } else if (c == 27 || c == '`') launcher_open = 0;
        return;
    }
    if (c == '`') { launcher_open = 1; return; }
    if (c == '1' && active_view != 1) { active_view = 0; return; }
    if (c == '2' && active_view != 1) { active_view = 1; return; }
    if (c == '3' && active_view != 1) { active_view = 2; return; }
    if (active_view != 1) return;
    if (c == '\n' || c == '\r') execute_terminal();
    else if (c == '\b') {
        if (terminal_input_len > 0) terminal_input[--terminal_input_len] = 0;
    } else if (c >= 32 && c < 127 && terminal_input_len < MAX_INPUT - 1) {
        terminal_input[terminal_input_len++] = c;
        terminal_input[terminal_input_len] = 0;
    }
}

static void handle_click(int mx, int my) {
    if (inside(mx, my, 14, PANEL_H + 52, 196, 42)) active_view = 0;
    else if (inside(mx, my, 14, PANEL_H + 102, 196, 42)) active_view = 1;
    else if (inside(mx, my, 14, PANEL_H + 152, 196, 42)) active_view = 2;

    if (active_view == 0) {
        int x = 258;
        int panel_y = 286;
        if (inside(mx, my, x + 22, panel_y + 162, 154, 42)) active_view = 1;
        else if (inside(mx, my, x + 188, panel_y + 162, 134, 42)) active_view = 2;
    }
}

void gui_init(void) {
    mouse_set_bounds((int)fb_width(), (int)fb_height());
    terminal_cwd = fat32_is_mounted() ? fat32_root_cluster() : 0;
    strcpy(terminal_output,
        "ZapOS Nova Shell\n"
        "A redesigned native workspace.\n"
        "Type 'help' to inspect the underlying system.\n");
    terminal_output_len = (int)strlen(terminal_output);
}

void gui_run(void) {
    for (;;) {
        if (fullscreen_active) {
            if (scheduler_task_state(fullscreen_owner) == TASK_TERMINATED) {
                fullscreen_active = 0;
                fullscreen_owner = -1;
            } else {
                fb_fill_rect(0, 0, (int)fb_width(), (int)fb_height(), 0x000000);
                fb_blit_rgb(0, 0, (int)fb_width(), (int)fb_height(), fullscreen_buffer, DOOM_BLIT_W, DOOM_BLIT_H);
                fb_swap_buffers();
                pit_sleep(10);
                continue;
            }
        }

        int mx, my; uint8_t buttons;
        mouse_get_state(&mx, &my, &buttons);
        int left = (buttons & MOUSE_LEFT_BUTTON) != 0;
        if (left && !previous_left) handle_click(mx, my);
        previous_left = left;

        char c;
        while ((c = keyboard_getchar()) != 0) handle_key(c);

        draw_wallpaper();
        draw_topbar();
        draw_sidebar();
        if (active_view == 0) draw_overview();
        else if (active_view == 1) draw_terminal();
        else draw_system();
        if (launcher_open) draw_launcher();
        draw_cursor(mx, my);
        fb_swap_buffers();
        pit_sleep(10);
    }
}
