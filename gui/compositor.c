#include <gui/compositor.h>
#include <gui/framebuffer.h>
#include <gui/svgicon.h>
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
#include <net/png.h>
#include <fs/fat32.h>
#include <drivers/ac97.h>
#include <drivers/wav.h>
#include <js/js.h>
#include <js/dom_binding.h>
#include <kernel/kheap.h>
#include <kernel/elf.h>
#include <kernel/apic.h>
#include <gui/shell.h>
#include <string.h>

#define MAX_WINDOWS   10
#define TITLEBAR_H    28
#define TASKBAR_H     44
#define FM_ROW_H      16
#define FM_MAX_ENTRIES 24
#define FM_PREVIEW_MAX 2048
#define FM_AUDIO_MAX   (2 * 1024 * 1024)
#define BR_MAX_URL     224
#define TERM_SCROLLBACK 4096
#define TERM_INPUT_MAX  120
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
/* A .wasm module fetched via WebAssembly.instantiate(url) -- see
 * br_wasm_fetch() below. Generous like BR_IMAGE_FETCH_CAP; a hobby-scale
 * compiled module is nowhere near this. */
#define BR_WASM_FETCH_CAP  (512u * 1024)
/* No dynamic collections anywhere in this file -- tabs, history, and
 * bookmarks are all fixed-size arrays, same as windows[]/fm_entries[]. */
#define BR_MAX_TABS       6
#define BR_MAX_HISTORY    20
#define BR_MAX_BOOKMARKS  32
#define BR_TAB_H          18
#define BR_TAB_W          74
#define BR_TAB_GAP        2
#define BR_TAB_CLOSE_W    14
#define BR_NAVBTN_W       18
#define BR_NAVBTN_GAP     2
#define BR_URL_ROW_H      20
#define BR_BM_ROW_H       16

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
    int is_terminal;
    int is_smp_monitor;
    int open;
    int icon_id; /* enum svg_icon_id -- which dock/titlebar vector icon */
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

/* Windowed app graphics (SYS_WIN_OPEN/SYS_WIN_BLIT, see
 * kernel/syscall.c): the gap between SYS_WRITE (text only) and the
 * fullscreen takeover above (the ENTIRE screen, for DOOM) -- a normal
 * desktop window a task draws its own pixels into, same as every
 * built-in app here. Deliberately a small, SEPARATE, fixed-size table
 * (APP_WINDOW_MAX slots), NOT folded into windows[]/window_order[]
 * above: those are a compile-time-fixed set (the 8 built-in apps,
 * created once in gui_init(), with no removal/reuse machinery at all),
 * whereas app windows are opened and destroyed at arbitrary times by
 * arbitrary tasks -- retrofitting that onto windows[] would risk the 8
 * working built-in windows for no benefit. Ownership/cleanup mirrors
 * fs_active/fs_owner_pid above exactly: each slot remembers the pid
 * that opened it, and draw_app_windows() (called once per frame from
 * draw_frame(), below) frees the slot the instant
 * scheduler_task_state(owner_pid) == TASK_TERMINATED -- this is the
 * ENTIRE cleanup mechanism. Deliberate scope cuts, all follow-on work:
 * no SYS_WIN_CLOSE (a window only ever closes by its owner exiting),
 * no drag support, no close button, no dock icon/taskbar entry. */
#define APP_WINDOW_MAX       4
#define APP_WINDOW_MAX_W     400
#define APP_WINDOW_MAX_H     300
#define APP_WINDOW_TITLE_MAX 24

typedef struct {
    int in_use;
    int owner_pid;
    uint32_t w, h;
    char title[APP_WINDOW_TITLE_MAX];
    uint32_t *pixels; /* kmalloc'd w*h*4 bytes, zeroed at open time; 0xAARRGGBB per pixel (top byte = alpha) */
    int is_borderless; /* set at open time from WIN_FLAG_BORDERLESS (include/kernel/syscall.h) -- see draw_app_windows() */
} app_window_t;

static app_window_t app_windows[APP_WINDOW_MAX]; /* zero-initialized statically -- no in_use slot until gui_app_window_open() sets one */

int gui_app_window_open(int owner_pid, const char *title, uint32_t w, uint32_t h, uint32_t flags) {
    if (w == 0 || h == 0 || w > APP_WINDOW_MAX_W || h > APP_WINDOW_MAX_H) return -1;

    int slot = -1;
    for (int i = 0; i < APP_WINDOW_MAX; i++) {
        if (!app_windows[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    uint32_t *pixels = (uint32_t *)kmalloc(w * h * sizeof(uint32_t));
    if (!pixels) return -1;
    /* Zeroed -- alpha byte 0 means "fully transparent" under the new
     * 0xAARRGGBB format, so an app window shows nothing at all until its
     * first gui_app_window_blit(), same "blank until first draw" spirit
     * the old opaque-black zeroed buffer had. */
    memset(pixels, 0, (size_t)w * h * sizeof(uint32_t));

    app_window_t *win = &app_windows[slot];
    win->in_use = 1;
    win->owner_pid = owner_pid;
    win->w = w;
    win->h = h;
    strncpy(win->title, title, APP_WINDOW_TITLE_MAX - 1);
    win->title[APP_WINDOW_TITLE_MAX - 1] = 0;
    win->pixels = pixels;
    win->is_borderless = (flags & WIN_FLAG_BORDERLESS) != 0;
    return slot;
}

int gui_app_window_blit(int handle, const void *pixels) {
    if (handle < 0 || handle >= APP_WINDOW_MAX || !app_windows[handle].in_use) return -1;
    app_window_t *win = &app_windows[handle];
    memcpy(win->pixels, pixels, (size_t)win->w * win->h * sizeof(uint32_t));
    return 0;
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
static int br_wasm_fetch(const char *url, uint8_t **out_data, uint32_t *out_len);

/* Browser state -- up to BR_MAX_TABS pages open at once (a fixed array,
 * same convention as every other collection in this file, e.g.
 * windows[]/fm_entries[] -- no dynamic growth anywhere in this kernel).
 * Each tab owns its own DOM tree / stylesheet / layout / scroll /
 * decoded-image cache / back-forward history, so switching the active
 * tab is just repointing br_active_tab at a different slot -- nothing
 * needs re-fetching or re-laying-out. Pages are rendered with a real
 * (if pragmatic) CSS box-model layout: fetch -> dom_parse ->
 * css_extract_style_blocks -> (run inline <script>s, which may mutate
 * the DOM before it's ever drawn) -> layout_run -> a flat list of
 * positioned, styled render items in the tab's `layout`, which is what
 * draw_browser() and the link/onclick click hit-tests actually walk.
 * Unlike the old reader-mode renderer, the DOM tree and stylesheet are
 * kept alive for as long as the page is loaded (not freed right after
 * the first layout) so a JS onclick handler can mutate the tree and
 * trigger a br_relayout() -- both are only torn down right before the
 * tab's next page (or the tab itself, on close) replaces them.
 *
 * Known limitation, not fixed here: the JS engine (js/dom_binding.c's
 * onclick_table, js/value.c's arena) is a single global instance, not
 * per-tab -- switching to a tab you haven't just (re)fetched, after
 * fetching a *different* tab in between, can dispatch onclick handlers
 * against a stale/wrong table until that tab is reloaded. Pre-existing
 * single-page assumption in the JS subsystem; making it per-tab is out
 * of scope for tab/history/bookmark plumbing. */
struct br_image_slot {
    const struct dom_node *node;
    struct bmp_image img;
};

typedef struct {
    char url[BR_MAX_URL];
    int url_len;
    int scroll;
    char status_msg[64];
    struct layout_doc layout;
    int layout_alloced;
    struct dom_node *dom_root;
    struct css_stylesheet stylesheet;
    int stylesheet_valid;
    struct br_image_slot images[BR_MAX_IMAGES];
    int image_count;
    /* Standard back/forward stack: history[0..history_count) is every
     * URL ever navigated to (in order), history_pos is which one is
     * currently displayed. Going back then navigating somewhere new
     * truncates everything past history_pos before appending -- see
     * br_history_push(). history_pos == -1 means nothing has loaded
     * yet (brand-new tab). */
    char history[BR_MAX_HISTORY][BR_MAX_URL];
    int history_count;
    int history_pos;
} br_tab_t;

static br_tab_t br_tabs[BR_MAX_TABS];
static int br_tab_count = 0;
static int br_active_tab = 0;
static int br_editing_url = 0;
static int br_bookmarks_open = 0;
static int br_window_idx = -1;

static br_tab_t *br_active(void) { return &br_tabs[br_active_tab]; }

/* Bookmarks -- one URL per line in a root-level FAT32 file, loaded once
 * (lazily, the first time the star button or the list-toggle button is
 * drawn or clicked) and rewritten in full on every change; there are at
 * most BR_MAX_BOOKMARKS of them, so a full rewrite is cheap enough not
 * to need anything smarter (same "just overwrite the whole file" model
 * fm_save_current_file uses for NOTES.TXT). */
#define BR_BOOKMARKS_FILE "BOOKMARKS.TXT"
static char br_bookmarks[BR_MAX_BOOKMARKS][BR_MAX_URL];
static int br_bookmark_count = 0;
static int br_bookmarks_loaded = 0;

static void br_tab_free_resources(br_tab_t *t) {
    if (t->dom_root) { dom_free(t->dom_root); t->dom_root = NULL; }
    if (t->stylesheet_valid) { css_stylesheet_free(&t->stylesheet); t->stylesheet_valid = 0; }
    for (int i = 0; i < t->image_count; i++) bmp_free(&t->images[i].img);
    t->image_count = 0;
    if (t->layout_alloced) { layout_doc_free(&t->layout); t->layout_alloced = 0; }
}

/* (Re)initializes a tab slot to a fresh, blank state -- the same
 * starting URL/status the single-tab browser used to boot with.
 * Assumes any previous occupant's resources were already freed via
 * br_tab_free_resources() (both callers below do that first; a
 * never-used slot has nothing to free). */
static void br_tab_reset(br_tab_t *t) {
    strcpy(t->url, "example.com/");
    t->url_len = (int)strlen(t->url);
    t->scroll = 0;
    strcpy(t->status_msg, "Type a URL and press Enter");
    t->dom_root = NULL;
    t->stylesheet_valid = 0;
    t->image_count = 0;
    t->history_count = 0;
    t->history_pos = -1;
    layout_doc_alloc(&t->layout);
    t->layout_alloced = 1;
}

/* Opens a new tab and makes it active -- a no-op past BR_MAX_TABS (the
 * tab strip has no room to show more anyway). Wired to both the "+"
 * button and Ctrl+T (see br_handle_key). */
static void br_tab_open(void) {
    if (br_tab_count >= BR_MAX_TABS) return;
    br_tab_reset(&br_tabs[br_tab_count]);
    br_active_tab = br_tab_count;
    br_tab_count++;
}

/* Closes tab `idx`, shifting every later tab left to keep the array
 * contiguous (no holes -- same convention as fm/window arrays). Closing
 * the last remaining tab just resets it in place instead, since a
 * browser with zero tabs open has nowhere to draw a tab strip at all. */
static void br_tab_close(int idx) {
    if (idx < 0 || idx >= br_tab_count) return;
    br_tab_free_resources(&br_tabs[idx]);
    if (br_tab_count == 1) {
        br_tab_reset(&br_tabs[0]);
        br_active_tab = 0;
        return;
    }
    for (int i = idx; i < br_tab_count - 1; i++) br_tabs[i] = br_tabs[i + 1];
    br_tab_count--;

    /* The slot that just fell off the end still holds copies of
     * pointers now owned by whatever tab got shifted into its old
     * neighbor's place -- clear them (without freeing, that would be a
     * double free) so the next br_tab_open() to reuse this slot
     * kmalloc's fresh ones instead of leaking over these. */
    br_tab_t *vacated = &br_tabs[br_tab_count];
    vacated->dom_root = NULL;
    vacated->stylesheet_valid = 0;
    vacated->image_count = 0;
    vacated->layout_alloced = 0;

    if (br_active_tab >= br_tab_count) br_active_tab = br_tab_count - 1;
    else if (br_active_tab > idx) br_active_tab--;
}

static void br_tab_switch(int idx) {
    if (idx < 0 || idx >= br_tab_count) return;
    br_active_tab = idx;
    br_editing_url = 0;
    br_bookmarks_open = 0;
}

static int br_bookmark_find(const char *url) {
    for (int i = 0; i < br_bookmark_count; i++) if (strcmp(br_bookmarks[i], url) == 0) return i;
    return -1;
}

/* Reads BOOKMARKS.TXT (one URL per line) out of the FAT32 root -- the
 * same find-by-name-via-fat32_list_dir pattern the File Manager and
 * shell use for every other named file, since there's no
 * fat32_open_by_name. A missing file or no disk at all both just leave
 * the bookmark list empty rather than erroring. */
static void br_bookmarks_load(void) {
    br_bookmarks_loaded = 1;
    br_bookmark_count = 0;
    if (!fat32_is_mounted()) return;

    struct fat_dirent_info entries[FM_MAX_ENTRIES];
    int count = fat32_list_dir(fat32_root_cluster(), entries, FM_MAX_ENTRIES);
    for (int i = 0; i < count; i++) {
        if (entries[i].is_dir || strcmp(entries[i].name, BR_BOOKMARKS_FILE) != 0) continue;

        static char buf[BR_MAX_BOOKMARKS * BR_MAX_URL + 1];
        uint32_t cap = entries[i].size < sizeof(buf) - 1 ? entries[i].size : sizeof(buf) - 1;
        uint32_t got = fat32_read_file(entries[i].cluster, entries[i].size, buf, cap);
        buf[got] = 0;

        char *p = buf;
        while (*p && br_bookmark_count < BR_MAX_BOOKMARKS) {
            int j = 0;
            while (*p && *p != '\n' && *p != '\r' && j < BR_MAX_URL - 1) br_bookmarks[br_bookmark_count][j++] = *p++;
            br_bookmarks[br_bookmark_count][j] = 0;
            if (j > 0) br_bookmark_count++;
            while (*p == '\n' || *p == '\r') p++;
        }
        break;
    }
}

static void br_bookmarks_save(void) {
    if (!fat32_is_mounted()) return;
    static char buf[BR_MAX_BOOKMARKS * BR_MAX_URL + 1];
    int len = 0;
    for (int i = 0; i < br_bookmark_count; i++) {
        int l = (int)strlen(br_bookmarks[i]);
        if (len + l + 1 >= (int)sizeof(buf)) break;
        memcpy(buf + len, br_bookmarks[i], (size_t)l);
        len += l;
        buf[len++] = '\n';
    }
    fat32_write_file(fat32_root_cluster(), BR_BOOKMARKS_FILE, buf, (uint32_t)len);
}

static int br_bookmark_is_set(const char *url) {
    if (!br_bookmarks_loaded) br_bookmarks_load();
    return br_bookmark_find(url) >= 0;
}

/* Toggles `url` in the bookmark list and persists the whole list right
 * away -- there's no separate "save" step, same as fm_save_current_file
 * being the only way NOTES.TXT edits reach disk. */
static void br_bookmark_toggle(const char *url) {
    if (!br_bookmarks_loaded) br_bookmarks_load();
    int idx = br_bookmark_find(url);
    if (idx >= 0) {
        for (int i = idx; i < br_bookmark_count - 1; i++) strcpy(br_bookmarks[i], br_bookmarks[i + 1]);
        br_bookmark_count--;
    } else if (br_bookmark_count < BR_MAX_BOOKMARKS) {
        strncpy(br_bookmarks[br_bookmark_count], url, BR_MAX_URL - 1);
        br_bookmarks[br_bookmark_count][BR_MAX_URL - 1] = 0;
        br_bookmark_count++;
    }
    br_bookmarks_save();
}

/* Terminal state -- a single instance, same convention as every other
 * window here. `term_scrollback` is a bounded FIFO of everything ever
 * printed (the shell's own output, and -- via terminal_route_output(),
 * called from kernel/syscall.c's SYS_WRITE case -- whatever the one
 * foreground program it launched writes); term_print() drops the
 * oldest bytes to make room rather than growing, so drawing just means
 * wrapping the whole buffer and showing the last N rows (see
 * draw_terminal()). `term_owner_pid` is the pid (if any) whose
 * SYS_WRITE output currently routes here instead of just the serial
 * log -- cleared once that task terminates (polled once a frame, same
 * pattern as the fullscreen-takeover's fs_owner_pid). */
static uint32_t term_cwd = 0;
static char term_scrollback[TERM_SCROLLBACK];
static int term_sb_len = 0;
static char term_input[TERM_INPUT_MAX];
static int term_input_len = 0;
static int term_focused = 0;
static int term_owner_pid = -1;

static void term_print(const char *s) {
    int len = (int)strlen(s);
    if (len >= TERM_SCROLLBACK) { s += len - (TERM_SCROLLBACK - 1); len = TERM_SCROLLBACK - 1; }
    if (term_sb_len + len > TERM_SCROLLBACK) {
        int drop = term_sb_len + len - TERM_SCROLLBACK;
        if (drop > term_sb_len) drop = term_sb_len;
        memmove(term_scrollback, term_scrollback + drop, (size_t)(term_sb_len - drop));
        term_sb_len -= drop;
    }
    memcpy(term_scrollback + term_sb_len, s, (size_t)len);
    term_sb_len += len;
}

void terminal_route_output(int pid, const char *s) {
    if (pid == term_owner_pid) term_print(s);
}

/* Feeds typed characters into the input line -- Enter submits it to
 * shell_execute() (see gui/shell.c), Backspace edits, everything else
 * appends. Only consumes keys while term_focused (set by clicking
 * inside the window body -- see the dispatch loop in gui_run()), same
 * "clicking anywhere deselects the others" convention as
 * br_editing_url/fm_editing. */
static void term_handle_key(char c) {
    if (!term_focused) return;

    if (c == '\n' || c == '\r') {
        term_print("> ");
        term_print(term_input);
        term_print("\n");
        if (strcmp(term_input, "clear") == 0) {
            term_sb_len = 0;
        } else if (term_input_len > 0) {
            int pid = shell_execute(term_input, &term_cwd, term_print);
            if (pid >= 0) term_owner_pid = pid;
        }
        term_input_len = 0;
        term_input[0] = 0;
    } else if (c == '\b') {
        if (term_input_len > 0) term_input[--term_input_len] = 0;
    } else if (c >= 32 && c < 127 && term_input_len < TERM_INPUT_MAX - 1) {
        term_input[term_input_len++] = c;
        term_input[term_input_len] = 0;
    }
}

static void term_handle_click(void) {
    term_focused = 1;
}

/* Wraps `text` (the scrollback) into lines and draws only the last
 * `max_rows` of them -- a simple "recompute every frame" tail view
 * rather than a real scrolling viewport, cheap enough at this buffer
 * size (see TERM_SCROLLBACK) to just redo it at 60fps. */
static void term_draw_scrollback(int x, int y, int max_width, int max_rows) {
    int chars_per_line = max_width / 8;
    if (chars_per_line < 1) chars_per_line = 1;
    if (chars_per_line > 62) chars_per_line = 62;

    static char lines[48][64];
    int line_count = 0;
    char cur[64];
    int col = 0, li = 0;

    for (int i = 0; i < term_sb_len; i++) {
        char ch = term_scrollback[i];
        int flush = (ch == '\n' || col >= chars_per_line);
        if (flush) {
            cur[li] = 0;
            if (line_count < 48) strcpy(lines[line_count++], cur);
            else {
                for (int k = 0; k < 47; k++) strcpy(lines[k], lines[k + 1]);
                strcpy(lines[47], cur);
            }
            li = 0; col = 0;
            if (ch == '\n') continue;
        }
        if (li < 63) cur[li++] = ch;
        col++;
    }
    if (li > 0) {
        cur[li] = 0;
        if (line_count < 48) strcpy(lines[line_count++], cur);
        else {
            for (int k = 0; k < 47; k++) strcpy(lines[k], lines[k + 1]);
            strcpy(lines[47], cur);
        }
    }

    int total = line_count < 48 ? line_count : 48;
    int start = total > max_rows ? total - max_rows : 0;
    for (int i = start; i < total; i++) {
        fb_draw_string(x, y + (i - start) * FM_ROW_H, lines[i], COL_TEXT, 1);
    }
}

static void draw_terminal(const gui_window_t *w) {
    if (term_owner_pid >= 0 && scheduler_task_state(term_owner_pid) == TASK_TERMINATED) {
        term_owner_pid = -1;
    }

    int x = w->x + 10;
    int y = w->y + TITLEBAR_H + 8;
    int content_w = w->w - 20;
    int input_row_h = FM_ROW_H + 6;
    int scrollback_rows = (w->h - TITLEBAR_H - 16 - input_row_h) / FM_ROW_H;

    term_draw_scrollback(x, y, content_w, scrollback_rows);

    int input_y = w->y + w->h - input_row_h;
    fb_fill_rect(x, input_y, content_w, FM_ROW_H + 2, 0x0F1330);
    char prompt[TERM_INPUT_MAX + 3];
    strcpy(prompt, "$ ");
    strcat(prompt, term_input);
    fb_draw_string(x + 4, input_y + 3, prompt, term_focused ? 0x8FE3A8 : COL_MUTED, 1);
}

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
    win->is_terminal = 0;
    win->is_smp_monitor = 0;
    win->open = 1;
    win->icon_id = ICON_ABOUT; /* placeholder; gui_init() sets the real one */
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

/* The window drawn topmost among currently-open windows, if any (-1 if
 * none are open) -- used to gate keyboard shortcuts that should only
 * fire for whichever window currently "has focus" (Ctrl+T/Ctrl+W/
 * Alt+Left/Alt+Right in the Browser -- see br_handle_key() and
 * gui_run()), the same notion of focus draw_frame() already uses to
 * pick which titlebar gets the brighter gradient. */
static int gui_frontmost_window(void) {
    int top = -1;
    for (int i = 0; i < window_count; i++) if (windows[window_order[i]].open) top = window_order[i];
    return top;
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
    svgicon_init();

    int about = add_window(SX(120), SY(90), 340, 190, "About ZapOS", "ABT",
               "A fully custom 32-bit OS kernel",
               "GUI + drivers written from scratch", 0x3E6FF0);
    windows[about].icon_id = ICON_ABOUT;

    int sys = add_window(SX(560), SY(160), 300, 170, "System Monitor", "SYS",
               "Kernel heap + paging: online",
               "PS/2 keyboard + mouse: online", 0x2FBF71);
    windows[sys].is_smp_monitor = 1;
    windows[sys].icon_id = ICON_SYSTEM;

    int roadmap = add_window(SX(260), SY(340), 320, 150, "Roadmap", "MAP",
               "Next up: process isolation + a filesystem",
               "See README.md for the plan", 0xE0954C);
    windows[roadmap].icon_id = ICON_ROADMAP;

    int pm = add_window(SX(640), SY(420), 320, 190, "Process Monitor", "PROC", NULL, NULL, 0xB05CE0);
    windows[pm].is_process_monitor = 1;
    windows[pm].icon_id = ICON_PROCESS;

    int net = add_window(SX(120), SY(460), 340, 190, "Network", "NET", NULL, NULL, 0x3ED0D8);
    windows[net].is_network = 1;
    windows[net].icon_id = ICON_NETWORK;

    if (fat32_is_mounted()) {
        int fm = add_window(SX(480), SY(560), 380, 220, "File Manager", "FILE", NULL, NULL, 0xF2C14E);
        windows[fm].is_file_manager = 1;
        windows[fm].icon_id = ICON_FILES;
        fm_current_dir = fat32_root_cluster();
        fm_refresh();
    }

    if (net_is_up()) {
        js_set_binary_fetcher(br_wasm_fetch);
        int br = add_window(SX(600), SY(60), 500, 500, "Browser", "WWW", NULL, NULL, 0x62D8FF);
        windows[br].is_browser = 1;
        windows[br].icon_id = ICON_BROWSER;
        br_window_idx = br;
        br_tab_open();
    }

    if (fat32_is_mounted()) {
        int term = add_window(SX(680), SY(540), 460, 210, "Terminal", "TERM", NULL, NULL, 0x4CD980);
        windows[term].is_terminal = 1;
        windows[term].icon_id = ICON_TERMINAL;
        term_cwd = fat32_root_cluster();
        term_print("ZapOS terminal -- type 'help' for commands\n");
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

/* Deliberately temporary/easy-to-remove proof-of-life display for the
 * SMP bring-up MVP (see kernel/smp.c, kernel/apic.c): just reads the
 * two counters the AP itself increments and shows them changing over
 * time. If no AP was ever woken (single-CPU boot, or bring-up failed),
 * both values just stay at 0 forever -- a correct, harmless steady
 * state, not an error. */
static void draw_smp_monitor(const gui_window_t *w) {
    int x = w->x + 14;
    int y = w->y + TITLEBAR_H + 58;

    draw_labeled_uint(x, y, "AP cores started: ", apic_ap_started_count(), 0x8FE3A8);
    draw_labeled_uint(x, y + 20, "AP heartbeat: ", apic_ap_heartbeat(), 0xE0954C);
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

/* Any file this small edit-in-place model makes sense for -- plain text
 * and the source extensions draw_highlighted_text() below knows how to
 * color. Saving just overwrites the file under its own name (see
 * fm_save_current_file), so this isn't NOTES.TXT-specific anymore. */
static int fm_is_editable(const char *name) {
    return strcmp(name, "NOTES.TXT") == 0 || fm_has_ext(name, "TXT") || fm_has_ext(name, "JS") ||
           fm_has_ext(name, "PY") || fm_has_ext(name, "C") || fm_has_ext(name, "H") || fm_has_ext(name, "MD");
}

enum fm_lang { LANG_NONE, LANG_C, LANG_JS, LANG_PY };

static enum fm_lang fm_lang_for_name(const char *name) {
    if (fm_has_ext(name, "C") || fm_has_ext(name, "H")) return LANG_C;
    if (fm_has_ext(name, "JS")) return LANG_JS;
    if (fm_has_ext(name, "PY")) return LANG_PY;
    return LANG_NONE;
}

static int is_keyword(enum fm_lang lang, const char *word, int len) {
    static const char *c_kw[] = {
        "if", "else", "while", "for", "return", "int", "char", "void", "struct", "static",
        "const", "unsigned", "long", "short", "float", "double", "break", "continue", "switch",
        "case", "default", "sizeof", "typedef", "enum", "union", "do", "goto", "NULL", 0
    };
    static const char *js_kw[] = {
        "function", "var", "let", "const", "if", "else", "while", "for", "return", "true",
        "false", "null", "undefined", "new", "this", "typeof", "break", "continue", "switch",
        "case", "default", "do", "in", "of", "class", "extends", "try", "catch", "throw", 0
    };
    static const char *py_kw[] = {
        "def", "return", "if", "elif", "else", "for", "while", "in", "import", "class",
        "True", "False", "None", "and", "or", "not", "break", "continue", "pass", "from",
        "as", "with", "lambda", "try", "except", "finally", "raise", "yield", "global", "print", 0
    };
    const char **list = lang == LANG_C ? c_kw : lang == LANG_JS ? js_kw : lang == LANG_PY ? py_kw : (const char **)0;
    if (!list) return 0;
    for (int i = 0; list[i]; i++) {
        int klen = (int)strlen(list[i]);
        if (klen == len && memcmp(word, list[i], (size_t)len) == 0) return 1;
    }
    return 0;
}

/* A real, single-pass tokenizing highlighter -- not a fixed palette
 * swap. Tracks string/line-comment state across wrapped-line
 * boundaries so a string or comment that happens to wrap still colors
 * correctly. Known gaps (documented, not silent): only single-line
 * comments (C/JS "//", Python "#") -- multi-line C-style comments
 * aren't tracked as a separate state and just get colored token-by-
 * token like ordinary code; escaped-quote handling only checks for a
 * single preceding backslash; no Python triple-quoted strings. Returns
 * the number of rows it actually drew. */
static int draw_highlighted_text(int x, int y, int max_width, int max_rows, const char *text, enum fm_lang lang) {
    int chars_per_line = max_width / 8;
    if (chars_per_line < 1) chars_per_line = 1;
    if (chars_per_line > 62) chars_per_line = 62;

    int col = 0, row = 0;
    int in_string = 0;
    char string_quote = 0;
    int in_line_comment = 0;

    char run[64];
    int run_len = 0;
    uint32_t run_color = COL_TEXT;
    int run_x = x;

    for (const char *p = text; *p && row < max_rows; p++) {
        char c = *p;

        if (c == '\n') {
            if (run_len > 0) { run[run_len] = 0; fb_draw_string(run_x, y + row * FM_ROW_H, run, run_color, 1); run_len = 0; }
            in_line_comment = 0;
            col = 0; row++;
            run_x = x;
            continue;
        }
        if (col >= chars_per_line) {
            if (run_len > 0) { run[run_len] = 0; fb_draw_string(run_x, y + row * FM_ROW_H, run, run_color, 1); run_len = 0; }
            col = 0; row++;
            run_x = x;
            if (row >= max_rows) break;
        }

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            int wlen = 0;
            while ((p[wlen] >= 'a' && p[wlen] <= 'z') || (p[wlen] >= 'A' && p[wlen] <= 'Z') ||
                   (p[wlen] >= '0' && p[wlen] <= '9') || p[wlen] == '_') wlen++;
            uint32_t word_color = is_keyword(lang, p, wlen) ? 0x569CD6 : COL_TEXT;
            for (int i = 0; i < wlen; i++) {
                if (col >= chars_per_line) {
                    if (run_len > 0) { run[run_len] = 0; fb_draw_string(run_x, y + row * FM_ROW_H, run, run_color, 1); run_len = 0; }
                    col = 0; row++; run_x = x;
                    if (row >= max_rows) { wlen = i; break; }
                }
                if (run_len > 0 && word_color != run_color) {
                    run[run_len] = 0; fb_draw_string(run_x, y + row * FM_ROW_H, run, run_color, 1); run_len = 0;
                }
                if (run_len == 0) { run_color = word_color; run_x = x + col * 8; }
                if (run_len < 63) run[run_len++] = p[i];
                col++;
            }
            if (run_len > 0) { run[run_len] = 0; fb_draw_string(run_x, y + row * FM_ROW_H, run, run_color, 1); run_len = 0; }
            p += wlen - 1;
            continue;
        }

        uint32_t ch_color = COL_TEXT;
        if (in_line_comment) {
            ch_color = 0x6A9955;
        } else if (in_string) {
            ch_color = 0xCE9178;
            if (c == string_quote && (p == text || p[-1] != '\\')) in_string = 0;
        } else if (c == '"' || c == '\'') {
            in_string = 1; string_quote = c;
            ch_color = 0xCE9178;
        } else if ((lang == LANG_C || lang == LANG_JS) && c == '/' && p[1] == '/') {
            in_line_comment = 1;
            ch_color = 0x6A9955;
        } else if (lang == LANG_PY && c == '#') {
            in_line_comment = 1;
            ch_color = 0x6A9955;
        } else if ((c >= '0' && c <= '9') || (c == '.' && p[1] >= '0' && p[1] <= '9')) {
            ch_color = 0xB5CEA8;
        }

        if (run_len > 0 && ch_color != run_color) {
            run[run_len] = 0; fb_draw_string(run_x, y + row * FM_ROW_H, run, run_color, 1); run_len = 0;
        }
        if (run_len == 0) { run_color = ch_color; run_x = x + col * 8; }
        if (run_len < 63) run[run_len++] = c;
        col++;
    }
    if (run_len > 0 && row < max_rows) { run[run_len] = 0; fb_draw_string(run_x, y + row * FM_ROW_H, run, run_color, 1); }
    return row < max_rows ? row + 1 : max_rows;
}

/* Called when a row in the listing is clicked: navigate into directories,
 * open an audio-preview screen for .WAV files, open a read-only preview
 * for other files, or an editable one for recognized text/source
 * extensions (see fm_is_editable) -- fat32_write_file can save any of
 * them back under their own name, not just NOTES.TXT. */
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
    fm_editing = fm_is_editable(e->name);
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

static void fm_save_current_file(void) {
    int ok = fat32_write_file(fm_current_dir, fm_preview_name, fm_preview_buf, fm_preview_len);
    strcpy(fm_status_msg, ok ? "Saved -- persists across reboot" : "Save failed");
    fm_status_until = pit_ticks() + 200;
    fm_dirty = 0;
}

/* Feeds typed characters into the open file's edit buffer -- Enter
 * inserts a real newline (so multi-line source files are actually
 * editable), Ctrl+S (0x13, see drivers/keyboard.c's C0 control-code
 * mapping) saves -- or, while an audio file is open, P/S for
 * play/stop. Called from gui_run() unconditionally on every keypress.
 * Append/backspace-from-the-end only, no cursor movement or inserting
 * into the middle of the text. */
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
    if (c == 0x13) { /* Ctrl+S */
        fm_save_current_file();
    } else if (c == '\n' || c == '\r') {
        if (fm_preview_len < FM_PREVIEW_MAX) { fm_preview_buf[fm_preview_len++] = '\n'; fm_dirty = 1; }
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
                           fm_dirty ? "editing -- Ctrl+S to save" : "Ctrl+S to save, Backspace to edit",
                           COL_MUTED, 1);
        }
        enum fm_lang lang = fm_lang_for_name(fm_preview_name);
        int preview_rows = (w->h - TITLEBAR_H - 70) / FM_ROW_H;
        if (lang != LANG_NONE) draw_highlighted_text(x, y + 52, content_w, preview_rows, fm_preview_buf, lang);
        else draw_wrapped_text(x, y + 52, content_w, preview_rows, fm_preview_buf, COL_TEXT);
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

/* Splits "example.com:8000/path" (an optional "http://" or "https://"
 * prefix is skipped, and remembered in *is_https_out) into a host, a
 * port (defaulting to 80, or 443 if the URL said https), and a path
 * (defaulting to "/"). A bare host with no scheme prefix (e.g. the
 * default new-tab URL) is treated as http, same as before TLS existed. */
static void br_parse_url(const char *url, char *host_out, int host_cap, uint16_t *port_out,
                          char *path_out, int path_cap, int *is_https_out) {
    const char *p = url;
    *is_https_out = 0;
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' &&
        p[4] == 's' && p[5] == ':' && p[6] == '/' && p[7] == '/') {
        *is_https_out = 1;
        p += 8;
    } else if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' &&
        p[4] == ':' && p[5] == '/' && p[6] == '/') {
        p += 7;
    }
    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < host_cap - 1) host_out[i++] = *p++;
    host_out[i] = 0;

    *port_out = *is_https_out ? 443 : 80;
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

/* Chrome layout, top to bottom: tab strip, then the URL row (back/
 * forward/star/bookmarks-list buttons + the URL field), then the status
 * line, then the page content. These two return the top of the tab
 * strip and the URL row respectively; every other piece of browser
 * chrome (drawing, click hit-testing, content area) is defined relative
 * to them so they can never disagree with each other. */
static int br_tabstrip_y(const gui_window_t *w) { return w->y + TITLEBAR_H + 6; }
static int br_urlrow_y(const gui_window_t *w) { return br_tabstrip_y(w) + BR_TAB_H + 4; }

/* Content-area geometry shared by drawing, scrolling, and link hit-
 * testing, so they can never disagree with each other. */
static void br_content_area(int *x, int *y, int *w, int *h) {
    const gui_window_t *win = &windows[br_window_idx];
    *x = win->x + 10;
    *y = br_urlrow_y(win) + 46;
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
 * navigation state (the active tab's url/status_msg) -- a missing
 * image shouldn't clobber the address bar or status line. Returns 0
 * for schemes this browser can't fetch (data:, empty) rather than 1
 * with a nonsense host/path. A relative reference inherits the
 * referencing page's own scheme (*is_https_out is read on entry as
 * that page's scheme, then overwritten with the resolved result). */
static int br_resolve_subresource(const char *base_host, uint16_t base_port, const char *base_path,
                                   const char *url, char *host_out, int host_cap,
                                   uint16_t *port_out, char *path_out, int path_cap, int *is_https_out) {
    if (!url[0] || url[0] == '#') return 0;
    if (strncmp(url, "data:", 5) == 0) return 0;
    if (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0) {
        br_parse_url(url, host_out, host_cap, port_out, path_out, path_cap, is_https_out);
        return 1;
    }
    if (url[0] == '/' && url[1] == '/') {
        /* Scheme-relative ("//host/path") -- inherit the referencing
         * page's own scheme (already in *is_https_out on entry), same
         * as parse_location() does for a Location header. Without
         * this, a scheme-relative <img>/<link> (common for CDN-hosted
         * assets, e.g. DuckDuckGo's favicon proxy) fell through to the
         * relative-path branch below and got wrongly joined onto the
         * current page's own path. */
        char tmp[136];
        strcpy(tmp, *is_https_out ? "https:" : "http:");
        int n = (int)strlen(tmp);
        int ul = (int)strlen(url);
        if (n + ul < (int)sizeof(tmp)) memcpy(tmp + n, url, (size_t)ul + 1);
        br_parse_url(tmp, host_out, host_cap, port_out, path_out, path_cap, is_https_out);
        return 1;
    }
    strncpy(host_out, base_host, host_cap - 1);
    host_out[host_cap - 1] = 0;
    *port_out = base_port;
    br_join_path(base_path, url, path_out, path_cap);
    return 1;
}

/* The current page's own host/port/path/scheme, stashed right before
 * running its scripts so br_wasm_fetch() (registered once with
 * js_set_binary_fetcher(), called from deep inside the JS interpreter
 * with nothing but a URL string to go on) can resolve a relative
 * WebAssembly.instantiate(url) the same way an <img src> gets resolved. */
static char br_wasm_ctx_host[64];
static uint16_t br_wasm_ctx_port;
static char br_wasm_ctx_path[64];
static int br_wasm_ctx_is_https;

/* js_binary_fetch_fn implementation (see include/js/dom_binding.h):
 * resolves `url` against the page context above and fetches it exactly
 * like an <img>/<link> subresource, handing the caller a kmalloc'd
 * buffer it owns. */
static int br_wasm_fetch(const char *url, uint8_t **out_data, uint32_t *out_len) {
    char host[64], path[64];
    uint16_t port;
    int is_https = br_wasm_ctx_is_https;
    if (!br_resolve_subresource(br_wasm_ctx_host, br_wasm_ctx_port, br_wasm_ctx_path, url,
                                 host, sizeof(host), &port, path, sizeof(path), &is_https)) {
        return 0;
    }
    uint8_t *buf = (uint8_t *)kmalloc(BR_WASM_FETCH_CAP + 1);
    if (!buf) return 0;
    int status;
    uint32_t blen;
    if (!http_get(is_https, host, port, path, &status, (char *)buf, BR_WASM_FETCH_CAP, &blen, NULL, 0) ||
        status < 200 || status >= 300) {
        kfree(buf);
        return 0;
    }
    *out_data = buf;
    *out_len = blen;
    return 1;
}

/* Decoded <img> cache for the active tab's page (struct br_image_slot
 * and the per-tab `images`/`image_count` fields are declared with the
 * rest of br_tab_t, up near the other browser state) -- keyed by the
 * DOM node so layout_get_image() (called from net/layout.c while laying
 * out that exact <img> element) can look its pixels back up. Filled by
 * br_load_subresources() right after the DOM parses and before the
 * first layout, since layout needs each image's natural size to
 * reserve space for it. Freed and reset by br_tab_free_resources() on
 * navigation/close.
 *
 * layout_run() (and therefore layout_get_image()) is only ever called
 * synchronously while working on whichever tab br_active() names at
 * that moment (br_fetch_page/br_relayout both operate on br_active()),
 * so reading br_active()'s own cache here is always the right one --
 * this never runs "for" a tab that isn't currently active. */
int layout_get_image(const struct dom_node *node, int *out_w, int *out_h, const uint32_t **out_pixels) {
    br_tab_t *t = br_active();
    for (int i = 0; i < t->image_count; i++) {
        if (t->images[i].node == node) {
            *out_w = t->images[i].img.width;
            *out_h = t->images[i].img.height;
            *out_pixels = t->images[i].img.pixels;
            return 1;
        }
    }
    return 0;
}

static void br_images_reset(br_tab_t *t) {
    for (int i = 0; i < t->image_count; i++) bmp_free(&t->images[i].img);
    t->image_count = 0;
}

/* Walks the DOM fetching every <link rel="stylesheet"> and <img> it
 * finds, one HTTP request at a time (this browser only ever has one TCP
 * connection open at once) -- external CSS is parsed straight into the
 * page's stylesheet, images are decoded into the tab's images[] for
 * layout_get_image() to find. Best-effort: a failed/unsupported
 * sub-resource is silently skipped rather than aborting the page, same
 * as a real browser would just show a broken-image icon and move on. */
static void br_load_subresources(br_tab_t *t, struct dom_node *node, const char *base_host,
                                  uint16_t base_port, int base_is_https, const char *base_path) {
    for (struct dom_node *child = node->children; child; child = child->next) {
        if (child->type != DOM_ELEMENT) continue;

        if (strcmp(child->tag, "link") == 0 && strcmp(child->rel, "stylesheet") == 0 && child->href[0]) {
            char host[64], path[64]; uint16_t port; int is_https = base_is_https;
            if (br_resolve_subresource(base_host, base_port, base_path, child->href,
                                        host, sizeof(host), &port, path, sizeof(path), &is_https)) {
                char *buf = kmalloc(BR_CSS_FETCH_CAP + 1);
                if (buf) {
                    int status; uint32_t blen;
                    if (http_get(is_https, host, port, path, &status, buf, BR_CSS_FETCH_CAP, &blen, NULL, 0) &&
                        status >= 200 && status < 300) {
                        css_parse_into(&t->stylesheet, buf, blen);
                    }
                    kfree(buf);
                }
            }
        } else if (strcmp(child->tag, "img") == 0 && child->href[0] && t->image_count < BR_MAX_IMAGES) {
            char host[64], path[64]; uint16_t port; int is_https = base_is_https;
            if (br_resolve_subresource(base_host, base_port, base_path, child->href,
                                        host, sizeof(host), &port, path, sizeof(path), &is_https)) {
                char *buf = kmalloc(BR_IMAGE_FETCH_CAP + 1);
                if (buf) {
                    int status; uint32_t blen;
                    if (http_get(is_https, host, port, path, &status, buf, BR_IMAGE_FETCH_CAP, &blen, NULL, 0) &&
                        status >= 200 && status < 300) {
                        struct bmp_image img;
                        if (bmp_decode((const uint8_t *)buf, blen, &img) ||
                            png_decode((const uint8_t *)buf, blen, &img)) {
                            t->images[t->image_count].node = child;
                            t->images[t->image_count].img = img;
                            t->image_count++;
                        }
                    }
                    kfree(buf);
                }
            }
        }

        br_load_subresources(t, child, base_host, base_port, base_is_https, base_path);
    }
}

/* Standard back/forward-stack push: appends `url` right after the
 * current position, discarding anything that was ahead of it (the redo
 * branch a previous br_go_back() left behind) -- exactly what "go back,
 * then navigate somewhere new" is supposed to do. Drops the oldest
 * entry to make room past BR_MAX_HISTORY rather than growing, same
 * fixed-capacity convention as everything else in this file. */
static void br_history_push(br_tab_t *t, const char *url) {
    int new_pos = t->history_pos + 1;
    if (new_pos >= BR_MAX_HISTORY) {
        for (int i = 1; i < BR_MAX_HISTORY; i++) strcpy(t->history[i - 1], t->history[i]);
        new_pos = BR_MAX_HISTORY - 1;
    }
    strncpy(t->history[new_pos], url, BR_MAX_URL - 1);
    t->history[new_pos][BR_MAX_URL - 1] = 0;
    t->history_pos = new_pos;
    t->history_count = new_pos + 1;
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
 * format, via the File Manager -- see fm_play_audio()).
 *
 * `record_history` is 0 for br_go_back()/br_go_forward() (they're
 * replaying a URL already in the tab's history, not creating a new
 * entry) and 1 for every other navigation path (typing a URL, clicking
 * a link, an onclick-driven navigation). There's no page cache of any
 * kind, so going back/forward re-fetches over the network exactly like
 * a fresh navigation -- a deliberate simplification given this browser
 * has nowhere to cache a rendered page anyway. */
static void br_fetch_page(br_tab_t *t, int record_history) {
    char host[64], path[64];
    uint16_t port;
    int is_https;
    br_parse_url(t->url, host, sizeof(host), &port, path, sizeof(path), &is_https);

    char *body = (char *)kmalloc(BR_FETCH_CAP + 1);
    if (!body) {
        strcpy(t->status_msg, "Out of memory");
        return;
    }

    int status;
    uint32_t body_len;
    char content_type[BR_CONTENT_TYPE_MAX];

    if (!http_get(is_https, host, port, path, &status, body, BR_FETCH_CAP, &body_len,
                   content_type, sizeof(content_type))) {
        strcpy(t->status_msg, "Failed to load (DNS/TCP error)");
        t->layout.item_count = 0;
        t->layout.link_count = 0;
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
        if (t->dom_root) { dom_free(t->dom_root); t->dom_root = NULL; }
        if (t->stylesheet_valid) { css_stylesheet_free(&t->stylesheet); t->stylesheet_valid = 0; }
        br_images_reset(t);
        js_arena_reset();
        js_dom_reset();

        char title[DOM_MAX_TITLE];
        t->dom_root = dom_parse(body, body_len, title, sizeof(title));

        css_stylesheet_init(&t->stylesheet);
        css_extract_style_blocks(&t->stylesheet, body, body_len);
        t->stylesheet_valid = 1;

        /* External <link rel=stylesheet> and <img> both need their own
         * HTTP fetch, done here (before layout, after DOM/inline-CSS)
         * so the external rules are in the cascade and every image's
         * natural size is known by the time layout_run() needs it. */
        br_load_subresources(t, t->dom_root, host, port, is_https, path);

        /* Custom-property (var()) resolution needs the FINAL stylesheet
         * -- including whatever external sheets br_load_subresources()
         * just appended, since a page's own theme variables commonly
         * live in one of those rather than an inline <style> block --
         * so this runs after subresources load and before anything
         * else reads a computed style. */
        css_resolve_custom_properties(&t->stylesheet);

        /* Scripts run before the first layout so DOM mutations they
         * make (innerHTML, textContent, style) show up immediately
         * rather than requiring a second pass. */
        strncpy(br_wasm_ctx_host, host, sizeof(br_wasm_ctx_host) - 1);
        br_wasm_ctx_host[sizeof(br_wasm_ctx_host) - 1] = 0;
        br_wasm_ctx_port = port;
        strncpy(br_wasm_ctx_path, path, sizeof(br_wasm_ctx_path) - 1);
        br_wasm_ctx_path[sizeof(br_wasm_ctx_path) - 1] = 0;
        br_wasm_ctx_is_https = is_https;

        struct js_env *global_env = js_make_global_env(t->dom_root);
        js_run_inline_scripts(body, body_len, global_env);
        js_dom_clear_relayout_flag();

        int content_x, content_y, content_w, content_h;
        br_content_area(&content_x, &content_y, &content_w, &content_h);
        layout_run(t->dom_root, &t->stylesheet, content_w, &t->layout);
        strncpy(t->layout.title, title, DOM_MAX_TITLE - 1);

        kfree(body);
        t->scroll = 0;

        char numbuf[12];
        utoa((unsigned int)status, numbuf);
        strcpy(t->status_msg, status >= 200 && status < 300 ? "OK " : "HTTP ");
        strcat(t->status_msg, numbuf);

        /* A page that loaded at all (even a 404/500 error page the
         * server rendered as HTML) is still something back/forward
         * should be able to return to -- only the DNS/TCP failure path
         * above (no response at all) skips history. */
        if (record_history) br_history_push(t, t->url);
        return;
    }

    char fname[32];
    br_derive_filename(path, fname, sizeof(fname));

    if (!fat32_is_mounted()) {
        strcpy(t->status_msg, "Fetched, but no disk to save it to");
    } else if (fat32_write_file(fat32_root_cluster(), fname, body, body_len)) {
        char numbuf[16];
        utoa(body_len, numbuf);
        strcpy(t->status_msg, "Downloaded ");
        strcat(t->status_msg, fname);
        strcat(t->status_msg, " (");
        strcat(t->status_msg, numbuf);
        strcat(t->status_msg, "B)");
        fm_refresh();
    } else {
        strcpy(t->status_msg, "Download failed (disk full?)");
    }
    kfree(body);
}

/* DuckDuckGo's plain server-rendered results page -- no JS, minimal CSS,
 * about as close to "will actually render in this engine" as a real
 * search engine gets (Google's own results page assumes a full modern
 * JS/CSS stack this from-scratch renderer doesn't have). Only reachable
 * at all now that net/tls.c exists -- it redirects http to https. */
#define BR_SEARCH_HOST "html.duckduckgo.com"
#define BR_SEARCH_PATH_PREFIX "/html/?q="

/* Heuristic for "is this address-bar text a URL, or a search query" --
 * the same rough rule real browsers use: an explicit scheme, or any
 * '.' before the first space, reads as a URL; anything else (no dot at
 * all, or a space before one) is a search query. Not spec-perfect (a
 * bare "localhost" reads as a search, same tradeoff most browsers make
 * without a fuller heuristic), just good enough for everyday typing. */
static int br_looks_like_url(const char *s) {
    if (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0) return 1;
    for (const char *p = s; *p; p++) {
        if (*p == ' ') return 0;
        if (*p == '.') return 1;
    }
    return 0;
}

/* Percent-encodes `in` for use as a URL query-string value (RFC 3986
 * unreserved characters pass through, a space becomes '+' as query
 * strings conventionally use, everything else becomes %XX) -- stops
 * early rather than overflowing if the encoded form would exceed
 * out_cap, same hard-truncate convention the rest of this file uses. */
static void br_percent_encode(const char *in, char *out, int out_cap) {
    static const char hex[] = "0123456789ABCDEF";
    int j = 0;
    for (const char *p = in; *p && j < out_cap - 4; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = c;
        } else if (c == ' ') {
            out[j++] = '+';
        } else {
            out[j++] = '%';
            out[j++] = hex[((uint8_t)c) >> 4];
            out[j++] = hex[((uint8_t)c) & 0xF];
        }
    }
    out[j] = 0;
}

/* Rewrites t->url in place into a DuckDuckGo query URL if it doesn't
 * look like a URL at all -- called right before every address-bar
 * Enter triggers a fetch, so typing a search term and pressing Enter
 * just works, the same as any other browser's combined address/search
 * bar. Leaves t->url alone (returns without touching it) for anything
 * that already looks like a URL. */
static void br_maybe_search(br_tab_t *t) {
    if (br_looks_like_url(t->url)) return;

    static const char prefix[] = "https://" BR_SEARCH_HOST BR_SEARCH_PATH_PREFIX;
    char query[BR_MAX_URL];
    strncpy(query, t->url, sizeof(query) - 1);
    query[sizeof(query) - 1] = 0;

    int encoded_cap = BR_MAX_URL - (int)sizeof(prefix); /* sizeof(prefix) includes prefix's own NUL */
    if (encoded_cap < 1) encoded_cap = 1;
    char encoded[BR_MAX_URL];
    if (encoded_cap > (int)sizeof(encoded)) encoded_cap = (int)sizeof(encoded);
    br_percent_encode(query, encoded, encoded_cap);

    strcpy(t->url, prefix);
    int n = (int)strlen(t->url);
    int el = (int)strlen(encoded);
    if (n + el < BR_MAX_URL) { memcpy(t->url + n, encoded, (size_t)el); n += el; }
    t->url[n] = 0;
    t->url_len = n;
}

static void br_fetch(void) {
    br_fetch_page(br_active(), 1);
}

/* Back/forward just rewind/replay the active tab's history stack and
 * re-fetch -- see br_fetch_page()'s comment on why re-fetching (rather
 * than caching) is what "going back" means in this browser. No-ops at
 * the bounds (nothing before the oldest entry, nothing after the
 * newest), same "disabled at the edges" behavior as the on-screen
 * back/forward buttons (see draw_browser). */
static void br_go_back(void) {
    br_tab_t *t = br_active();
    if (t->history_pos <= 0) return;
    t->history_pos--;
    strncpy(t->url, t->history[t->history_pos], BR_MAX_URL - 1);
    t->url[BR_MAX_URL - 1] = 0;
    t->url_len = (int)strlen(t->url);
    br_fetch_page(t, 0);
}

static void br_go_forward(void) {
    br_tab_t *t = br_active();
    if (t->history_pos < 0 || t->history_pos >= t->history_count - 1) return;
    t->history_pos++;
    strncpy(t->url, t->history[t->history_pos], BR_MAX_URL - 1);
    t->url[BR_MAX_URL - 1] = 0;
    t->url_len = (int)strlen(t->url);
    br_fetch_page(t, 0);
}

/* Resolves `href` (as found on an <a> in the just-loaded page) against
 * `t`'s current url, writes the resolved absolute "scheme://host[:port]/path"
 * back into it, and returns 1 -- or returns 0 (no navigation) for
 * fragment-only/mailto:/javascript: links. A relative href inherits
 * the current page's own scheme (an https page's relative links stay
 * https, rather than silently downgrading -- see br_parse_url()). */
static int br_resolve_href(br_tab_t *t, const char *href) {
    if (href[0] == '#' || href[0] == 0) return 0;
    if (strncmp(href, "mailto:", 7) == 0 || strncmp(href, "javascript:", 11) == 0) return 0;

    if (strncmp(href, "https://", 8) == 0 || strncmp(href, "http://", 7) == 0) {
        strncpy(t->url, href, BR_MAX_URL - 1);
        t->url[BR_MAX_URL - 1] = 0;
        t->url_len = (int)strlen(t->url);
        return 1;
    }

    char host[64], path[64];
    uint16_t port;
    int is_https;
    br_parse_url(t->url, host, sizeof(host), &port, path, sizeof(path), &is_https);

    char new_path[64];
    br_join_path(path, href, new_path, sizeof(new_path));

    const char *scheme = is_https ? "https://" : "http://";
    int n = (int)strlen(scheme);
    memcpy(t->url, scheme, (size_t)n);
    int hl = (int)strlen(host);
    if (n + hl < BR_MAX_URL) { memcpy(t->url + n, host, (size_t)hl); n += hl; }
    int default_port = is_https ? 443 : 80;
    if (port != default_port && n < BR_MAX_URL - 8) {
        char portbuf[8];
        utoa(port, portbuf);
        t->url[n++] = ':';
        int pl = (int)strlen(portbuf);
        if (n + pl < BR_MAX_URL) { memcpy(t->url + n, portbuf, (size_t)pl); n += pl; }
    }
    int pl = (int)strlen(new_path);
    if (n + pl < BR_MAX_URL) { memcpy(t->url + n, new_path, (size_t)pl); n += pl; }
    t->url[n] = 0;
    t->url_len = n;
    return 1;
}

static void br_navigate(const char *href) {
    br_tab_t *t = br_active();
    if (br_resolve_href(t, href)) br_fetch();
}

/* Re-runs layout against the (possibly JS-mutated) live DOM tree and
 * stylesheet -- called after an onclick handler changes innerHTML,
 * textContent, or style. Does not touch scroll position or the DOM
 * tree/stylesheet themselves. */
static void br_relayout(void) {
    br_tab_t *t = br_active();
    if (!t->dom_root || !t->stylesheet_valid) return;
    int content_x, content_y, content_w, content_h;
    br_content_area(&content_x, &content_y, &content_w, &content_h);
    layout_run(t->dom_root, &t->stylesheet, content_w, &t->layout);
}

/* Tab strip geometry -- tab `i`'s rect, and the "+" (new tab) button
 * that follows the last one. The rightmost BR_TAB_CLOSE_W of each tab
 * is its close-box hit area (see br_handle_click); drawing mirrors this
 * exactly (see draw_browser_tabstrip) so the "x" glyph always lands
 * inside its own hit box. */
static void br_tab_rect(const gui_window_t *w, int i, int *x, int *y, int *tw, int *th) {
    *x = w->x + 10 + i * (BR_TAB_W + BR_TAB_GAP);
    *y = br_tabstrip_y(w);
    *tw = BR_TAB_W;
    *th = BR_TAB_H;
}

static void br_tab_plus_rect(const gui_window_t *w, int *x, int *y, int *tw, int *th) {
    *x = w->x + 10 + br_tab_count * (BR_TAB_W + BR_TAB_GAP);
    *y = br_tabstrip_y(w);
    *tw = 20;
    *th = BR_TAB_H;
}

/* Back/forward/star(bookmark)/bookmarks-list buttons, packed left of
 * the URL field in that order (index 0..3) -- index 4 isn't a real
 * button, but reusing this formula for "one slot past the last button"
 * is exactly where the URL field's left edge belongs (see
 * br_urlfield_rect), so there's only one place that ever has to agree
 * on button width/gap. */
static void br_navbtn_rect(const gui_window_t *w, int index, int *x, int *y, int *bw, int *bh) {
    *x = w->x + 10 + index * (BR_NAVBTN_W + BR_NAVBTN_GAP);
    *y = br_urlrow_y(w);
    *bw = BR_NAVBTN_W;
    *bh = BR_URL_ROW_H;
}

static void br_urlfield_rect(const gui_window_t *w, int *x, int *y, int *fw, int *fh) {
    int bx, by, bw, bh;
    br_navbtn_rect(w, 4, &bx, &by, &bw, &bh);
    *x = bx;
    *y = by;
    *fw = (w->x + w->w - 10) - bx;
    *fh = BR_URL_ROW_H;
}

/* Bookmarks dropdown, anchored under the bookmarks-list button (index
 * 3) and floating on top of the page content -- it doesn't need its
 * own scroll, BR_MAX_BOOKMARKS is small enough that `rows` just clips
 * to however many actually fit in the window. */
static void br_bookmarks_panel_rect(const gui_window_t *w, int *x, int *y, int *pw, int *ph, int *rows) {
    int bx, by, bw, bh;
    br_navbtn_rect(w, 3, &bx, &by, &bw, &bh);
    *x = bx;
    *y = by + bh + 2;
    *pw = 220;
    if (*x + *pw > w->x + w->w - 6) *pw = (w->x + w->w - 6) - *x;

    int max_rows = (w->y + w->h - *y - 6) / BR_BM_ROW_H;
    *rows = br_bookmark_count < max_rows ? br_bookmark_count : max_rows;
    if (*rows < 1) *rows = 1;
    *ph = *rows * BR_BM_ROW_H + 6;
}

/* Returns the clicked bookmark's index, -1 if the click landed inside
 * the panel but not on a valid row (an empty list, or padding), or -2
 * if it missed the panel entirely -- callers use -2 to tell "dismiss,
 * the user clicked away" apart from "dismiss, they clicked a blank
 * row", though both currently just close the panel without navigating. */
static int br_bookmarks_hit_test(const gui_window_t *w, int mx, int my) {
    int px, py, pw, ph, rows;
    br_bookmarks_panel_rect(w, &px, &py, &pw, &ph, &rows);
    if (mx < px || mx >= px + pw || my < py || my >= py + ph) return -2;
    if (br_bookmark_count == 0) return -1;
    int row = (my - py - 3) / BR_BM_ROW_H;
    if (row < 0 || row >= rows) return -1;
    return row;
}

static void br_handle_click(const gui_window_t *w, int mx, int my) {
    if (br_bookmarks_open) {
        int hit = br_bookmarks_hit_test(w, mx, my);
        br_bookmarks_open = 0;
        if (hit >= 0) br_navigate(br_bookmarks[hit]);
        return;
    }

    int ts_y = br_tabstrip_y(w);
    if (my >= ts_y && my < ts_y + BR_TAB_H) {
        for (int i = 0; i < br_tab_count; i++) {
            int tx, ty, tw_, th_;
            br_tab_rect(w, i, &tx, &ty, &tw_, &th_);
            if (mx < tx || mx >= tx + tw_) continue;
            if (mx >= tx + tw_ - BR_TAB_CLOSE_W) br_tab_close(i);
            else br_tab_switch(i);
            return;
        }
        int px, py, pw_, ph_;
        br_tab_plus_rect(w, &px, &py, &pw_, &ph_);
        if (mx >= px && mx < px + pw_) br_tab_open();
        return;
    }

    int url_y = br_urlrow_y(w);
    if (my >= url_y && my < url_y + BR_URL_ROW_H) {
        br_tab_t *t = br_active();
        int bx, by, bw, bh;
        br_navbtn_rect(w, 0, &bx, &by, &bw, &bh);
        if (mx >= bx && mx < bx + bw) { br_editing_url = 0; br_go_back(); return; }
        br_navbtn_rect(w, 1, &bx, &by, &bw, &bh);
        if (mx >= bx && mx < bx + bw) { br_editing_url = 0; br_go_forward(); return; }
        br_navbtn_rect(w, 2, &bx, &by, &bw, &bh);
        if (mx >= bx && mx < bx + bw) { br_editing_url = 0; br_bookmark_toggle(t->url); return; }
        br_navbtn_rect(w, 3, &bx, &by, &bw, &bh);
        if (mx >= bx && mx < bx + bw) { br_editing_url = 0; br_bookmarks_open = 1; return; }
        br_editing_url = 1;
        return;
    }
    br_editing_url = 0;

    br_tab_t *t = br_active();
    int content_x, content_y, content_w, content_h;
    br_content_area(&content_x, &content_y, &content_w, &content_h);
    if (mx < content_x || my < content_y) return;

    int doc_x = mx - content_x;
    int doc_y = (my - content_y) + t->scroll;

    for (int i = 0; i < t->layout.item_count; i++) {
        const struct layout_item *it = &t->layout.items[i];
        if (it->type != LAYOUT_ITEM_TEXT && it->type != LAYOUT_ITEM_IMAGE) continue;
        if (doc_x < it->x || doc_x >= it->x + it->w || doc_y < it->y || doc_y >= it->y + it->h) continue;

        if (it->link_id >= 0) {
            br_navigate(t->layout.links[it->link_id].href);
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

/* Ctrl+T/Ctrl+W arrive here as ordinary characters -- 0x14/0x17, the
 * same Ctrl+letter -> C0 control code mapping drivers/keyboard.c uses
 * for the text editor's Ctrl+S -- so they're handled up front,
 * independent of br_editing_url, and gated on the Browser actually
 * being the frontmost window (so typing Ctrl+T while the Terminal has
 * focus doesn't reach into the Browser). Alt+Left/Alt+Right (back/
 * forward) have no ASCII form at all -- see gui_run()'s own raw-
 * scancode handling for those, right next to the up/down-arrow scroll
 * keys this same window already responds to. */
static void br_handle_key(char c) {
    if (br_window_idx < 0) return;
    if (c == 0x14 || c == 0x17) {
        if (gui_frontmost_window() == br_window_idx) {
            if (c == 0x14) br_tab_open();
            else br_tab_close(br_active_tab);
        }
        return;
    }

    if (!br_editing_url) return;
    br_tab_t *t = br_active();
    if (c == '\n' || c == '\r') {
        br_maybe_search(t);
        br_fetch();
    } else if (c == '\b') {
        if (t->url_len > 0) t->url_len--;
    } else if (c >= 32 && c < 127 && t->url_len < BR_MAX_URL - 1) {
        t->url[t->url_len++] = c;
    }
    t->url[t->url_len] = 0;
}

/* Tab labels are the page title if one loaded, else the raw URL --
 * truncated hard (no ellipsis) to whatever fits left of the close box,
 * the same "just cut it off" simplification the rest of this file uses
 * for fixed-width text (see e.g. fm_derive... any of the FM listing
 * lines). */
static void br_tab_label(const br_tab_t *t, char *out, int cap) {
    const char *src = t->layout.title[0] ? t->layout.title : t->url;
    int i = 0;
    while (src[i] && i < cap - 1) { out[i] = src[i]; i++; }
    out[i] = 0;
}

static void draw_browser_tabstrip(const gui_window_t *w) {
    for (int i = 0; i < br_tab_count; i++) {
        int tx, ty, tw_, th_;
        br_tab_rect(w, i, &tx, &ty, &tw_, &th_);
        int active = (i == br_active_tab);
        fb_fill_rect(tx, ty, tw_, th_, active ? 0x2A3360 : 0x161B38);
        fb_draw_rect(tx, ty, tw_, th_, active ? 0x62D8FF : 0x3A4270);

        char label[8];
        br_tab_label(&br_tabs[i], label, sizeof(label));
        fb_draw_string(tx + 3, ty + 5, label, active ? COL_TEXT : COL_MUTED, 1);

        int cx = tx + tw_ - BR_TAB_CLOSE_W;
        fb_draw_string(cx + 3, ty + 5, "x", 0xE05252, 1);
    }

    if (br_tab_count < BR_MAX_TABS) {
        int px, py, pw_, ph_;
        br_tab_plus_rect(w, &px, &py, &pw_, &ph_);
        fb_fill_rect(px, py, pw_, ph_, 0x161B38);
        fb_draw_rect(px, py, pw_, ph_, 0x3A4270);
        fb_draw_string(px + 6, py + 5, "+", 0x8FE3A8, 1);
    }
}

static void draw_browser_bookmarks_panel(const gui_window_t *w) {
    int px, py, pw_, ph_, rows;
    br_bookmarks_panel_rect(w, &px, &py, &pw_, &ph_, &rows);
    fb_fill_rect(px, py, pw_, ph_, 0x10142C);
    fb_draw_rect(px, py, pw_, ph_, 0x62D8FF);

    if (br_bookmark_count == 0) {
        fb_draw_string(px + 4, py + 4, "No bookmarks yet", COL_MUTED, 1);
        return;
    }
    for (int i = 0; i < rows; i++) {
        fb_draw_string(px + 4, py + 3 + i * BR_BM_ROW_H, br_bookmarks[i], COL_TEXT, 1);
    }
}

static void draw_browser(const gui_window_t *w) {
    br_tab_t *t = br_active();
    int x = w->x + 10;

    draw_browser_tabstrip(w);

    int url_y = br_urlrow_y(w);
    int bx, by, bw, bh;

    br_navbtn_rect(w, 0, &bx, &by, &bw, &bh);
    int can_back = t->history_pos > 0;
    fb_fill_rect(bx, by, bw, bh, 0x0D131C);
    fb_draw_rect(bx, by, bw, bh, can_back ? 0x3A4270 : 0x20264A);
    fb_draw_string(bx + 5, by + 6, "<", can_back ? COL_TEXT : COL_MUTED, 1);

    br_navbtn_rect(w, 1, &bx, &by, &bw, &bh);
    int can_fwd = t->history_pos >= 0 && t->history_pos < t->history_count - 1;
    fb_fill_rect(bx, by, bw, bh, 0x0D131C);
    fb_draw_rect(bx, by, bw, bh, can_fwd ? 0x3A4270 : 0x20264A);
    fb_draw_string(bx + 5, by + 6, ">", can_fwd ? COL_TEXT : COL_MUTED, 1);

    int bookmarked = br_bookmark_is_set(t->url);
    br_navbtn_rect(w, 2, &bx, &by, &bw, &bh);
    fb_fill_rect(bx, by, bw, bh, 0x0D131C);
    fb_draw_rect(bx, by, bw, bh, bookmarked ? 0xF2C14E : 0x3A4270);
    fb_draw_string(bx + 5, by + 6, "*", bookmarked ? 0xF2C14E : COL_MUTED, 1);

    br_navbtn_rect(w, 3, &bx, &by, &bw, &bh);
    fb_fill_rect(bx, by, bw, bh, 0x0D131C);
    fb_draw_rect(bx, by, bw, bh, br_bookmarks_open ? 0x62D8FF : 0x3A4270);
    fb_draw_string(bx + 5, by + 6, "v", COL_TEXT, 1);

    int fx, fy, fw_, fh_;
    br_urlfield_rect(w, &fx, &fy, &fw_, &fh_);
    fb_fill_rect(fx, fy, fw_, fh_, 0x0D131C);
    fb_draw_rect(fx, fy, fw_, fh_, br_editing_url ? 0x62D8FF : 0x3A4270);
    fb_draw_string(fx + 4, fy + 6, t->url, COL_TEXT, 1);

    fb_draw_string(x, url_y + 26, t->status_msg, COL_MUTED, 1);

    if (br_bookmarks_open) draw_browser_bookmarks_panel(w);

    if (!net_is_up()) {
        fb_draw_string(x, url_y + 44, "no NIC detected", 0xE05252, 1);
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

    int max_scroll = t->layout.content_height - content_h;
    if (max_scroll < 0) max_scroll = 0;
    if (t->scroll > max_scroll) t->scroll = max_scroll;
    if (t->scroll < 0) t->scroll = 0;

    /* Four passes so backgrounds always sit under images/rules/text,
     * regardless of the order layout emitted them in. */
    for (int pass = 0; pass < 4; pass++) {
        enum layout_item_type want = pass == 0 ? LAYOUT_ITEM_RECT :
                                      pass == 1 ? LAYOUT_ITEM_IMAGE :
                                      pass == 2 ? LAYOUT_ITEM_HR : LAYOUT_ITEM_TEXT;
        for (int i = 0; i < t->layout.item_count; i++) {
            const struct layout_item *it = &t->layout.items[i];
            if (it->type != want) continue;
            if (it->y + it->h < t->scroll || it->y > t->scroll + content_h) continue;

            int sx = content_x + it->x;
            int sy = content_y + (it->y - t->scroll);
            if (it->type == LAYOUT_ITEM_RECT) {
                if (it->has_gradient && it->radius <= 0) {
                    if (it->gradient_horizontal) fb_fill_gradient_h(sx, sy, it->w, it->h, it->color, it->color2);
                    else fb_fill_gradient_v(sx, sy, it->w, it->h, it->color, it->color2);
                } else if (it->radius > 0) {
                    /* A gradient AND rounded corners together would need
                     * a rounded-rect gradient fill this codebase doesn't
                     * have -- radius wins (matches this browser's
                     * existing "approximate, don't crash" tolerance for
                     * combinations it can't render exactly). */
                    fb_fill_rounded_rect(sx, sy, it->w, it->h, it->radius, it->color);
                } else {
                    fb_fill_rect(sx, sy, it->w, it->h, it->color);
                }
            }
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

/* Window/badge corner radius, shared by draw_window()'s nested-rounded-
 * rect trick and draw_taskbar_icon()'s badge fill, so the chrome reads
 * as one consistent rounding across dock + titlebars. */
#define CHROME_RADIUS 7

static void draw_window(const gui_window_t *w, int focused) {
    draw_shadow(w->x, w->y, w->w, w->h);

    uint32_t accent_dark = ((w->accent >> 1) & 0x7F7F7F);
    uint32_t border_color = focused ? w->accent : accent_dark;

    /* Rounded corners: there's no rounded-stroke or rounded-gradient
     * primitive, only fb_fill_rounded_rect (solid fill). Fake the look
     * with two nested filled rounded rects -- a full-window-sized one in
     * the border/accent color, then a smaller inset one (in the body
     * color) on top -- so only the outer rect's corner arcs remain
     * visible as a rounded frame. The gradient title bar and body content
     * below are then drawn as ordinary rects, inset just enough that
     * their own square corners fall inside the already-rounded frame
     * rather than redrawing square corners over it. */
    fb_fill_rounded_rect(w->x, w->y, w->w, w->h, CHROME_RADIUS, border_color);
    fb_fill_rounded_rect(w->x + 2, w->y + 2, w->w - 4, w->h - 4,
                          CHROME_RADIUS > 2 ? CHROME_RADIUS - 2 : 0, 0x1B2040);

    /* title bar */
    fb_fill_gradient_v(w->x + CHROME_RADIUS, w->y + 2, w->w - 2 * CHROME_RADIUS, TITLEBAR_H - 2,
                        focused ? w->accent : accent_dark,
                        focused ? accent_dark : (accent_dark >> 1) & 0x7F7F7F);

    /* titlebar icon + title text, icon tinted white same as focused/open
     * dock icons -- text shifted right to make room. */
    int ticon_size = 16;
    int ticon_x = w->x + 8, ticon_y = w->y + (TITLEBAR_H - ticon_size) / 2;
    svgicon_draw((enum svg_icon_id)w->icon_id, ticon_x, ticon_y, ticon_size, 0xFFFFFF, 255);
    fb_draw_string(ticon_x + ticon_size + 6, w->y + 10, w->title, 0xFFFFFF, 1);

    /* close button */
    int cbx = w->x + w->w - 20, cby = w->y + 8;
    fb_fill_rect(cbx, cby, 12, 12, 0xE05252);
    fb_draw_line(cbx + 2, cby + 2, cbx + 9, cby + 9, 0xFFFFFF);
    fb_draw_line(cbx + 9, cby + 2, cbx + 2, cby + 9, 0xFFFFFF);

    if (w->body_line1) fb_draw_string(w->x + 14, w->y + TITLEBAR_H + 16, w->body_line1, COL_TEXT, 1);
    if (w->body_line2) fb_draw_string(w->x + 14, w->y + TITLEBAR_H + 34, w->body_line2, COL_MUTED, 1);
    if (w->is_smp_monitor) draw_smp_monitor(w);
    if (w->is_process_monitor) draw_process_monitor(w);
    if (w->is_network) draw_network(w);
    if (w->is_file_manager) draw_file_manager(w);
    if (w->is_browser) draw_browser(w);
    if (w->is_terminal) draw_terminal(w);
}

/* Alpha-composites an app window's own w*h 0xAARRGGBB pixel buffer onto
 * the desktop at (x, y), top-left-anchored -- the replacement for the
 * old fb_blit_rgb() opaque nearest-neighbor copy, now that the top byte
 * is a meaningful per-pixel alpha (see gui_app_window_blit()'s doc
 * comment in include/gui/compositor.h). alpha=0 is skipped entirely
 * (cheap early-out for a mostly-transparent window, e.g. a borderless
 * desktop pet's background) rather than calling fb_blend_pixel() for a
 * no-op 0%-opacity blend; alpha=255 calls through exactly like the old
 * opaque blit did (fb_blend_pixel() with alpha=255 fully replaces the
 * destination pixel), so a caller that always writes alpha=255 (every
 * existing bordered app-window user) renders pixel-identically to
 * before this change. */
static void blit_app_window_pixels(int x, int y, const app_window_t *win) {
    for (uint32_t row = 0; row < win->h; row++) {
        for (uint32_t col = 0; col < win->w; col++) {
            uint32_t p = win->pixels[row * win->w + col];
            uint8_t alpha = (uint8_t)(p >> 24);
            if (alpha == 0) continue;
            fb_blend_pixel(x + (int)col, y + (int)row, p & 0x00FFFFFF, alpha);
        }
    }
}

/* Draws every currently-open app window (see the app_windows[] table
 * and gui_app_window_open()/gui_app_window_blit() above), and reclaims
 * any whose owning task has exited -- called once per frame from
 * draw_frame(), the exact same "poll scheduler_task_state() once a
 * frame" pattern fs_active/fs_owner_pid already uses for SYS_BLIT's
 * fullscreen takeover. NOT called at all while fs_active is set (see
 * draw_frame()'s early return), matching how the fullscreen path
 * already skips every other kind of drawing.
 *
 * Bordered windows (flags=0, the default) reuse draw_window()'s
 * nested-rounded-rect trick and CHROME_RADIUS so these read as the same
 * visual family as the 8 built-in windows, just with a fixed cascaded
 * position (by slot index) instead of a draggable one -- no drag
 * support, no close button, no dock icon for these in this pass
 * (deliberate scope cuts, follow-on work: a real window manager for app
 * windows). Borderless windows (WIN_FLAG_BORDERLESS) skip every bit of
 * that chrome -- no shadow, no rounded frame, no title bar/text -- and
 * just alpha-composite their own pixel buffer directly at the slot's
 * (x, y), with no inset/offset at all: the whole point is floating
 * un-boxed pixels directly on the desktop (a "desktop pet"), not a
 * window with an invisible border. */
static void draw_app_windows(void) {
    for (int i = 0; i < APP_WINDOW_MAX; i++) {
        app_window_t *win = &app_windows[i];
        if (!win->in_use) continue;

        if (scheduler_task_state(win->owner_pid) == TASK_TERMINATED) {
            kfree(win->pixels);
            memset(win, 0, sizeof(*win));
            continue;
        }

        /* Fixed absolute offsets (NOT the SX()/SY() desktop-scaling
         * helpers the built-in windows use) so the worst case -- slot 3,
         * both dimensions at the APP_WINDOW_MAX_W/H cap -- still lands
         * safely clear of the taskbar even on the smallest resolution
         * this kernel supports (1024x768, where SX()/SY() are a no-op):
         * x_max = 40+3*30+404 = 534 <= 1024; y_max = 40+3*30+334 = 464,
         * comfortably above a 1024x768 desktop's taskbar top at 724. */
        int x = 40 + i * 30;
        int y = 40 + i * 30;

        if (win->is_borderless) {
            blit_app_window_pixels(x, y, win);
            continue;
        }

        int w = (int)win->w + 4;  /* +4 = the 2px inset border on each side, matching draw_window()'s nested-rect trick */
        int h = (int)win->h + TITLEBAR_H + 6;

        draw_shadow(x, y, w, h);
        fb_fill_rounded_rect(x, y, w, h, CHROME_RADIUS, 0x3E6FF0);
        fb_fill_rounded_rect(x + 2, y + 2, w - 4, h - 4,
                              CHROME_RADIUS > 2 ? CHROME_RADIUS - 2 : 0, 0x1B2040);
        fb_fill_gradient_v(x + CHROME_RADIUS, y + 2, w - 2 * CHROME_RADIUS, TITLEBAR_H - 2,
                            0x3E6FF0, 0x1F3878);
        fb_draw_string(x + 10, y + 10, win->title, 0xFFFFFF, 1);

        blit_app_window_pixels(x + 2, y + TITLEBAR_H + 2, win);
    }
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
    fb_fill_rounded_rect(ix, iy, iw, ih, CHROME_RADIUS, bg);

    /* vector icon centered in the slot, leaving margin inside
     * TASKBAR_ICON_W x TASKBAR_ICON_H and room below for the focus
     * underline -- tinted white + full alpha when open/focused, muted +
     * dimmed when closed (via alpha_scale, no second cached variant). */
    int icon_size = 20;
    int icx = ix + (iw - icon_size) / 2;
    int icy = iy + (ih - icon_size) / 2 - 1;
    uint32_t tint = w->open ? 0xFFFFFF : 0x7A83A8;
    uint8_t alpha_scale = w->open ? 255 : 140;
    svgicon_draw((enum svg_icon_id)w->icon_id, icx, icy, icon_size, tint, alpha_scale);

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

    draw_app_windows();

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
                /* Clicking anywhere deselects every text-input widget; the
                 * specific click handler below re-focuses its own if the
                 * click actually landed on it. */
                br_editing_url = 0;
                if (fm_viewing_file) fm_editing = 0;
                term_focused = 0;

                if (!hit_titlebar) {
                    for (int oi = window_count - 1; oi >= 0; oi--) {
                        int wi = window_order[oi];
                        gui_window_t *w = &windows[wi];
                        if (!w->open) continue;
                        if (mx >= w->x && mx < w->x + w->w && my >= w->y + TITLEBAR_H && my < w->y + w->h) {
                            bring_to_front(oi);
                            if (w->is_file_manager) fm_handle_click(w, my);
                            else if (w->is_browser) br_handle_click(w, mx, my);
                            else if (w->is_terminal) term_handle_click();
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
            term_handle_key(c);
        }

        static int scroll_cooldown = 0;
        if (scroll_cooldown > 0) {
            scroll_cooldown--;
        } else if (keyboard_key_pressed(0x48)) { /* up arrow */
            br_active()->scroll -= LAYOUT_LINE_H;
            if (br_active()->scroll < 0) br_active()->scroll = 0;
            scroll_cooldown = 4;
        } else if (keyboard_key_pressed(0x50)) { /* down arrow */
            br_active()->scroll += LAYOUT_LINE_H;
            scroll_cooldown = 4;
        }

        /* Alt+Left/Alt+Right for back/forward -- mouse-driven testing
         * (small on-screen back/forward buttons, PS/2 delta jitter) is
         * fiddly enough in this environment that history needs a
         * keyboard path independent of them. Only fires while the
         * Browser is the frontmost window, same gate br_handle_key()
         * uses for Ctrl+T/Ctrl+W. keyboard_alt_held() mirrors
         * ctrl_held/shift_held in drivers/keyboard.c. */
        static int nav_key_cooldown = 0;
        if (nav_key_cooldown > 0) {
            nav_key_cooldown--;
        } else if (br_window_idx >= 0 && keyboard_alt_held() && gui_frontmost_window() == br_window_idx) {
            if (keyboard_key_pressed(0x4B)) { br_go_back(); nav_key_cooldown = 10; }
            else if (keyboard_key_pressed(0x4D)) { br_go_forward(); nav_key_cooldown = 10; }
        }

        /* WebSocket messages/close, checked once per frame -- the same
         * "polled from gui_run() itself" pattern fs_owner_pid/
         * term_owner_pid above already use for noticing state that
         * changed off in some other subsystem between frames, just for
         * incoming network frames instead of task exits. There's no
         * event loop anywhere in this engine (see js/dom_binding.c's
         * onclick dispatch for the only other precedent: a native call
         * invoking a JS handler inline, synchronously, rather than
         * queuing it) -- this is that same model, just triggered by "did
         * a WebSocket frame arrive" instead of "did a click happen."
         * js_dom_ws_poll() is a no-op whenever the current page never
         * opened a WebSocket, so this doesn't need its own gate. */
        js_dom_ws_poll();

        draw_frame(mx, my);
        fb_swap_buffers();
        pit_sleep(16);
    }
}
