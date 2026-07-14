/* Bookkeeping shared across the compiler's stages: the growable
 * section buffers, the relocation list, and the function/global symbol
 * tables. See include/cc/module.h. */
#include <cc/module.h>
#include <kernel/kheap.h>
#include <string.h>
#include <stdarg.h>

/* Freestanding build has no vsnprintf; this subset of format specifiers
 * (%s, %d, %c, %%) is all cc_module_errorf's call sites ever use. */
static void tiny_vformat(char *out, uint32_t cap, const char *fmt, va_list ap) {
    uint32_t o = 0;
    for (uint32_t i = 0; fmt[i] && o + 1 < cap; i++) {
        if (fmt[i] != '%') { out[o++] = fmt[i]; continue; }
        i++;
        if (fmt[i] == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && o + 1 < cap) out[o++] = *s++;
        } else if (fmt[i] == 'd') {
            int v = va_arg(ap, int);
            char tmp[12];
            int n = 0;
            unsigned int u = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;
            if (v < 0 && o + 1 < cap) out[o++] = '-';
            if (u == 0) tmp[n++] = '0';
            while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
            while (n > 0 && o + 1 < cap) out[o++] = tmp[--n];
        } else if (fmt[i] == 'c') {
            char c = (char)va_arg(ap, int);
            if (o + 1 < cap) out[o++] = c;
        } else if (fmt[i] == '%') {
            out[o++] = '%';
        } else if (fmt[i]) {
            out[o++] = fmt[i];
        }
    }
    out[o] = 0;
}

void cc_module_init(struct cc_module *m) {
    memset(m, 0, sizeof(*m));
    cc_buf_init(&m->text);
    cc_buf_init(&m->rodata);
    cc_buf_init(&m->data);
}

void cc_module_free(struct cc_module *m) {
    cc_buf_free(&m->text);
    cc_buf_free(&m->rodata);
    cc_buf_free(&m->data);
    struct cc_arena_block *b = m->arena;
    while (b) {
        struct cc_arena_block *n = b->next;
        kfree(b);
        b = n;
    }
    m->arena = NULL;
}

#define CC_ARENA_BLOCK_SIZE (16u * 1024)

void *cc_alloc(struct cc_module *m, uint32_t size) {
    size = (size + 15u) & ~15u; /* keep everything pointer-aligned */
    struct cc_arena_block *b = m->arena;
    if (!b || b->len + size > b->cap) {
        uint32_t cap = size > CC_ARENA_BLOCK_SIZE ? size : CC_ARENA_BLOCK_SIZE;
        struct cc_arena_block *nb = (struct cc_arena_block *)kmalloc(sizeof(struct cc_arena_block) + cap);
        nb->next = m->arena;
        nb->len = 0;
        nb->cap = cap;
        m->arena = nb;
        b = nb;
    }
    void *p = b->data + b->len;
    b->len += size;
    memset(p, 0, size);
    return p;
}

void cc_module_errorf(struct cc_module *m, int line, const char *fmt, ...) {
    if (m->error) return; /* keep the first error -- most useful one to report */
    m->error = 1;
    char body[CC_ERR_LEN - 24];
    va_list ap;
    va_start(ap, fmt);
    tiny_vformat(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (line > 0) {
        char linestr[16];
        int n = 0;
        unsigned int u = (unsigned int)line;
        char tmp[12];
        if (u == 0) tmp[n++] = '0';
        while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
        int o = 0;
        linestr[o++] = 'l'; linestr[o++] = 'i'; linestr[o++] = 'n'; linestr[o++] = 'e'; linestr[o++] = ' ';
        while (n > 0) linestr[o++] = tmp[--n];
        linestr[o++] = ':'; linestr[o++] = ' '; linestr[o] = 0;
        uint32_t i = 0;
        for (; linestr[i] && i < sizeof(m->errmsg) - 1; i++) m->errmsg[i] = linestr[i];
        uint32_t j = 0;
        for (; body[j] && i < sizeof(m->errmsg) - 1; i++, j++) m->errmsg[i] = body[j];
        m->errmsg[i] = 0;
    } else {
        strncpy(m->errmsg, body, sizeof(m->errmsg) - 1);
        m->errmsg[sizeof(m->errmsg) - 1] = 0;
    }
}

uint32_t cc_add_rodata_string(struct cc_module *m, const char *bytes, uint32_t len) {
    uint32_t off = m->rodata.len;
    cc_buf_push_bytes(&m->rodata, bytes, len);
    cc_buf_push_byte(&m->rodata, 0);
    return off;
}

void cc_add_reloc(struct cc_module *m, uint32_t text_offset, enum cc_section section, uint32_t offset_in_section) {
    if (m->reloc_count >= CC_MAX_RELOCS) {
        cc_module_errorf(m, 0, "internal limit: too many relocations");
        return;
    }
    struct cc_reloc *r = &m->relocs[m->reloc_count++];
    r->text_offset = text_offset;
    r->section = section;
    r->offset_in_section = offset_in_section;
}

struct cc_func *cc_find_func(struct cc_module *m, const char *name) {
    for (int i = 0; i < m->func_count; i++) {
        if (strcmp(m->funcs[i].name, name) == 0) return &m->funcs[i];
    }
    return NULL;
}

struct cc_func *cc_add_func(struct cc_module *m, const char *name) {
    if (m->func_count >= CC_MAX_FUNCS) {
        cc_module_errorf(m, 0, "internal limit: too many functions");
        return NULL;
    }
    struct cc_func *f = &m->funcs[m->func_count++];
    memset(f, 0, sizeof(*f));
    strncpy(f->name, name, sizeof(f->name) - 1);
    return f;
}

struct cc_global *cc_find_global(struct cc_module *m, const char *name) {
    for (int i = 0; i < m->global_count; i++) {
        if (strcmp(m->globals[i].name, name) == 0) return &m->globals[i];
    }
    return NULL;
}

struct cc_global *cc_add_global(struct cc_module *m, const char *name) {
    if (m->global_count >= CC_MAX_GLOBALS) {
        cc_module_errorf(m, 0, "internal limit: too many global variables");
        return NULL;
    }
    struct cc_global *g = &m->globals[m->global_count++];
    memset(g, 0, sizeof(*g));
    strncpy(g->name, name, sizeof(g->name) - 1);
    return g;
}
