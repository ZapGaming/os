/* A from-scratch WebAssembly binary-format parser + stack-machine
 * interpreter. See include/net/wasm.h for the scope this is held to
 * (i32 only, no tables/i64/f32/f64, single bounded linear memory) and
 * why.
 *
 * Execution model: nested `call` instructions recurse through this
 * file's exec_function() using an ordinary C function call per WASM
 * call -- but each such C stack frame is small (a handful of pointers
 * and indices, no large local arrays), because the actual WASM
 * operand stack and block-label stack are NOT C-stack-local: they are
 * single kmalloc'd arrays owned by the wasm_instance and shared by
 * every frame in the active call chain, each frame just remembering
 * where its own region starts (frame_base/label_base) and restoring
 * that on return. This matters because kernel tasks run on a
 * TASK_STACK_SIZE == 16KB stack (see include/kernel/scheduler.h) --
 * putting a few-KB operand stack in every recursive C frame would
 * blow that after only a handful of nested WASM calls. WASM_MAX_CALL_
 * DEPTH below is a deliberately conservative bound on that recursion
 * for the same reason. */
#include <net/wasm.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

#define WASM_VALUE_STACK_SIZE 4096  /* shared by the whole active call chain, see file comment above */
#define WASM_LABEL_STACK_SIZE 512
#define WASM_MAX_CALL_DEPTH   48    /* recursion guard -- see file comment above re: 16KB task stacks */

enum {
    OP_UNREACHABLE = 0x00, OP_NOP = 0x01, OP_BLOCK = 0x02, OP_LOOP = 0x03,
    OP_IF = 0x04, OP_ELSE = 0x05, OP_END = 0x0B, OP_BR = 0x0C,
    OP_BR_IF = 0x0D, OP_BR_TABLE = 0x0E, OP_RETURN = 0x0F, OP_CALL = 0x10,
    OP_CALL_INDIRECT = 0x11, OP_DROP = 0x1A, OP_SELECT = 0x1B,
    OP_LOCAL_GET = 0x20, OP_LOCAL_SET = 0x21, OP_LOCAL_TEE = 0x22,
    OP_GLOBAL_GET = 0x23, OP_GLOBAL_SET = 0x24,
    OP_I32_LOAD = 0x28, OP_I32_LOAD8_S = 0x2C, OP_I32_LOAD8_U = 0x2D,
    OP_I32_LOAD16_S = 0x2E, OP_I32_LOAD16_U = 0x2F,
    OP_I32_STORE = 0x36, OP_I32_STORE8 = 0x3A, OP_I32_STORE16 = 0x3B,
    OP_MEMORY_SIZE = 0x3F, OP_MEMORY_GROW = 0x40, OP_I32_CONST = 0x41,
    OP_I32_EQZ = 0x45, OP_I32_EQ = 0x46, OP_I32_NE = 0x47,
    OP_I32_LT_S = 0x48, OP_I32_LT_U = 0x49, OP_I32_GT_S = 0x4A, OP_I32_GT_U = 0x4B,
    OP_I32_LE_S = 0x4C, OP_I32_LE_U = 0x4D, OP_I32_GE_S = 0x4E, OP_I32_GE_U = 0x4F,
    OP_I32_ADD = 0x6A, OP_I32_SUB = 0x6B, OP_I32_MUL = 0x6C,
    OP_I32_DIV_S = 0x6D, OP_I32_DIV_U = 0x6E, OP_I32_REM_S = 0x6F, OP_I32_REM_U = 0x70,
    OP_I32_AND = 0x71, OP_I32_OR = 0x72, OP_I32_XOR = 0x73,
    OP_I32_SHL = 0x74, OP_I32_SHR_S = 0x75, OP_I32_SHR_U = 0x76,
};

/* Private to this file (struct wasm_instance only holds a `void *` for
 * it -- see include/net/wasm.h). Declared up here, ahead of
 * wasm_instantiate() below, because instantiate() needs to run the
 * Start section's function (if any) via exec_function() before this
 * file's actual interpreter section defines it. */
enum { LABEL_BLOCK, LABEL_LOOP, LABEL_IF };

struct wasm_label {
    uint8_t kind;
    uint8_t arity;          /* 0 or 1 -- values carried across a branch to this label */
    uint32_t stack_height;  /* value_stack depth at label entry */
    uint32_t target_pc;     /* loop: start of loop body. block/if: just past the matching `end`. */
};

static int exec_function(struct wasm_instance *inst, uint32_t func_idx,
                          uint32_t *sp_io, uint32_t *label_top_io, int depth,
                          int32_t *result_out);

/* ---- LEB128 (the encoding used for every varint in the format) ------ */

static int read_u32leb(const uint8_t *buf, uint32_t len, uint32_t *pos, uint32_t *out) {
    uint32_t result = 0, p = *pos;
    for (int i = 0; i < 5; i++) {
        if (p >= len) return 0;
        uint8_t b = buf[p++];
        result |= (uint32_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) { *pos = p; *out = result; return 1; }
    }
    return 0; /* more than 5 bytes -- malformed for a value that must fit in 32 bits */
}

static int read_i32leb(const uint8_t *buf, uint32_t len, uint32_t *pos, int32_t *out) {
    int32_t result = 0; int shift = 0; uint32_t p = *pos; uint8_t b = 0;
    for (int i = 0; i < 5; i++) {
        if (p >= len) return 0;
        b = buf[p++];
        result |= ((int32_t)(b & 0x7F)) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 32 && (b & 0x40)) result |= -((int32_t)1 << shift);
            *pos = p; *out = result; return 1;
        }
    }
    return 0;
}

/* Blocktype immediates (on block/loop/if) are a signed LEB128: -0x40
 * means "no result", -1 means "one i32 result" (0x7F's signed-LEB
 * encoding), anything else is a result type this interpreter doesn't
 * have (i64/f32/f64/v128/funcref/externref) or a multi-value-proposal
 * type-index blocktype (encoded as a non-negative value) -- none of
 * which this interpreter supports, so all of those are rejected here,
 * in the one place blocktypes are decoded. */
static int read_blocktype(const uint8_t *code, uint32_t len, uint32_t *pos, int *arity_out) {
    int32_t v;
    if (!read_i32leb(code, len, pos, &v)) return 0;
    if (v == -0x40) { *arity_out = 0; return 1; }
    if (v == -1) { *arity_out = 1; return 1; }
    return 0;
}

/* Reads one instruction (opcode + immediates) starting at *pos and
 * advances *pos past it, reporting the opcode via *opcode_out. This
 * switch statement IS the definition of "instruction this interpreter
 * supports" -- validate_function_body() rejects any module where this
 * returns 0 for some opcode, and exec_function()'s dispatch switch
 * must cover everything accepted here (it traps with an "internal
 * error" message, rather than silently doing the wrong thing, if the
 * two ever get out of sync). Returns 0 if the opcode is unsupported OR
 * if decoding immediates runs past `len` (truncated/corrupt input). */
static int decode_and_skip(const uint8_t *code, uint32_t len, uint32_t *pos, uint8_t *opcode_out) {
    if (*pos >= len) return 0;
    uint8_t op = code[(*pos)++];
    *opcode_out = op;
    uint32_t u32_tmp;
    int32_t i32_tmp;
    int arity_tmp;

    switch (op) {
        case OP_UNREACHABLE: case OP_NOP: case OP_ELSE: case OP_END: case OP_RETURN:
        case OP_DROP: case OP_SELECT:
        case OP_I32_EQZ: case OP_I32_EQ: case OP_I32_NE:
        case OP_I32_LT_S: case OP_I32_LT_U: case OP_I32_GT_S: case OP_I32_GT_U:
        case OP_I32_LE_S: case OP_I32_LE_U: case OP_I32_GE_S: case OP_I32_GE_U:
        case OP_I32_ADD: case OP_I32_SUB: case OP_I32_MUL:
        case OP_I32_DIV_S: case OP_I32_DIV_U: case OP_I32_REM_S: case OP_I32_REM_U:
        case OP_I32_AND: case OP_I32_OR: case OP_I32_XOR:
        case OP_I32_SHL: case OP_I32_SHR_S: case OP_I32_SHR_U:
            return 1; /* no immediates */

        case OP_BLOCK: case OP_LOOP: case OP_IF:
            return read_blocktype(code, len, pos, &arity_tmp);

        case OP_BR: case OP_BR_IF: case OP_CALL:
        case OP_LOCAL_GET: case OP_LOCAL_SET: case OP_LOCAL_TEE:
        case OP_GLOBAL_GET: case OP_GLOBAL_SET:
            return read_u32leb(code, len, pos, &u32_tmp);

        case OP_BR_TABLE: {
            uint32_t count;
            if (!read_u32leb(code, len, pos, &count)) return 0;
            for (uint32_t i = 0; i < count; i++) {
                if (!read_u32leb(code, len, pos, &u32_tmp)) return 0;
            }
            return read_u32leb(code, len, pos, &u32_tmp); /* default label */
        }

        case OP_CALL_INDIRECT:
            if (!read_u32leb(code, len, pos, &u32_tmp)) return 0; /* typeidx */
            if (*pos >= len) return 0;
            (*pos)++; /* reserved table-index byte -- always 0x00 in the MVP; unchecked since call_indirect always traps anyway */
            return 1;

        case OP_I32_LOAD: case OP_I32_LOAD8_S: case OP_I32_LOAD8_U:
        case OP_I32_LOAD16_S: case OP_I32_LOAD16_U:
        case OP_I32_STORE: case OP_I32_STORE8: case OP_I32_STORE16:
            if (!read_u32leb(code, len, pos, &u32_tmp)) return 0; /* align (a hint -- we never use it) */
            return read_u32leb(code, len, pos, &u32_tmp);         /* offset */

        case OP_MEMORY_SIZE: case OP_MEMORY_GROW:
            if (*pos >= len) return 0;
            (*pos)++; /* reserved byte */
            return 1;

        case OP_I32_CONST:
            return read_i32leb(code, len, pos, &i32_tmp);

        default:
            return 0; /* every f32/f64/i64/v128/reftype/bulk-memory opcode, and anything else unrecognized */
    }
}

/* Scans forward from `pos` (the start of a block/loop/if body) to find
 * that construct's matching `end`, and (if else_pos_out is non-NULL)
 * a same-depth `else`. Called once when a block/loop/if is entered by
 * normal execution -- not re-scanned on every loop iteration, since
 * the result is cached in the label pushed for it (see exec_function).
 * Only ever called on code that already passed validate_function_
 * body(), so decode_and_skip() failing here would mean a validator/
 * scanner mismatch bug, not a malformed module. */
static int find_matching_end(const uint8_t *code, uint32_t len, uint32_t pos,
                              uint32_t *end_pos_out, uint32_t *else_pos_out) {
    int depth = 0;
    if (else_pos_out) *else_pos_out = 0xFFFFFFFFu;
    while (pos < len) {
        uint32_t opcode_pos = pos;
        uint8_t op;
        if (!decode_and_skip(code, len, &pos, &op)) return 0;
        if (op == OP_BLOCK || op == OP_LOOP || op == OP_IF) {
            depth++;
        } else if (op == OP_END) {
            if (depth == 0) { *end_pos_out = opcode_pos; return 1; }
            depth--;
        } else if (op == OP_ELSE) {
            if (depth == 0 && else_pos_out) *else_pos_out = opcode_pos;
        }
    }
    return 0; /* ran off the end without finding `end` -- malformed */
}

/* One pass over a function body validating every opcode is in this
 * interpreter's supported set (see decode_and_skip). This is the
 * enforcement point for "f32/f64 (and i64/tables/etc) are rejected at
 * module-validation time" -- called once per function during
 * wasm_parse_module(), before anything is ever executed. */
static int validate_function_body(const uint8_t *code, uint32_t len) {
    uint32_t pos = 0;
    while (pos < len) {
        uint32_t opcode_pos = pos;
        uint8_t op;
        if (!decode_and_skip(code, len, &pos, &op)) {
            serial_printf("wasm: unsupported or malformed opcode 0x%x at code offset %u -- rejecting module\n",
                          code[opcode_pos], opcode_pos);
            return 0;
        }
    }
    return 1;
}

/* ---- Section parsers -------------------------------------------------
 * Each takes a pointer *into* module->raw (so any bytes they keep --
 * export names copied out aside -- are just pointers into that one
 * owned buffer, freed all at once by wasm_free_module()) plus that
 * section's byte length, and returns 1/0 like everything else here. */

static int parse_type_section(struct wasm_module *mod, const uint8_t *sec, uint32_t seclen) {
    uint32_t pos = 0, count;
    if (!read_u32leb(sec, seclen, &pos, &count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (mod->type_count >= WASM_MAX_TYPES) {
            serial_printf("wasm: too many types (max %u)\n", WASM_MAX_TYPES);
            return 0;
        }
        if (pos >= seclen || sec[pos++] != 0x60) {
            serial_printf("wasm: bad functype form byte\n");
            return 0;
        }
        struct wasm_functype *ft = &mod->types[mod->type_count++];
        uint32_t pc;
        if (!read_u32leb(sec, seclen, &pos, &pc)) return 0;
        if (pc > WASM_MAX_PARAMS) {
            serial_printf("wasm: function has too many params (max %u)\n", WASM_MAX_PARAMS);
            return 0;
        }
        ft->param_count = (uint8_t)pc;
        for (uint32_t p = 0; p < pc; p++) {
            if (pos >= seclen) return 0;
            uint8_t vt = sec[pos++];
            if (vt != 0x7F) {
                serial_printf("wasm: non-i32 param type 0x%x unsupported\n", vt);
                return 0;
            }
            ft->param_types[p] = vt;
        }
        uint32_t rc;
        if (!read_u32leb(sec, seclen, &pos, &rc)) return 0;
        if (rc > 1) {
            serial_printf("wasm: multi-value function results unsupported\n");
            return 0;
        }
        ft->result_count = (uint8_t)rc;
        if (rc == 1) {
            if (pos >= seclen) return 0;
            uint8_t vt = sec[pos++];
            if (vt != 0x7F) {
                serial_printf("wasm: non-i32 result type 0x%x unsupported\n", vt);
                return 0;
            }
            ft->result_type = vt;
        }
    }
    return pos == seclen;
}

/* Function imports get a stub slot (so index numbering for everything
 * defined after them stays correct) but no code -- calling one traps
 * cleanly at runtime. Any other import kind is rejected outright: see
 * include/net/wasm.h for why that's safer than faking one up. */
static int parse_import_section(struct wasm_module *mod, const uint8_t *sec, uint32_t seclen) {
    uint32_t pos = 0, count;
    if (!read_u32leb(sec, seclen, &pos, &count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t namelen;
        if (!read_u32leb(sec, seclen, &pos, &namelen)) return 0;
        if (pos + namelen > seclen) return 0;
        pos += namelen; /* import module name -- not retained, no host-binding mechanism exists yet */
        if (!read_u32leb(sec, seclen, &pos, &namelen)) return 0;
        if (pos + namelen > seclen) return 0;
        pos += namelen; /* import field name */

        if (pos >= seclen) return 0;
        uint8_t kind = sec[pos++];
        if (kind == 0) {
            uint32_t typeidx;
            if (!read_u32leb(sec, seclen, &pos, &typeidx)) return 0;
            if (typeidx >= mod->type_count) {
                serial_printf("wasm: function import references unknown type %u\n", typeidx);
                return 0;
            }
            if (mod->func_count >= WASM_MAX_FUNCS) {
                serial_printf("wasm: too many functions (max %u)\n", WASM_MAX_FUNCS);
                return 0;
            }
            struct wasm_func *fn = &mod->funcs[mod->func_count++];
            fn->type_idx = typeidx;
            fn->is_imported = 1;
            fn->code = NULL;
            fn->code_len = 0;
            fn->local_count = 0;
            mod->imported_func_count++;
        } else if (kind == 1) {
            serial_printf("wasm: imported tables unsupported (no host bindings) -- rejecting module\n");
            return 0;
        } else if (kind == 2) {
            serial_printf("wasm: imported memory unsupported (no host bindings) -- rejecting module\n");
            return 0;
        } else if (kind == 3) {
            serial_printf("wasm: imported globals unsupported (no host bindings) -- rejecting module\n");
            return 0;
        } else {
            serial_printf("wasm: unknown import kind %u\n", kind);
            return 0;
        }
    }
    return pos == seclen;
}

static int parse_function_section(struct wasm_module *mod, const uint8_t *sec, uint32_t seclen) {
    uint32_t pos = 0, count;
    if (!read_u32leb(sec, seclen, &pos, &count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t typeidx;
        if (!read_u32leb(sec, seclen, &pos, &typeidx)) return 0;
        if (typeidx >= mod->type_count) {
            serial_printf("wasm: function references unknown type %u\n", typeidx);
            return 0;
        }
        if (mod->func_count >= WASM_MAX_FUNCS) {
            serial_printf("wasm: too many functions (max %u)\n", WASM_MAX_FUNCS);
            return 0;
        }
        struct wasm_func *fn = &mod->funcs[mod->func_count++];
        fn->type_idx = typeidx;
        fn->is_imported = 0;
        fn->code = NULL;
        fn->code_len = 0;
        fn->local_count = 0;
    }
    return pos == seclen;
}

static int parse_memory_section(struct wasm_module *mod, const uint8_t *sec, uint32_t seclen) {
    uint32_t pos = 0, count;
    if (!read_u32leb(sec, seclen, &pos, &count)) return 0;
    if (count > 1) {
        serial_printf("wasm: multiple memories unsupported\n");
        return 0;
    }
    if (count == 1) {
        if (pos >= seclen) return 0;
        uint8_t flag = sec[pos++];
        uint32_t minp;
        if (!read_u32leb(sec, seclen, &pos, &minp)) return 0;
        uint32_t maxp = 0;
        int has_max = 0;
        if (flag == 1) {
            if (!read_u32leb(sec, seclen, &pos, &maxp)) return 0;
            has_max = 1;
        } else if (flag != 0) {
            serial_printf("wasm: unsupported memory limits flag %u\n", flag);
            return 0;
        }
        if (minp > WASM_MAX_MEMORY_PAGES) {
            serial_printf("wasm: module wants %u initial memory pages, over this interpreter's %u page cap\n",
                          minp, WASM_MAX_MEMORY_PAGES);
            return 0;
        }
        mod->has_memory = 1;
        mod->memory_min_pages = minp;
        mod->memory_max_pages = (has_max && maxp < WASM_MAX_MEMORY_PAGES) ? maxp : WASM_MAX_MEMORY_PAGES;
        if (mod->memory_max_pages < minp) mod->memory_max_pages = minp;
    }
    return pos == seclen;
}

/* Only `i32.const <N> end` global initializers are supported (see
 * include/net/wasm.h); anything else (an imported-global reference --
 * moot anyway since global imports are rejected -- or a more exotic
 * expression) is rejected here rather than misevaluated. */
static int parse_global_section(struct wasm_module *mod, const uint8_t *sec, uint32_t seclen) {
    uint32_t pos = 0, count;
    if (!read_u32leb(sec, seclen, &pos, &count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (mod->global_count >= WASM_MAX_GLOBALS) {
            serial_printf("wasm: too many globals (max %u)\n", WASM_MAX_GLOBALS);
            return 0;
        }
        if (pos >= seclen) return 0;
        uint8_t vt = sec[pos++];
        if (vt != 0x7F) {
            serial_printf("wasm: non-i32 global type 0x%x unsupported\n", vt);
            return 0;
        }
        if (pos >= seclen) return 0;
        uint8_t mut = sec[pos++];
        if (pos >= seclen || sec[pos++] != OP_I32_CONST) {
            serial_printf("wasm: unsupported global init expression (only i32.const is supported)\n");
            return 0;
        }
        int32_t val;
        if (!read_i32leb(sec, seclen, &pos, &val)) return 0;
        if (pos >= seclen || sec[pos++] != OP_END) {
            serial_printf("wasm: malformed global init expression\n");
            return 0;
        }
        struct wasm_global *g = &mod->globals[mod->global_count++];
        g->is_mutable = (mut == 1);
        g->init_value = val;
    }
    return pos == seclen;
}

static int parse_export_section(struct wasm_module *mod, const uint8_t *sec, uint32_t seclen) {
    uint32_t pos = 0, count;
    if (!read_u32leb(sec, seclen, &pos, &count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (mod->export_count >= WASM_MAX_EXPORTS) {
            serial_printf("wasm: too many exports (max %u)\n", WASM_MAX_EXPORTS);
            return 0;
        }
        uint32_t namelen;
        if (!read_u32leb(sec, seclen, &pos, &namelen)) return 0;
        if (namelen >= WASM_MAX_NAME_LEN) {
            serial_printf("wasm: export name too long (max %u chars)\n", WASM_MAX_NAME_LEN - 1);
            return 0;
        }
        if (pos + namelen > seclen) return 0;
        struct wasm_export *ex = &mod->exports[mod->export_count++];
        memcpy(ex->name, sec + pos, namelen);
        ex->name[namelen] = '\0';
        pos += namelen;
        if (pos >= seclen) return 0;
        ex->kind = sec[pos++];
        uint32_t idx;
        if (!read_u32leb(sec, seclen, &pos, &idx)) return 0;
        ex->index = idx;
    }
    return pos == seclen;
}

static int parse_code_section(struct wasm_module *mod, const uint8_t *sec, uint32_t seclen) {
    uint32_t pos = 0, count;
    if (!read_u32leb(sec, seclen, &pos, &count)) return 0;
    uint32_t expected = mod->func_count - mod->imported_func_count;
    if (count != expected) {
        serial_printf("wasm: code section has %u entries, expected %u\n", count, expected);
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t body_size;
        if (!read_u32leb(sec, seclen, &pos, &body_size)) return 0;
        uint32_t body_start = pos;
        if (body_start + body_size > seclen) {
            serial_printf("wasm: code entry runs past the end of the code section\n");
            return 0;
        }

        struct wasm_func *fn = &mod->funcs[mod->imported_func_count + i];
        struct wasm_functype *ft = &mod->types[fn->type_idx];

        uint32_t local_decl_count;
        if (!read_u32leb(sec, seclen, &pos, &local_decl_count)) return 0;
        uint32_t total_locals = ft->param_count;
        for (uint32_t d = 0; d < local_decl_count; d++) {
            uint32_t rep;
            if (!read_u32leb(sec, seclen, &pos, &rep)) return 0;
            if (pos >= seclen) return 0;
            uint8_t vt = sec[pos++];
            if (vt != 0x7F) {
                serial_printf("wasm: non-i32 local type 0x%x unsupported\n", vt);
                return 0;
            }
            if (total_locals + rep > WASM_MAX_LOCALS) {
                serial_printf("wasm: function has too many locals (max %u)\n", WASM_MAX_LOCALS);
                return 0;
            }
            total_locals += rep;
        }
        if (pos > body_start + body_size) {
            serial_printf("wasm: malformed local declarations\n");
            return 0;
        }

        const uint8_t *instrs = sec + pos;
        uint32_t instr_len = body_start + body_size - pos;
        if (!validate_function_body(instrs, instr_len)) return 0;

        fn->code = instrs;
        fn->code_len = instr_len;
        fn->local_count = total_locals;

        pos = body_start + body_size;
    }
    return pos == seclen;
}

static int parse_data_section(struct wasm_module *mod, const uint8_t *sec, uint32_t seclen) {
    uint32_t pos = 0, count;
    if (!read_u32leb(sec, seclen, &pos, &count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (mod->data_seg_count >= WASM_MAX_DATA_SEGS) {
            serial_printf("wasm: too many data segments (max %u)\n", WASM_MAX_DATA_SEGS);
            return 0;
        }
        uint32_t memidx;
        if (!read_u32leb(sec, seclen, &pos, &memidx)) return 0;
        if (memidx != 0) {
            serial_printf("wasm: data segment for nonzero memory index unsupported\n");
            return 0;
        }
        if (pos >= seclen || sec[pos++] != OP_I32_CONST) {
            serial_printf("wasm: unsupported data segment offset expression\n");
            return 0;
        }
        int32_t off;
        if (!read_i32leb(sec, seclen, &pos, &off)) return 0;
        if (pos >= seclen || sec[pos++] != OP_END) {
            serial_printf("wasm: malformed data segment offset expression\n");
            return 0;
        }
        uint32_t datalen;
        if (!read_u32leb(sec, seclen, &pos, &datalen)) return 0;
        if (pos + datalen > seclen) return 0;
        struct wasm_data_seg *d = &mod->data_segs[mod->data_seg_count++];
        d->offset = (uint32_t)off;
        d->data = sec + pos;
        d->len = datalen;
        pos += datalen;
    }
    return pos == seclen;
}

int wasm_parse_module(const uint8_t *data, uint32_t len, struct wasm_module *out) {
    memset(out, 0, sizeof(*out));
    if (len < 8) {
        serial_printf("wasm: input too short to be a module\n");
        return 0;
    }
    static const uint8_t magic[4] = {0x00, 0x61, 0x73, 0x6D};
    static const uint8_t version[4] = {0x01, 0x00, 0x00, 0x00};
    if (memcmp(data, magic, 4) != 0) {
        serial_printf("wasm: bad magic number\n");
        return 0;
    }
    if (memcmp(data + 4, version, 4) != 0) {
        serial_printf("wasm: unsupported binary version (only MVP version 1 is supported)\n");
        return 0;
    }

    out->raw = (uint8_t *)kmalloc(len);
    if (!out->raw) {
        serial_printf("wasm: out of memory copying module\n");
        return 0;
    }
    memcpy(out->raw, data, len);
    out->raw_len = len;

    uint32_t pos = 8;
    int seen_code_section = 0;
    while (pos < len) {
        uint8_t section_id = out->raw[pos++];
        uint32_t section_size;
        if (!read_u32leb(out->raw, len, &pos, &section_size)) {
            serial_printf("wasm: truncated section header\n");
            goto fail;
        }
        if ((uint64_t)pos + section_size > (uint64_t)len) {
            serial_printf("wasm: section runs past the end of the module\n");
            goto fail;
        }
        const uint8_t *sec = out->raw + pos;
        int ok = 1;
        switch (section_id) {
            case 0: break; /* custom section -- skipped */
            case 1: ok = parse_type_section(out, sec, section_size); break;
            case 2: ok = parse_import_section(out, sec, section_size); break;
            case 3: ok = parse_function_section(out, sec, section_size); break;
            case 4: break; /* table section -- skipped, see include/net/wasm.h */
            case 5: ok = parse_memory_section(out, sec, section_size); break;
            case 6: ok = parse_global_section(out, sec, section_size); break;
            case 7: ok = parse_export_section(out, sec, section_size); break;
            case 8: {
                uint32_t p = 0, idx;
                ok = read_u32leb(sec, section_size, &p, &idx);
                if (ok) { out->has_start = 1; out->start_func_idx = idx; }
                break;
            }
            case 9: break; /* element section -- skipped, see include/net/wasm.h */
            case 10: ok = parse_code_section(out, sec, section_size); seen_code_section = 1; break;
            case 11: ok = parse_data_section(out, sec, section_size); break;
            default:
                serial_printf("wasm: unknown section id %u\n", section_id);
                ok = 0;
                break;
        }
        if (!ok) goto fail;
        pos += section_size;
    }

    if (!seen_code_section && out->func_count > out->imported_func_count) {
        serial_printf("wasm: functions declared with no code section\n");
        goto fail;
    }
    for (uint32_t i = out->imported_func_count; i < out->func_count; i++) {
        if (!out->funcs[i].code) {
            serial_printf("wasm: function %u has no body\n", i);
            goto fail;
        }
    }
    return 1;

fail:
    kfree(out->raw);
    out->raw = NULL;
    return 0;
}

void wasm_free_module(struct wasm_module *mod) {
    if (mod->raw) kfree(mod->raw);
    mod->raw = NULL;
    mod->raw_len = 0;
}

/* ---- Instantiation ---------------------------------------------------*/

int wasm_instantiate(struct wasm_module *mod, struct wasm_instance *out) {
    memset(out, 0, sizeof(*out));
    out->module = mod;

    if (mod->has_memory) {
        uint32_t bytes = mod->memory_min_pages * WASM_PAGE_SIZE;
        if (bytes > 0) {
            out->memory = (uint8_t *)kmalloc(bytes);
            if (!out->memory) {
                serial_printf("wasm: out of memory allocating linear memory\n");
                return 0;
            }
            memset(out->memory, 0, bytes);
        }
        out->memory_pages = mod->memory_min_pages;
        out->memory_max_pages = mod->memory_max_pages;
    }

    for (uint32_t i = 0; i < mod->data_seg_count; i++) {
        struct wasm_data_seg *d = &mod->data_segs[i];
        uint64_t end = (uint64_t)d->offset + d->len;
        if (!out->memory || end > (uint64_t)out->memory_pages * WASM_PAGE_SIZE) {
            serial_printf("wasm: data segment %u out of bounds\n", i);
            if (out->memory) kfree(out->memory);
            memset(out, 0, sizeof(*out));
            return 0;
        }
        memcpy(out->memory + d->offset, d->data, d->len);
    }

    for (uint32_t i = 0; i < mod->global_count; i++) {
        out->globals[i] = mod->globals[i].init_value;
    }

    out->value_stack = (int32_t *)kmalloc(WASM_VALUE_STACK_SIZE * sizeof(int32_t));
    struct wasm_label *labels = (struct wasm_label *)kmalloc(WASM_LABEL_STACK_SIZE * sizeof(struct wasm_label));
    if (!out->value_stack || !labels) {
        serial_printf("wasm: out of memory allocating interpreter stacks\n");
        if (out->memory) kfree(out->memory);
        if (out->value_stack) kfree(out->value_stack);
        if (labels) kfree(labels);
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->label_stack = labels;
    out->module = mod;

    if (mod->has_start) {
        uint32_t sp = 0, label_top = 0;
        int32_t unused_result;
        if (!exec_function(out, mod->start_func_idx, &sp, &label_top, 1, &unused_result)) {
            serial_printf("wasm: start function trapped -- instantiation failed\n");
            wasm_free_instance(out);
            return 0;
        }
    }
    return 1;
}

void wasm_free_instance(struct wasm_instance *inst) {
    if (inst->memory) kfree(inst->memory);
    if (inst->value_stack) kfree(inst->value_stack);
    if (inst->label_stack) kfree(inst->label_stack);
    memset(inst, 0, sizeof(*inst));
}

/* ---- Interpreter ------------------------------------------------------
 * A plain operand-stack machine. `stack_height` on a label records the
 * value_stack depth at the moment that block/loop/if was entered, so a
 * branch to it truncates back to exactly that (plus the label's arity
 * worth of result values carried across, per the spec's br semantics)
 * regardless of what the branching code pushed in between. */

static int vs_push(int32_t *stack, uint32_t *sp, int32_t v) {
    if (*sp >= WASM_VALUE_STACK_SIZE) return 0;
    stack[(*sp)++] = v;
    return 1;
}

static int vs_pop(int32_t *stack, uint32_t *sp, uint32_t floor, int32_t *out) {
    if (*sp <= floor) return 0;
    *out = stack[--(*sp)];
    return 1;
}

/* Executes a `br`/`br_if`/`br_table` to the label `label_idx` levels
 * out from the innermost active one (0 = innermost), per the spec's
 * branch semantics: pop the label's arity worth of values, truncate
 * the stack back to the label's entry height, push those values back,
 * then either jump to the loop's start (keeping the loop's own label
 * active, since we're continuing it) or to just past the block/if's
 * `end` (popping it, since we've now exited it). Returns 0 if
 * label_idx doesn't name an active label in the current function
 * (a branch can never cross a function boundary). */
static int do_branch(struct wasm_label *labels, uint32_t label_base, uint32_t *label_top,
                      int32_t *stack, uint32_t *sp, uint32_t label_idx, uint32_t *pc_out) {
    if (label_idx >= (*label_top - label_base)) return 0;
    struct wasm_label *lbl = &labels[*label_top - 1 - label_idx];
    int32_t carried = 0;
    if (lbl->arity == 1) {
        if (!vs_pop(stack, sp, 0, &carried)) return 0;
    }
    *sp = lbl->stack_height;
    if (lbl->arity == 1) {
        if (!vs_push(stack, sp, carried)) return 0;
    }
    *pc_out = lbl->target_pc;
    *label_top = (lbl->kind == LABEL_LOOP) ? (*label_top - label_idx) : (*label_top - label_idx - 1);
    return 1;
}

static int32_t load_le32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static void store_le32(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); p[2] = (uint8_t)(u >> 16); p[3] = (uint8_t)(u >> 24);
}
static int32_t load_le16(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
static void store_le16(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8);
}

static int mem_bounds_ok(struct wasm_instance *inst, uint32_t addr, uint32_t size) {
    if (!inst->memory) return 0;
    uint64_t end = (uint64_t)addr + size;
    return end <= (uint64_t)inst->memory_pages * WASM_PAGE_SIZE;
}

#define POP(dst) do { if (!vs_pop(stack, &sp, floor, &(dst))) { trap_reason = "stack underflow"; goto trap; } } while (0)
#define PUSH(val) do { if (!vs_push(stack, &sp, (val))) { trap_reason = "value stack overflow"; goto trap; } } while (0)

static int exec_function(struct wasm_instance *inst, uint32_t func_idx,
                          uint32_t *sp_io, uint32_t *label_top_io, int depth,
                          int32_t *result_out) {
    if (depth > WASM_MAX_CALL_DEPTH) {
        serial_printf("wasm: trap: call stack too deep (>%d levels)\n", WASM_MAX_CALL_DEPTH);
        return 0;
    }
    struct wasm_module *mod = inst->module;
    if (func_idx >= mod->func_count) {
        serial_printf("wasm: trap: call to invalid function index %u\n", func_idx);
        return 0;
    }
    struct wasm_func *fn = &mod->funcs[func_idx];
    if (fn->is_imported) {
        serial_printf("wasm: trap: call to imported function %u with no host binding\n", func_idx);
        return 0;
    }
    struct wasm_functype *ft = &mod->types[fn->type_idx];

    if (*sp_io < ft->param_count) {
        serial_printf("wasm: trap: stack underflow calling function %u\n", func_idx);
        return 0;
    }

    int32_t *stack = inst->value_stack;
    struct wasm_label *labels = (struct wasm_label *)inst->label_stack;

    uint32_t frame_base = *sp_io - ft->param_count;
    uint32_t label_base = *label_top_io;
    uint32_t local_count = fn->local_count;

    if (frame_base + local_count > WASM_VALUE_STACK_SIZE) {
        serial_printf("wasm: trap: value stack overflow entering function %u\n", func_idx);
        return 0;
    }
    for (uint32_t i = ft->param_count; i < local_count; i++) stack[frame_base + i] = 0;

    uint32_t sp = frame_base + local_count;
    uint32_t floor = sp; /* operand-stack floor for this frame -- pops/pushes never cross below this */
    uint32_t label_top = label_base;

    const uint8_t *code = fn->code;
    uint32_t code_len = fn->code_len;
    uint32_t pc = 0;

    int ok = 1;
    const char *trap_reason = NULL;

    while (pc < code_len) {
        uint8_t op = code[pc++];
        switch (op) {
            case OP_UNREACHABLE:
                trap_reason = "unreachable instruction executed";
                goto trap;
            case OP_NOP:
                break;

            case OP_BLOCK: {
                int arity;
                if (!read_blocktype(code, code_len, &pc, &arity)) { trap_reason = "unsupported block type"; goto trap; }
                uint32_t end_pos;
                if (!find_matching_end(code, code_len, pc, &end_pos, NULL)) { trap_reason = "malformed block"; goto trap; }
                if (label_top >= WASM_LABEL_STACK_SIZE) { trap_reason = "too many nested blocks"; goto trap; }
                labels[label_top].kind = LABEL_BLOCK;
                labels[label_top].arity = (uint8_t)arity;
                labels[label_top].stack_height = sp;
                labels[label_top].target_pc = end_pos + 1;
                label_top++;
                break;
            }
            case OP_LOOP: {
                int arity;
                if (!read_blocktype(code, code_len, &pc, &arity)) { trap_reason = "unsupported block type"; goto trap; }
                if (label_top >= WASM_LABEL_STACK_SIZE) { trap_reason = "too many nested blocks"; goto trap; }
                labels[label_top].kind = LABEL_LOOP;
                labels[label_top].arity = 0; /* branching into a loop carries no values in this (pre-multi-value) subset */
                labels[label_top].stack_height = sp;
                labels[label_top].target_pc = pc; /* loop body start, for br to jump back to */
                label_top++;
                break;
            }
            case OP_IF: {
                int arity;
                if (!read_blocktype(code, code_len, &pc, &arity)) { trap_reason = "unsupported block type"; goto trap; }
                uint32_t end_pos, else_pos;
                if (!find_matching_end(code, code_len, pc, &end_pos, &else_pos)) { trap_reason = "malformed if"; goto trap; }
                int32_t cond;
                POP(cond);
                uint32_t body_start = pc;
                if (cond != 0) {
                    if (label_top >= WASM_LABEL_STACK_SIZE) { trap_reason = "too many nested blocks"; goto trap; }
                    labels[label_top].kind = LABEL_IF;
                    labels[label_top].arity = (uint8_t)arity;
                    labels[label_top].stack_height = sp;
                    labels[label_top].target_pc = end_pos + 1;
                    label_top++;
                    pc = body_start;
                } else if (else_pos != 0xFFFFFFFFu) {
                    if (label_top >= WASM_LABEL_STACK_SIZE) { trap_reason = "too many nested blocks"; goto trap; }
                    labels[label_top].kind = LABEL_IF;
                    labels[label_top].arity = (uint8_t)arity;
                    labels[label_top].stack_height = sp;
                    labels[label_top].target_pc = end_pos + 1;
                    label_top++;
                    pc = else_pos + 1;
                } else {
                    pc = end_pos + 1; /* no else, condition false: skip the whole construct, nothing pushed */
                }
                break;
            }
            case OP_ELSE: {
                if (label_top <= label_base) { trap_reason = "else with no matching if"; goto trap; }
                pc = labels[label_top - 1].target_pc; /* fell through the true branch -- skip the false branch */
                label_top--;
                break;
            }
            case OP_END: {
                if (label_top > label_base) label_top--;
                break; /* label_top == label_base: this is the function body's own closing end */
            }
            case OP_BR: {
                uint32_t idx;
                if (!read_u32leb(code, code_len, &pc, &idx)) { trap_reason = "truncated br"; goto trap; }
                if (!do_branch(labels, label_base, &label_top, stack, &sp, idx, &pc)) { trap_reason = "invalid branch target"; goto trap; }
                break;
            }
            case OP_BR_IF: {
                uint32_t idx;
                if (!read_u32leb(code, code_len, &pc, &idx)) { trap_reason = "truncated br_if"; goto trap; }
                int32_t cond;
                POP(cond);
                if (cond != 0) {
                    if (!do_branch(labels, label_base, &label_top, stack, &sp, idx, &pc)) { trap_reason = "invalid branch target"; goto trap; }
                }
                break;
            }
            case OP_BR_TABLE: {
                uint32_t count;
                if (!read_u32leb(code, code_len, &pc, &count)) { trap_reason = "truncated br_table"; goto trap; }
                int32_t n;
                POP(n);
                uint32_t chosen = 0;
                int found = 0;
                for (uint32_t k = 0; k < count; k++) {
                    uint32_t lbl;
                    if (!read_u32leb(code, code_len, &pc, &lbl)) { trap_reason = "truncated br_table"; goto trap; }
                    if (!found && n >= 0 && (uint32_t)n == k) { chosen = lbl; found = 1; }
                }
                uint32_t default_lbl;
                if (!read_u32leb(code, code_len, &pc, &default_lbl)) { trap_reason = "truncated br_table"; goto trap; }
                if (!found) chosen = default_lbl;
                if (!do_branch(labels, label_base, &label_top, stack, &sp, chosen, &pc)) { trap_reason = "invalid branch target"; goto trap; }
                break;
            }
            case OP_RETURN:
                goto done;
            case OP_CALL: {
                uint32_t callee;
                if (!read_u32leb(code, code_len, &pc, &callee)) { trap_reason = "truncated call"; goto trap; }
                if (!exec_function(inst, callee, &sp, &label_top, depth + 1, NULL)) { trap_reason = "callee trapped"; goto trap; }
                break;
            }
            case OP_CALL_INDIRECT:
                trap_reason = "call_indirect unsupported (no function table support)";
                goto trap;

            case OP_DROP: {
                int32_t discard;
                POP(discard);
                break;
            }
            case OP_SELECT: {
                int32_t c, b, a;
                POP(c); POP(b); POP(a);
                PUSH(c != 0 ? a : b);
                break;
            }

            case OP_LOCAL_GET: {
                uint32_t idx;
                if (!read_u32leb(code, code_len, &pc, &idx)) { trap_reason = "truncated local.get"; goto trap; }
                if (idx >= local_count) { trap_reason = "local index out of range"; goto trap; }
                PUSH(stack[frame_base + idx]);
                break;
            }
            case OP_LOCAL_SET: {
                uint32_t idx;
                if (!read_u32leb(code, code_len, &pc, &idx)) { trap_reason = "truncated local.set"; goto trap; }
                if (idx >= local_count) { trap_reason = "local index out of range"; goto trap; }
                int32_t v;
                POP(v);
                stack[frame_base + idx] = v;
                break;
            }
            case OP_LOCAL_TEE: {
                uint32_t idx;
                if (!read_u32leb(code, code_len, &pc, &idx)) { trap_reason = "truncated local.tee"; goto trap; }
                if (idx >= local_count) { trap_reason = "local index out of range"; goto trap; }
                if (sp <= floor) { trap_reason = "stack underflow"; goto trap; }
                stack[frame_base + idx] = stack[sp - 1];
                break;
            }
            case OP_GLOBAL_GET: {
                uint32_t idx;
                if (!read_u32leb(code, code_len, &pc, &idx)) { trap_reason = "truncated global.get"; goto trap; }
                if (idx >= mod->global_count) { trap_reason = "global index out of range"; goto trap; }
                PUSH(inst->globals[idx]);
                break;
            }
            case OP_GLOBAL_SET: {
                uint32_t idx;
                if (!read_u32leb(code, code_len, &pc, &idx)) { trap_reason = "truncated global.set"; goto trap; }
                if (idx >= mod->global_count) { trap_reason = "global index out of range"; goto trap; }
                if (!mod->globals[idx].is_mutable) { trap_reason = "write to immutable global"; goto trap; }
                int32_t v;
                POP(v);
                inst->globals[idx] = v;
                break;
            }

            case OP_I32_LOAD: case OP_I32_LOAD8_S: case OP_I32_LOAD8_U:
            case OP_I32_LOAD16_S: case OP_I32_LOAD16_U: {
                uint32_t align, offset;
                if (!read_u32leb(code, code_len, &pc, &align)) { trap_reason = "truncated load"; goto trap; }
                if (!read_u32leb(code, code_len, &pc, &offset)) { trap_reason = "truncated load"; goto trap; }
                (void)align;
                int32_t base;
                POP(base);
                uint32_t addr = (uint32_t)base + offset;
                uint32_t size = (op == OP_I32_LOAD) ? 4 : (op == OP_I32_LOAD16_S || op == OP_I32_LOAD16_U) ? 2 : 1;
                if (!mem_bounds_ok(inst, addr, size)) { trap_reason = "out-of-bounds memory access"; goto trap; }
                int32_t val;
                switch (op) {
                    case OP_I32_LOAD: val = load_le32(inst->memory + addr); break;
                    case OP_I32_LOAD8_S: val = (int32_t)(int8_t)inst->memory[addr]; break;
                    case OP_I32_LOAD8_U: val = (int32_t)(uint32_t)inst->memory[addr]; break;
                    case OP_I32_LOAD16_S: val = (int32_t)(int16_t)load_le16(inst->memory + addr); break;
                    default: val = (int32_t)(uint32_t)(uint16_t)load_le16(inst->memory + addr); break;
                }
                PUSH(val);
                break;
            }
            case OP_I32_STORE: case OP_I32_STORE8: case OP_I32_STORE16: {
                uint32_t align, offset;
                if (!read_u32leb(code, code_len, &pc, &align)) { trap_reason = "truncated store"; goto trap; }
                if (!read_u32leb(code, code_len, &pc, &offset)) { trap_reason = "truncated store"; goto trap; }
                (void)align;
                int32_t value, base;
                POP(value);
                POP(base);
                uint32_t addr = (uint32_t)base + offset;
                uint32_t size = (op == OP_I32_STORE) ? 4 : (op == OP_I32_STORE16) ? 2 : 1;
                if (!mem_bounds_ok(inst, addr, size)) { trap_reason = "out-of-bounds memory access"; goto trap; }
                if (op == OP_I32_STORE) store_le32(inst->memory + addr, value);
                else if (op == OP_I32_STORE16) store_le16(inst->memory + addr, value);
                else inst->memory[addr] = (uint8_t)value;
                break;
            }
            case OP_MEMORY_SIZE:
                pc++; /* reserved byte */
                PUSH((int32_t)inst->memory_pages);
                break;
            case OP_MEMORY_GROW: {
                pc++; /* reserved byte */
                int32_t delta;
                POP(delta);
                if (delta < 0) { PUSH(-1); break; }
                uint32_t old_pages = inst->memory_pages;
                if (delta == 0) { PUSH((int32_t)old_pages); break; }
                uint32_t new_pages = old_pages + (uint32_t)delta;
                if (new_pages < old_pages || new_pages > inst->memory_max_pages) { PUSH(-1); break; }
                uint8_t *newmem = (uint8_t *)kmalloc((size_t)new_pages * WASM_PAGE_SIZE);
                if (!newmem) { PUSH(-1); break; }
                memset(newmem, 0, (size_t)new_pages * WASM_PAGE_SIZE);
                if (inst->memory) {
                    memcpy(newmem, inst->memory, (size_t)old_pages * WASM_PAGE_SIZE);
                    kfree(inst->memory);
                }
                inst->memory = newmem;
                inst->memory_pages = new_pages;
                PUSH((int32_t)old_pages);
                break;
            }

            case OP_I32_CONST: {
                int32_t v;
                if (!read_i32leb(code, code_len, &pc, &v)) { trap_reason = "truncated i32.const"; goto trap; }
                PUSH(v);
                break;
            }

            case OP_I32_EQZ: { int32_t a; POP(a); PUSH(a == 0); break; }
            case OP_I32_EQ: { int32_t b, a; POP(b); POP(a); PUSH(a == b); break; }
            case OP_I32_NE: { int32_t b, a; POP(b); POP(a); PUSH(a != b); break; }
            case OP_I32_LT_S: { int32_t b, a; POP(b); POP(a); PUSH(a < b); break; }
            case OP_I32_LT_U: { int32_t b, a; POP(b); POP(a); PUSH((uint32_t)a < (uint32_t)b); break; }
            case OP_I32_GT_S: { int32_t b, a; POP(b); POP(a); PUSH(a > b); break; }
            case OP_I32_GT_U: { int32_t b, a; POP(b); POP(a); PUSH((uint32_t)a > (uint32_t)b); break; }
            case OP_I32_LE_S: { int32_t b, a; POP(b); POP(a); PUSH(a <= b); break; }
            case OP_I32_LE_U: { int32_t b, a; POP(b); POP(a); PUSH((uint32_t)a <= (uint32_t)b); break; }
            case OP_I32_GE_S: { int32_t b, a; POP(b); POP(a); PUSH(a >= b); break; }
            case OP_I32_GE_U: { int32_t b, a; POP(b); POP(a); PUSH((uint32_t)a >= (uint32_t)b); break; }

            case OP_I32_ADD: { int32_t b, a; POP(b); POP(a); PUSH(a + b); break; }
            case OP_I32_SUB: { int32_t b, a; POP(b); POP(a); PUSH(a - b); break; }
            case OP_I32_MUL: { int32_t b, a; POP(b); POP(a); PUSH(a * b); break; }
            case OP_I32_DIV_S: {
                int32_t b, a; POP(b); POP(a);
                if (b == 0) { trap_reason = "integer divide by zero"; goto trap; }
                if (a == (int32_t)0x80000000 && b == -1) { trap_reason = "integer overflow"; goto trap; }
                PUSH(a / b);
                break;
            }
            case OP_I32_DIV_U: {
                int32_t b, a; POP(b); POP(a);
                if (b == 0) { trap_reason = "integer divide by zero"; goto trap; }
                PUSH((int32_t)((uint32_t)a / (uint32_t)b));
                break;
            }
            case OP_I32_REM_S: {
                int32_t b, a; POP(b); POP(a);
                if (b == 0) { trap_reason = "integer divide by zero"; goto trap; }
                PUSH((a == (int32_t)0x80000000 && b == -1) ? 0 : a % b);
                break;
            }
            case OP_I32_REM_U: {
                int32_t b, a; POP(b); POP(a);
                if (b == 0) { trap_reason = "integer divide by zero"; goto trap; }
                PUSH((int32_t)((uint32_t)a % (uint32_t)b));
                break;
            }
            case OP_I32_AND: { int32_t b, a; POP(b); POP(a); PUSH(a & b); break; }
            case OP_I32_OR: { int32_t b, a; POP(b); POP(a); PUSH(a | b); break; }
            case OP_I32_XOR: { int32_t b, a; POP(b); POP(a); PUSH(a ^ b); break; }
            case OP_I32_SHL: { int32_t b, a; POP(b); POP(a); PUSH(a << (b & 31)); break; }
            case OP_I32_SHR_S: { int32_t b, a; POP(b); POP(a); PUSH(a >> (b & 31)); break; }
            case OP_I32_SHR_U: { int32_t b, a; POP(b); POP(a); PUSH((int32_t)((uint32_t)a >> (b & 31))); break; }

            default:
                trap_reason = "internal error: opcode accepted by validator but not implemented";
                goto trap;
        }
    }
    goto done;

trap:
    serial_printf("wasm: trap in function %u: %s\n", func_idx, trap_reason ? trap_reason : "?");
    ok = 0;

done:
    if (!ok) {
        *sp_io = frame_base;
        *label_top_io = label_base;
        return 0;
    }
    {
        int32_t retval = 0;
        if (ft->result_count == 1) {
            if (sp <= floor) {
                serial_printf("wasm: trap: function %u fell through without leaving its declared result\n", func_idx);
                *sp_io = frame_base;
                *label_top_io = label_base;
                return 0;
            }
            retval = stack[sp - 1];
        }
        *sp_io = frame_base;
        if (ft->result_count == 1) stack[(*sp_io)++] = retval;
        *label_top_io = label_base;
        if (result_out) *result_out = retval;
    }
    return 1;
}

#undef POP
#undef PUSH

int wasm_call_export(struct wasm_instance *inst, const char *export_name,
                      const int32_t *args, int argc, int32_t *result_out) {
    struct wasm_module *mod = inst->module;
    struct wasm_export *ex = NULL;
    for (uint32_t i = 0; i < mod->export_count; i++) {
        if (mod->exports[i].kind == 0 && strcmp(mod->exports[i].name, export_name) == 0) {
            ex = &mod->exports[i];
            break;
        }
    }
    if (!ex) {
        serial_printf("wasm: no exported function named '%s'\n", export_name);
        return 0;
    }
    if (ex->index >= mod->func_count) {
        serial_printf("wasm: export '%s' references an invalid function index\n", export_name);
        return 0;
    }
    struct wasm_func *fn = &mod->funcs[ex->index];
    if (fn->is_imported) {
        serial_printf("wasm: export '%s' is an imported function with no host binding\n", export_name);
        return 0;
    }
    struct wasm_functype *ft = &mod->types[fn->type_idx];
    if ((uint32_t)argc != ft->param_count) {
        serial_printf("wasm: '%s' expects %u args, got %d\n", export_name, ft->param_count, argc);
        return 0;
    }

    uint32_t sp = 0, label_top = 0;
    for (int i = 0; i < argc; i++) {
        if (!vs_push(inst->value_stack, &sp, args[i])) {
            serial_printf("wasm: too many arguments\n");
            return 0;
        }
    }
    int32_t result = 0;
    if (!exec_function(inst, ex->index, &sp, &label_top, 1, &result)) return 0;
    if (result_out) *result_out = result;
    return 1;
}
