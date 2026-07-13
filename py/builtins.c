#include <py/py.h>
#include <py/error.h>
#include <py/builtins.h>
#include <string.h>

static void (*g_out)(const char *) = 0;

void py_builtins_set_out(void (*out)(const char *)) { g_out = out; }

static void def_native(struct py_env *env, const char *name, py_native_fn fn) {
    struct py_object *obj = py_new_object(PY_OBJ_NATIVE);
    obj->native_fn = fn;
    obj->native_name = name;
    py_env_declare(env, name, py_make_object(obj));
}

static py_value native_print(py_value *args, int argc) {
    int n = argc > 16 ? 16 : argc;
    const char *strs[16];
    uint32_t total = 1; /* trailing '\n' */
    for (int i = 0; i < n; i++) {
        strs[i] = py_to_str(args[i]);
        total += (uint32_t)strlen(strs[i]) + 1; /* +1 for separating space or nothing */
    }
    char *buf = (char *)py_alloc(total + 1);
    uint32_t pos = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0) buf[pos++] = ' ';
        uint32_t l = (uint32_t)strlen(strs[i]);
        memcpy(buf + pos, strs[i], l);
        pos += l;
    }
    buf[pos++] = '\n';
    buf[pos] = 0;
    if (g_out) g_out(buf);
    return py_none();
}

static py_value native_len(py_value *args, int argc) {
    if (argc != 1) { py_error_set("TypeError: ", "len() takes exactly one argument", NULL); return py_none(); }
    if (args[0].type == PY_VAL_STR) return py_make_int((int64_t)strlen(args[0].as.s));
    if (args[0].type == PY_VAL_OBJ && args[0].as.obj->kind == PY_OBJ_LIST) return py_make_int(args[0].as.obj->length);
    py_error_set("TypeError: ", "object has no len()", NULL);
    return py_none();
}

static py_value native_str(py_value *args, int argc) {
    if (argc != 1) { py_error_set("TypeError: ", "str() takes exactly one argument", NULL); return py_none(); }
    return py_make_str(py_to_str(args[0]));
}

static py_value native_int(py_value *args, int argc) {
    if (argc == 0) return py_make_int(0);
    if (argc != 1) { py_error_set("TypeError: ", "int() takes at most one argument", NULL); return py_none(); }
    py_value v = args[0];
    if (v.type == PY_VAL_STR || py_is_number(v)) return py_make_int(py_to_int64(v));
    py_error_set("TypeError: ", "int() argument must be a string or a number", NULL);
    return py_none();
}

/* Hand-rolled ASCII-decimal-to-double: no strtod()/atof() in this
 * freestanding build. No exponent notation -- out of scope. */
static double str_to_double(const char *s) {
    double sign = 1.0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1.0; s++; } else if (*s == '+') s++;
    double intpart = 0.0;
    while (*s >= '0' && *s <= '9') { intpart = intpart * 10.0 + (double)(*s - '0'); s++; }
    double frac = 0.0, scale = 0.1;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { frac += (double)(*s - '0') * scale; scale *= 0.1; s++; }
    }
    return sign * (intpart + frac);
}

static py_value native_float(py_value *args, int argc) {
    if (argc == 0) return py_make_float(0.0);
    if (argc != 1) { py_error_set("TypeError: ", "float() takes at most one argument", NULL); return py_none(); }
    py_value v = args[0];
    if (v.type == PY_VAL_STR) return py_make_float(str_to_double(v.as.s));
    if (py_is_number(v)) return py_make_float(py_to_double(v));
    py_error_set("TypeError: ", "float() argument must be a string or a number", NULL);
    return py_none();
}

static py_value native_range(py_value *args, int argc) {
    int64_t start = 0, stop = 0, step = 1;
    if (argc == 1) {
        stop = py_to_int64(args[0]);
    } else if (argc == 2) {
        start = py_to_int64(args[0]);
        stop = py_to_int64(args[1]);
    } else if (argc == 3) {
        start = py_to_int64(args[0]);
        stop = py_to_int64(args[1]);
        step = py_to_int64(args[2]);
        if (step == 0) { py_error_set("ValueError: ", "range() arg 3 must not be zero", NULL); return py_none(); }
    } else {
        py_error_set("TypeError: ", "range expected 1 to 3 arguments", NULL);
        return py_none();
    }
    struct py_object *rg = py_new_object(PY_OBJ_RANGE);
    rg->range_start = start;
    rg->range_stop = stop;
    rg->range_step = step;
    return py_make_object(rg);
}

void py_register_builtins(struct py_env *env) {
    def_native(env, "print", native_print);
    def_native(env, "len", native_len);
    def_native(env, "str", native_str);
    def_native(env, "int", native_int);
    def_native(env, "float", native_float);
    def_native(env, "range", native_range);
}
