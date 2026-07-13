#ifndef JS_JS_H
#define JS_JS_H

#include <stdint.h>

/* ---- Arena allocator -----------------------------------------------
 * Every AST node, object, string, and environment created while running
 * a page's scripts comes from one bump-allocated arena. There is no
 * garbage collector: js_arena_reset() throws the whole thing away (one
 * kfree, replaced by a fresh block) whenever a new page is fetched, and
 * nothing is ever freed piecemeal in between -- safe because a page's
 * scripts and their DOM only need to live as long as that page is
 * loaded. Numbers are plain 32-bit integers, not IEEE754 doubles: this
 * kernel is built with -mno-80387 -mno-sse (no FPU/SSE state is ever
 * initialized), so floating point literally cannot be generated
 * anywhere in it -- not a shortcut, a hard constraint. */
void js_arena_reset(void);
void *js_alloc(uint32_t size);
char *js_strdup(const char *s);

/* ---- Values ---------------------------------------------------------*/
enum js_type { JS_UNDEFINED, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ };

struct js_object;

typedef struct js_value {
    enum js_type type;
    union {
        int boolean;
        int32_t number;
        const char *string;
        struct js_object *object;
    } as;
} js_value;

/* JS_OBJ_ARROW is split out from JS_OBJ_FUNCTION only so js_call() can
 * tell them apart: an arrow function must NOT get its own `this`
 * binding (it resolves lexically through closure_env), everything else
 * about invoking one is identical to a regular function. */
enum js_obj_kind { JS_OBJ_PLAIN, JS_OBJ_ARRAY, JS_OBJ_FUNCTION, JS_OBJ_ARROW, JS_OBJ_NATIVE, JS_OBJ_DOM_ELEMENT, JS_OBJ_DOM_STYLE };

struct js_prop {
    char *name;
    js_value value;
    struct js_prop *next;
};

struct js_node; /* AST, defined below */
struct js_env;

/* `fn_obj` is the JS_OBJ_NATIVE object being called (== the object a
 * plain js_native_fn ptr is stored on) -- passed through so a native
 * can carry its own per-instance context via `native_data` below (e.g.
 * a WASM export binding needs to remember *which* wasm_instance and
 * *which* export name each wrapper function was created for). Existing
 * natives that don't need context (console.log, Math.*) just ignore it. */
typedef js_value (*js_native_fn)(js_value this_val, js_value *args, int argc, struct js_object *fn_obj);

struct js_object {
    enum js_obj_kind kind;
    struct js_prop *props;
    /* JS_OBJ_FUNCTION */
    struct js_node *func_node;
    struct js_env *closure_env;
    /* JS_OBJ_NATIVE */
    js_native_fn native_fn;
    void *native_data; /* opaque, arena-allocated context for native_fn; NULL if unused */
    /* JS_OBJ_DOM_ELEMENT */
    struct dom_node *dom_node;
};

js_value js_undefined(void);
js_value js_null_value(void);
js_value js_make_bool(int b);
js_value js_make_num(int32_t n);
js_value js_make_str(const char *s);
js_value js_make_object(struct js_object *obj);
struct js_object *js_new_object(enum js_obj_kind kind);

js_value js_get_prop(struct js_object *obj, const char *name);
void js_set_prop(struct js_object *obj, const char *name, js_value value);

int js_to_bool(js_value v);
int32_t js_to_num(js_value v);
/* Returns a NUL-terminated string valid for the lifetime of the current
 * page's arena (never freed piecemeal, so safe to hold onto). */
const char *js_to_string(js_value v);

/* ---- Environments (scope chain) -------------------------------------*/
struct js_binding {
    char *name;
    js_value value;
    int is_const;
    struct js_binding *next;
};

struct js_env {
    struct js_binding *vars;
    struct js_env *parent;
};

struct js_env *js_env_new(struct js_env *parent);
/* Declares in THIS scope (used for function params, var/let/const). */
void js_env_declare(struct js_env *env, const char *name, js_value value, int is_const);
/* Walks up the chain; returns 1 and fills *out if found. */
int js_env_get(struct js_env *env, const char *name, js_value *out);
/* Walks up the chain to find an existing binding and assigns it; returns
 * 0 (silently -- matches loose non-strict-mode JS) if never declared. */
int js_env_set(struct js_env *env, const char *name, js_value value);

/* ---- AST -------------------------------------------------------------*/
enum js_node_type {
    JS_PROGRAM, JS_VAR_DECL, JS_FUNC_DECL, JS_IF, JS_FOR, JS_WHILE, JS_BLOCK,
    JS_RETURN, JS_BREAK, JS_CONTINUE, JS_EXPR_STMT, JS_EMPTY,
    JS_NUM_LIT, JS_STR_LIT, JS_BOOL_LIT, JS_NULL_LIT, JS_UNDEF_LIT, JS_THIS,
    JS_IDENT, JS_ARRAY_LIT, JS_OBJECT_LIT, JS_FUNC_EXPR,
    JS_BINARY, JS_LOGICAL, JS_UNARY, JS_UPDATE, JS_ASSIGN,
    JS_CALL, JS_MEMBER, JS_CONDITIONAL, JS_SEQ,
    /* ES6+ additions -- see js/parser.c and js/interp.c for how each is
     * built/evaluated. */
    JS_ARROW_FUNC,      /* func-shaped: params+body; name unused */
    JS_CLASS_DECL,      /* statement form: `class Foo { ... }` */
    JS_CLASS_EXPR,      /* expression form: `class { ... }` / `class Foo { ... }` as a value */
    JS_NEW,             /* call-shaped: callee+args, e.g. `new Foo(1)` */
    JS_TEMPLATE_LIT,    /* backtick string; `parts` alternates STR_LIT chunks and interpolated exprs */
    JS_SPREAD,          /* unary-shaped: operand is the spread expression, in array lits / call args */
    JS_ARRAY_PATTERN,   /* array_lit-shaped destructuring target: `[a, b]` */
    JS_OBJECT_PATTERN,  /* object_lit-shaped destructuring target: `{a, b}` / `{a: x}` */
};

struct js_node {
    enum js_node_type type;
    struct js_node *next; /* sibling: statement lists, argument lists, etc. */
    union {
        struct { struct js_node *body; } program;
        /* one node per declarator; destructuring (`const {a,b}=o`) sets
         * `pattern` (an ARRAY_PATTERN/OBJECT_PATTERN node) and leaves
         * `name` NULL instead of using a plain identifier name. */
        struct { char *name; struct js_node *init; struct js_node *pattern; } var_decl;
        struct { char *name; struct js_node *params; struct js_node *body; } func; /* also used for ARROW_FUNC and class methods */
        struct { struct js_node *test, *cons, *alt; } if_stmt;
        struct { struct js_node *init, *test, *update, *body; } for_stmt;
        struct { struct js_node *test, *body; } while_stmt;
        struct { struct js_node *stmts; } block;
        struct { struct js_node *value; } return_stmt;
        struct { struct js_node *expr; } expr_stmt;
        struct { int32_t value; } num_lit;
        struct { char *value; } str_lit;
        struct { int value; } bool_lit;
        struct { char *name; } ident;
        struct { struct js_node *elements; } array_lit; /* also used for ARRAY_PATTERN */
        struct { struct js_node *props; } object_lit; /* each: var_decl-shaped, name+init; also used for OBJECT_PATTERN (name=source key, init=target) */
        struct { const char *op; struct js_node *left, *right; } binary;
        struct { const char *op; struct js_node *operand; int prefix; } unary; /* also used for SPREAD (operand only) */
        struct { const char *op; struct js_node *target; struct js_node *value; } assign;
        struct { struct js_node *callee; struct js_node *args; } call; /* also used for NEW */
        struct { struct js_node *object; struct js_node *property; int computed; } member;
        struct { struct js_node *test, *cons, *alt; } conditional;
        struct { struct js_node *left, *right; } seq;
        struct { char *name; struct js_node *methods; } class_decl; /* methods: func-shaped nodes, chained; one may be named "constructor" */
        struct { struct js_node *parts; } template_lit;
    } u;
};

struct js_node *js_node_new(enum js_node_type type);

/* ---- Parser ------------------------------------------------------------*/
struct js_lexer;
struct js_node *js_parse_program(struct js_lexer *lx);

/* ---- Interpreter -------------------------------------------------------*/
/* Runs every top-level statement in `program` against `env`. */
void js_run_program(struct js_node *program, struct js_env *env);
/* Invokes a JS_OBJ_FUNCTION value (e.g. an onclick handler) directly. */
js_value js_call(js_value fn, js_value this_val, js_value *args, int argc);

#endif
