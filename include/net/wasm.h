#ifndef NET_WASM_H
#define NET_WASM_H

#include <stdint.h>

/* A from-scratch WebAssembly *interpreter* (no JIT) for the binary
 * format described in the WebAssembly Core Specification. Scoped down
 * hard, mirroring the scope cuts this codebase already makes for its
 * other from-scratch parsers (net/png.c's 8-bit/non-interlaced-only
 * cut, net/tls.c's single-cipher-suite cut):
 *
 *   - i32 ONLY. No i64, no f32/f64. This matches include/js/js.h's
 *     documented reason for the JS engine also being int32-only: this
 *     kernel is built with -mno-sse -mno-80387 -mgeneral-regs-only, so
 *     emitting so much as one float instruction anywhere in the C code
 *     that implements this interpreter is a hard compile error -- it's
 *     not a style choice, the compiler physically will not do it. i64
 *     is left out too (not a hardware constraint, just scope: the task
 *     this was built for doesn't need 64-bit ints, and every i64
 *     opcode is another dozen cases of interpreter logic for no
 *     payoff). A module using ANY f32/f64 instruction, or declaring an
 *     f32/f64/i64 param, result, local, global, or block type, is
 *     rejected during wasm_parse_module() with a clean failure --
 *     never silently misread, never executed with wrong semantics.
 *
 *   - No tables, no call_indirect (it parses, but always traps cleanly
 *     at the moment it would actually execute -- see net/wasm.c). No
 *     bulk-memory/reference-types/SIMD opcodes (0xFC/0xFD prefixes) --
 *     rejected like any other unsupported opcode. Table and Element
 *     sections are present in real modules sometimes even when unused
 *     (e.g. clang emits an empty one); those are simply skipped byte-
 *     for-byte rather than rejected, since an *unused* table shouldn't
 *     stop an otherwise-supported module from running.
 *
 *   - Imports: function imports are parsed and given a slot in the
 *     function index space (so index numbering for everything after
 *     them stays correct) but are never bound to a host function --
 *     calling one traps cleanly with "no host binding" rather than
 *     silently doing nothing or crashing. Table/memory/global imports
 *     are rejected outright at parse time: unlike a function import
 *     (safe to leave as a stub that just fails if actually called), a
 *     module that imports its *memory* or a *global* would run with
 *     silently wrong state if we faked one up, so this pass refuses
 *     the whole module instead. Wiring real host imports (e.g. so JS
 *     can supply them) is left to whoever integrates this interpreter
 *     into the browser/JS engine.
 *
 *   - Linear memory: at most one memory (per the spec's own MVP
 *     restriction), backed by a plain kmalloc'd byte buffer. Grows via
 *     memory.grow, but never past WASM_MAX_MEMORY_PAGES regardless of
 *     what the module declares as its max -- a hard cap so a hostile
 *     or just-large module can't run away with the kernel's shared
 *     32MB kheap (see kernel/kheap.c), the same reasoning as
 *     net/bmp.h's BMP_MAX_DIMENSION cap on image size.
 *
 *   - This is a structural parser + an opcode allow-list, not a full
 *     WASM validator: it does not simulate the operand-stack type
 *     discipline the spec's validation algorithm requires (e.g. it
 *     trusts that a function's declared result actually ends up on
 *     the stack). A module that is byte-well-formed but fails real
 *     spec validation may behave unpredictably rather than being
 *     rejected outright. Every module produced by a real toolchain
 *     (clang --target=wasm32, wat2wasm, etc.) is spec-valid, so this
 *     only matters for deliberately-adversarial input.
 *
 * Ownership/API convention follows net/png.h and net/bmp.h: the caller
 * owns the struct wasm_module/struct wasm_instance storage (stack or
 * kmalloc'd, doesn't matter), the *_parse_module/_instantiate calls
 * kmalloc internal buffers into it, and wasm_free_module()/
 * wasm_free_instance() release just those. All functions return 1 for
 * success / 0 for failure, in that same convention. */

#define WASM_MAX_TYPES      64
#define WASM_MAX_FUNCS      128
#define WASM_MAX_PARAMS     8
#define WASM_MAX_GLOBALS    32
#define WASM_MAX_EXPORTS    64
#define WASM_MAX_DATA_SEGS  16
#define WASM_MAX_NAME_LEN   64
#define WASM_MAX_LOCALS     64  /* params + declared locals, per function */

#define WASM_PAGE_SIZE         65536u
#define WASM_MAX_MEMORY_PAGES  64  /* hard cap regardless of module's own max: 64*64KB = 4MB */

struct wasm_functype {
    uint8_t param_types[WASM_MAX_PARAMS]; /* always 0x7F (i32) -- anything else is rejected at parse time */
    uint8_t param_count;
    uint8_t result_count; /* 0 or 1: this is the pre-multi-value MVP subset, not a scope cut */
    uint8_t result_type;  /* valid iff result_count == 1; always 0x7F (i32) */
};

struct wasm_func {
    uint32_t type_idx;
    int is_imported;      /* 1 => no code; calling it traps cleanly at runtime */
    const uint8_t *code;  /* points into module->raw; NULL if is_imported */
    uint32_t code_len;
    uint32_t local_count; /* param_count + declared locals, all i32 */
};

struct wasm_global {
    int is_mutable;
    int32_t init_value;   /* only `i32.const <N> end` init exprs are supported */
};

struct wasm_export {
    char name[WASM_MAX_NAME_LEN];
    uint8_t kind;   /* 0=func, 1=table, 2=memory, 3=global (table exports are recorded but useless -- no table support) */
    uint32_t index;
};

struct wasm_data_seg {
    uint32_t offset;
    const uint8_t *data; /* points into module->raw */
    uint32_t len;
};

struct wasm_module {
    uint8_t *raw;      /* kmalloc'd copy of the whole input; code/data pointers above point into this */
    uint32_t raw_len;

    struct wasm_functype types[WASM_MAX_TYPES];
    uint32_t type_count;

    struct wasm_func funcs[WASM_MAX_FUNCS]; /* imported functions first, then defined ones, per the spec's index space */
    uint32_t func_count;
    uint32_t imported_func_count;

    struct wasm_global globals[WASM_MAX_GLOBALS];
    uint32_t global_count;

    struct wasm_export exports[WASM_MAX_EXPORTS];
    uint32_t export_count;

    int has_memory;
    uint32_t memory_min_pages;
    uint32_t memory_max_pages; /* already clamped to WASM_MAX_MEMORY_PAGES */

    struct wasm_data_seg data_segs[WASM_MAX_DATA_SEGS];
    uint32_t data_seg_count;

    int has_start;
    uint32_t start_func_idx;
};

struct wasm_instance {
    struct wasm_module *module; /* borrowed -- caller must keep the module alive as long as the instance lives */

    uint8_t *memory;            /* kmalloc'd, memory_pages * WASM_PAGE_SIZE bytes; NULL if module has no memory */
    uint32_t memory_pages;
    uint32_t memory_max_pages;

    int32_t globals[WASM_MAX_GLOBALS]; /* mutable runtime copies of module->globals[]' init values */

    int32_t *value_stack; /* kmalloc'd operand+locals stack, shared across the whole active call chain (see net/wasm.c) */
    void *label_stack;    /* kmalloc'd; opaque here (struct wasm_label is private to net/wasm.c) */
};

/* Parses a WASM binary module from `data` (length `len`) into `out`.
 * Copies the input into a kmalloc'd buffer it owns, then walks every
 * section, rejecting anything outside the scope documented above.
 * Returns 1 on success (free with wasm_free_module()), 0 on any
 * unsupported or malformed input (logged via serial_printf). */
int wasm_parse_module(const uint8_t *data, uint32_t len, struct wasm_module *out);
void wasm_free_module(struct wasm_module *mod);

/* Instantiates a parsed module: allocates linear memory, applies Data
 * segments, initializes globals, and (if the module has a Start
 * section) runs the start function. `mod` must outlive `out`. Returns
 * 1 on success (free with wasm_free_instance()), 0 on failure (e.g.
 * the start function trapped, or a data segment doesn't fit). */
int wasm_instantiate(struct wasm_module *mod, struct wasm_instance *out);
void wasm_free_instance(struct wasm_instance *inst);

/* Looks up an exported function by name and calls it with `argc` i32
 * arguments (must match the function's declared param count exactly).
 * On success returns 1 and, if the function has a result, writes it to
 * *result_out (writes 0 if the function has no result). Returns 0 on
 * anything from "no such export" to a runtime trap (divide by zero,
 * out-of-bounds memory access, unreachable, call stack too deep, a
 * call to an unbound import, call_indirect, ...) -- all logged via
 * serial_printf, all trapped cleanly rather than corrupting memory. */
int wasm_call_export(struct wasm_instance *inst, const char *export_name,
                      const int32_t *args, int argc, int32_t *result_out);

#endif
