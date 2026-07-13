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
#include <net/dns.h>
#include <net/http.h>
#include <net/dom.h>
#include <net/css.h>
#include <net/layout.h>
#include <net/bmp.h>
#include <fs/fat32.h>
#include <drivers/ac97.h>
#include <drivers/wav.h>
#include <js/js.h>
#include <js/dom_binding.h>
#include <kernel/kheap.h>
#include <kernel/elf.h>
#include <string.h>

#define MAX_WINDOWS   8
#define TITLEBAR_H    28
#define TASKBAR_H     44
#define FM_ROW_H      16
#define FM_MAX_ENTRIES 24
#define FM_PREVIEW_MAX 2048
#define FM_AUDIO_MAX   (2 * 1024 * 1024)
#define BR_MAX_URL     96
/* Well under http.c's own HTTP_RAW_BUF_SIZE (2MB) -- both have to fit
 * in the 8MB kheap arena at once, alongside anything else already
 * resident (e.g. a WAV file the File Manager has open). */
#define BR_FETCH_CAP   (2u * 1024 * 1024 - 65536)
#define BR_CONTENT_TYPE_MAX 64
#define BR_MAX_IMAGES     8
#define BR_IMAGE_FETCH_CAP (300u * 1024)
/* Real minified CSS from a modern build pipeline (Tailwind, Next.js,
 * etc.) routinely runs 40-60KB for one bundle -- 32KB used to silently
 * truncate a chunk of the sheet (safely, just losing whatever rules
 * fell past the cut, not corrupting anything earlier). */
#define BR_CSS_FETCH_CAP   (128u * 1024)

typedef struct {
    int x, y, w, h;
    char title[32];
    char icon_label[5];
    const char *body_line1;
    const char *body_line2;
    uint32_t accent;
    int is_process_monitor;
    int is_network;
    int is_file_manager;
    int is_browser;
    int open;
} gui_window_t;

static gui_window_t windows[MAX_WINDOWS];
static int window_order[MAX_WINDOWS];
static int window_count = 0;

static int dragging_window = -1;
static int drag_dx = 0, drag_dy = 0;
static int prev_left = 0;

/* Fullscreen takeover, for SYS_BLIT (see kernel/syscall.c): a program
 * with its own isolated address space (DOOM, specifically) can claim
 * the whole display instead of drawing into a compositor window --
 * closer to how real DOS/console games actually worked than trying to
 * retrofit them into a windowed desktop. Only one at a time; a second
 * blit from a different pid than the current owner just takes over
 * (the desktop resumes on its own once the owning task's state is
 * TASK_TERMINATED, checked once per frame -- see gui_run()). */
static uint32_t fs_buffer[DOOM_BLIT_W * DOOM_BLIT_H];
static int fs_active = 0;
static int fs_owner_pid = -1;

void gui_blit_fullscreen(const uint32_t *pixels, int owner_pid) {
    memcpy(fs_buffer, pixels, sizeof(fs_buffer));
    fs_active = 1;
    fs_owner_pid = owner_pid;
}

#define TASKBAR_ICON_W 44
#define TASKBAR_ICON_H 32
#define TASKBAR_ICON_GAP 6
#define TASKBAR_ICONS_X 104

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
static char fm_status_msg[48] = "";
static uint32_t fm_status_until = 0;

/* Audio preview state -- set when the currently open file is a .WAV.
 * fm_audio_buf must stay alive for as long as playback could still be
 * reading from it via DMA, so it's only ever freed through
 * fm_close_audio(), which stops playback first. */
static int fm_is_audio = 0;
static uint8_t *fm_audio_buf = NULL;
static struct wav_info fm_wav;

static void fm_refresh(void);

/* Browser state -- a single instance, one page loaded at a time. Pages
 * are rendered with a real (if pragmatic) CSS box-model layout: fetch
 * -> dom_parse -> css_extract_style_blocks -> (run inline <script>s,
 * which may mutate the DOM before it's ever drawn) -> layout_run -> a
 * flat list of positioned, styled render items in br_layout, which is
 * what draw_browser() and the link/onclick click hit-tests actually
 * walk. Unlike the old reader-mode renderer, the DOM tree and
 * stylesheet are kept alive for as long as the page is loaded (not
 * freed right after the first layout) so a JS onclick handler can
 * mutate the tree and trigger a br_relayout() -- both are only torn
 * down right before the next page replaces them. */
static char br_url[BR_MAX_URL] = "example.com/";
static int br_url_len = 12;
static int br_editing_url = 0;
static int br_scroll = 0;
static char br_status_msg[64] = "Type a URL and press Enter";
static struct layout_doc br_layout;
static int br_window_idx = -1;
static struct dom_node *br_dom_root = NULL;
static struct css_stylesheet br_stylesheet;
static int br_stylesheet_valid = 0;

static int add_window(int x, int y, int w, int h, const char *title,
                       const char *icon_label,
                       const char *l1, const char *l2, uint32_t accent) {
    int idx = window_count;
    gui_window_t *win = &windows[idx];
    win->x = x; win->y = y; win->w = w; win->h = h;
    strcpy(win->title, title);
    strncpy(win->icon_label, icon_label, sizeof(win->icon_label) - 1);
    win->icon_label[sizeof(win->icon_label) - 1] = 0;
    win->body_line1 = l1;
    win->body_line2 = l2;
    win->accent = accent;
    win->is_process_monitor = 0;
    win->is_network = 0;
    win->is_file_manager = 0;
    win->is_browser = 0;
    win->open = 1;
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

/* Opens (if closed) and raises the window at windows[wi] -- used by the
 * taskbar launcher, which addresses windows by their fixed slot index
 * rather than by their current stacking position. */
static void open_and_focus(int wi) {
    windows[wi].open = 1;
    for (int i = 0; i < window_count; i++) {
        if (window_order[i] == wi) { bring_to_front(i); break; }
    }
}

/* Window x/y positions below are designed against a 1024x768 desktop;
 * on a bigger framebuffer (see boot/multiboot.asm's resolution request)
 * they'd otherwise all stay clustered in the top-left corner of a much
 * larger canvas. Scale positions (not sizes -- window bodies are already
 * sized to fit their content, no reason to stretch them) up to match,
 * but never shrink below the design baseline. */
static int SX(int v) {
    int fw = (int)fb_width();
    return fw <= 1024 ? v : v * fw / 1024;
}
static int SY(int v) {
    int fh = (int)fb_height();
    return fh <= 768 ? v : v * fh / 768;
}

void gui_init(void) {
    add_window(SX(120), SY(90), 340, 190, "About ZapOS", "ABT",
               "A fully custom 32-bit OS kernel",
               "GUI + drivers written from scratch", 0x3E6FF0);
    add_window(SX(560), SY(160), 300, 170, "System Monitor", "SYS",
               "Kernel heap + paging: online",
               "PS/2 keyboard + mouse: online", 0x2FBF71);
    add_window(SX(260), SY(340), 320, 150, "Roadmap", "MAP",
               "Next up: process isolation + a filesystem",
               "See README.md for the plan", 0xE0954C);

    int pm = add_window(SX(640), SY(420), 320, 190, "Process Monitor", "PROC", NULL, NULL, 0xB05CE0);
    windows[pm].is_process_monitor = 1;

    int net = add_window(SX(120), SY(460), 340, 190, "Network", "NET", NULL, NULL, 0x3ED0D8);
    windows[net].is_network = 1;

    if (fat32_is_mounted()) {
        int fm = add_window(SX(480), SY(560), 380, 220, "File Manager", "FILE", NULL, NULL, 0xF2C14E);
        windows[fm].is_file_manager = 1;
        fm_current_dir = fat32_root_cluster();
        fm_refresh();
    }

    if (net_is_up()) {
        int br = add_window(SX(600), SY(60), 500, 500, "Browser", "WWW", NULL, NULL, 0x62D8FF);
        windows[br].is_browser = 1;
        br_window_idx = br;
        layout_doc_alloc(&br_layout);
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
    char namebuf[16];
    strcpy(namebuf, net_get_driver_name());
    int nlen = (int)strlen(namebuf);
    while (nlen < 9 && nlen < (int)sizeof(namebuf) - 1) namebuf[nlen++] = ' ';
    namebuf[nlen] = 0;
    fb_draw_string(x, y, namebuf, COL_MUTED, 1);
    fb_draw_string(x + fb_text_width(namebuf, 1), y, macbuf, COL_TEXT, 1);

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

/* Stops any in-flight playback *before* freeing the buffer it's DMAing
 * from -- ac97_play_pcm() reads directly out of fm_audio_buf, so
 * freeing it while a clip is still playing would let the controller
 * keep reading freed memory. */
static void fm_close_audio(void) {
    if (fm_audio_buf) {
        ac97_stop();
        kfree(fm_audio_buf);
        fm_audio_buf = NULL;
    }
    fm_is_audio = 0;
}

static void fm_refresh(void) {
    fm_close_audio();
    fm_entry_count = fat32_list_dir(fm_current_dir, fm_entries, FM_MAX_ENTRIES);
    fm_viewing_file = 0;
    fm_editing = 0;
    fm_dirty = 0;
}

static int fm_is_notes_txt(const char *name) {
    return strcmp(name, "NOTES.TXT") == 0;
}

static int fm_has_ext(const char *name, const char *ext) {
    int nlen = (int)strlen(name), elen = (int)strlen(ext);
    if (nlen < elen + 1 || name[nlen - elen - 1] != '.') return 0;
    for (int i = 0; i < elen; i++) {
        char a = name[nlen - elen + i], b = ext[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Called when a row in the listing is clicked: navigate into directories,
 * open an audio-preview screen for .WAV files, open a read-only preview
 * for other files, or an editable one for the demo's NOTES.TXT (the
 * only file fat32_write_file knows how to save). */
static void fm_open_entry(int index) {
    if (index < 0 || index >= fm_entry_count) return;
    struct fat_dirent_info *e = &fm_entries[index];

    if (e->is_dir) {
        if (strcmp(e->name, ".") == 0) return;
        fm_current_dir = (e->cluster < 2) ? fat32_root_cluster() : e->cluster;
        fm_refresh();
        return;
    }

    fm_close_audio();
    strcpy(fm_preview_name, e->name);

    if (fm_has_ext(e->name, "ELF")) {
        uint8_t *buf = (uint8_t *)kmalloc(e->size > 0 ? e->size : 1);
        uint32_t got = buf ? fat32_read_file(e->cluster, e->size, buf, e->size) : 0;
        int pid = buf ? elf_load_and_run(buf, got) : -1;
        if (buf) kfree(buf);
        if (pid >= 0) {
            char numbuf[12];
            strcpy(fm_status_msg, "Launched as pid ");
            utoa((unsigned int)pid, numbuf);
            strcat(fm_status_msg, numbuf);
        } else {
            strcpy(fm_status_msg, "Failed to launch (see serial log)");
        }
        fm_status_until = pit_ticks() + 300;
        return;
    }

    if (fm_has_ext(e->name, "WAV")) {
        uint32_t cap = e->size < FM_AUDIO_MAX ? e->size : FM_AUDIO_MAX;
        fm_audio_buf = (uint8_t *)kmalloc(cap > 0 ? cap : 1);
        uint32_t got = fm_audio_buf ? fat32_read_file(e->cluster, e->size, fm_audio_buf, cap) : 0;
        fm_is_audio = fm_audio_buf && wav_parse(fm_audio_buf, got, &fm_wav);
        if (fm_audio_buf && !fm_is_audio) { kfree(fm_audio_buf); fm_audio_buf = NULL; }
        fm_viewing_file = 1;
        fm_editing = 0;
        fm_dirty = 0;
        fm_status_msg[0] = 0;
        return;
    }

    fm_preview_len = fat32_read_file(e->cluster, e->size, fm_preview_buf, FM_PREVIEW_MAX);
    fm_preview_buf[fm_preview_len] = 0;
    fm_viewing_file = 1;
    fm_editing = fm_is_notes_txt(e->name);
    fm_dirty = 0;
}

static void fm_play_audio(void) {
    if (!ac97_is_present()) {
        strcpy(fm_status_msg, "No audio device detected");
    } else if (fm_wav.bits_per_sample != 16 || fm_wav.sample_rate != 48000) {
        strcpy(fm_status_msg, "Unsupported format (need 16-bit/48000Hz)");
    } else if (!ac97_play_pcm(fm_wav.pcm, fm_wav.sample_count, fm_wav.channels == 2)) {
        strcpy(fm_status_msg, "Clip too long to play (max ~21s)");
    } else {
        strcpy(fm_status_msg, "Playing... (press S to stop)");
    }
    fm_status_until = pit_ticks() + 300;
}

static void fm_save_notes(void) {
    int ok = fat32_write_file(fm_current_dir, "NOTES.TXT", fm_preview_buf, fm_preview_len);
    strcpy(fm_status_msg, ok ? "Saved -- persists across reboot" : "Save failed");
    fm_status_until = pit_ticks() + 200;
    fm_dirty = 0;
}

/* Feeds typed characters into the open NOTES.TXT buffer (Enter saves),
 * or -- while an audio file is open -- handles P/S for play/stop.
 * Called from gui_run() unconditionally on every keypress. */
static void fm_handle_key(char c) {
    if (fm_viewing_file && fm_is_audio) {
        if (c == 'p' || c == 'P') fm_play_audio();
        else if (c == 's' || c == 'S') {
            ac97_stop();
            strcpy(fm_status_msg, "Stopped");
            fm_status_until = pit_ticks() + 150;
        }
        return;
    }
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

/* Wraps `text` to fit max_width pixels, drawing each wrapped row starting
 * at (x,y). Returns how many rows it actually drew (0 for empty text),
 * so callers stacking multiple wrapped blocks know how far to advance. */
static int draw_wrapped_text(int x, int y, int max_width, int max_rows, const char *text, uint32_t color) {
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
        row++;
    }
    return row;
}

static void fm_handle_click(const gui_window_t *w, int my) {
    if (!fat32_is_mounted()) return;
    int rel_y = my - (w->y + TITLEBAR_H + 12);

    if (fm_viewing_file) {
        if (rel_y >= 0 && rel_y < FM_ROW_H) { /* "<- back" row */
            fm_close_audio();
            fm_viewing_file = 0;
            fm_editing = 0;
        }
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

        if (fm_is_audio) {
            char info[64], numbuf[12];
            strcpy(info, "");
            utoa(fm_wav.sample_rate, numbuf);
            strcat(info, numbuf);
            strcat(info, " Hz, ");
            utoa((unsigned int)fm_wav.bits_per_sample, numbuf);
            strcat(info, numbuf);
            strcat(info, "-bit, ");
            strcat(info, fm_wav.channels == 2 ? "stereo" : "mono");
            fb_draw_string(x, y + 34, info, COL_MUTED, 1);
            fb_draw_string(x, y + 52, "Press P to play, S to stop", COL_MUTED, 1);
            if (pit_ticks() < fm_status_until) {
                fb_draw_string(x, y + 70, fm_status_msg, 0x8FE3A8, 1);
            }
            return;
        }

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

    char header[48], cntbuf[12];
    strcpy(header, fm_current_dir == fat32_root_cluster() ? "/  " : "(subfolder)  ");
    utoa((unsigned int)fm_entry_count, cntbuf);
    strcat(header, cntbuf);
    strcat(header, fm_entry_count == 1 ? " item" : " items");
    fb_draw_string(x, y, header, COL_MUTED, 1);

    int max_rows = (w->h - TITLEBAR_H - 30) / FM_ROW_H;
    for (int i = 0; i < fm_entry_count && i < max_rows; i++) {
        char line[40];
        uint32_t color;
        if (fm_entries[i].is_dir) {
            strcpy(line, "[DIR] ");
            strcat(line, fm_entries[i].name);
            color = 0x62D8FF;
        } else {
            char sizebuf[12];
            utoa(fm_entries[i].size, sizebuf);
            if (fm_has_ext(fm_entries[i].name, "WAV")) {
                strcpy(line, "[WAV] ");
                color = 0x8FE3A8;
            } else if (fm_has_ext(fm_entries[i].name, "ELF")) {
                strcpy(line, "[ELF] ");
                color = 0xE0B85C;
            } else {
                line[0] = 0;
                color = COL_TEXT;
            }
            strcat(line, fm_entries[i].name);
            strcat(line, "  ");
            strcat(line, sizebuf);
            strcat(line, "B");
        }
        fb_draw_string(x, y + 18 + i * FM_ROW_H, line, color, 1);
    }
    if (pit_ticks() < fm_status_until) {
        fb_draw_string(x, w->y + w->h - 18, fm_status_msg, 0x8FE3A8, 1);
    }
}

/* Splits "example.com:8000/path" (an optional "http://" prefix is
 * skipped) into a host, a port (defaulting to 80), and a path
 * (defaulting to "/"). */
static void br_parse_url(const char *url, char *host_out, int host_cap, uint16_t *port_out,
                          char *path_out, int path_cap) {
    const char *p = url;
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' &&
        p[4] == ':' && p[5] == '/' && p[6] == '/') {
        p += 7;
    }
    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < host_cap - 1) host_out[i++] = *p++;
    host_out[i] = 0;

    *port_out = 80;
    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
        if (port > 0 && port < 65536) *port_out = (uint16_t)port;
    }

    if (*p == '/') {
        int j = 0;
        while (*p && j < path_cap - 1) path_out[j++] = *p++;
        path_out[j] = 0;
    } else {
        strcpy(path_out, "/");
    }
}

/* Content-area geometry shared by drawing, scrolling, and link hit-
 * testing, so they can never disagree with each other. */
static void br_content_area(int *x, int *y, int *w, int *h) {
    const gui_window_t *win = &windows[br_window_idx];
    *x = win->x + 10;
    *y = win->y + TITLEBAR_H + 8 + 46;
    *w = win->w - 20;
    *h = win->y + win->h - *y - 8;
    if (*h < 0) *h = 0;
}

static int ct_contains(const char *content_type, const char *needle) {
    int nlen = (int)strlen(needle);
    for (const char *p = content_type; *p; p++) {
        int i = 0;
        while (i < nlen && p[i] && (p[i] | 0x20) == (needle[i] | 0x20)) i++;
        if (i == nlen) return 1;
    }
    return 0;
}

/* Builds an uppercased 8.3-ish filename for a download out of the URL
 * path's last segment (e.g. "/music/song.wav" -> "SONG.WAV"), falling
 * back to a generic name if the path has no real filename in it (a
 * bare "/", or one ending in "/"). Query strings are stripped. */
static void br_derive_filename(const char *path, char *out, int out_cap) {
    const char *last_slash = path;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p + 1;

    char tmp[64];
    int i = 0;
    while (last_slash[i] && last_slash[i] != '?' && i < (int)sizeof(tmp) - 1) {
        tmp[i] = last_slash[i];
        i++;
    }
    tmp[i] = 0;

    if (tmp[0] == 0) strcpy(tmp, "DOWNLOAD.BIN");
    for (int j = 0; tmp[j]; j++) {
        if (tmp[j] >= 'a' && tmp[j] <= 'z') tmp[j] = (char)(tmp[j] - 32);
    }
    strncpy(out, tmp, out_cap - 1);
    out[out_cap - 1] = 0;
}

/* Joins `href` against `base_path`'s directory (unless `href` is itself
 * root-relative, i.e. starts with '/'). Shared by br_resolve_href (for
 * <a> navigation) and br_resolve_subresource (for <link>/<img>), so the
 * two can never disagree about what a relative URL means. */
static void br_join_path(const char *base_path, const char *href, char *out, int out_cap) {
    if (href[0] == '/') {
        strncpy(out, href, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }
    char dir[64];
    strncpy(dir, base_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *found = NULL;
    for (char *p = dir; *p; p++) if (*p == '/') found = p;
    if (found) found[1] = 0;
    else strcpy(dir, "/");

    strncpy(out, dir, out_cap - 1);
    out[out_cap - 1] = 0;
    int dl = (int)strlen(out);
    int hl = (int)strlen(href);
    if (dl + hl < out_cap) strcpy(out + dl, href);
}

/* Resolves a sub-resource reference (a <link href> or <img src>, as
 * opposed to an <a href> the user actually navigates to) against the
 * page that referenced it, without touching the browser's own
 * navigation state (br_url/br_status_msg) -- a missing image shouldn't
 * clobber the address bar or status line. Returns 0 for schemes this
 * browser can't fetch (https:, data:, empty) rather than 1 with a
 * nonsense host/path. */
static int br_resolve_subresource(const char *base_host, uint16_t base_port, const char *base_path,
                                   const char *url, char *host_out, int host_cap,
                                   uint16_t *port_out, char *path_out, int path_cap) {
    if (!url[0] || url[0] == '#') return 0;
    if (strncmp(url, "https://", 8) == 0 || strncmp(url, "data:", 5) == 0) return 0;
    if (strncmp(url, "http://", 7) == 0) {
        br_parse_url(url, host_out, host_cap, port_out, path_out, path_cap);
        return 1;
    }
    strncpy(host_out, base_host, host_cap - 1);
    host_out[host_cap - 1] = 0;
    *port_out = base_port;
    br_join_path(base_path, url, path_out, path_cap);
    return 1;
}

/* Decoded <img> cache for the currently displayed page -- keyed by the
 * DOM node so layout_get_image() (called from net/layout.c while laying
 * out that exact <img> element) can look its pixels back up. Filled by
 * br_load_subresources() right after the DOM parses and before the
 * first layout, since layout needs each image's natural size to
 * reserve space for it. Freed and reset on every navigation. */
struct br_image_slot {
    const struct dom_node *node;
    struct bmp_image img;
};
static struct br_image_slot br_images[BR_MAX_IMAGES];
static int br_image_count = 0;

int layout_get_image(const struct dom_node *node, int *out_w, int *out_h, const uint32_t **out_pixels) {
    for (int i = 0; i < br_image_count; i++) {
        if (br_images[i].node == node) {
            *out_w = br_images[i].img.width;
            *out_h = br_images[i].img.height;
            *out_pixels = br_images[i].img.pixels;
            return 1;
        }
    }
    return 0;
}

static void br_images_reset(void) {
    for (int i = 0; i < br_image_count; i++) bmp_free(&br_images[i].img);
    br_image_count = 0;
}

/* Walks the DOM fetching every <link rel="stylesheet"> and <img> it
 * finds, one HTTP request at a time (this browser only ever has one TCP
 * connection open at once) -- external CSS is parsed straight into the
 * page's stylesheet, images are decoded into br_images[] for
 * layout_get_image() to find. Best-effort: a failed/unsupported
 * sub-resource is silently skipped rather than aborting the page, same
 * as a real browser would just show a broken-image icon and move on. */
static void br_load_subresources(struct dom_node *node, const char *base_host, uint16_t base_port,
                                  const char *base_path) {
    for (struct dom_node *child = node->children; child; child = child->next) {
        if (child->type != DOM_ELEMENT) continue;

        if (strcmp(child->tag, "link") == 0 && strcmp(child->rel, "stylesheet") == 0 && child->href[0]) {
            char host[64], path[64]; uint16_t port;
            if (br_resolve_subresource(base_host, base_port, base_path, child->href,
                                        host, sizeof(host), &port, path, sizeof(path))) {
                char *buf = kmalloc(BR_CSS_FETCH_CAP + 1);
                if (buf) {
                    int status; uint32_t blen;
                    if (http_get(host, port, path, &status, buf, BR_CSS_FETCH_CAP, &blen, NULL, 0) &&
                        status >= 200 && status < 300) {
                        css_parse_into(&br_stylesheet, buf, blen);
                    }
                    kfree(buf);
                }
            }
        } else if (strcmp(child->tag, "img") == 0 && child->href[0] && br_image_count < BR_MAX_IMAGES) {
            char host[64], path[64]; uint16_t port;
            if (br_resolve_subresource(base_host, base_port, base_path, child->href,
                                        host, sizeof(host), &port, path, sizeof(path))) {
                char *buf = kmalloc(BR_IMAGE_FETCH_CAP + 1);
                if (buf) {
                    int status; uint32_t blen;
                    if (http_get(host, port, path, &status, buf, BR_IMAGE_FETCH_CAP, &blen, NULL, 0) &&
                        status >= 200 && status < 300) {
                        struct bmp_image img;
                        if (bmp_decode((const uint8_t *)buf, blen, &img)) {
                            br_images[br_image_count].node = child;
                            br_images[br_image_count].img = img;
                            br_image_count++;
                        }
                    }
                    kfree(buf);
                }
            }
        }

        br_load_subresources(child, base_host, base_port, base_path);
    }
}

/* Runs synchronously on the GUI's own task -- the screen won't redraw
 * until this returns (a few seconds for a small page, longer for a
 * multi-MB download). A real async fetch would need a dedicated task
 * and a way to hand the result back; out of scope for this pass.
 *
 * HTML responses (by Content-Type, or a path ending in "/"/.htm/.html
 * when the server sent no Content-Type at all) go through the usual
 * DOM/CSS/layout pipeline. Everything else -- audio, video, archives,
 * anything -- is saved to the FAT32 disk's root directory instead of
 * being rendered, so it shows up in the File Manager afterward. There
 * is no decoding of any kind: a saved .mkv or .mp3 is just bytes on
 * disk, not something this OS can play (WAV is the only playable audio
 * format, via the File Manager -- see fm_play_audio()). */
static void br_fetch(void) {
    char host[64], path[64];
    uint16_t port;
    br_parse_url(br_url, host, sizeof(host), &port, path, sizeof(path));

    char *body = (char *)kmalloc(BR_FETCH_CAP + 1);
    if (!body) {
        strcpy(br_status_msg, "Out of memory");
        return;
    }

    int status;
    uint32_t body_len;
    char content_type[BR_CONTENT_TYPE_MAX];

    if (!http_get(host, port, path, &status, body, BR_FETCH_CAP, &body_len,
                   content_type, sizeof(content_type))) {
        strcpy(br_status_msg, "Failed to load (DNS/TCP error)");
        br_layout.item_count = 0;
        br_layout.link_count = 0;
        kfree(body);
        return;
    }
    body[body_len] = 0;

    int path_len = (int)strlen(path);
    int looks_like_page = path_len == 0 || path[path_len - 1] == '/' ||
                           (path_len > 5 && strcmp(path + path_len - 5, ".html") == 0) ||
                           (path_len > 4 && strcmp(path + path_len - 4, ".htm") == 0);
    int is_html = content_type[0] ? ct_contains(content_type, "text/html") : looks_like_page;

    if (is_html) {
        /* Tear down the previous page's DOM/stylesheet/JS state before
         * building the new one -- all three only need to live as long
         * as the page that owns them is displayed. */
        if (br_dom_root) { dom_free(br_dom_root); br_dom_root = NULL; }
        if (br_stylesheet_valid) { css_stylesheet_free(&br_stylesheet); br_stylesheet_valid = 0; }
        br_images_reset();
        js_arena_reset();
        js_dom_reset();

        char title[DOM_MAX_TITLE];
        br_dom_root = dom_parse(body, body_len, title, sizeof(title));

        css_stylesheet_init(&br_stylesheet);
        css_extract_style_blocks(&br_stylesheet, body, body_len);
        br_stylesheet_valid = 1;

        /* External <link rel=stylesheet> and <img> both need their own
         * HTTP fetch, done here (before layout, after DOM/inline-CSS)
         * so the external rules are in the cascade and every image's
         * natural size is known by the time layout_run() needs it. */
        br_load_subresources(br_dom_root, host, port, path);

        /* Custom-property (var()) resolution needs the FINAL stylesheet
         * -- including whatever external sheets br_load_subresources()
         * just appended, since a page's own theme variables commonly
         * live in one of those rather than an inline <style> block --
         * so this runs after subresources load and before anything
         * else reads a computed style. */
        css_resolve_custom_properties(&br_stylesheet);

        /* Scripts run before the first layout so DOM mutations they
         * make (innerHTML, textContent, style) show up immediately
         * rather than requiring a second pass. */
        struct js_env *global_env = js_make_global_env(br_dom_root);
        js_run_inline_scripts(body, body_len, global_env);
        js_dom_clear_relayout_flag();

        int content_x, content_y, content_w, content_h;
        br_content_area(&content_x, &content_y, &content_w, &content_h);
        layout_run(br_dom_root, &br_stylesheet, content_w, &br_layout);
        strncpy(br_layout.title, title, DOM_MAX_TITLE - 1);

        kfree(body);
        br_scroll = 0;

        char numbuf[12];
        utoa((unsigned int)status, numbuf);
        strcpy(br_status_msg, status >= 200 && status < 300 ? "OK " : "HTTP ");
        strcat(br_status_msg, numbuf);
        return;
    }

    char fname[32];
    br_derive_filename(path, fname, sizeof(fname));

    if (!fat32_is_mounted()) {
        strcpy(br_status_msg, "Fetched, but no disk to save it to");
    } else if (fat32_write_file(fat32_root_cluster(), fname, body, body_len)) {
        char numbuf[16];
        utoa(body_len, numbuf);
        strcpy(br_status_msg, "Downloaded ");
        strcat(br_status_msg, fname);
        strcat(br_status_msg, " (");
        strcat(br_status_msg, numbuf);
        strcat(br_status_msg, "B)");
        fm_refresh();
    } else {
        strcpy(br_status_msg, "Download failed (disk full?)");
    }
    kfree(body);
}

/* Resolves `href` (as found on an <a> in the just-loaded page) against
 * the current br_url, writes the resolved absolute "host[:port]/path"
 * into br_url, and returns 1 -- or returns 0 (no navigation) for
 * fragment-only/mailto:/javascript: links and for https: links, which
 * this browser can't fetch (no TLS client). */
static int br_resolve_href(const char *href) {
    if (href[0] == '#' || href[0] == 0) return 0;
    if (strncmp(href, "mailto:", 7) == 0 || strncmp(href, "javascript:", 11) == 0) return 0;
    if (strncmp(href, "https://", 8) == 0) {
        strcpy(br_status_msg, "HTTPS not supported (no TLS client)");
        return 0;
    }

    if (strncmp(href, "http://", 7) == 0) {
        strncpy(br_url, href, BR_MAX_URL - 1);
        br_url[BR_MAX_URL - 1] = 0;
        br_url_len = (int)strlen(br_url);
        return 1;
    }

    char host[64], path[64];
    uint16_t port;
    br_parse_url(br_url, host, sizeof(host), &port, path, sizeof(path));

    char new_path[64];
    br_join_path(path, href, new_path, sizeof(new_path));

    int n = 0;
    n += (int)strlen(host);
    strncpy(br_url, host, BR_MAX_URL - 1);
    if (port != 80 && n < BR_MAX_URL - 8) {
        char portbuf[8];
        utoa(port, portbuf);
        br_url[n++] = ':';
        int pl = (int)strlen(portbuf);
        if (n + pl < BR_MAX_URL) { memcpy(br_url + n, portbuf, (size_t)pl); n += pl; }
    }
    int pl = (int)strlen(new_path);
    if (n + pl < BR_MAX_URL) { memcpy(br_url + n, new_path, (size_t)pl); n += pl; }
    br_url[n] = 0;
    br_url_len = n;
    return 1;
}

static void br_navigate(const char *href) {
    if (br_resolve_href(href)) br_fetch();
}

/* Re-runs layout against the (possibly JS-mutated) live DOM tree and
 * stylesheet -- called after an onclick handler changes innerHTML,
 * textContent, or style. Does not touch scroll position or the DOM
 * tree/stylesheet themselves. */
static void br_relayout(void) {
    if (!br_dom_root || !br_stylesheet_valid) return;
    int content_x, content_y, content_w, content_h;
    br_content_area(&content_x, &content_y, &content_w, &content_h);
    layout_run(br_dom_root, &br_stylesheet, content_w, &br_layout);
}

static void br_handle_click(const gui_window_t *w, int mx, int my) {
    int rel_y = my - (w->y + TITLEBAR_H + 8);
    br_editing_url = (rel_y >= 0 && rel_y < 20);
    if (br_editing_url) return;

    int content_x, content_y, content_w, content_h;
    br_content_area(&content_x, &content_y, &content_w, &content_h);
    if (mx < content_x || my < content_y) return;

    int doc_x = mx - content_x;
    int doc_y = (my - content_y) + br_scroll;

    for (int i = 0; i < br_layout.item_count; i++) {
        const struct layout_item *it = &br_layout.items[i];
        if (it->type != LAYOUT_ITEM_TEXT && it->type != LAYOUT_ITEM_IMAGE) continue;
        if (doc_x < it->x || doc_x >= it->x + it->w || doc_y < it->y || doc_y >= it->y + it->h) continue;

        if (it->link_id >= 0) {
            br_navigate(br_layout.links[it->link_id].href);
            return;
        }
        if (it->owner && js_dom_dispatch_click((struct dom_node *)it->owner)) {
            if (js_dom_needs_relayout()) {
                br_relayout();
                js_dom_clear_relayout_flag();
            }
            return;
        }
    }
}

static void br_handle_key(char c) {
    if (!br_editing_url) return;
    if (c == '\n' || c == '\r') {
        br_fetch();
    } else if (c == '\b') {
        if (br_url_len > 0) br_url_len--;
    } else if (c >= 32 && c < 127 && br_url_len < BR_MAX_URL - 1) {
        br_url[br_url_len++] = c;
    }
    br_url[br_url_len] = 0;
}

static void draw_browser(const gui_window_t *w) {
    int x = w->x + 10;
    int y = w->y + TITLEBAR_H + 8;
    int inner_w = w->w - 20;

    fb_fill_rect(x, y, inner_w, 20, 0x0D131C);
    fb_draw_rect(x, y, inner_w, 20, br_editing_url ? 0x62D8FF : 0x3A4270);
    fb_draw_string(x + 4, y + 6, br_url, COL_TEXT, 1);

    fb_draw_string(x, y + 26, br_status_msg, COL_MUTED, 1);

    if (!net_is_up()) {
        fb_draw_string(x, y + 44, "no NIC detected", 0xE05252, 1);
        return;
    }

    int content_x, content_y, content_w, content_h;
    br_content_area(&content_x, &content_y, &content_w, &content_h);

    /* A real browser's default page canvas is light with black text --
     * pages that never set their own background (the common case) rely
     * on that default for contrast against their (also-defaulted)
     * black text. Filled first so any background a page DOES set (via
     * body/html {background-color: ...}) paints over it. */
    fb_fill_rect(content_x, content_y, content_w, content_h, 0xF4F4F6);

    int max_scroll = br_layout.content_height - content_h;
    if (max_scroll < 0) max_scroll = 0;
    if (br_scroll > max_scroll) br_scroll = max_scroll;
    if (br_scroll < 0) br_scroll = 0;

    /* Four passes so backgrounds always sit under images/rules/text,
     * regardless of the order layout emitted them in. */
    for (int pass = 0; pass < 4; pass++) {
        enum layout_item_type want = pass == 0 ? LAYOUT_ITEM_RECT :
                                      pass == 1 ? LAYOUT_ITEM_IMAGE :
                                      pass == 2 ? LAYOUT_ITEM_HR : LAYOUT_ITEM_TEXT;
        for (int i = 0; i < br_layout.item_count; i++) {
            const struct layout_item *it = &br_layout.items[i];
            if (it->type != want) continue;
            if (it->y + it->h < br_scroll || it->y > br_scroll + content_h) continue;

            int sx = content_x + it->x;
            int sy = content_y + (it->y - br_scroll);
            if (it->type == LAYOUT_ITEM_RECT) fb_fill_rect(sx, sy, it->w, it->h, it->color);
            else if (it->type == LAYOUT_ITEM_HR) fb_draw_line(sx, sy, sx + it->w, sy, it->color);
            else if (it->type == LAYOUT_ITEM_IMAGE) {
                if (it->pixels) {
                    /* it->w/it->h are the *display* size (CSS width/
                     * height, if the page set one); the pixel buffer's
                     * own dimensions -- needed as the blit's source
                     * size when those differ -- live in the image
                     * cache, keyed by the same <img> node. */
                    int nat_w = 0, nat_h = 0;
                    const uint32_t *ignored = NULL;
                    layout_get_image(it->owner, &nat_w, &nat_h, &ignored);
                    fb_blit_rgb(sx, sy, it->w, it->h, it->pixels,
                                nat_w > 0 ? nat_w : it->w, nat_h > 0 ? nat_h : it->h);
                } else {
                    fb_draw_rect(sx, sy, it->w, it->h, 0xA8AFD6); /* broken-image placeholder */
                }
            } else fb_draw_string(sx, sy, it->text, it->color, 1);
        }
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
    fb_draw_line(cbx + 2, cby + 2, cbx + 9, cby + 9, 0xFFFFFF);
    fb_draw_line(cbx + 9, cby + 2, cbx + 2, cby + 9, 0xFFFFFF);

    /* body */
    fb_fill_rect(w->x, w->y + TITLEBAR_H, w->w, w->h - TITLEBAR_H, 0x1B2040);
    fb_draw_rect(w->x, w->y, w->w, w->h, focused ? w->accent : accent_dark);

    if (w->body_line1) fb_draw_string(w->x + 14, w->y + TITLEBAR_H + 16, w->body_line1, COL_TEXT, 1);
    if (w->body_line2) fb_draw_string(w->x + 14, w->y + TITLEBAR_H + 34, w->body_line2, COL_MUTED, 1);
    if (w->is_process_monitor) draw_process_monitor(w);
    if (w->is_network) draw_network(w);
    if (w->is_file_manager) draw_file_manager(w);
    if (w->is_browser) draw_browser(w);
}

/* Geometry for the taskbar icon of windows[slot] -- shared between drawing
 * and click hit-testing so they can never drift apart. */
static void taskbar_icon_rect(int slot, int *ix, int *iy, int *iw, int *ih) {
    *ix = TASKBAR_ICONS_X + slot * (TASKBAR_ICON_W + TASKBAR_ICON_GAP);
    *iy = (int)fb_height() - TASKBAR_H + (TASKBAR_H - TASKBAR_ICON_H) / 2;
    *iw = TASKBAR_ICON_W;
    *ih = TASKBAR_ICON_H;
}

/* One dock icon per window ZapOS knows about, open or closed -- closing a
 * window (via its titlebar's X) only hides it, so this dock is also the
 * only way to bring one back: click a closed icon to reopen it, click an
 * already-open one to bring it back to front. A bright fill + underline
 * marks whichever window is currently focused; open-but-not-focused gets
 * a dimmer tint; closed gets a flat, dark slot. */
static void draw_taskbar_icon(int slot, int focused_wi) {
    int ix, iy, iw, ih;
    taskbar_icon_rect(slot, &ix, &iy, &iw, &ih);
    const gui_window_t *w = &windows[slot];

    uint32_t bg = 0x171E38;
    if (w->open) bg = (slot == focused_wi) ? w->accent : ((w->accent >> 1) & 0x7F7F7F);
    fb_fill_rect(ix, iy, iw, ih, bg);
    fb_draw_rect(ix, iy, iw, ih, w->open ? w->accent : 0x2A3350);

    int tw = fb_text_width(w->icon_label, 1);
    fb_draw_string(ix + (iw - tw) / 2, iy + 6, w->icon_label,
                    w->open ? 0xFFFFFF : 0x7A83A8, 1);

    if (w->open) fb_fill_rect(ix + 4, iy + ih - 4, iw - 8, 2, 0xFFFFFF);
}

static void draw_taskbar(int focused_wi) {
    int y = (int)fb_height() - TASKBAR_H;
    fb_fill_gradient_v(0, y, fb_width(), TASKBAR_H, 0x10132C, 0x05060F);
    fb_draw_line(0, y, fb_width(), y, 0x3A4270);

    fb_draw_string(14, y + 16, "ZapOS", 0xFFFFFF, 1);

    for (int i = 0; i < window_count; i++) draw_taskbar_icon(i, focused_wi);

    draw_clock(fb_width() - 90, y + 12);
}

static void draw_cursor(int x, int y) {
    fb_fill_triangle(x, y, x, y + 16, x + 11, y + 12, 0x000000);
    fb_fill_triangle(x + 1, y + 2, x + 1, y + 13, x + 9, y + 11, 0xFFFFFF);
}

/* Scales fs_buffer (DOOM_BLIT_W x DOOM_BLIT_H) up to fill as much of
 * the real screen as fits without distorting its aspect ratio, letter-
 * boxed and centered above the taskbar. */
static void draw_fullscreen_app(void) {
    fb_fill_rect(0, 0, (int)fb_width(), (int)fb_height(), 0x000000);

    int avail_h = (int)fb_height() - TASKBAR_H;
    int avail_w = (int)fb_width();
    int scale = avail_w / DOOM_BLIT_W;
    int scale_h = avail_h / DOOM_BLIT_H;
    if (scale_h < scale) scale = scale_h;
    if (scale < 1) scale = 1;

    int w = DOOM_BLIT_W * scale, h = DOOM_BLIT_H * scale;
    int x = (avail_w - w) / 2, y = (avail_h - h) / 2;
    fb_blit_rgb(x, y, w, h, fs_buffer, DOOM_BLIT_W, DOOM_BLIT_H);
}

static void draw_frame(int mx, int my) {
    if (fs_active) {
        draw_fullscreen_app();
        if (scheduler_task_state(fs_owner_pid) == TASK_TERMINATED) {
            fs_active = 0;
            fs_owner_pid = -1;
        }
        return;
    }

    fb_fill_gradient_v(0, 0, fb_width(), fb_height(), COL_BG_TOP, COL_BG_BOTTOM);
    fb_draw_string(24, 20, "ZapOS", COL_TEXT, 3);
    fb_draw_string(24, 56, "a custom 32-bit OS", COL_MUTED, 1);

    int top_open_pos = -1;
    for (int i = 0; i < window_count; i++) {
        if (windows[window_order[i]].open) top_open_pos = i;
    }
    for (int i = 0; i < window_count; i++) {
        int wi = window_order[i];
        if (!windows[wi].open) continue;
        draw_window(&windows[wi], i == top_open_pos);
    }

    draw_taskbar(top_open_pos >= 0 ? window_order[top_open_pos] : -1);
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
            int consumed = 0;
            for (int i = 0; i < window_count; i++) {
                int ix, iy, iw, ih;
                taskbar_icon_rect(i, &ix, &iy, &iw, &ih);
                if (mx >= ix && mx < ix + iw && my >= iy && my < iy + ih) {
                    open_and_focus(i);
                    consumed = 1;
                    break;
                }
            }

            if (!consumed) {
                int hit_titlebar = 0;
                for (int oi = window_count - 1; oi >= 0; oi--) {
                    int wi = window_order[oi];
                    gui_window_t *w = &windows[wi];
                    if (!w->open) continue;
                    if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + TITLEBAR_H) {
                        int cbx = w->x + w->w - 20, cby = w->y + 8;
                        if (mx >= cbx && mx < cbx + 12 && my >= cby && my < cby + 12) {
                            w->open = 0;
                            dragging_window = -1;
                        } else {
                            bring_to_front(oi);
                            dragging_window = wi;
                            drag_dx = mx - w->x;
                            drag_dy = my - w->y;
                        }
                        hit_titlebar = 1;
                        break;
                    }
                }
                /* Clicking anywhere deselects both text-input widgets; the
                 * specific click handler below re-focuses its own if the
                 * click actually landed on it. */
                br_editing_url = 0;
                if (fm_viewing_file) fm_editing = 0;

                if (!hit_titlebar) {
                    for (int oi = window_count - 1; oi >= 0; oi--) {
                        int wi = window_order[oi];
                        gui_window_t *w = &windows[wi];
                        if (!w->open) continue;
                        if (mx >= w->x && mx < w->x + w->w && my >= w->y + TITLEBAR_H && my < w->y + w->h) {
                            bring_to_front(oi);
                            if (w->is_file_manager) fm_handle_click(w, my);
                            else if (w->is_browser) br_handle_click(w, mx, my);
                            break;
                        }
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
            br_handle_key(c);
        }

        static int scroll_cooldown = 0;
        if (scroll_cooldown > 0) {
            scroll_cooldown--;
        } else if (keyboard_key_pressed(0x48)) { /* up arrow */
            br_scroll -= LAYOUT_LINE_H;
            if (br_scroll < 0) br_scroll = 0;
            scroll_cooldown = 4;
        } else if (keyboard_key_pressed(0x50)) { /* down arrow */
            br_scroll += LAYOUT_LINE_H;
            scroll_cooldown = 4;
        }

        draw_frame(mx, my);
        fb_swap_buffers();
        pit_sleep(16);
    }
}
