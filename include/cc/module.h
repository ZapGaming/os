#ifndef CC_MODULE_H
#define CC_MODULE_H

#include <cc/ast.h>
#include <cc/emit.h>
#include <stdint.h>

#define CC_MAX_PARAMS 8
#define CC_MAX_FUNCS 64
#define CC_MAX_GLOBALS 128
#define CC_MAX_RELOCS 4096
#define CC_ERR_LEN 160

enum cc_section { CC_SEC_TEXT, CC_SEC_RODATA, CC_SEC_DATA, CC_SEC_BSS };

/* A reference, recorded while emitting `text`, to an absolute address
 * that isn't known until every function/global has been laid out (see
 * cc/compile.c's link pass). `text_offset` is the position of the
 * pending imm32 field (as returned by emit_mov_reg_absmem() etc.);
 * `section`+`offset_in_section` says what final address belongs there. */
struct cc_reloc {
    uint32_t text_offset;
    enum cc_section section;
    uint32_t offset_in_section;
};

struct cc_func {
    char name[64];
    int param_count;
    struct cc_type param_types[CC_MAX_PARAMS];
    struct cc_type ret_type;
    int is_void;
    int has_body;
    int is_builtin;     /* body supplied by cc/builtins.c, already emitted at registration time */
    uint32_t text_offset; /* filled in once its body has actually been emitted */
};

struct cc_global {
    char name[64];
    struct cc_type type;
    enum cc_section section; /* CC_SEC_DATA (has an initializer) or CC_SEC_BSS (zero-initialized) */
    uint32_t offset;         /* offset within that section */
    uint32_t size;           /* total bytes: 4 for a scalar, 4*array_len for an array */
};

/* One node in the AST arena's block list -- see cc_alloc() below. Same
 * "one big bump-allocated arena, thrown away all at once" spirit as
 * js/value.c's js_alloc()/py/value.c's py_alloc(), just built from a
 * chain of kmalloc'd blocks instead of one fixed-size block, since a
 * source file's node count isn't bounded ahead of time the way a
 * kernel-lifetime page's DOM/script arena roughly is. */
struct cc_arena_block {
    struct cc_arena_block *next;
    uint32_t len, cap;
    uint8_t data[];
};

struct cc_module {
    struct cc_buf text;
    struct cc_buf rodata;
    struct cc_buf data;
    uint32_t bss_size;

    struct cc_reloc relocs[CC_MAX_RELOCS];
    int reloc_count;

    struct cc_func funcs[CC_MAX_FUNCS];
    int func_count;

    struct cc_global globals[CC_MAX_GLOBALS];
    int global_count;

    struct cc_arena_block *arena;

    char errmsg[CC_ERR_LEN];
    int error;
};

void cc_module_init(struct cc_module *m);
void cc_module_free(struct cc_module *m);
void cc_module_errorf(struct cc_module *m, int line, const char *fmt, ...);
/* Bump-allocates `size` zeroed bytes from the module's AST arena --
 * used for every AST node (see cc/parser.c) and any other compile-time-
 * only allocation. Freed all at once by cc_module_free(). */
void *cc_alloc(struct cc_module *m, uint32_t size);

/* Appends `len` bytes (already escape-decoded) plus a NUL terminator to
 * .rodata; returns the offset the string starts at. Used by the parser
 * as soon as it lexes a string literal (see cc/parser.c). */
uint32_t cc_add_rodata_string(struct cc_module *m, const char *bytes, uint32_t len);

void cc_add_reloc(struct cc_module *m, uint32_t text_offset, enum cc_section section, uint32_t offset_in_section);

struct cc_func *cc_find_func(struct cc_module *m, const char *name);
struct cc_func *cc_add_func(struct cc_module *m, const char *name);

struct cc_global *cc_find_global(struct cc_module *m, const char *name);
struct cc_global *cc_add_global(struct cc_module *m, const char *name);

#endif
