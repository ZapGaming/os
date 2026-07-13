#include <js/js.h>
#include <js/dom_binding.h>
#include <kernel/serial.h>
#include <string.h>

enum js_flow { JS_FLOW_NORMAL, JS_FLOW_RETURN, JS_FLOW_BREAK, JS_FLOW_CONTINUE };

struct js_result {
    enum js_flow flow;
    js_value value;
};

static struct js_result js_result_normal(void) {
    struct js_result r; r.flow = JS_FLOW_NORMAL; r.value = js_undefined(); return r;
}

static js_value eval_expr(struct js_node *node, struct js_env *env);
static struct js_result eval_stmt(struct js_node *node, struct js_env *env);

/* Declares direct-child function declarations (and pre-declares direct
 * var declarations as undefined) before a block/program/function body
 * actually runs, so a function can be called before its textual
 * position -- a common, simplified subset of real JS hoisting (nested
 * blocks hoist their own children only when THEY execute). */
static void hoist(struct js_node *stmts, struct js_env *env) {
    for (struct js_node *s = stmts; s; s = s->next) {
        if (s->type == JS_FUNC_DECL) {
            struct js_object *fn = js_new_object(JS_OBJ_FUNCTION);
            fn->func_node = s;
            fn->closure_env = env;
            js_env_declare(env, s->u.func.name, js_make_object(fn), 0);
        } else if (s->type == JS_VAR_DECL && s->u.var_decl.name) {
            /* Destructuring decls (name==NULL, see js.h) aren't
             * pre-declared -- they're rare to reference before their
             * own statement runs, and doing it right would mean walking
             * the pattern for every leaf name here too. */
            js_value existing;
            if (!js_env_get(env, s->u.var_decl.name, &existing)) {
                js_env_declare(env, s->u.var_decl.name, js_undefined(), 0);
            }
        }
    }
}

/* Recursively binds `value` against a destructuring pattern (array/object,
 * possibly nested), declaring each leaf identifier in `env` -- shared by
 * var/let/const destructuring and by destructured function/arrow params. */
static void bind_pattern(struct js_node *pattern, js_value value, struct js_env *env) {
    if (pattern->type == JS_IDENT) {
        js_env_declare(env, pattern->u.ident.name, value, 0);
        return;
    }
    if (pattern->type == JS_ARRAY_PATTERN) {
        struct js_object *arr = value.type == JS_OBJ ? value.as.object : NULL;
        int i = 0;
        for (struct js_node *el = pattern->u.array_lit.elements; el; el = el->next, i++) {
            js_value item = arr ? js_get_prop(arr, js_to_string(js_make_num(i))) : js_undefined();
            bind_pattern(el, item, env);
        }
        return;
    }
    if (pattern->type == JS_OBJECT_PATTERN) {
        struct js_object *obj = value.type == JS_OBJ ? value.as.object : NULL;
        for (struct js_node *p = pattern->u.object_lit.props; p; p = p->next) {
            js_value item = obj ? js_get_prop(obj, p->u.var_decl.name) : js_undefined();
            bind_pattern(p->u.var_decl.init, item, env);
        }
        return;
    }
}

static js_value eval_lvalue_get(struct js_node *target, struct js_env *env) {
    if (target->type == JS_IDENT) {
        js_value v;
        if (js_env_get(env, target->u.ident.name, &v)) return v;
        return js_undefined();
    }
    if (target->type == JS_MEMBER) {
        js_value obj_val = eval_expr(target->u.member.object, env);
        if (obj_val.type != JS_OBJ) return js_undefined();
        const char *name = target->u.member.computed
            ? js_to_string(eval_expr(target->u.member.property, env))
            : target->u.member.property->u.ident.name;
        int handled = 0;
        js_value v = js_dom_get_prop(obj_val.as.object, name, &handled);
        if (handled) return v;
        return js_get_prop(obj_val.as.object, name);
    }
    return js_undefined();
}

static void eval_lvalue_set(struct js_node *target, struct js_env *env, js_value value) {
    if (target->type == JS_IDENT) {
        if (!js_env_set(env, target->u.ident.name, value)) {
            js_env_declare(env, target->u.ident.name, value, 0);
        }
        return;
    }
    if (target->type == JS_MEMBER) {
        js_value obj_val = eval_expr(target->u.member.object, env);
        if (obj_val.type != JS_OBJ) return;
        const char *name = target->u.member.computed
            ? js_to_string(eval_expr(target->u.member.property, env))
            : target->u.member.property->u.ident.name;
        int handled = 0;
        js_dom_set_prop(obj_val.as.object, name, value, &handled);
        if (!handled) js_set_prop(obj_val.as.object, name, value);
        return;
    }
}

static int32_t num_binop(const char *op, int32_t a, int32_t b) {
    if (strcmp(op, "+") == 0) return a + b;
    if (strcmp(op, "-") == 0) return a - b;
    if (strcmp(op, "*") == 0) return a * b;
    if (strcmp(op, "/") == 0) return b != 0 ? a / b : 0;
    if (strcmp(op, "%") == 0) return b != 0 ? a % b : 0;
    return 0;
}

static int loose_equal(js_value a, js_value b) {
    if (a.type == b.type) {
        switch (a.type) {
            case JS_UNDEFINED: case JS_NULL: return 1;
            case JS_BOOL: return a.as.boolean == b.as.boolean;
            case JS_NUM: return a.as.number == b.as.number;
            case JS_STR: return strcmp(a.as.string, b.as.string) == 0;
            case JS_OBJ: return a.as.object == b.as.object;
        }
    }
    if ((a.type == JS_UNDEFINED || a.type == JS_NULL) && (b.type == JS_UNDEFINED || b.type == JS_NULL)) return 1;
    if (a.type == JS_OBJ || b.type == JS_OBJ) return 0;
    return js_to_num(a) == js_to_num(b);
}

static js_value eval_binary(const char *op, js_value l, js_value r) {
    if (strcmp(op, "+") == 0 && (l.type == JS_STR || r.type == JS_STR)) {
        const char *ls = js_to_string(l), *rs = js_to_string(r);
        char *buf = (char *)js_alloc((uint32_t)(strlen(ls) + strlen(rs) + 1));
        strcpy(buf, ls);
        strcat(buf, rs);
        return js_make_str(buf);
    }
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
        return js_make_num(num_binop(op, js_to_num(l), js_to_num(r)));
    }
    if (strcmp(op, "==") == 0) return js_make_bool(loose_equal(l, r));
    if (strcmp(op, "!=") == 0) return js_make_bool(!loose_equal(l, r));
    if (strcmp(op, "===") == 0) return js_make_bool(l.type == r.type && loose_equal(l, r));
    if (strcmp(op, "!==") == 0) return js_make_bool(!(l.type == r.type && loose_equal(l, r)));
    if (l.type == JS_STR && r.type == JS_STR) {
        int c = strcmp(l.as.string, r.as.string);
        if (strcmp(op, "<") == 0) return js_make_bool(c < 0);
        if (strcmp(op, ">") == 0) return js_make_bool(c > 0);
        if (strcmp(op, "<=") == 0) return js_make_bool(c <= 0);
        if (strcmp(op, ">=") == 0) return js_make_bool(c >= 0);
    }
    int32_t ln = js_to_num(l), rn = js_to_num(r);
    if (strcmp(op, "<") == 0) return js_make_bool(ln < rn);
    if (strcmp(op, ">") == 0) return js_make_bool(ln > rn);
    if (strcmp(op, "<=") == 0) return js_make_bool(ln <= rn);
    if (strcmp(op, ">=") == 0) return js_make_bool(ln >= rn);
    return js_undefined();
}

js_value js_call(js_value fn, js_value this_val, js_value *args, int argc) {
    if (fn.type != JS_OBJ) {
        serial_printf("js: attempted to call a non-function\n");
        return js_undefined();
    }
    if (fn.as.object->kind == JS_OBJ_NATIVE) {
        return fn.as.object->native_fn(this_val, args, argc);
    }
    if (fn.as.object->kind != JS_OBJ_FUNCTION && fn.as.object->kind != JS_OBJ_ARROW) {
        serial_printf("js: attempted to call a non-function\n");
        return js_undefined();
    }
    if (fn.as.object->func_node->type == JS_CLASS_DECL || fn.as.object->func_node->type == JS_CLASS_EXPR) {
        /* A class's JS_OBJ_FUNCTION wrapper stores a class_decl-shaped
         * node (name+methods), not a func-shaped one (params+body) --
         * reading .params/.body off it would read the wrong union
         * member. Real JS also rejects calling a class without `new`. */
        serial_printf("js: class constructor cannot be invoked without 'new'\n");
        return js_undefined();
    }

    struct js_env *call_env = js_env_new(fn.as.object->closure_env);
    /* Arrow functions have no own `this` -- it resolves lexically
     * through closure_env, so it deliberately isn't declared here. */
    if (fn.as.object->kind == JS_OBJ_FUNCTION) {
        js_env_declare(call_env, "this", this_val, 0);
    }

    struct js_node *param = fn.as.object->func_node->u.func.params;
    for (int i = 0; param; param = param->next, i++) {
        js_value pv = i < argc ? args[i] : js_undefined();
        if (param->type == JS_IDENT) {
            js_env_declare(call_env, param->u.ident.name, pv, 0);
        } else {
            bind_pattern(param, pv, call_env);
        }
    }

    struct js_node *body = fn.as.object->func_node->u.func.body;
    hoist(body->u.block.stmts, call_env);
    for (struct js_node *s = body->u.block.stmts; s; s = s->next) {
        struct js_result r = eval_stmt(s, call_env);
        if (r.flow == JS_FLOW_RETURN) return r.value;
    }
    return js_undefined();
}

/* Evaluates a call/new argument list into `args` (capped at `cap`,
 * matching JS_CALL's existing fixed-size buffer), expanding any
 * JS_SPREAD entries element-by-element via their "length" prop --
 * covers spreading a JS_OBJ_ARRAY, which is what f(...args) needs. */
static int eval_args(struct js_node *arg_list, struct js_env *env, js_value *args, int cap) {
    int argc = 0;
    for (struct js_node *a = arg_list; a && argc < cap; a = a->next) {
        if (a->type == JS_SPREAD) {
            js_value sv = eval_expr(a->u.unary.operand, env);
            if (sv.type == JS_OBJ) {
                int32_t slen = js_to_num(js_get_prop(sv.as.object, "length"));
                for (int32_t k = 0; k < slen && argc < cap; k++, argc++) {
                    args[argc] = js_get_prop(sv.as.object, js_to_string(js_make_num(k)));
                }
            }
        } else {
            args[argc++] = eval_expr(a, env);
        }
    }
    return argc;
}

static js_value eval_expr(struct js_node *node, struct js_env *env) {
    switch (node->type) {
        case JS_NUM_LIT: return js_make_num(node->u.num_lit.value);
        case JS_STR_LIT: return js_make_str(node->u.str_lit.value);
        case JS_BOOL_LIT: return js_make_bool(node->u.bool_lit.value);
        case JS_NULL_LIT: return js_null_value();
        case JS_UNDEF_LIT: return js_undefined();
        case JS_THIS: {
            js_value v;
            return js_env_get(env, "this", &v) ? v : js_undefined();
        }
        case JS_IDENT: {
            js_value v;
            if (js_env_get(env, node->u.ident.name, &v)) return v;
            serial_printf("js: '%s' is not defined\n", node->u.ident.name);
            return js_undefined();
        }
        case JS_ARRAY_LIT: {
            struct js_object *arr = js_new_object(JS_OBJ_ARRAY);
            int i = 0;
            for (struct js_node *el = node->u.array_lit.elements; el; el = el->next) {
                if (el->type == JS_SPREAD) {
                    js_value sv = eval_expr(el->u.unary.operand, env);
                    if (sv.type == JS_OBJ) {
                        int32_t slen = js_to_num(js_get_prop(sv.as.object, "length"));
                        for (int32_t k = 0; k < slen; k++, i++) {
                            js_set_prop(arr, js_to_string(js_make_num(i)), js_get_prop(sv.as.object, js_to_string(js_make_num(k))));
                        }
                    }
                } else {
                    js_set_prop(arr, js_to_string(js_make_num(i)), eval_expr(el, env));
                    i++;
                }
            }
            js_set_prop(arr, "length", js_make_num(i));
            return js_make_object(arr);
        }
        case JS_OBJECT_LIT: {
            struct js_object *obj = js_new_object(JS_OBJ_PLAIN);
            for (struct js_node *p = node->u.object_lit.props; p; p = p->next) {
                js_set_prop(obj, p->u.var_decl.name, eval_expr(p->u.var_decl.init, env));
            }
            return js_make_object(obj);
        }
        case JS_FUNC_EXPR: {
            struct js_object *fn = js_new_object(JS_OBJ_FUNCTION);
            fn->func_node = node;
            fn->closure_env = env;
            return js_make_object(fn);
        }
        case JS_ARROW_FUNC: {
            struct js_object *fn = js_new_object(JS_OBJ_ARROW);
            fn->func_node = node;
            fn->closure_env = env;
            return js_make_object(fn);
        }
        case JS_CLASS_EXPR: {
            struct js_object *cls = js_new_object(JS_OBJ_FUNCTION);
            cls->func_node = node;
            cls->closure_env = env;
            return js_make_object(cls);
        }
        case JS_NEW: {
            js_value ctor = eval_expr(node->u.call.callee, env);
            js_value args[8];
            int argc = eval_args(node->u.call.args, env, args, 8);
            if (ctor.type != JS_OBJ || (ctor.as.object->kind != JS_OBJ_FUNCTION && ctor.as.object->kind != JS_OBJ_ARROW)) {
                serial_printf("js: 'new' target is not a constructor\n");
                return js_undefined();
            }
            struct js_object *instance = js_new_object(JS_OBJ_PLAIN);
            js_value instance_val = js_make_object(instance);
            struct js_node *class_node = ctor.as.object->func_node;
            if (class_node->type == JS_CLASS_DECL || class_node->type == JS_CLASS_EXPR) {
                js_value constructor_fn = js_undefined();
                int has_constructor = 0;
                for (struct js_node *m = class_node->u.class_decl.methods; m; m = m->next) {
                    struct js_object *method = js_new_object(JS_OBJ_FUNCTION);
                    method->func_node = m;
                    method->closure_env = ctor.as.object->closure_env;
                    if (strcmp(m->u.func.name, "constructor") == 0) {
                        constructor_fn = js_make_object(method);
                        has_constructor = 1;
                    } else {
                        js_set_prop(instance, m->u.func.name, js_make_object(method));
                    }
                }
                if (has_constructor) js_call(constructor_fn, instance_val, args, argc);
            } else {
                /* `new` on a plain function: no methods to copy, just run
                 * its body with `this` bound to the new object. */
                js_call(ctor, instance_val, args, argc);
            }
            return instance_val;
        }
        case JS_TEMPLATE_LIT: {
            /* Fixed-size part buffer, same style as JS_CALL's args[8] --
             * plenty for real-world template strings, and this engine
             * has no dynamic array to fall back to mid-expression. */
            const char *strs[32];
            int n = 0;
            for (struct js_node *p = node->u.template_lit.parts; p && n < 32; p = p->next, n++) {
                strs[n] = js_to_string(eval_expr(p, env));
            }
            uint32_t total = 0;
            for (int k = 0; k < n; k++) total += (uint32_t)strlen(strs[k]);
            char *out = (char *)js_alloc(total + 1);
            out[0] = 0;
            for (int k = 0; k < n; k++) strcat(out, strs[k]);
            return js_make_str(out);
        }
        case JS_MEMBER:
            return eval_lvalue_get(node, env);
        case JS_CALL: {
            js_value this_val = js_undefined();
            js_value fn;
            if (node->u.call.callee->type == JS_MEMBER) {
                this_val = eval_expr(node->u.call.callee->u.member.object, env);
                const char *name = node->u.call.callee->u.member.computed
                    ? js_to_string(eval_expr(node->u.call.callee->u.member.property, env))
                    : node->u.call.callee->u.member.property->u.ident.name;
                int handled = 0;
                fn = this_val.type == JS_OBJ ? js_dom_get_prop(this_val.as.object, name, &handled) : js_undefined();
                if (!handled) fn = this_val.type == JS_OBJ ? js_get_prop(this_val.as.object, name) : js_undefined();
            } else {
                fn = eval_expr(node->u.call.callee, env);
            }
            js_value args[8];
            int argc = eval_args(node->u.call.args, env, args, 8);
            return js_call(fn, this_val, args, argc);
        }
        case JS_ASSIGN: {
            js_value rhs = eval_expr(node->u.assign.value, env);
            if (strcmp(node->u.assign.op, "=") != 0) {
                js_value cur = eval_lvalue_get(node->u.assign.target, env);
                char op2[2] = { node->u.assign.op[0], 0 };
                rhs = eval_binary(op2, cur, rhs);
            }
            eval_lvalue_set(node->u.assign.target, env, rhs);
            return rhs;
        }
        case JS_UPDATE: {
            js_value cur = eval_lvalue_get(node->u.unary.operand, env);
            int32_t n = js_to_num(cur);
            int32_t next = strcmp(node->u.unary.op, "++") == 0 ? n + 1 : n - 1;
            eval_lvalue_set(node->u.unary.operand, env, js_make_num(next));
            return js_make_num(node->u.unary.prefix ? next : n);
        }
        case JS_UNARY: {
            if (strcmp(node->u.unary.op, "typeof") == 0) {
                js_value v = eval_expr(node->u.unary.operand, env);
                switch (v.type) {
                    case JS_UNDEFINED: return js_make_str("undefined");
                    case JS_NULL: return js_make_str("object");
                    case JS_BOOL: return js_make_str("boolean");
                    case JS_NUM: return js_make_str("number");
                    case JS_STR: return js_make_str("string");
                    case JS_OBJ: return js_make_str(v.as.object->kind == JS_OBJ_FUNCTION ||
                                                     v.as.object->kind == JS_OBJ_ARROW ||
                                                     v.as.object->kind == JS_OBJ_NATIVE ? "function" : "object");
                }
            }
            js_value v = eval_expr(node->u.unary.operand, env);
            if (strcmp(node->u.unary.op, "!") == 0) return js_make_bool(!js_to_bool(v));
            if (strcmp(node->u.unary.op, "-") == 0) return js_make_num(-js_to_num(v));
            return js_make_num(js_to_num(v));
        }
        case JS_BINARY:
            return eval_binary(node->u.binary.op, eval_expr(node->u.binary.left, env),
                                eval_expr(node->u.binary.right, env));
        case JS_LOGICAL: {
            js_value l = eval_expr(node->u.binary.left, env);
            if (strcmp(node->u.binary.op, "&&") == 0) {
                return js_to_bool(l) ? eval_expr(node->u.binary.right, env) : l;
            }
            return js_to_bool(l) ? l : eval_expr(node->u.binary.right, env);
        }
        case JS_CONDITIONAL:
            return js_to_bool(eval_expr(node->u.conditional.test, env))
                ? eval_expr(node->u.conditional.cons, env)
                : eval_expr(node->u.conditional.alt, env);
        case JS_SEQ:
            eval_expr(node->u.seq.left, env);
            return eval_expr(node->u.seq.right, env);
        default:
            return js_undefined();
    }
}

static struct js_result eval_stmt(struct js_node *node, struct js_env *env) {
    switch (node->type) {
        case JS_VAR_DECL: {
            js_value v = node->u.var_decl.init ? eval_expr(node->u.var_decl.init, env) : js_undefined();
            if (node->u.var_decl.pattern) bind_pattern(node->u.var_decl.pattern, v, env);
            else js_env_declare(env, node->u.var_decl.name, v, 0);
            return js_result_normal();
        }
        case JS_FUNC_DECL:
            return js_result_normal(); /* already hoisted */
        case JS_CLASS_DECL: {
            /* Not hoisted (see hoist()'s doc comment) -- available from
             * this statement onward, same as a `const X = function(){}`. */
            struct js_object *cls = js_new_object(JS_OBJ_FUNCTION);
            cls->func_node = node;
            cls->closure_env = env;
            js_env_declare(env, node->u.class_decl.name, js_make_object(cls), 0);
            return js_result_normal();
        }
        case JS_EXPR_STMT:
            eval_expr(node->u.expr_stmt.expr, env);
            return js_result_normal();
        case JS_BLOCK: {
            struct js_env *block_env = js_env_new(env);
            hoist(node->u.block.stmts, block_env);
            for (struct js_node *s = node->u.block.stmts; s; s = s->next) {
                struct js_result r = eval_stmt(s, block_env);
                if (r.flow != JS_FLOW_NORMAL) return r;
            }
            return js_result_normal();
        }
        case JS_IF:
            if (js_to_bool(eval_expr(node->u.if_stmt.test, env))) {
                return eval_stmt(node->u.if_stmt.cons, env);
            } else if (node->u.if_stmt.alt) {
                return eval_stmt(node->u.if_stmt.alt, env);
            }
            return js_result_normal();
        case JS_WHILE:
            while (js_to_bool(eval_expr(node->u.while_stmt.test, env))) {
                struct js_result r = eval_stmt(node->u.while_stmt.body, env);
                if (r.flow == JS_FLOW_BREAK) break;
                if (r.flow == JS_FLOW_RETURN) return r;
            }
            return js_result_normal();
        case JS_FOR: {
            struct js_env *for_env = js_env_new(env);
            if (node->u.for_stmt.init) {
                if (node->u.for_stmt.init->type == JS_VAR_DECL) {
                    for (struct js_node *d = node->u.for_stmt.init; d; d = d->next) eval_stmt(d, for_env);
                } else {
                    eval_expr(node->u.for_stmt.init, for_env);
                }
            }
            while (!node->u.for_stmt.test || js_to_bool(eval_expr(node->u.for_stmt.test, for_env))) {
                struct js_result r = eval_stmt(node->u.for_stmt.body, for_env);
                if (r.flow == JS_FLOW_BREAK) break;
                if (r.flow == JS_FLOW_RETURN) return r;
                if (node->u.for_stmt.update) eval_expr(node->u.for_stmt.update, for_env);
            }
            return js_result_normal();
        }
        case JS_RETURN: {
            struct js_result r;
            r.flow = JS_FLOW_RETURN;
            r.value = node->u.return_stmt.value ? eval_expr(node->u.return_stmt.value, env) : js_undefined();
            return r;
        }
        case JS_BREAK: { struct js_result r; r.flow = JS_FLOW_BREAK; r.value = js_undefined(); return r; }
        case JS_CONTINUE: { struct js_result r; r.flow = JS_FLOW_CONTINUE; r.value = js_undefined(); return r; }
        case JS_EMPTY:
        default:
            return js_result_normal();
    }
}

void js_run_program(struct js_node *program, struct js_env *env) {
    if (!program) return;
    hoist(program->u.program.body, env);
    for (struct js_node *s = program->u.program.body; s; s = s->next) {
        struct js_result r = eval_stmt(s, env);
        if (r.flow == JS_FLOW_RETURN) break; /* top-level return -- just stop */
    }
}
