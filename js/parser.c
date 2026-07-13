#include <js/js.h>
#include <js/lexer.h>
#include <kernel/serial.h>
#include <string.h>

static int js_had_error = 0;

static void parse_error(struct js_lexer *lx, const char *msg) {
    serial_printf("js: parse error: %s (near '%s')\n", msg, lx->cur.text);
    js_had_error = 1;
}

static int is_punct(struct js_lexer *lx, const char *text) {
    return lx->cur.type == TOK_PUNCT && strcmp(lx->cur.text, text) == 0;
}
static int is_keyword(struct js_lexer *lx, const char *text) {
    return lx->cur.type == TOK_KEYWORD && strcmp(lx->cur.text, text) == 0;
}
static int eat_punct(struct js_lexer *lx, const char *text) {
    if (is_punct(lx, text)) { js_lexer_next(lx); return 1; }
    return 0;
}
static void expect_punct(struct js_lexer *lx, const char *text) {
    if (!eat_punct(lx, text)) parse_error(lx, text);
}
static int eat_keyword(struct js_lexer *lx, const char *text) {
    if (is_keyword(lx, text)) { js_lexer_next(lx); return 1; }
    return 0;
}

static struct js_node *parse_expr(struct js_lexer *lx);
static struct js_node *parse_assign(struct js_lexer *lx);
static struct js_node *parse_statement(struct js_lexer *lx);
static struct js_node *parse_block(struct js_lexer *lx);
static struct js_node *parse_func_body_and_params(struct js_lexer *lx, struct js_node **params_out);
static struct js_node *parse_binding_target(struct js_lexer *lx);
static struct js_node *parse_arg_list(struct js_lexer *lx);
static struct js_node *parse_class_body(struct js_lexer *lx, struct js_node *cls);
static struct js_node *parse_template_literal(struct js_lexer *lx);

/* Shared by function params, arrow-function params, and destructuring
 * var declarations: an identifier, or an object/array destructuring
 * pattern (which can itself nest). Returns NULL (consuming nothing
 * committed as a pattern) if the current token can't start one, so
 * callers that are only guessing -- see try_parse_arrow_function --
 * can tell "not a binding target" apart from a real parse error. */
static struct js_node *parse_binding_target(struct js_lexer *lx) {
    if (lx->cur.type == TOK_IDENT) {
        struct js_node *p = js_node_new(JS_IDENT);
        p->u.ident.name = js_strdup(lx->cur.text);
        js_lexer_next(lx);
        return p;
    }
    if (eat_punct(lx, "{")) {
        struct js_node *n = js_node_new(JS_OBJECT_PATTERN);
        struct js_node *head = NULL, *tail = NULL;
        while (!is_punct(lx, "}") && lx->cur.type != TOK_EOF) {
            struct js_node *prop = js_node_new(JS_VAR_DECL);
            prop->u.var_decl.name = js_strdup(lx->cur.text); /* source key */
            js_lexer_next(lx);
            if (eat_punct(lx, ":")) {
                prop->u.var_decl.init = parse_binding_target(lx); /* `{a: renamed}` */
            } else {
                struct js_node *ident = js_node_new(JS_IDENT);
                ident->u.ident.name = js_strdup(prop->u.var_decl.name);
                prop->u.var_decl.init = ident; /* shorthand `{a}` */
            }
            if (!head) head = prop; else tail->next = prop;
            tail = prop;
            if (!eat_punct(lx, ",")) break;
        }
        expect_punct(lx, "}");
        n->u.object_lit.props = head;
        return n;
    }
    if (eat_punct(lx, "[")) {
        struct js_node *n = js_node_new(JS_ARRAY_PATTERN);
        struct js_node *head = NULL, *tail = NULL;
        while (!is_punct(lx, "]") && lx->cur.type != TOK_EOF) {
            struct js_node *el = parse_binding_target(lx);
            if (!el) break;
            if (!head) head = el; else tail->next = el;
            tail = el;
            if (!eat_punct(lx, ",")) break;
        }
        expect_punct(lx, "]");
        n->u.array_lit.elements = head;
        return n;
    }
    return NULL;
}

/* Call-argument list, `(` already consumed; consumes the closing `)`.
 * Shared by ordinary calls and `new` so spread (`f(...args)`) only
 * needs to be handled in one place. */
static struct js_node *parse_arg_list(struct js_lexer *lx) {
    struct js_node *head = NULL, *tail = NULL;
    while (!is_punct(lx, ")") && lx->cur.type != TOK_EOF) {
        struct js_node *arg;
        if (eat_punct(lx, "...")) {
            arg = js_node_new(JS_SPREAD);
            arg->u.unary.operand = parse_assign(lx);
        } else {
            arg = parse_assign(lx);
        }
        if (!head) head = arg; else tail->next = arg;
        tail = arg;
        if (!eat_punct(lx, ",")) break;
    }
    expect_punct(lx, ")");
    return head;
}

/* Splits a TOK_TEMPLATE's raw body into alternating STR_LIT chunks and
 * interpolated-expression nodes chained through ->next (always starts
 * and ends with a chunk, possibly empty, so the interpreter can just
 * concatenate js_to_string() of every part in order). Each ${...} gets
 * its own throwaway sub-lexer over that byte range so the full
 * expression grammar (whatever parse_expr already handles) is
 * available inside interpolations. */
static struct js_node *parse_template_literal(struct js_lexer *lx) {
    const char *raw = lx->cur.template_raw;
    uint32_t len = lx->cur.template_len;
    js_lexer_next(lx);

    struct js_node *head = NULL, *tail = NULL;
    uint32_t i = 0;
    for (;;) {
        uint32_t start = i;
        while (i < len && !(raw[i] == '$' && i + 1 < len && raw[i + 1] == '{')) i++;
        uint32_t seg_len = i - start;
        char *chunk = (char *)js_alloc(seg_len + 1);
        int ci = 0;
        for (uint32_t k = start; k < i;) {
            char ch = raw[k];
            if (ch == '\\' && k + 1 < i) { k++; ch = js_lexer_decode_escape(raw[k]); }
            chunk[ci++] = ch;
            k++;
        }
        chunk[ci] = 0;
        struct js_node *strn = js_node_new(JS_STR_LIT);
        strn->u.str_lit.value = chunk;
        if (!head) head = strn; else tail->next = strn;
        tail = strn;

        if (i >= len) break;
        i += 2; /* skip "${" */
        uint32_t expr_start = i;
        int depth = 1;
        while (i < len && depth > 0) {
            if (raw[i] == '{') depth++;
            else if (raw[i] == '}') { depth--; if (depth == 0) break; }
            i++;
        }
        struct js_lexer sub;
        js_lexer_init(&sub, raw + expr_start, i - expr_start);
        struct js_node *expr = parse_expr(&sub);
        tail->next = expr;
        tail = expr;
        if (i < len) i++; /* skip '}' */
    }

    struct js_node *n = js_node_new(JS_TEMPLATE_LIT);
    n->u.template_lit.parts = head;
    return n;
}

/* Class body: a sequence of `name(params) { body }` methods, one of
 * which may be named "constructor". No getters/setters/static/fields --
 * this engine has no prototype chain (see js.h's JS_OBJ kinds), so
 * `new` just copies bound method closures onto a plain object (see
 * js/interp.c's JS_NEW); that only needs methods, not the extra forms. */
static struct js_node *parse_class_body(struct js_lexer *lx, struct js_node *cls) {
    expect_punct(lx, "{");
    struct js_node *head = NULL, *tail = NULL;
    while (!is_punct(lx, "}") && lx->cur.type != TOK_EOF) {
        struct js_node *m = js_node_new(JS_FUNC_EXPR);
        m->u.func.name = js_strdup(lx->cur.text);
        js_lexer_next(lx);
        m->u.func.body = parse_func_body_and_params(lx, &m->u.func.params);
        if (!head) head = m; else tail->next = m;
        tail = m;
    }
    expect_punct(lx, "}");
    cls->u.class_decl.methods = head;
    return cls;
}

static struct js_node *parse_primary(struct js_lexer *lx) {
    if (lx->cur.type == TOK_NUM) {
        struct js_node *n = js_node_new(JS_NUM_LIT);
        n->u.num_lit.value = lx->cur.num_value;
        js_lexer_next(lx);
        return n;
    }
    if (lx->cur.type == TOK_STR) {
        struct js_node *n = js_node_new(JS_STR_LIT);
        n->u.str_lit.value = js_strdup(lx->cur.text);
        js_lexer_next(lx);
        return n;
    }
    if (is_keyword(lx, "true") || is_keyword(lx, "false")) {
        struct js_node *n = js_node_new(JS_BOOL_LIT);
        n->u.bool_lit.value = is_keyword(lx, "true");
        js_lexer_next(lx);
        return n;
    }
    if (eat_keyword(lx, "null")) return js_node_new(JS_NULL_LIT);
    if (eat_keyword(lx, "undefined")) return js_node_new(JS_UNDEF_LIT);
    if (eat_keyword(lx, "this")) return js_node_new(JS_THIS);
    if (lx->cur.type == TOK_TEMPLATE) return parse_template_literal(lx);
    if (lx->cur.type == TOK_IDENT) {
        struct js_node *n = js_node_new(JS_IDENT);
        n->u.ident.name = js_strdup(lx->cur.text);
        js_lexer_next(lx);
        return n;
    }
    if (eat_keyword(lx, "function")) {
        struct js_node *n = js_node_new(JS_FUNC_EXPR);
        if (lx->cur.type == TOK_IDENT) { n->u.func.name = js_strdup(lx->cur.text); js_lexer_next(lx); }
        n->u.func.body = parse_func_body_and_params(lx, &n->u.func.params);
        return n;
    }
    if (eat_keyword(lx, "class")) {
        struct js_node *n = js_node_new(JS_CLASS_EXPR);
        if (lx->cur.type == TOK_IDENT) { n->u.class_decl.name = js_strdup(lx->cur.text); js_lexer_next(lx); }
        return parse_class_body(lx, n);
    }
    if (eat_keyword(lx, "new")) {
        /* NewExpression: callee is a member chain (no calls), followed by
         * at most one argument list -- `new a.b.C(1).d` means
         * `(new a.b.C(1)).d`, not `new (a.b.C(1).d)`. Returning the
         * JS_NEW node from here (instead of parsing it inline in
         * parse_call_member) lets that function's normal `.`/`[]`/`(`
         * loop take over afterward and chain correctly off the result. */
        struct js_node *n = js_node_new(JS_NEW);
        struct js_node *callee = parse_primary(lx);
        for (;;) {
            if (eat_punct(lx, ".")) {
                struct js_node *m = js_node_new(JS_MEMBER);
                m->u.member.object = callee;
                struct js_node *prop = js_node_new(JS_IDENT);
                prop->u.ident.name = js_strdup(lx->cur.text);
                js_lexer_next(lx);
                m->u.member.property = prop;
                m->u.member.computed = 0;
                callee = m;
            } else if (eat_punct(lx, "[")) {
                struct js_node *m = js_node_new(JS_MEMBER);
                m->u.member.object = callee;
                m->u.member.property = parse_expr(lx);
                m->u.member.computed = 1;
                expect_punct(lx, "]");
                callee = m;
            } else {
                break;
            }
        }
        n->u.call.callee = callee;
        if (eat_punct(lx, "(")) n->u.call.args = parse_arg_list(lx);
        return n;
    }
    if (eat_punct(lx, "(")) {
        struct js_node *n = parse_expr(lx);
        expect_punct(lx, ")");
        return n;
    }
    if (eat_punct(lx, "[")) {
        struct js_node *n = js_node_new(JS_ARRAY_LIT);
        struct js_node *head = NULL, *tail = NULL;
        while (!is_punct(lx, "]") && lx->cur.type != TOK_EOF) {
            struct js_node *el;
            if (eat_punct(lx, "...")) {
                el = js_node_new(JS_SPREAD);
                el->u.unary.operand = parse_assign(lx);
            } else {
                el = parse_assign(lx);
            }
            if (!head) head = el; else tail->next = el;
            tail = el;
            if (!eat_punct(lx, ",")) break;
        }
        expect_punct(lx, "]");
        n->u.array_lit.elements = head;
        return n;
    }
    if (eat_punct(lx, "{")) {
        struct js_node *n = js_node_new(JS_OBJECT_LIT);
        struct js_node *head = NULL, *tail = NULL;
        while (!is_punct(lx, "}") && lx->cur.type != TOK_EOF) {
            char key[JS_TOKEN_MAX_LEN];
            strcpy(key, lx->cur.text);
            js_lexer_next(lx); /* ident or string key */
            expect_punct(lx, ":");
            struct js_node *value = parse_assign(lx);
            struct js_node *prop = js_node_new(JS_VAR_DECL);
            prop->u.var_decl.name = js_strdup(key);
            prop->u.var_decl.init = value;
            if (!head) head = prop; else tail->next = prop;
            tail = prop;
            if (!eat_punct(lx, ",")) break;
        }
        expect_punct(lx, "}");
        n->u.object_lit.props = head;
        return n;
    }

    parse_error(lx, "expected expression");
    struct js_node *n = js_node_new(JS_NUM_LIT);
    n->u.num_lit.value = 0;
    if (lx->cur.type != TOK_EOF) js_lexer_next(lx);
    return n;
}

static struct js_node *parse_call_member(struct js_lexer *lx) {
    struct js_node *n = parse_primary(lx);
    for (;;) {
        if (eat_punct(lx, ".")) {
            struct js_node *m = js_node_new(JS_MEMBER);
            m->u.member.object = n;
            struct js_node *prop = js_node_new(JS_IDENT);
            prop->u.ident.name = js_strdup(lx->cur.text);
            js_lexer_next(lx);
            m->u.member.property = prop;
            m->u.member.computed = 0;
            n = m;
        } else if (eat_punct(lx, "[")) {
            struct js_node *m = js_node_new(JS_MEMBER);
            m->u.member.object = n;
            m->u.member.property = parse_expr(lx);
            m->u.member.computed = 1;
            expect_punct(lx, "]");
            n = m;
        } else if (eat_punct(lx, "(")) {
            struct js_node *c = js_node_new(JS_CALL);
            c->u.call.callee = n;
            c->u.call.args = parse_arg_list(lx);
            n = c;
        } else {
            break;
        }
    }
    return n;
}

static struct js_node *parse_postfix(struct js_lexer *lx) {
    struct js_node *n = parse_call_member(lx);
    if (is_punct(lx, "++") || is_punct(lx, "--")) {
        struct js_node *u = js_node_new(JS_UPDATE);
        u->u.unary.op = js_strdup(lx->cur.text);
        u->u.unary.operand = n;
        u->u.unary.prefix = 0;
        js_lexer_next(lx);
        return u;
    }
    return n;
}

static struct js_node *parse_unary(struct js_lexer *lx) {
    if (is_punct(lx, "!") || is_punct(lx, "-") || is_punct(lx, "+") || is_keyword(lx, "typeof")) {
        struct js_node *n = js_node_new(JS_UNARY);
        n->u.unary.op = js_strdup(lx->cur.text);
        js_lexer_next(lx);
        n->u.unary.operand = parse_unary(lx);
        return n;
    }
    if (is_punct(lx, "++") || is_punct(lx, "--")) {
        struct js_node *n = js_node_new(JS_UPDATE);
        n->u.unary.op = js_strdup(lx->cur.text);
        n->u.unary.prefix = 1;
        js_lexer_next(lx);
        n->u.unary.operand = parse_unary(lx);
        return n;
    }
    return parse_postfix(lx);
}

static struct js_node *parse_binary_level(struct js_lexer *lx, const char **ops, int nops,
                                           enum js_node_type node_type,
                                           struct js_node *(*next_level)(struct js_lexer *)) {
    struct js_node *left = next_level(lx);
    for (;;) {
        int matched = 0;
        for (int i = 0; i < nops; i++) {
            if (is_punct(lx, ops[i])) {
                const char *op = js_strdup(ops[i]);
                js_lexer_next(lx);
                struct js_node *right = next_level(lx);
                struct js_node *n = js_node_new(node_type);
                n->u.binary.op = op;
                n->u.binary.left = left;
                n->u.binary.right = right;
                left = n;
                matched = 1;
                break;
            }
        }
        if (!matched) break;
    }
    return left;
}

static struct js_node *parse_mul(struct js_lexer *lx) {
    static const char *ops[] = {"*", "/", "%"};
    return parse_binary_level(lx, ops, 3, JS_BINARY, parse_unary);
}
static struct js_node *parse_add(struct js_lexer *lx) {
    static const char *ops[] = {"+", "-"};
    return parse_binary_level(lx, ops, 2, JS_BINARY, parse_mul);
}
static struct js_node *parse_rel(struct js_lexer *lx) {
    static const char *ops[] = {"<=", ">=", "<", ">"};
    return parse_binary_level(lx, ops, 4, JS_BINARY, parse_add);
}
static struct js_node *parse_eq(struct js_lexer *lx) {
    static const char *ops[] = {"===", "!==", "==", "!="};
    return parse_binary_level(lx, ops, 4, JS_BINARY, parse_rel);
}
static struct js_node *parse_logic_and(struct js_lexer *lx) {
    static const char *ops[] = {"&&"};
    return parse_binary_level(lx, ops, 1, JS_LOGICAL, parse_eq);
}
static struct js_node *parse_logic_or(struct js_lexer *lx) {
    static const char *ops[] = {"||"};
    return parse_binary_level(lx, ops, 1, JS_LOGICAL, parse_logic_and);
}

static struct js_node *parse_conditional(struct js_lexer *lx) {
    struct js_node *test = parse_logic_or(lx);
    if (eat_punct(lx, "?")) {
        struct js_node *n = js_node_new(JS_CONDITIONAL);
        n->u.conditional.test = test;
        n->u.conditional.cons = parse_assign(lx);
        expect_punct(lx, ":");
        n->u.conditional.alt = parse_assign(lx);
        return n;
    }
    return test;
}

/* Arrow functions (`x => ...`, `(a, b) => ...`) share a prefix with a
 * parenthesized expression and a plain identifier, so there's no way to
 * tell without looking past the closing `)` for `=>`. Snapshot the
 * lexer (a flat struct -- `src` just points at the caller's buffer, so
 * a copy/restore is safe and cheap) and speculatively parse a param
 * list; restore and return NULL if it doesn't pan out. */
static struct js_node *try_parse_arrow_function(struct js_lexer *lx) {
    struct js_lexer save = *lx;
    struct js_node *params = NULL;
    int ok = 0;

    if (lx->cur.type == TOK_IDENT) {
        struct js_node *p = js_node_new(JS_IDENT);
        p->u.ident.name = js_strdup(lx->cur.text);
        js_lexer_next(lx);
        if (is_punct(lx, "=>")) { params = p; ok = 1; }
    } else if (is_punct(lx, "(")) {
        js_lexer_next(lx);
        struct js_node *head = NULL, *tail = NULL;
        int bad = 0;
        while (!is_punct(lx, ")") && lx->cur.type != TOK_EOF) {
            struct js_node *p = parse_binding_target(lx);
            if (!p) { bad = 1; break; }
            if (!head) head = p; else tail->next = p;
            tail = p;
            if (!eat_punct(lx, ",")) break;
        }
        if (!bad && eat_punct(lx, ")") && is_punct(lx, "=>")) { params = head; ok = 1; }
    }

    if (!ok) { *lx = save; return NULL; }

    js_lexer_next(lx); /* consume "=>" */
    struct js_node *n = js_node_new(JS_ARROW_FUNC);
    n->u.func.params = params;
    if (is_punct(lx, "{")) {
        n->u.func.body = parse_block(lx);
    } else {
        /* Expression body: `x => x + 1` behaves like `x => { return x + 1; }`. */
        struct js_node *ret = js_node_new(JS_RETURN);
        ret->u.return_stmt.value = parse_assign(lx);
        struct js_node *block = js_node_new(JS_BLOCK);
        block->u.block.stmts = ret;
        n->u.func.body = block;
    }
    return n;
}

static struct js_node *parse_assign(struct js_lexer *lx) {
    struct js_node *arrow = try_parse_arrow_function(lx);
    if (arrow) return arrow;
    struct js_node *left = parse_conditional(lx);
    static const char *ops[] = {"=", "+=", "-=", "*=", "/=", "%="};
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        if (is_punct(lx, ops[i])) {
            const char *op = js_strdup(ops[i]);
            js_lexer_next(lx);
            struct js_node *n = js_node_new(JS_ASSIGN);
            n->u.assign.op = op;
            n->u.assign.target = left;
            n->u.assign.value = parse_assign(lx);
            return n;
        }
    }
    return left;
}

static struct js_node *parse_expr(struct js_lexer *lx) {
    struct js_node *left = parse_assign(lx);
    if (eat_punct(lx, ",")) {
        struct js_node *n = js_node_new(JS_SEQ);
        n->u.seq.left = left;
        n->u.seq.right = parse_expr(lx);
        return n;
    }
    return left;
}

static struct js_node *parse_var_decl_list(struct js_lexer *lx) {
    struct js_node *head = NULL, *tail = NULL;
    for (;;) {
        struct js_node *n = js_node_new(JS_VAR_DECL);
        if (is_punct(lx, "{") || is_punct(lx, "[")) {
            n->u.var_decl.pattern = parse_binding_target(lx);
        } else {
            n->u.var_decl.name = js_strdup(lx->cur.text);
            js_lexer_next(lx);
        }
        if (eat_punct(lx, "=")) n->u.var_decl.init = parse_assign(lx);
        if (!head) head = n; else tail->next = n;
        tail = n;
        if (!eat_punct(lx, ",")) break;
    }
    return head;
}

static struct js_node *parse_func_body_and_params(struct js_lexer *lx, struct js_node **params_out) {
    expect_punct(lx, "(");
    struct js_node *phead = NULL, *ptail = NULL;
    while (!is_punct(lx, ")") && lx->cur.type != TOK_EOF) {
        struct js_node *p = parse_binding_target(lx);
        if (!p) { parse_error(lx, "parameter"); break; }
        if (!phead) phead = p; else ptail->next = p;
        ptail = p;
        if (!eat_punct(lx, ",")) break;
    }
    expect_punct(lx, ")");
    *params_out = phead;
    return parse_block(lx);
}

static struct js_node *parse_block(struct js_lexer *lx) {
    expect_punct(lx, "{");
    struct js_node *n = js_node_new(JS_BLOCK);
    struct js_node *head = NULL, *tail = NULL;
    while (!is_punct(lx, "}") && lx->cur.type != TOK_EOF) {
        uint32_t before = lx->pos;
        struct js_node *stmt = parse_statement(lx);
        if (stmt) {
            if (!head) head = stmt; else tail->next = stmt;
            tail = stmt;
            while (tail->next) tail = tail->next;
        }
        if (lx->pos == before) js_lexer_next(lx); /* guarantee forward progress */
    }
    expect_punct(lx, "}");
    n->u.block.stmts = head;
    return n;
}

static struct js_node *parse_statement(struct js_lexer *lx) {
    if (is_punct(lx, "{")) return parse_block(lx);
    if (is_punct(lx, ";")) { js_lexer_next(lx); return js_node_new(JS_EMPTY); }

    if (is_keyword(lx, "var") || is_keyword(lx, "let") || is_keyword(lx, "const")) {
        js_lexer_next(lx);
        struct js_node *decls = parse_var_decl_list(lx);
        eat_punct(lx, ";");
        return decls;
    }
    if (eat_keyword(lx, "function")) {
        struct js_node *n = js_node_new(JS_FUNC_DECL);
        n->u.func.name = js_strdup(lx->cur.text);
        js_lexer_next(lx);
        n->u.func.body = parse_func_body_and_params(lx, &n->u.func.params);
        return n;
    }
    if (eat_keyword(lx, "class")) {
        struct js_node *n = js_node_new(JS_CLASS_DECL);
        n->u.class_decl.name = js_strdup(lx->cur.text);
        js_lexer_next(lx);
        return parse_class_body(lx, n);
    }
    if (eat_keyword(lx, "if")) {
        struct js_node *n = js_node_new(JS_IF);
        expect_punct(lx, "(");
        n->u.if_stmt.test = parse_expr(lx);
        expect_punct(lx, ")");
        n->u.if_stmt.cons = parse_statement(lx);
        if (eat_keyword(lx, "else")) n->u.if_stmt.alt = parse_statement(lx);
        return n;
    }
    if (eat_keyword(lx, "for")) {
        struct js_node *n = js_node_new(JS_FOR);
        expect_punct(lx, "(");
        if (is_keyword(lx, "var") || is_keyword(lx, "let") || is_keyword(lx, "const")) {
            js_lexer_next(lx);
            n->u.for_stmt.init = parse_var_decl_list(lx);
        } else if (!is_punct(lx, ";")) {
            n->u.for_stmt.init = parse_expr(lx);
        }
        expect_punct(lx, ";");
        if (!is_punct(lx, ";")) n->u.for_stmt.test = parse_expr(lx);
        expect_punct(lx, ";");
        if (!is_punct(lx, ")")) n->u.for_stmt.update = parse_expr(lx);
        expect_punct(lx, ")");
        n->u.for_stmt.body = parse_statement(lx);
        return n;
    }
    if (eat_keyword(lx, "while")) {
        struct js_node *n = js_node_new(JS_WHILE);
        expect_punct(lx, "(");
        n->u.while_stmt.test = parse_expr(lx);
        expect_punct(lx, ")");
        n->u.while_stmt.body = parse_statement(lx);
        return n;
    }
    if (eat_keyword(lx, "return")) {
        struct js_node *n = js_node_new(JS_RETURN);
        if (!is_punct(lx, ";") && lx->cur.type != TOK_EOF && !is_punct(lx, "}")) {
            n->u.return_stmt.value = parse_expr(lx);
        }
        eat_punct(lx, ";");
        return n;
    }
    if (eat_keyword(lx, "break")) { eat_punct(lx, ";"); return js_node_new(JS_BREAK); }
    if (eat_keyword(lx, "continue")) { eat_punct(lx, ";"); return js_node_new(JS_CONTINUE); }

    struct js_node *n = js_node_new(JS_EXPR_STMT);
    n->u.expr_stmt.expr = parse_expr(lx);
    eat_punct(lx, ";");
    return n;
}

struct js_node *js_parse_program(struct js_lexer *lx) {
    js_had_error = 0;
    struct js_node *program = js_node_new(JS_PROGRAM);
    struct js_node *head = NULL, *tail = NULL;
    while (lx->cur.type != TOK_EOF) {
        uint32_t before = lx->pos;
        struct js_node *stmt = parse_statement(lx);
        if (stmt) {
            if (!head) head = stmt; else tail->next = stmt;
            tail = stmt;
            while (tail->next) tail = tail->next;
        }
        if (lx->pos == before) js_lexer_next(lx);
    }
    program->u.program.body = head;
    return js_had_error ? NULL : program;
}
