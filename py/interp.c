#include <py/py.h>
#include <py/error.h>
#include <string.h>

enum py_flow { PY_FLOW_NORMAL, PY_FLOW_RETURN, PY_FLOW_BREAK, PY_FLOW_CONTINUE, PY_FLOW_ERROR };

struct py_result {
    enum py_flow flow;
    py_value value;
};

static struct py_result result_normal(void) { struct py_result r; r.flow = PY_FLOW_NORMAL; r.value = py_none(); return r; }
static struct py_result result_return(py_value v) { struct py_result r; r.flow = PY_FLOW_RETURN; r.value = v; return r; }
static struct py_result result_break(void) { struct py_result r; r.flow = PY_FLOW_BREAK; r.value = py_none(); return r; }
static struct py_result result_continue(void) { struct py_result r; r.flow = PY_FLOW_CONTINUE; r.value = py_none(); return r; }
static struct py_result result_error(void) { struct py_result r; r.flow = PY_FLOW_ERROR; r.value = py_none(); return r; }

static py_value eval_expr(struct py_node *node, struct py_env *env);
static struct py_result eval_stmt(struct py_node *node, struct py_env *env);
static struct py_result eval_block(struct py_node *block, struct py_env *env);

/* Builds "'name'" + suffix into a small stack buffer and hands it to
 * py_error_set() (which copies it out immediately), e.g.
 * error_quoted("NameError: ", "x", " is not defined") produces
 * "NameError: 'x' is not defined". */
static void error_quoted(const char *category, const char *name, const char *suffix) {
    char buf[96];
    uint32_t p = 0;
    buf[p++] = '\'';
    uint32_t nlen = (uint32_t)strlen(name);
    if (nlen > 88) nlen = 88;
    memcpy(buf + p, name, nlen);
    p += nlen;
    buf[p++] = '\'';
    buf[p] = 0;
    py_error_set(category, buf, suffix);
}

/* ---- Hand-rolled numeric helpers (no <math.h> in this freestanding
 * environment) --------------------------------------------------------*/

static double dbl_floor(double x) {
    double t = (double)(int64_t)x;
    if (t > x) t -= 1.0;
    return t;
}

/* Plain / and % on int64_t are fine here despite this being a 32-bit
 * target with no 32-bit libgcc.a installed: py/i64_helpers.c defines
 * __divdi3/__moddi3/__divmoddi4 itself (same fix already used by
 * userprogs/doom/doomlibc.c for the identical problem), so the calls
 * gcc emits for these resolve within this same link unit. */
static int64_t int_floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q -= 1;
    return q;
}

static int64_t int_floormod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

static py_value numeric_pow(py_value l, py_value r) {
    if (l.type != PY_VAL_FLOAT && r.type != PY_VAL_FLOAT) {
        int64_t base = py_to_int64(l), exp = py_to_int64(r);
        if (exp >= 0) {
            int64_t result = 1;
            for (int64_t i = 0; i < exp; i++) result *= base;
            return py_make_int(result);
        }
        double b = (double)base, result = 1.0;
        for (int64_t i = 0; i < -exp; i++) result *= b;
        if (result == 0.0) { py_error_set("ZeroDivisionError: ", "0.0 cannot be raised to a negative power", NULL); return py_none(); }
        return py_make_float(1.0 / result);
    }
    /* Float base and/or float exponent: exponentiation-by-squaring only
     * works for integer-valued exponents, and there's no pow()/log() in
     * this freestanding build to fall back on for fractional exponents
     * (e.g. 2 ** 0.5) -- documented limitation of this bounded subset. */
    double base = py_to_double(l);
    double exp = py_to_double(r);
    double exp_floor = dbl_floor(exp);
    if (exp_floor != exp) {
        py_error_set("NotImplementedError: ", "'**' with a non-integer exponent is not supported", NULL);
        return py_none();
    }
    int neg = exp < 0;
    double aexp = neg ? -exp : exp;
    double result = 1.0;
    for (double i = 0; i < aexp; i += 1.0) result *= base;
    if (neg) {
        if (result == 0.0) { py_error_set("ZeroDivisionError: ", "0.0 cannot be raised to a negative power", NULL); return py_none(); }
        result = 1.0 / result;
    }
    return py_make_float(result);
}

static py_value numeric_binop(const char *op, py_value l, py_value r) {
    if (strcmp(op, "/") == 0) {
        double rd = py_to_double(r);
        if (rd == 0.0) { py_error_set("ZeroDivisionError: ", "division by zero", NULL); return py_none(); }
        return py_make_float(py_to_double(l) / rd);
    }
    if (strcmp(op, "**") == 0) return numeric_pow(l, r);

    int use_float = (l.type == PY_VAL_FLOAT || r.type == PY_VAL_FLOAT);
    if (!use_float) {
        int64_t a = py_to_int64(l), b = py_to_int64(r);
        if (strcmp(op, "+") == 0) return py_make_int(a + b);
        if (strcmp(op, "-") == 0) return py_make_int(a - b);
        if (strcmp(op, "*") == 0) return py_make_int(a * b);
        if (strcmp(op, "//") == 0) {
            if (b == 0) { py_error_set("ZeroDivisionError: ", "integer division or modulo by zero", NULL); return py_none(); }
            return py_make_int(int_floordiv(a, b));
        }
        if (strcmp(op, "%") == 0) {
            if (b == 0) { py_error_set("ZeroDivisionError: ", "integer division or modulo by zero", NULL); return py_none(); }
            return py_make_int(int_floormod(a, b));
        }
    }
    double a = py_to_double(l), b = py_to_double(r);
    if (strcmp(op, "+") == 0) return py_make_float(a + b);
    if (strcmp(op, "-") == 0) return py_make_float(a - b);
    if (strcmp(op, "*") == 0) return py_make_float(a * b);
    if (strcmp(op, "//") == 0) {
        if (b == 0.0) { py_error_set("ZeroDivisionError: ", "float floor division by zero", NULL); return py_none(); }
        return py_make_float(dbl_floor(a / b));
    }
    if (strcmp(op, "%") == 0) {
        if (b == 0.0) { py_error_set("ZeroDivisionError: ", "float modulo by zero", NULL); return py_none(); }
        return py_make_float(a - dbl_floor(a / b) * b);
    }
    return py_none();
}

static int values_equal(py_value a, py_value b) {
    if (py_is_number(a) && py_is_number(b)) return py_to_double(a) == py_to_double(b);
    if (a.type == PY_VAL_STR && b.type == PY_VAL_STR) return strcmp(a.as.s, b.as.s) == 0;
    if (a.type == PY_VAL_NONE && b.type == PY_VAL_NONE) return 1;
    if (a.type == PY_VAL_OBJ && b.type == PY_VAL_OBJ) return a.as.obj == b.as.obj;
    return 0;
}

static py_value eval_compare(const char *op, py_value l, py_value r) {
    if (strcmp(op, "==") == 0) return py_make_bool(values_equal(l, r));
    if (strcmp(op, "!=") == 0) return py_make_bool(!values_equal(l, r));
    if (py_is_number(l) && py_is_number(r)) {
        double a = py_to_double(l), b = py_to_double(r);
        if (strcmp(op, "<") == 0) return py_make_bool(a < b);
        if (strcmp(op, ">") == 0) return py_make_bool(a > b);
        if (strcmp(op, "<=") == 0) return py_make_bool(a <= b);
        if (strcmp(op, ">=") == 0) return py_make_bool(a >= b);
    }
    if (l.type == PY_VAL_STR && r.type == PY_VAL_STR) {
        int c = strcmp(l.as.s, r.as.s);
        if (strcmp(op, "<") == 0) return py_make_bool(c < 0);
        if (strcmp(op, ">") == 0) return py_make_bool(c > 0);
        if (strcmp(op, "<=") == 0) return py_make_bool(c <= 0);
        if (strcmp(op, ">=") == 0) return py_make_bool(c >= 0);
    }
    py_error_set("TypeError: ", "unsupported operand type(s) for comparison", NULL);
    return py_none();
}

static py_value eval_binary(const char *op, py_value l, py_value r) {
    if (strcmp(op, "+") == 0 && l.type == PY_VAL_STR && r.type == PY_VAL_STR) {
        uint32_t la = (uint32_t)strlen(l.as.s), lb = (uint32_t)strlen(r.as.s);
        char *buf = (char *)py_alloc(la + lb + 1);
        memcpy(buf, l.as.s, la);
        memcpy(buf + la, r.as.s, lb + 1);
        return py_make_str(buf);
    }
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
        strcmp(op, "//") == 0 || strcmp(op, "%") == 0 || strcmp(op, "**") == 0) {
        if (!py_is_number(l) || !py_is_number(r)) {
            py_error_set("TypeError: ", "unsupported operand type(s) for ", op);
            return py_none();
        }
        return numeric_binop(op, l, r);
    }
    return eval_compare(op, l, r);
}

/* ---- lvalues: identifiers and list[index] --------------------------- */

static py_value eval_index_get(struct py_node *object_expr, struct py_node *index_expr, struct py_env *env) {
    py_value obj_val = eval_expr(object_expr, env);
    if (py_has_error()) return py_none();
    py_value idx_val = eval_expr(index_expr, env);
    if (py_has_error()) return py_none();

    if (obj_val.type == PY_VAL_STR) {
        int64_t len = (int64_t)strlen(obj_val.as.s);
        int64_t idx = py_to_int64(idx_val);
        if (idx < 0) idx += len;
        if (idx < 0 || idx >= len) { py_error_set("IndexError: ", "string index out of range", NULL); return py_none(); }
        char *buf = (char *)py_alloc(2);
        buf[0] = obj_val.as.s[idx];
        buf[1] = 0;
        return py_make_str(buf);
    }
    if (obj_val.type == PY_VAL_OBJ && obj_val.as.obj->kind == PY_OBJ_LIST) {
        struct py_object *lst = obj_val.as.obj;
        int64_t idx = py_to_int64(idx_val);
        if (idx < 0) idx += lst->length;
        if (idx < 0 || idx >= lst->length) { py_error_set("IndexError: ", "list index out of range", NULL); return py_none(); }
        return lst->items[idx];
    }
    py_error_set("TypeError: ", "object is not subscriptable", NULL);
    return py_none();
}

static py_value eval_lvalue_get(struct py_node *target, struct py_env *env) {
    if (target->type == PY_IDENT) {
        py_value v;
        if (py_env_get(env, target->u.ident.name, &v)) return v;
        error_quoted("NameError: ", target->u.ident.name, " is not defined");
        return py_none();
    }
    if (target->type == PY_INDEX) return eval_index_get(target->u.index_expr.object, target->u.index_expr.index, env);
    py_error_set("SyntaxError: ", "invalid assignment target", NULL);
    return py_none();
}

static void eval_lvalue_set(struct py_node *target, struct py_env *env, py_value value) {
    if (py_has_error()) return;
    if (target->type == PY_IDENT) {
        if (!py_env_set(env, target->u.ident.name, value)) py_env_declare(env, target->u.ident.name, value);
        return;
    }
    if (target->type == PY_INDEX) {
        py_value obj_val = eval_expr(target->u.index_expr.object, env);
        if (py_has_error()) return;
        py_value idx_val = eval_expr(target->u.index_expr.index, env);
        if (py_has_error()) return;
        if (obj_val.type != PY_VAL_OBJ || obj_val.as.obj->kind != PY_OBJ_LIST) {
            py_error_set("TypeError: ", "object does not support item assignment", NULL);
            return;
        }
        struct py_object *lst = obj_val.as.obj;
        int64_t idx = py_to_int64(idx_val);
        if (idx < 0) idx += lst->length;
        if (idx < 0 || idx >= lst->length) { py_error_set("IndexError: ", "list assignment index out of range", NULL); return; }
        lst->items[idx] = value;
        return;
    }
    py_error_set("SyntaxError: ", "invalid assignment target", NULL);
}

/* ---- Calling functions ------------------------------------------------*/

#define PY_MAX_CALL_DEPTH 200
static int g_call_depth = 0;

static py_value py_call(py_value fn_val, py_value *args, int argc) {
    if (py_has_error()) return py_none();
    if (fn_val.type != PY_VAL_OBJ) { py_error_set("TypeError: ", "object is not callable", NULL); return py_none(); }
    struct py_object *fn = fn_val.as.obj;
    if (fn->kind == PY_OBJ_NATIVE) return fn->native_fn(args, argc);
    if (fn->kind != PY_OBJ_FUNC) { py_error_set("TypeError: ", "object is not callable", NULL); return py_none(); }

    int nparams = 0;
    for (struct py_node *p = fn->func_node->u.func.params; p; p = p->next) nparams++;
    if (argc != nparams) {
        py_error_set("TypeError: ", fn->func_node->u.func.name, "() called with the wrong number of arguments");
        return py_none();
    }
    if (g_call_depth >= PY_MAX_CALL_DEPTH) {
        py_error_set("RecursionError: ", "maximum recursion depth exceeded", NULL);
        return py_none();
    }

    struct py_env *call_env = py_env_new(fn->closure_env);
    struct py_node *p = fn->func_node->u.func.params;
    for (int i = 0; i < argc && p; i++, p = p->next) py_env_declare(call_env, p->u.ident.name, args[i]);

    g_call_depth++;
    struct py_result r = eval_block(fn->func_node->u.func.body, call_env);
    g_call_depth--;

    if (r.flow == PY_FLOW_RETURN) return r.value;
    return py_none(); /* implicit "return None" -- also covers the error path */
}

/* ---- Expressions --------------------------------------------------- */

static py_value eval_expr(struct py_node *node, struct py_env *env) {
    if (py_has_error()) return py_none();
    switch (node->type) {
        case PY_INT_LIT: return py_make_int(node->u.int_lit.value);
        case PY_FLOAT_LIT: return py_make_float(node->u.float_lit.value);
        case PY_STR_LIT: return py_make_str(node->u.str_lit.value);
        case PY_BOOL_LIT: return py_make_bool(node->u.bool_lit.value);
        case PY_NONE_LIT: return py_none();
        case PY_IDENT: {
            py_value v;
            if (py_env_get(env, node->u.ident.name, &v)) return v;
            error_quoted("NameError: ", node->u.ident.name, " is not defined");
            return py_none();
        }
        case PY_LIST_LIT: {
            int count = 0;
            for (struct py_node *e = node->u.list_lit.elements; e; e = e->next) count++;
            struct py_object *lst = py_new_list(count);
            int i = 0;
            for (struct py_node *e = node->u.list_lit.elements; e; e = e->next, i++) {
                lst->items[i] = eval_expr(e, env);
                if (py_has_error()) return py_none();
            }
            return py_make_object(lst);
        }
        case PY_INDEX:
            return eval_index_get(node->u.index_expr.object, node->u.index_expr.index, env);
        case PY_CALL: {
            py_value fn = eval_expr(node->u.call.callee, env);
            if (py_has_error()) return py_none();
            py_value args[16];
            int argc = 0;
            for (struct py_node *a = node->u.call.args; a && argc < 16; a = a->next, argc++) {
                args[argc] = eval_expr(a, env);
                if (py_has_error()) return py_none();
            }
            return py_call(fn, args, argc);
        }
        case PY_UNARY: {
            py_value v = eval_expr(node->u.unary.operand, env);
            if (py_has_error()) return py_none();
            if (!py_is_number(v)) { py_error_set("TypeError: ", "bad operand type for unary operator", NULL); return py_none(); }
            int negate = strcmp(node->u.unary.op, "-") == 0;
            if (v.type == PY_VAL_FLOAT) return py_make_float(negate ? -v.as.f : v.as.f);
            int64_t n = py_to_int64(v);
            return py_make_int(negate ? -n : n);
        }
        case PY_NOT: {
            py_value v = eval_expr(node->u.unary.operand, env);
            if (py_has_error()) return py_none();
            return py_make_bool(!py_to_bool(v));
        }
        case PY_LOGICAL: {
            py_value l = eval_expr(node->u.binary.left, env);
            if (py_has_error()) return py_none();
            if (strcmp(node->u.binary.op, "and") == 0) {
                if (!py_to_bool(l)) return l;
                return eval_expr(node->u.binary.right, env);
            }
            if (py_to_bool(l)) return l;
            return eval_expr(node->u.binary.right, env);
        }
        case PY_BINARY: {
            py_value l = eval_expr(node->u.binary.left, env);
            if (py_has_error()) return py_none();
            py_value r = eval_expr(node->u.binary.right, env);
            if (py_has_error()) return py_none();
            return eval_binary(node->u.binary.op, l, r);
        }
        default:
            return py_none();
    }
}

/* ---- Statements ------------------------------------------------------*/

static struct py_result eval_block(struct py_node *block, struct py_env *env) {
    for (struct py_node *s = block->u.block.stmts; s; s = s->next) {
        struct py_result r = eval_stmt(s, env);
        if (r.flow != PY_FLOW_NORMAL) return r;
    }
    return result_normal();
}

static struct py_result eval_stmt(struct py_node *node, struct py_env *env) {
    if (py_has_error()) return result_error();
    switch (node->type) {
        case PY_PASS:
            return result_normal();
        case PY_EXPR_STMT:
            eval_expr(node->u.expr_stmt.expr, env);
            return py_has_error() ? result_error() : result_normal();
        case PY_ASSIGN: {
            py_value rhs = eval_expr(node->u.assign.value, env);
            if (py_has_error()) return result_error();
            if (strcmp(node->u.assign.op, "=") != 0) {
                py_value cur = eval_lvalue_get(node->u.assign.target, env);
                if (py_has_error()) return result_error();
                rhs = eval_binary(node->u.assign.op, cur, rhs);
                if (py_has_error()) return result_error();
            }
            eval_lvalue_set(node->u.assign.target, env, rhs);
            return py_has_error() ? result_error() : result_normal();
        }
        case PY_FUNC_DEF: {
            struct py_object *fn = py_new_object(PY_OBJ_FUNC);
            fn->func_node = node;
            fn->closure_env = env;
            py_env_declare(env, node->u.func.name, py_make_object(fn));
            return result_normal();
        }
        case PY_IF: {
            py_value t = eval_expr(node->u.if_stmt.test, env);
            if (py_has_error()) return result_error();
            if (py_to_bool(t)) return eval_block(node->u.if_stmt.body, env);
            if (node->u.if_stmt.orelse) {
                if (node->u.if_stmt.orelse->type == PY_IF) return eval_stmt(node->u.if_stmt.orelse, env);
                return eval_block(node->u.if_stmt.orelse, env);
            }
            return result_normal();
        }
        case PY_WHILE: {
            for (;;) {
                if (py_has_error()) return result_error();
                py_value t = eval_expr(node->u.while_stmt.test, env);
                if (py_has_error()) return result_error();
                if (!py_to_bool(t)) break;
                struct py_result r = eval_block(node->u.while_stmt.body, env);
                if (r.flow == PY_FLOW_BREAK) break;
                if (r.flow == PY_FLOW_RETURN || r.flow == PY_FLOW_ERROR) return r;
            }
            return result_normal();
        }
        case PY_FOR: {
            py_value iter_val = eval_expr(node->u.for_stmt.iter, env);
            if (py_has_error()) return result_error();
            const char *var = node->u.for_stmt.var;

            if (iter_val.type == PY_VAL_OBJ && iter_val.as.obj->kind == PY_OBJ_RANGE) {
                struct py_object *rg = iter_val.as.obj;
                int64_t step = rg->range_step;
                for (int64_t i = rg->range_start; step > 0 ? i < rg->range_stop : i > rg->range_stop; i += step) {
                    py_env_declare(env, var, py_make_int(i));
                    struct py_result r = eval_block(node->u.for_stmt.body, env);
                    if (r.flow == PY_FLOW_BREAK) break;
                    if (r.flow == PY_FLOW_RETURN || r.flow == PY_FLOW_ERROR) return r;
                }
            } else if (iter_val.type == PY_VAL_OBJ && iter_val.as.obj->kind == PY_OBJ_LIST) {
                struct py_object *lst = iter_val.as.obj;
                for (int i = 0; i < lst->length; i++) {
                    py_env_declare(env, var, lst->items[i]);
                    struct py_result r = eval_block(node->u.for_stmt.body, env);
                    if (r.flow == PY_FLOW_BREAK) break;
                    if (r.flow == PY_FLOW_RETURN || r.flow == PY_FLOW_ERROR) return r;
                }
            } else if (iter_val.type == PY_VAL_STR) {
                for (const char *p = iter_val.as.s; *p; p++) {
                    char *buf = (char *)py_alloc(2);
                    buf[0] = *p; buf[1] = 0;
                    py_env_declare(env, var, py_make_str(buf));
                    struct py_result r = eval_block(node->u.for_stmt.body, env);
                    if (r.flow == PY_FLOW_BREAK) break;
                    if (r.flow == PY_FLOW_RETURN || r.flow == PY_FLOW_ERROR) return r;
                }
            } else {
                py_error_set("TypeError: ", "object is not iterable", NULL);
                return result_error();
            }
            return result_normal();
        }
        case PY_RETURN: {
            py_value v = node->u.return_stmt.value ? eval_expr(node->u.return_stmt.value, env) : py_none();
            if (py_has_error()) return result_error();
            return result_return(v);
        }
        case PY_BREAK: return result_break();
        case PY_CONTINUE: return result_continue();
        default:
            return result_normal();
    }
}

void py_run_program(struct py_node *program, struct py_env *env) {
    if (!program) return;
    for (struct py_node *s = program->u.program.body; s; s = s->next) {
        struct py_result r = eval_stmt(s, env);
        if (r.flow == PY_FLOW_RETURN || r.flow == PY_FLOW_ERROR) break;
    }
}
