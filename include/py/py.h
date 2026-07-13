#ifndef PY_PY_H
#define PY_PY_H

#include <stdint.h>

/* ---- Arena allocator -----------------------------------------------
 * Same pattern as js/value.c: one big kmalloc'd arena, bump-allocated,
 * reset once per py_run() call. Nothing is ever freed piecemeal --
 * safe because a script's AST, environments and values only need to
 * live for the duration of that one run. UNLIKE the JS engine, this
 * interpreter is compiled with real x87 float support (see the
 * Makefile's PY_CFLAGS: everything under py, this directory's sibling,
 * is built without -mno-80387 and without -mgeneral-regs-only), so
 * py_value below uses a genuine `double` for PY_VAL_FLOAT. */
void py_arena_reset(void);
void *py_alloc(uint32_t size);
char *py_strdup(const char *s);

/* ---- Values ---------------------------------------------------------*/
enum py_type { PY_VAL_NONE, PY_VAL_BOOL, PY_VAL_INT, PY_VAL_FLOAT, PY_VAL_STR, PY_VAL_OBJ };

struct py_object;
struct py_node; /* AST, defined below */
struct py_env;

typedef struct py_value {
    enum py_type type;
    union {
        int boolean;
        int64_t i;
        double f;
        const char *s;
        struct py_object *obj;
    } as;
} py_value;

enum py_obj_kind { PY_OBJ_LIST, PY_OBJ_FUNC, PY_OBJ_NATIVE, PY_OBJ_RANGE };

typedef py_value (*py_native_fn)(py_value *args, int argc);

struct py_object {
    enum py_obj_kind kind;
    /* PY_OBJ_LIST */
    py_value *items;
    int length;
    /* PY_OBJ_FUNC */
    struct py_node *func_node;
    struct py_env *closure_env;
    /* PY_OBJ_NATIVE */
    py_native_fn native_fn;
    const char *native_name;
    /* PY_OBJ_RANGE */
    int64_t range_start, range_stop, range_step;
};

py_value py_none(void);
py_value py_make_bool(int b);
py_value py_make_int(int64_t n);
py_value py_make_float(double f);
py_value py_make_str(const char *s);
py_value py_make_object(struct py_object *obj);
struct py_object *py_new_object(enum py_obj_kind kind);
struct py_object *py_new_list(int length);

int py_to_bool(py_value v);
int py_is_number(py_value v);   /* INT, FLOAT, or BOOL */
double py_to_double(py_value v);
int64_t py_to_int64(py_value v);
/* str()-style conversion: no quotes around strings. Valid for the
 * lifetime of the current run's arena. */
const char *py_to_str(py_value v);

/* ---- Environments (scope chain) -------------------------------------*/
struct py_binding {
    char *name;
    py_value value;
    struct py_binding *next;
};

struct py_env {
    struct py_binding *vars;
    struct py_env *parent;
};

struct py_env *py_env_new(struct py_env *parent);
/* Declares/updates in THIS scope. */
void py_env_declare(struct py_env *env, const char *name, py_value value);
/* Walks up the chain; returns 1 and fills *out if found. */
int py_env_get(struct py_env *env, const char *name, py_value *out);
/* Walks up the chain to find an existing binding and assigns it there;
 * returns 0 if never declared anywhere in the chain (caller then
 * declares fresh in the current scope -- same permissive rule
 * js/interp.c uses for js_env_set, chosen deliberately here too since
 * this interpreter has no global/nonlocal keywords to disambiguate
 * "new local" from "assign outer" the way real Python does). */
int py_env_set(struct py_env *env, const char *name, py_value value);

void py_register_builtins(struct py_env *env);

/* ---- AST -------------------------------------------------------------*/
enum py_node_type {
    PY_PROGRAM, PY_BLOCK, PY_IF, PY_WHILE, PY_FOR, PY_FUNC_DEF,
    PY_RETURN, PY_BREAK, PY_CONTINUE, PY_PASS, PY_EXPR_STMT, PY_ASSIGN,

    PY_INT_LIT, PY_FLOAT_LIT, PY_STR_LIT, PY_BOOL_LIT, PY_NONE_LIT,
    PY_LIST_LIT, PY_IDENT,
    PY_BINARY, PY_UNARY, PY_LOGICAL, PY_NOT,
    PY_CALL, PY_INDEX,
};

struct py_node {
    enum py_node_type type;
    struct py_node *next; /* sibling: statement lists, argument lists, param lists, etc. */
    union {
        struct { struct py_node *body; } program;
        struct { struct py_node *stmts; } block;
        struct { struct py_node *test, *body, *orelse; } if_stmt; /* orelse: PY_IF (elif) or PY_BLOCK (else) or NULL */
        struct { struct py_node *test, *body; } while_stmt;
        struct { char *var; struct py_node *iter, *body; } for_stmt;
        struct { char *name; struct py_node *params; struct py_node *body; } func;
        struct { struct py_node *value; } return_stmt;
        struct { struct py_node *expr; } expr_stmt;
        struct { const char *op; struct py_node *target; struct py_node *value; } assign; /* op "=" for plain assignment */
        struct { int64_t value; } int_lit;
        struct { double value; } float_lit;
        struct { char *value; } str_lit;
        struct { int value; } bool_lit;
        struct { char *name; } ident;
        struct { struct py_node *elements; } list_lit;
        struct { const char *op; struct py_node *left, *right; } binary;
        struct { const char *op; struct py_node *operand; } unary;
        struct { struct py_node *callee; struct py_node *args; } call;
        struct { struct py_node *object, *index; } index_expr;
    } u;
};

struct py_node *py_node_new(enum py_node_type type);

/* ---- Parser ------------------------------------------------------------*/
struct py_lexer;
/* Returns NULL (and leaves an error message retrievable via py_run's
 * error path) on any syntax error. */
struct py_node *py_parse_program(struct py_lexer *lx);

/* ---- Interpreter -------------------------------------------------------*/
void py_run_program(struct py_node *program, struct py_env *env);

/* ---- Public entry point ------------------------------------------------
 * The only thing external callers (a terminal/shell) need. Runs `source`
 * to completion. Every executed print(...) calls `out` once with the
 * space-joined str()'d arguments plus a trailing '\n'. On a syntax or
 * runtime error, calls `out` exactly once with a one-line "SomeError:
 * ...\n" message and stops -- never crashes, never loops forever, never
 * reads past the end of `source`. */
void py_run(const char *source, void (*out)(const char *));

#endif
