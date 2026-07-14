#include <gui/editor.h>
#include <kernel/kheap.h>
#include <fs/fat32.h>
#include <string.h>

static void copy_name(char *dst, const char *src) {
    int i = 0;
    if (!src) src = "UNTITLED.TXT";
    while (src[i] && i < NOVA_EDITOR_NAME - 1) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        dst[i] = c;
        i++;
    }
    dst[i] = 0;
}

static int find_file(uint32_t cwd, const char *name, struct fat_dirent_info *out) {
    struct fat_dirent_info entries[64];
    int count = fat32_list_dir(cwd, entries, 64);
    for (int i = 0; i < count; i++) {
        if (!entries[i].is_dir && strcmp(entries[i].name, name) == 0) {
            if (out) *out = entries[i];
            return 1;
        }
    }
    return 0;
}

void nova_editor_init(struct nova_editor *e, uint32_t cwd) {
    memset(e, 0, sizeof(*e));
    e->text = (char *)kmalloc(NOVA_EDITOR_CAP + 1);
    e->cwd = cwd;
    if (e->text) e->text[0] = 0;
    copy_name(e->filename, "UNTITLED.TXT");
}

void nova_editor_destroy(struct nova_editor *e) {
    if (e->text) kfree(e->text);
    memset(e, 0, sizeof(*e));
}

int nova_editor_new(struct nova_editor *e, const char *name) {
    if (!e || !e->text) return 0;
    e->len = 0;
    e->cursor = 0;
    e->scroll_line = 0;
    e->text[0] = 0;
    e->dirty = 0;
    e->loaded = 1;
    copy_name(e->filename, name);
    return 1;
}

int nova_editor_open(struct nova_editor *e, const char *name) {
    if (!e || !e->text || !fat32_is_mounted()) return 0;
    char upper[NOVA_EDITOR_NAME];
    copy_name(upper, name);
    struct fat_dirent_info info;
    if (!find_file(e->cwd, upper, &info)) return 0;
    uint32_t cap = info.size < NOVA_EDITOR_CAP ? info.size : NOVA_EDITOR_CAP;
    uint32_t got = fat32_read_file(info.cluster, info.size, e->text, cap);
    e->text[got] = 0;
    e->len = got;
    e->cursor = got;
    e->scroll_line = 0;
    e->dirty = 0;
    e->loaded = 1;
    copy_name(e->filename, upper);
    return 1;
}

int nova_editor_save(struct nova_editor *e) {
    if (!e || !e->text || !e->loaded || !fat32_is_mounted()) return 0;
    if (!fat32_write_file(e->cwd, e->filename, e->text, e->len)) return 0;
    e->dirty = 0;
    return 1;
}

int nova_editor_insert(struct nova_editor *e, char c) {
    if (!e || !e->text || !e->loaded || e->len >= NOVA_EDITOR_CAP) return 0;
    memmove(e->text + e->cursor + 1, e->text + e->cursor, e->len - e->cursor + 1);
    e->text[e->cursor++] = c;
    e->len++;
    e->dirty = 1;
    return 1;
}

int nova_editor_backspace(struct nova_editor *e) {
    if (!e || !e->text || !e->loaded || e->cursor == 0) return 0;
    memmove(e->text + e->cursor - 1, e->text + e->cursor, e->len - e->cursor + 1);
    e->cursor--;
    e->len--;
    e->dirty = 1;
    return 1;
}

void nova_editor_move(struct nova_editor *e, int delta) {
    if (!e || !e->loaded) return;
    int next = (int)e->cursor + delta;
    if (next < 0) next = 0;
    if ((uint32_t)next > e->len) next = (int)e->len;
    e->cursor = (uint32_t)next;
}

uint32_t nova_editor_line_count(const struct nova_editor *e) {
    if (!e || !e->loaded) return 0;
    uint32_t lines = 1;
    for (uint32_t i = 0; i < e->len; i++) if (e->text[i] == '\n') lines++;
    return lines;
}
