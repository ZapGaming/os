#ifndef GUI_EDITOR_H
#define GUI_EDITOR_H

#include <stdint.h>

#define NOVA_EDITOR_CAP (64u * 1024u)
#define NOVA_EDITOR_NAME 16

struct nova_editor {
    char *text;
    uint32_t len;
    uint32_t cursor;
    uint32_t scroll_line;
    uint32_t cwd;
    int dirty;
    int loaded;
    char filename[NOVA_EDITOR_NAME];
};

void nova_editor_init(struct nova_editor *editor, uint32_t cwd);
void nova_editor_destroy(struct nova_editor *editor);
int nova_editor_new(struct nova_editor *editor, const char *name);
int nova_editor_open(struct nova_editor *editor, const char *name);
int nova_editor_save(struct nova_editor *editor);
int nova_editor_insert(struct nova_editor *editor, char c);
int nova_editor_backspace(struct nova_editor *editor);
void nova_editor_move(struct nova_editor *editor, int delta);
uint32_t nova_editor_line_count(const struct nova_editor *editor);

#endif
