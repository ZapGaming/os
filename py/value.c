#include <py/py.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

#define PY_ARENA_SIZE (256u * 1024)

static uint8_t *py_arena = NULL;
static uint32_t py_arena_used = 0;
static uint8_t py_oom_scratch[256]; /* absorbs allocations after OOM so callers never see NULL */

void py_arena_reset(void) {
    if (!py_arena) py_arena = (uint8_t *)kmalloc(PY_ARENA_SIZE);
    py_arena_used = 0;
}

void *py_alloc(uint32_t size) {
    size = (size + 7) & ~7u;
    if (!py_arena || py_arena_used + size > PY_ARENA_SIZE) {
        serial_printf("py: arena exhausted (script is too large for this pass)\n");
        return py_oom_scratch;
    }
    void *p = py_arena + py_arena_used;
    py_arena_used += size;
    return p;
}

char *py_strdup(const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    char *copy = (char *)py_alloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

py_value py_none(void) { py_value v; v.type = PY_VAL_NONE; v.as.i = 0; return v; }
py_value py_make_bool(int b) { py_value v; v.type = PY_VAL_BOOL; v.as.boolean = b ? 1 : 0; return v; }
py_value py_make_int(int64_t n) { py_value v; v.type = PY_VAL_INT; v.as.i = n; return v; }
py_value py_make_float(double f) { py_value v; v.type = PY_VAL_FLOAT; v.as.f = f; return v; }
py_value py_make_str(const char *s) { py_value v; v.type = PY_VAL_STR; v.as.s = s; return v; }
py_value py_make_object(struct py_object *obj) { py_value v; v.type = PY_VAL_OBJ; v.as.obj = obj; return v; }

struct py_object *py_new_object(enum py_obj_kind kind) {
    struct py_object *obj = (struct py_object *)py_alloc(sizeof(struct py_object));
    memset(obj, 0, sizeof(*obj));
    obj->kind = kind;
    return obj;
}

struct py_object *py_new_list(int length) {
    struct py_object *obj = py_new_object(PY_OBJ_LIST);
    obj->length = length;
    obj->items = length > 0 ? (py_value *)py_alloc((uint32_t)(sizeof(py_value) * (uint32_t)length)) : NULL;
    return obj;
}

int py_is_number(py_value v) { return v.type == PY_VAL_INT || v.type == PY_VAL_FLOAT || v.type == PY_VAL_BOOL; }

double py_to_double(py_value v) {
    switch (v.type) {
        case PY_VAL_BOOL: return v.as.boolean ? 1.0 : 0.0;
        case PY_VAL_INT: return (double)v.as.i;
        case PY_VAL_FLOAT: return v.as.f;
        default: return 0.0;
    }
}

int64_t py_to_int64(py_value v) {
    switch (v.type) {
        case PY_VAL_BOOL: return v.as.boolean ? 1 : 0;
        case PY_VAL_INT: return v.as.i;
        case PY_VAL_FLOAT: return (int64_t)v.as.f; /* truncates toward zero, matches Python's int(float) */
        case PY_VAL_STR: {
            int64_t n = 0, sign = 1;
            const char *s = v.as.s;
            while (*s == ' ') s++;
            if (*s == '-') { sign = -1; s++; }
            else if (*s == '+') s++;
            while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
            return n * sign;
        }
        default: return 0;
    }
}

int py_to_bool(py_value v) {
    switch (v.type) {
        case PY_VAL_NONE: return 0;
        case PY_VAL_BOOL: return v.as.boolean;
        case PY_VAL_INT: return v.as.i != 0;
        case PY_VAL_FLOAT: return v.as.f != 0.0;
        case PY_VAL_STR: return v.as.s[0] != 0;
        case PY_VAL_OBJ:
            if (v.as.obj->kind == PY_OBJ_LIST) return v.as.obj->length != 0;
            return 1;
    }
    return 0;
}

static void itoa64(int64_t n, char *out) {
    if (n < 0) { *out++ = '-'; n = -n; }
    char tmp[24];
    int i = 0;
    if (n == 0) tmp[i++] = '0';
    while (n > 0) { tmp[i++] = (char)('0' + (int)(n % 10)); n /= 10; }
    while (i > 0) *out++ = tmp[--i];
    *out = 0;
}

/* Floor toward negative infinity -- no <math.h> in this freestanding
 * environment, so this is hand-rolled: truncating cast to int64 is
 * already "floor" for non-negative x, and one off for negative x with
 * any fractional part. */
static double dbl_floor(double x) {
    double t = (double)(int64_t)x;
    if (t > x) t -= 1.0;
    return t;
}

/* Formats with up to 6 fractional digits (rounded), trimming trailing
 * zeros but always keeping at least one digit after the point so
 * str(2.0) reads "2.0" and not "2" -- that's how a real Python float
 * is told apart from an int at a glance, which is the property the
 * spec cares about (e.g. 1/2 must print "0.5", not "0"). This is not
 * a shortest-round-trip formatter like CPython's repr(); it doesn't
 * need to be for this bounded interpreter. */
static char *dtoa(double v) {
    char *buf = (char *)py_alloc(48);
    uint32_t pos = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    double ip_d = dbl_floor(v);
    double frac = v - ip_d;
    int64_t frac_scaled = (int64_t)(frac * 1000000.0 + 0.5);
    if (frac_scaled >= 1000000) { frac_scaled -= 1000000; ip_d += 1.0; }
    int64_t ip = (int64_t)ip_d;
    char ibuf[24];
    itoa64(ip, ibuf);
    if (neg) buf[pos++] = '-';
    for (const char *p = ibuf; *p; p++) buf[pos++] = *p;
    buf[pos++] = '.';
    char fbuf[8];
    for (int i = 5; i >= 0; i--) { fbuf[i] = (char)('0' + (int)(frac_scaled % 10)); frac_scaled /= 10; }
    int flen = 6;
    while (flen > 1 && fbuf[flen - 1] == '0') flen--;
    for (int i = 0; i < flen; i++) buf[pos++] = fbuf[i];
    buf[pos] = 0;
    return buf;
}

static const char *obj_to_str(struct py_object *obj, int quote_strings);

const char *py_to_str(py_value v) {
    switch (v.type) {
        case PY_VAL_NONE: return "None";
        case PY_VAL_BOOL: return v.as.boolean ? "True" : "False";
        case PY_VAL_INT: {
            char *buf = (char *)py_alloc(24);
            itoa64(v.as.i, buf);
            return buf;
        }
        case PY_VAL_FLOAT: return dtoa(v.as.f);
        case PY_VAL_STR: return v.as.s;
        case PY_VAL_OBJ: return obj_to_str(v.as.obj, 0);
    }
    return "";
}

/* Used only for elements nested inside a printed list, matching (very
 * roughly) how real Python's str([...]) uses repr() on elements so
 * strings show their quotes: str(['a', 1]) -> "['a', 1]". */
static const char *py_repr(py_value v) {
    if (v.type == PY_VAL_STR) {
        uint32_t len = (uint32_t)strlen(v.as.s);
        char *buf = (char *)py_alloc(len + 3);
        buf[0] = '\'';
        memcpy(buf + 1, v.as.s, len);
        buf[len + 1] = '\'';
        buf[len + 2] = 0;
        return buf;
    }
    return py_to_str(v);
}

static const char *obj_to_str(struct py_object *obj, int quote_strings) {
    (void)quote_strings;
    switch (obj->kind) {
        case PY_OBJ_FUNC:
            return obj->func_node && obj->func_node->u.func.name
                ? obj->func_node->u.func.name : "<function>";
        case PY_OBJ_NATIVE:
            return obj->native_name ? obj->native_name : "<builtin function>";
        case PY_OBJ_RANGE:
            return "range(...)";
        case PY_OBJ_LIST: {
            /* Build "[e0, e1, ...]" in the arena directly. */
            uint32_t cap = 4;
            for (int i = 0; i < obj->length; i++) cap += (uint32_t)strlen(py_repr(obj->items[i])) + 2;
            char *buf = (char *)py_alloc(cap);
            uint32_t pos = 0;
            buf[pos++] = '[';
            for (int i = 0; i < obj->length; i++) {
                if (i > 0) { buf[pos++] = ','; buf[pos++] = ' '; }
                const char *s = py_repr(obj->items[i]);
                uint32_t l = (uint32_t)strlen(s);
                memcpy(buf + pos, s, l);
                pos += l;
            }
            buf[pos++] = ']';
            buf[pos] = 0;
            return buf;
        }
    }
    return "<object>";
}

struct py_env *py_env_new(struct py_env *parent) {
    struct py_env *env = (struct py_env *)py_alloc(sizeof(struct py_env));
    env->vars = NULL;
    env->parent = parent;
    return env;
}

void py_env_declare(struct py_env *env, const char *name, py_value value) {
    for (struct py_binding *b = env->vars; b; b = b->next) {
        if (strcmp(b->name, name) == 0) { b->value = value; return; }
    }
    struct py_binding *b = (struct py_binding *)py_alloc(sizeof(struct py_binding));
    b->name = py_strdup(name);
    b->value = value;
    b->next = env->vars;
    env->vars = b;
}

int py_env_get(struct py_env *env, const char *name, py_value *out) {
    for (struct py_env *e = env; e; e = e->parent) {
        for (struct py_binding *b = e->vars; b; b = b->next) {
            if (strcmp(b->name, name) == 0) { *out = b->value; return 1; }
        }
    }
    return 0;
}

int py_env_set(struct py_env *env, const char *name, py_value value) {
    for (struct py_env *e = env; e; e = e->parent) {
        for (struct py_binding *b = e->vars; b; b = b->next) {
            if (strcmp(b->name, name) == 0) { b->value = value; return 1; }
        }
    }
    return 0;
}

struct py_node *py_node_new(enum py_node_type type) {
    struct py_node *n = (struct py_node *)py_alloc(sizeof(struct py_node));
    memset(n, 0, sizeof(*n));
    n->type = type;
    return n;
}
