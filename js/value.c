#include <js/js.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

#define JS_ARENA_SIZE (256u * 1024)

static uint8_t *js_arena = NULL;
static uint32_t js_arena_used = 0;
static uint8_t js_oom_scratch[256]; /* absorbs allocations after OOM so callers never see NULL */

void js_arena_reset(void) {
    if (!js_arena) js_arena = (uint8_t *)kmalloc(JS_ARENA_SIZE);
    js_arena_used = 0;
}

void *js_alloc(uint32_t size) {
    size = (size + 7) & ~7u;
    if (!js_arena || js_arena_used + size > JS_ARENA_SIZE) {
        serial_printf("js: arena exhausted (page's script is too large for this pass)\n");
        return js_oom_scratch;
    }
    void *p = js_arena + js_arena_used;
    js_arena_used += size;
    return p;
}

char *js_strdup(const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    char *copy = (char *)js_alloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

js_value js_undefined(void) { js_value v; v.type = JS_UNDEFINED; v.as.number = 0; return v; }
js_value js_null_value(void) { js_value v; v.type = JS_NULL; v.as.number = 0; return v; }
js_value js_make_bool(int b) { js_value v; v.type = JS_BOOL; v.as.boolean = b ? 1 : 0; return v; }
js_value js_make_num(int32_t n) { js_value v; v.type = JS_NUM; v.as.number = n; return v; }
js_value js_make_str(const char *s) { js_value v; v.type = JS_STR; v.as.string = s; return v; }
js_value js_make_object(struct js_object *obj) { js_value v; v.type = JS_OBJ; v.as.object = obj; return v; }

struct js_object *js_new_object(enum js_obj_kind kind) {
    struct js_object *obj = (struct js_object *)js_alloc(sizeof(struct js_object));
    memset(obj, 0, sizeof(*obj));
    obj->kind = kind;
    return obj;
}

js_value js_get_prop(struct js_object *obj, const char *name) {
    for (struct js_prop *p = obj->props; p; p = p->next) {
        if (strcmp(p->name, name) == 0) return p->value;
    }
    return js_undefined();
}

void js_set_prop(struct js_object *obj, const char *name, js_value value) {
    for (struct js_prop *p = obj->props; p; p = p->next) {
        if (strcmp(p->name, name) == 0) { p->value = value; return; }
    }
    struct js_prop *p = (struct js_prop *)js_alloc(sizeof(struct js_prop));
    p->name = js_strdup(name);
    p->value = value;
    p->next = obj->props;
    obj->props = p;
}

int js_to_bool(js_value v) {
    switch (v.type) {
        case JS_UNDEFINED: case JS_NULL: return 0;
        case JS_BOOL: return v.as.boolean;
        case JS_NUM: return v.as.number != 0;
        case JS_STR: return v.as.string[0] != 0;
        case JS_OBJ: return 1;
    }
    return 0;
}

int32_t js_to_num(js_value v) {
    switch (v.type) {
        case JS_UNDEFINED: case JS_NULL: return 0;
        case JS_BOOL: return v.as.boolean;
        case JS_NUM: return v.as.number;
        case JS_STR: {
            int32_t n = 0, sign = 1;
            const char *s = v.as.string;
            while (*s == ' ') s++;
            if (*s == '-') { sign = -1; s++; }
            else if (*s == '+') s++;
            if (!*s) return 0;
            while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
            return n * sign;
        }
        case JS_OBJ: return 0;
    }
    return 0;
}

static void utoa_signed(int32_t n, char *out) {
    if (n < 0) { *out++ = '-'; n = -n; }
    char tmp[12];
    int i = 0;
    if (n == 0) tmp[i++] = '0';
    while (n > 0) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    while (i > 0) *out++ = tmp[--i];
    *out = 0;
}

const char *js_to_string(js_value v) {
    switch (v.type) {
        case JS_UNDEFINED: return "undefined";
        case JS_NULL: return "null";
        case JS_BOOL: return v.as.boolean ? "true" : "false";
        case JS_STR: return v.as.string;
        case JS_NUM: {
            char *buf = (char *)js_alloc(16);
            utoa_signed(v.as.number, buf);
            return buf;
        }
        case JS_OBJ:
            return v.as.object->kind == JS_OBJ_ARRAY ? "[array]" :
                   v.as.object->kind == JS_OBJ_FUNCTION ? "[function]" : "[object]";
    }
    return "";
}

struct js_env *js_env_new(struct js_env *parent) {
    struct js_env *env = (struct js_env *)js_alloc(sizeof(struct js_env));
    env->vars = NULL;
    env->parent = parent;
    return env;
}

void js_env_declare(struct js_env *env, const char *name, js_value value, int is_const) {
    for (struct js_binding *b = env->vars; b; b = b->next) {
        if (strcmp(b->name, name) == 0) { b->value = value; return; }
    }
    struct js_binding *b = (struct js_binding *)js_alloc(sizeof(struct js_binding));
    b->name = js_strdup(name);
    b->value = value;
    b->is_const = is_const;
    b->next = env->vars;
    env->vars = b;
}

int js_env_get(struct js_env *env, const char *name, js_value *out) {
    for (struct js_env *e = env; e; e = e->parent) {
        for (struct js_binding *b = e->vars; b; b = b->next) {
            if (strcmp(b->name, name) == 0) { *out = b->value; return 1; }
        }
    }
    return 0;
}

int js_env_set(struct js_env *env, const char *name, js_value value) {
    for (struct js_env *e = env; e; e = e->parent) {
        for (struct js_binding *b = e->vars; b; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                if (!b->is_const) b->value = value;
                return 1;
            }
        }
    }
    return 0;
}

struct js_node *js_node_new(enum js_node_type type) {
    struct js_node *n = (struct js_node *)js_alloc(sizeof(struct js_node));
    memset(n, 0, sizeof(*n));
    n->type = type;
    return n;
}
