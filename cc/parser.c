/* Recursive-descent parser for the C subset -- see include/cc/parser.h
 * and the file comment on include/cc/codegen.h for exactly which
 * features are in scope. Deliberate simplification vs. real C, called
 * out here and in the final report rather than hidden: only ONE
 * declarator per declaration statement ("int a, *b;" -- not
 * supported, write two statements), no function prototypes without a
 * body (single translation unit, so every call target must already be
 * defined somewhere in the same file -- forward references are fine,
 * a function can call one defined later, since codegen registers every
 * top-level name before emitting any bodies).
 *
 * Every AST node is bump-allocated from the module's arena (cc_alloc)
 * -- see include/cc/module.h -- freed all at once when the whole
 * compile is done. */
#include <cc/parser.h>
#include <string.h>

static struct cc_node *node_new(struct cc_module *m, enum cc_node_type type, int line) {
    struct cc_node *n = (struct cc_node *)cc_alloc(m, sizeof(struct cc_node));
    n->type = type;
    n->line = line;
    n->next = NULL;
    return n;
}

static char *cc_strdup(struct cc_module *m, const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    char *p = (char *)cc_alloc(m, len + 1);
    memcpy(p, s, len + 1);
    return p;
}

static int is_punct(struct cc_lexer *lx, const char *s) {
    return lx->cur.type == CTOK_PUNCT && strcmp(lx->cur.text, s) == 0;
}
static int is_kw(struct cc_lexer *lx, const char *s) {
    return lx->cur.type == CTOK_KEYWORD && strcmp(lx->cur.text, s) == 0;
}

static int failed(struct cc_module *m, struct cc_lexer *lx) { return m->error || lx->error; }

static void expect_punct(struct cc_lexer *lx, struct cc_module *m, const char *s) {
    if (failed(m, lx)) return;
    if (!is_punct(lx, s)) {
        cc_module_errorf(m, lx->cur.line, "expected '%s' but got '%s'", s,
                          lx->cur.type == CTOK_EOF ? "<eof>" : lx->cur.text);
        return;
    }
    cc_lexer_next(lx);
}

static int accept_punct(struct cc_lexer *lx, const char *s) {
    if (is_punct(lx, s)) { cc_lexer_next(lx); return 1; }
    return 0;
}

static char *expect_ident(struct cc_lexer *lx, struct cc_module *m) {
    if (failed(m, lx)) return NULL;
    if (lx->cur.type != CTOK_IDENT) {
        cc_module_errorf(m, lx->cur.line, "expected an identifier but got '%s'",
                          lx->cur.type == CTOK_EOF ? "<eof>" : lx->cur.text);
        return NULL;
    }
    char *name = cc_strdup(m, lx->cur.text);
    cc_lexer_next(lx);
    return name;
}

/* ---- Expressions (lowest to highest precedence, standard C chain) --- */
static struct cc_node *parse_assignment(struct cc_lexer *lx, struct cc_module *m);
static struct cc_node *parse_unary(struct cc_lexer *lx, struct cc_module *m);

static struct cc_node *parse_primary(struct cc_lexer *lx, struct cc_module *m) {
    if (failed(m, lx)) return NULL;
    int line = lx->cur.line;
    if (lx->cur.type == CTOK_NUM) {
        struct cc_node *n = node_new(m, CC_NUM_LIT, line);
        n->u.num_lit.value = lx->cur.num_value;
        cc_lexer_next(lx);
        return n;
    }
    if (lx->cur.type == CTOK_STR) {
        uint32_t off = cc_add_rodata_string(m, lx->cur.str_value, lx->cur.str_len);
        struct cc_node *n = node_new(m, CC_STR_LIT, line);
        n->u.str_lit.rodata_offset = off;
        n->u.str_lit.len = lx->cur.str_len;
        cc_lexer_next(lx);
        return n;
    }
    if (lx->cur.type == CTOK_IDENT) {
        struct cc_node *n = node_new(m, CC_IDENT, line);
        n->u.ident.name = cc_strdup(m, lx->cur.text);
        cc_lexer_next(lx);
        return n;
    }
    if (accept_punct(lx, "(")) {
        struct cc_node *e = parse_assignment(lx, m);
        expect_punct(lx, m, ")");
        return e;
    }
    cc_module_errorf(m, line, "expected an expression but got '%s'",
                      lx->cur.type == CTOK_EOF ? "<eof>" : lx->cur.text);
    return NULL;
}

static struct cc_node *parse_postfix(struct cc_lexer *lx, struct cc_module *m) {
    struct cc_node *n = parse_primary(lx, m);
    for (;;) {
        if (failed(m, lx)) return n;
        int line = lx->cur.line;
        if (accept_punct(lx, "[")) {
            struct cc_node *idx = parse_assignment(lx, m);
            expect_punct(lx, m, "]");
            struct cc_node *ix = node_new(m, CC_INDEX, line);
            ix->u.index_expr.array = n;
            ix->u.index_expr.index = idx;
            n = ix;
        } else if (is_punct(lx, "(")) {
            if (n->type != CC_IDENT) {
                cc_module_errorf(m, line, "only calling a plain function name is supported");
                return n;
            }
            cc_lexer_next(lx);
            struct cc_node *call = node_new(m, CC_CALL, line);
            call->u.call.name = n->u.ident.name;
            struct cc_node *args = NULL, **tail = &args;
            if (!is_punct(lx, ")")) {
                for (;;) {
                    struct cc_node *a = parse_assignment(lx, m);
                    *tail = a; if (a) tail = &a->next;
                    if (!accept_punct(lx, ",")) break;
                }
            }
            expect_punct(lx, m, ")");
            call->u.call.args = args;
            n = call;
        } else if (accept_punct(lx, "++")) {
            struct cc_node *pn = node_new(m, CC_POSTINC, line);
            pn->u.incdec.operand = n;
            n = pn;
        } else if (accept_punct(lx, "--")) {
            struct cc_node *pn = node_new(m, CC_POSTDEC, line);
            pn->u.incdec.operand = n;
            n = pn;
        } else break;
    }
    return n;
}

static struct cc_node *parse_unary(struct cc_lexer *lx, struct cc_module *m) {
    if (failed(m, lx)) return NULL;
    int line = lx->cur.line;
    if (accept_punct(lx, "-")) {
        struct cc_node *n = node_new(m, CC_UNARY, line);
        strcpy(n->u.unary.op, "-");
        n->u.unary.operand = parse_unary(lx, m);
        return n;
    }
    if (accept_punct(lx, "!")) {
        struct cc_node *n = node_new(m, CC_UNARY, line);
        strcpy(n->u.unary.op, "!");
        n->u.unary.operand = parse_unary(lx, m);
        return n;
    }
    if (accept_punct(lx, "~")) {
        struct cc_node *n = node_new(m, CC_UNARY, line);
        strcpy(n->u.unary.op, "~");
        n->u.unary.operand = parse_unary(lx, m);
        return n;
    }
    if (accept_punct(lx, "&")) {
        struct cc_node *n = node_new(m, CC_ADDR, line);
        n->u.addr_deref.operand = parse_unary(lx, m);
        return n;
    }
    if (accept_punct(lx, "*")) {
        struct cc_node *n = node_new(m, CC_DEREF, line);
        n->u.addr_deref.operand = parse_unary(lx, m);
        return n;
    }
    if (accept_punct(lx, "++")) {
        struct cc_node *n = node_new(m, CC_PREINC, line);
        n->u.incdec.operand = parse_unary(lx, m);
        return n;
    }
    if (accept_punct(lx, "--")) {
        struct cc_node *n = node_new(m, CC_PREDEC, line);
        n->u.incdec.operand = parse_unary(lx, m);
        return n;
    }
    /* '+' as a no-op unary prefix, since it's common in the wild and
     * costs nothing to accept. */
    if (accept_punct(lx, "+")) return parse_unary(lx, m);
    return parse_postfix(lx, m);
}

/* One binary-operator precedence level: parses `next` (the tighter-
 * binding level below it), then repeatedly consumes any operator in
 * `ops` (NULL-terminated) at this level, left-associatively. Every
 * level in the standard C precedence chain below is just this loop
 * wired to the level beneath it with its own operator set -- shared
 * as a single helper rather than repeating the loop by hand at every
 * level. */
typedef struct cc_node *(*cc_parse_fn)(struct cc_lexer *, struct cc_module *);

static struct cc_node *parse_binop_level(struct cc_lexer *lx, struct cc_module *m,
                                          cc_parse_fn next, const char **ops,
                                          enum cc_node_type nodetype) {
    struct cc_node *left = next(lx, m);
    for (;;) {
        if (failed(m, lx)) return left;
        const char *matched = NULL;
        for (int i = 0; ops[i]; i++) if (is_punct(lx, ops[i])) { matched = ops[i]; break; }
        if (!matched) break;
        int line = lx->cur.line;
        cc_lexer_next(lx);
        struct cc_node *right = next(lx, m);
        struct cc_node *n = node_new(m, nodetype, line);
        /* binary and logical nodes share the same {op,left,right} shape */
        strcpy(n->u.binary.op, matched);
        n->u.binary.left = left;
        n->u.binary.right = right;
        left = n;
    }
    return left;
}

static const char *ops_mul[] = { "*", "/", "%", NULL };
static const char *ops_add[] = { "+", "-", NULL };
static const char *ops_shift[] = { "<<", ">>", NULL };
static const char *ops_rel[] = { "<", ">", "<=", ">=", NULL };
static const char *ops_eq[] = { "==", "!=", NULL };
static const char *ops_band[] = { "&", NULL };
static const char *ops_bxor[] = { "^", NULL };
static const char *ops_bor[] = { "|", NULL };
static const char *ops_land[] = { "&&", NULL };
static const char *ops_lor[] = { "||", NULL };

static struct cc_node *parse_mul(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_unary, ops_mul, CC_BINARY); }
static struct cc_node *parse_add(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_mul, ops_add, CC_BINARY); }
static struct cc_node *parse_shift(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_add, ops_shift, CC_BINARY); }
static struct cc_node *parse_rel(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_shift, ops_rel, CC_BINARY); }
static struct cc_node *parse_eq(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_rel, ops_eq, CC_BINARY); }
static struct cc_node *parse_band(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_eq, ops_band, CC_BINARY); }
static struct cc_node *parse_bxor(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_band, ops_bxor, CC_BINARY); }
static struct cc_node *parse_bor(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_bxor, ops_bor, CC_BINARY); }
static struct cc_node *parse_land(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_bor, ops_land, CC_LOGICAL); }
static struct cc_node *parse_lor(struct cc_lexer *lx, struct cc_module *m) { return parse_binop_level(lx, m, parse_land, ops_lor, CC_LOGICAL); }

static struct cc_node *parse_assignment(struct cc_lexer *lx, struct cc_module *m) {
    struct cc_node *left = parse_lor(lx, m);
    if (failed(m, lx)) return left;
    if (is_punct(lx, "=")) {
        int line = lx->cur.line;
        cc_lexer_next(lx);
        struct cc_node *right = parse_assignment(lx, m);
        struct cc_node *n = node_new(m, CC_ASSIGN, line);
        n->u.assign.target = left;
        n->u.assign.value = right;
        return n;
    }
    return left;
}

/* ---- Types -------------------------------------------------------- */
/* Consumes "int" or "void" plus any following '*' tokens. Returns 1 for
 * void (only legal as a function's return type or a no-params marker,
 * checked by callers), 0 for int. */
static int parse_type_base(struct cc_lexer *lx, struct cc_module *m, int *ptr_depth) {
    *ptr_depth = 0;
    int is_void = 0;
    if (is_kw(lx, "void")) { is_void = 1; cc_lexer_next(lx); }
    else if (is_kw(lx, "int")) { cc_lexer_next(lx); }
    else {
        cc_module_errorf(m, lx->cur.line, "expected a type ('int' or 'void') but got '%s'",
                          lx->cur.type == CTOK_EOF ? "<eof>" : lx->cur.text);
        return 0;
    }
    while (accept_punct(lx, "*")) (*ptr_depth)++;
    return is_void;
}

/* ---- Statements ------------------------------------------------------*/
static struct cc_node *parse_statement(struct cc_lexer *lx, struct cc_module *m);

static struct cc_node *parse_block(struct cc_lexer *lx, struct cc_module *m) {
    int line = lx->cur.line;
    expect_punct(lx, m, "{");
    struct cc_node *blk = node_new(m, CC_BLOCK, line);
    struct cc_node **tail = &blk->u.block.stmts;
    while (!failed(m, lx) && !is_punct(lx, "}") && lx->cur.type != CTOK_EOF) {
        struct cc_node *s = parse_statement(lx, m);
        *tail = s;
        if (s) tail = &s->next;
    }
    expect_punct(lx, m, "}");
    return blk;
}

/* Local ("int"-led) variable declaration -- also reused verbatim for a
 * for-loop's init clause, since that clause's grammar already ends in
 * the same ';' this consumes. */
static struct cc_node *parse_var_decl(struct cc_lexer *lx, struct cc_module *m) {
    int line = lx->cur.line;
    int ptr_depth;
    int is_void = parse_type_base(lx, m, &ptr_depth);
    if (is_void) { cc_module_errorf(m, line, "variables cannot have type 'void'"); return NULL; }
    char *name = expect_ident(lx, m);
    struct cc_node *n = node_new(m, CC_VAR_DECL, line);
    n->u.var_decl.name = name;
    n->u.var_decl.type.ptr_depth = ptr_depth;
    n->u.var_decl.type.is_array = 0;
    n->u.var_decl.type.array_len = 0;
    n->u.var_decl.init = NULL;
    if (accept_punct(lx, "[")) {
        if (failed(m, lx)) return n;
        if (lx->cur.type != CTOK_NUM) { cc_module_errorf(m, lx->cur.line, "expected an array size"); return n; }
        n->u.var_decl.type.is_array = 1;
        n->u.var_decl.type.array_len = lx->cur.num_value;
        cc_lexer_next(lx);
        expect_punct(lx, m, "]");
    }
    if (accept_punct(lx, "=")) {
        if (n->u.var_decl.type.is_array) {
            cc_module_errorf(m, line, "array initializers are not supported (assign elements individually)");
            return n;
        }
        n->u.var_decl.init = parse_assignment(lx, m);
    }
    expect_punct(lx, m, ";");
    return n;
}

static struct cc_node *parse_statement(struct cc_lexer *lx, struct cc_module *m) {
    if (failed(m, lx)) return NULL;
    int line = lx->cur.line;

    if (is_punct(lx, "{")) return parse_block(lx, m);
    if (accept_punct(lx, ";")) { struct cc_node *n = node_new(m, CC_EXPR_STMT, line); n->u.expr_stmt.expr = NULL; return n; }

    if (is_kw(lx, "int") || is_kw(lx, "void")) return parse_var_decl(lx, m);

    if (is_kw(lx, "if")) {
        cc_lexer_next(lx);
        expect_punct(lx, m, "(");
        struct cc_node *cond = parse_assignment(lx, m);
        expect_punct(lx, m, ")");
        struct cc_node *thenb = parse_statement(lx, m);
        struct cc_node *elseb = NULL;
        if (is_kw(lx, "else")) { cc_lexer_next(lx); elseb = parse_statement(lx, m); }
        struct cc_node *n = node_new(m, CC_IF, line);
        n->u.if_stmt.cond = cond;
        n->u.if_stmt.then_branch = thenb;
        n->u.if_stmt.else_branch = elseb;
        return n;
    }

    if (is_kw(lx, "while")) {
        cc_lexer_next(lx);
        expect_punct(lx, m, "(");
        struct cc_node *cond = parse_assignment(lx, m);
        expect_punct(lx, m, ")");
        struct cc_node *body = parse_statement(lx, m);
        struct cc_node *n = node_new(m, CC_WHILE, line);
        n->u.while_stmt.cond = cond;
        n->u.while_stmt.body = body;
        return n;
    }

    if (is_kw(lx, "for")) {
        cc_lexer_next(lx);
        expect_punct(lx, m, "(");
        struct cc_node *init;
        if (is_kw(lx, "int")) {
            init = parse_var_decl(lx, m); /* consumes its own ';' */
        } else if (accept_punct(lx, ";")) {
            init = NULL;
        } else {
            struct cc_node *e = parse_assignment(lx, m);
            expect_punct(lx, m, ";");
            struct cc_node *es = node_new(m, CC_EXPR_STMT, line);
            es->u.expr_stmt.expr = e;
            init = es;
        }
        struct cc_node *cond = NULL;
        if (!is_punct(lx, ";")) cond = parse_assignment(lx, m);
        expect_punct(lx, m, ";");
        struct cc_node *post = NULL;
        if (!is_punct(lx, ")")) post = parse_assignment(lx, m);
        expect_punct(lx, m, ")");
        struct cc_node *body = parse_statement(lx, m);
        struct cc_node *n = node_new(m, CC_FOR, line);
        n->u.for_stmt.init = init;
        n->u.for_stmt.cond = cond;
        n->u.for_stmt.post = post;
        n->u.for_stmt.body = body;
        return n;
    }

    if (is_kw(lx, "return")) {
        cc_lexer_next(lx);
        struct cc_node *n = node_new(m, CC_RETURN, line);
        if (!is_punct(lx, ";")) n->u.return_stmt.value = parse_assignment(lx, m);
        else n->u.return_stmt.value = NULL;
        expect_punct(lx, m, ";");
        return n;
    }

    if (is_kw(lx, "break")) { cc_lexer_next(lx); expect_punct(lx, m, ";"); return node_new(m, CC_BREAK, line); }
    if (is_kw(lx, "continue")) { cc_lexer_next(lx); expect_punct(lx, m, ";"); return node_new(m, CC_CONTINUE, line); }

    struct cc_node *e = parse_assignment(lx, m);
    expect_punct(lx, m, ";");
    struct cc_node *n = node_new(m, CC_EXPR_STMT, line);
    n->u.expr_stmt.expr = e;
    return n;
}

/* ---- Top level -------------------------------------------------------*/
static struct cc_node *parse_params(struct cc_lexer *lx, struct cc_module *m) {
    struct cc_node *params = NULL, **tail = &params;
    if (is_punct(lx, ")")) return NULL;
    if (is_kw(lx, "void")) { cc_lexer_next(lx); return NULL; } /* "int f(void)" -- zero params */
    for (;;) {
        int line = lx->cur.line;
        int ptr_depth;
        int is_void = parse_type_base(lx, m, &ptr_depth);
        if (is_void) { cc_module_errorf(m, line, "a parameter cannot have type 'void'"); return params; }
        char *name = expect_ident(lx, m);
        struct cc_node *p = node_new(m, CC_PARAM, line);
        p->u.param.name = name;
        p->u.param.type.ptr_depth = ptr_depth;
        p->u.param.type.is_array = 0;
        *tail = p; tail = &p->next;
        if (!accept_punct(lx, ",")) break;
    }
    return params;
}

struct cc_node *cc_parse_program(struct cc_lexer *lx, struct cc_module *m) {
    struct cc_node *program = node_new(m, CC_PROGRAM, 0);
    struct cc_node **tail = &program->u.program.decls;

    while (!failed(m, lx) && lx->cur.type != CTOK_EOF) {
        int line = lx->cur.line;
        if (!is_kw(lx, "int") && !is_kw(lx, "void")) {
            cc_module_errorf(m, line, "expected a type ('int' or 'void') at top level but got '%s'",
                              lx->cur.type == CTOK_EOF ? "<eof>" : lx->cur.text);
            break;
        }
        int ptr_depth;
        int is_void = parse_type_base(lx, m, &ptr_depth);
        char *name = expect_ident(lx, m);
        if (failed(m, lx)) break;

        if (is_punct(lx, "(")) {
            cc_lexer_next(lx);
            struct cc_node *params = parse_params(lx, m);
            expect_punct(lx, m, ")");
            struct cc_node *fn = node_new(m, CC_FUNC_DECL, line);
            fn->u.func_decl.name = name;
            fn->u.func_decl.ret_type.ptr_depth = ptr_depth;
            fn->u.func_decl.ret_type.is_array = 0;
            fn->u.func_decl.is_void = is_void;
            fn->u.func_decl.params = params;
            if (is_punct(lx, "{")) {
                fn->u.func_decl.body = parse_block(lx, m);
            } else {
                cc_module_errorf(m, line, "function '%s' has no body (prototypes without a body are not supported)", name);
                break;
            }
            *tail = fn; tail = &fn->next;
        } else {
            if (is_void) { cc_module_errorf(m, line, "variable '%s' cannot have type 'void'", name); break; }
            struct cc_node *gv = node_new(m, CC_GLOBAL_VAR, line);
            gv->u.global_var.name = name;
            gv->u.global_var.type.ptr_depth = ptr_depth;
            gv->u.global_var.type.is_array = 0;
            gv->u.global_var.type.array_len = 0;
            gv->u.global_var.has_init = 0;
            gv->u.global_var.init_value = 0;
            if (accept_punct(lx, "[")) {
                if (lx->cur.type != CTOK_NUM) { cc_module_errorf(m, lx->cur.line, "expected an array size"); break; }
                gv->u.global_var.type.is_array = 1;
                gv->u.global_var.type.array_len = lx->cur.num_value;
                cc_lexer_next(lx);
                expect_punct(lx, m, "]");
            }
            if (accept_punct(lx, "=")) {
                if (gv->u.global_var.type.is_array) {
                    cc_module_errorf(m, line, "global array initializers are not supported");
                    break;
                }
                int neg = accept_punct(lx, "-");
                if (lx->cur.type != CTOK_NUM) { cc_module_errorf(m, lx->cur.line, "a global initializer must be a constant integer"); break; }
                gv->u.global_var.init_value = neg ? -lx->cur.num_value : lx->cur.num_value;
                gv->u.global_var.has_init = 1;
                cc_lexer_next(lx);
            }
            expect_punct(lx, m, ";");
            *tail = gv; tail = &gv->next;
        }
    }
    return program;
}
