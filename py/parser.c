#include <py/py.h>
#include <py/lexer.h>
#include <py/error.h>
#include <string.h>

static int is_punct(struct py_lexer *lx, const char *text) {
    return lx->cur.type == PTOK_PUNCT && strcmp(lx->cur.text, text) == 0;
}
static int is_keyword(struct py_lexer *lx, const char *text) {
    return lx->cur.type == PTOK_KEYWORD && strcmp(lx->cur.text, text) == 0;
}
static int eat_punct(struct py_lexer *lx, const char *text) {
    if (is_punct(lx, text)) { py_lexer_next(lx); return 1; }
    return 0;
}
static int eat_keyword(struct py_lexer *lx, const char *text) {
    if (is_keyword(lx, text)) { py_lexer_next(lx); return 1; }
    return 0;
}
static void syntax_error(struct py_lexer *lx, const char *msg) {
    (void)lx;
    py_error_set("SyntaxError: ", msg, NULL);
}
static void expect_punct(struct py_lexer *lx, const char *text) {
    if (eat_punct(lx, text)) return;
    char buf[24];
    uint32_t p = 0;
    buf[p++] = '\'';
    for (const char *s = text; *s; s++) buf[p++] = *s;
    buf[p++] = '\'';
    buf[p] = 0;
    py_error_set("SyntaxError: expected ", buf, NULL);
}
static void expect_tok(struct py_lexer *lx, enum py_token_type t, const char *what) {
    if (lx->cur.type == t) { py_lexer_next(lx); return; }
    syntax_error(lx, what);
}

/* Appends a (possibly multi-node, ->next-chained) statement list onto
 * head/tail, walking to the true end of the chain -- mirrors js/parser.c's
 * approach for the same reason: a single parse_statement() call can
 * itself return more than one node (e.g. "x = 1; y = 2"). */
static void append_chain(struct py_node **head, struct py_node **tail, struct py_node *chain) {
    if (!chain) return;
    if (!*head) *head = chain; else (*tail)->next = chain;
    *tail = chain;
    while ((*tail)->next) *tail = (*tail)->next;
}

static struct py_node *parse_expr(struct py_lexer *lx);
static struct py_node *parse_statement(struct py_lexer *lx);
static struct py_node *parse_suite(struct py_lexer *lx);

/* ---- Expressions (lowest to highest precedence) ------------------- */

static struct py_node *parse_atom(struct py_lexer *lx) {
    if (lx->cur.type == PTOK_INT) {
        struct py_node *n = py_node_new(PY_INT_LIT);
        n->u.int_lit.value = lx->cur.int_value;
        py_lexer_next(lx);
        return n;
    }
    if (lx->cur.type == PTOK_FLOAT) {
        struct py_node *n = py_node_new(PY_FLOAT_LIT);
        n->u.float_lit.value = lx->cur.float_value;
        py_lexer_next(lx);
        return n;
    }
    if (lx->cur.type == PTOK_STR) {
        struct py_node *n = py_node_new(PY_STR_LIT);
        n->u.str_lit.value = py_strdup(lx->cur.text);
        py_lexer_next(lx);
        return n;
    }
    if (is_keyword(lx, "True") || is_keyword(lx, "False")) {
        struct py_node *n = py_node_new(PY_BOOL_LIT);
        n->u.bool_lit.value = is_keyword(lx, "True");
        py_lexer_next(lx);
        return n;
    }
    if (eat_keyword(lx, "None")) return py_node_new(PY_NONE_LIT);
    if (lx->cur.type == PTOK_IDENT) {
        struct py_node *n = py_node_new(PY_IDENT);
        n->u.ident.name = py_strdup(lx->cur.text);
        py_lexer_next(lx);
        return n;
    }
    if (eat_punct(lx, "(")) {
        struct py_node *n = parse_expr(lx);
        expect_punct(lx, ")");
        return n;
    }
    if (eat_punct(lx, "[")) {
        struct py_node *n = py_node_new(PY_LIST_LIT);
        struct py_node *head = NULL, *tail = NULL;
        while (!is_punct(lx, "]") && lx->cur.type != PTOK_EOF) {
            struct py_node *el = parse_expr(lx);
            append_chain(&head, &tail, el);
            if (!eat_punct(lx, ",")) break;
        }
        expect_punct(lx, "]");
        n->u.list_lit.elements = head;
        return n;
    }

    syntax_error(lx, "expected expression");
    struct py_node *n = py_node_new(PY_INT_LIT);
    n->u.int_lit.value = 0;
    if (lx->cur.type != PTOK_EOF) py_lexer_next(lx);
    return n;
}

static struct py_node *parse_trailer(struct py_lexer *lx) {
    struct py_node *n = parse_atom(lx);
    for (;;) {
        if (eat_punct(lx, "(")) {
            struct py_node *c = py_node_new(PY_CALL);
            c->u.call.callee = n;
            struct py_node *head = NULL, *tail = NULL;
            while (!is_punct(lx, ")") && lx->cur.type != PTOK_EOF) {
                struct py_node *arg = parse_expr(lx);
                append_chain(&head, &tail, arg);
                if (!eat_punct(lx, ",")) break;
            }
            expect_punct(lx, ")");
            c->u.call.args = head;
            n = c;
        } else if (eat_punct(lx, "[")) {
            struct py_node *ix = py_node_new(PY_INDEX);
            ix->u.index_expr.object = n;
            ix->u.index_expr.index = parse_expr(lx);
            expect_punct(lx, "]");
            n = ix;
        } else {
            break;
        }
    }
    return n;
}

static struct py_node *parse_factor(struct py_lexer *lx);

static struct py_node *parse_power(struct py_lexer *lx) {
    struct py_node *base = parse_trailer(lx);
    if (eat_punct(lx, "**")) {
        struct py_node *n = py_node_new(PY_BINARY);
        n->u.binary.op = "**";
        n->u.binary.left = base;
        n->u.binary.right = parse_factor(lx); /* right-associative */
        return n;
    }
    return base;
}

static struct py_node *parse_factor(struct py_lexer *lx) {
    if (is_punct(lx, "-") || is_punct(lx, "+")) {
        const char *op = py_strdup(lx->cur.text);
        py_lexer_next(lx);
        struct py_node *n = py_node_new(PY_UNARY);
        n->u.unary.op = op;
        n->u.unary.operand = parse_factor(lx);
        return n;
    }
    return parse_power(lx);
}

static struct py_node *parse_term(struct py_lexer *lx) {
    struct py_node *left = parse_factor(lx);
    for (;;) {
        const char *op = NULL;
        if (is_punct(lx, "*")) op = "*";
        else if (is_punct(lx, "//")) op = "//";
        else if (is_punct(lx, "/")) op = "/";
        else if (is_punct(lx, "%")) op = "%";
        if (!op) break;
        py_lexer_next(lx);
        struct py_node *right = parse_factor(lx);
        struct py_node *n = py_node_new(PY_BINARY);
        n->u.binary.op = op;
        n->u.binary.left = left;
        n->u.binary.right = right;
        left = n;
    }
    return left;
}

static struct py_node *parse_arith(struct py_lexer *lx) {
    struct py_node *left = parse_term(lx);
    for (;;) {
        const char *op = NULL;
        if (is_punct(lx, "+")) op = "+";
        else if (is_punct(lx, "-")) op = "-";
        if (!op) break;
        py_lexer_next(lx);
        struct py_node *right = parse_term(lx);
        struct py_node *n = py_node_new(PY_BINARY);
        n->u.binary.op = op;
        n->u.binary.left = left;
        n->u.binary.right = right;
        left = n;
    }
    return left;
}

static struct py_node *parse_comparison(struct py_lexer *lx) {
    struct py_node *left = parse_arith(lx);
    for (;;) {
        const char *op = NULL;
        if (is_punct(lx, "==")) op = "==";
        else if (is_punct(lx, "!=")) op = "!=";
        else if (is_punct(lx, "<=")) op = "<=";
        else if (is_punct(lx, ">=")) op = ">=";
        else if (is_punct(lx, "<")) op = "<";
        else if (is_punct(lx, ">")) op = ">";
        if (!op) break;
        py_lexer_next(lx);
        struct py_node *right = parse_arith(lx);
        struct py_node *n = py_node_new(PY_BINARY);
        n->u.binary.op = op;
        n->u.binary.left = left;
        n->u.binary.right = right;
        left = n;
    }
    return left;
}

static struct py_node *parse_not_test(struct py_lexer *lx) {
    if (eat_keyword(lx, "not")) {
        struct py_node *n = py_node_new(PY_NOT);
        n->u.unary.op = "not";
        n->u.unary.operand = parse_not_test(lx);
        return n;
    }
    return parse_comparison(lx);
}

static struct py_node *parse_and_test(struct py_lexer *lx) {
    struct py_node *left = parse_not_test(lx);
    while (eat_keyword(lx, "and")) {
        struct py_node *right = parse_not_test(lx);
        struct py_node *n = py_node_new(PY_LOGICAL);
        n->u.binary.op = "and";
        n->u.binary.left = left;
        n->u.binary.right = right;
        left = n;
    }
    return left;
}

static struct py_node *parse_or_test(struct py_lexer *lx) {
    struct py_node *left = parse_and_test(lx);
    while (eat_keyword(lx, "or")) {
        struct py_node *right = parse_and_test(lx);
        struct py_node *n = py_node_new(PY_LOGICAL);
        n->u.binary.op = "or";
        n->u.binary.left = left;
        n->u.binary.right = right;
        left = n;
    }
    return left;
}

static struct py_node *parse_expr(struct py_lexer *lx) {
    return parse_or_test(lx);
}

/* ---- Statements ------------------------------------------------------ */

static int at_simple_stmt_end(struct py_lexer *lx) {
    return lx->cur.type == PTOK_NEWLINE || lx->cur.type == PTOK_EOF ||
           lx->cur.type == PTOK_DEDENT || is_punct(lx, ";");
}

static struct py_node *parse_simple_statement(struct py_lexer *lx) {
    if (eat_keyword(lx, "return")) {
        struct py_node *n = py_node_new(PY_RETURN);
        if (!at_simple_stmt_end(lx)) n->u.return_stmt.value = parse_expr(lx);
        return n;
    }
    if (eat_keyword(lx, "break")) return py_node_new(PY_BREAK);
    if (eat_keyword(lx, "continue")) return py_node_new(PY_CONTINUE);
    if (eat_keyword(lx, "pass")) return py_node_new(PY_PASS);

    struct py_node *left = parse_expr(lx);

    static const char *aug[][2] = {
        { "+=", "+" }, { "-=", "-" }, { "*=", "*" }, { "/=", "/" },
        { "//=", "//" }, { "%=", "%" }, { "**=", "**" },
    };
    if (eat_punct(lx, "=")) {
        struct py_node *n = py_node_new(PY_ASSIGN);
        n->u.assign.op = "=";
        n->u.assign.target = left;
        n->u.assign.value = parse_expr(lx);
        return n;
    }
    for (unsigned i = 0; i < sizeof(aug) / sizeof(aug[0]); i++) {
        if (is_punct(lx, aug[i][0])) {
            py_lexer_next(lx);
            struct py_node *n = py_node_new(PY_ASSIGN);
            n->u.assign.op = aug[i][1];
            n->u.assign.target = left;
            n->u.assign.value = parse_expr(lx);
            return n;
        }
    }

    struct py_node *n = py_node_new(PY_EXPR_STMT);
    n->u.expr_stmt.expr = left;
    return n;
}

static struct py_node *parse_simple_statement_line(struct py_lexer *lx) {
    struct py_node *head = NULL, *tail = NULL;
    for (;;) {
        struct py_node *stmt = parse_simple_statement(lx);
        append_chain(&head, &tail, stmt);
        if (!eat_punct(lx, ";")) break;
        if (lx->cur.type == PTOK_NEWLINE || lx->cur.type == PTOK_EOF || lx->cur.type == PTOK_DEDENT) break;
    }
    if (lx->cur.type == PTOK_NEWLINE) py_lexer_next(lx);
    return head;
}

/* Suite := ':' NEWLINE INDENT statement+ DEDENT | ':' simple_stmt (';' simple_stmt)* NEWLINE */
static struct py_node *parse_suite(struct py_lexer *lx) {
    expect_punct(lx, ":");
    struct py_node *block = py_node_new(PY_BLOCK);
    if (lx->cur.type == PTOK_NEWLINE) {
        py_lexer_next(lx);
        expect_tok(lx, PTOK_INDENT, "expected an indented block");
        struct py_node *head = NULL, *tail = NULL;
        while (lx->cur.type != PTOK_DEDENT && lx->cur.type != PTOK_EOF && !py_has_error()) {
            uint32_t before = lx->pos;
            struct py_node *stmt = parse_statement(lx);
            append_chain(&head, &tail, stmt);
            if (lx->pos == before && !py_has_error()) py_lexer_next(lx); /* forward progress guard */
        }
        expect_tok(lx, PTOK_DEDENT, "expected DEDENT");
        block->u.block.stmts = head;
    } else {
        block->u.block.stmts = parse_simple_statement_line(lx);
    }
    return block;
}

static struct py_node *parse_if_or_elif(struct py_lexer *lx) {
    py_lexer_next(lx); /* 'if' or 'elif' */
    struct py_node *n = py_node_new(PY_IF);
    n->u.if_stmt.test = parse_expr(lx);
    n->u.if_stmt.body = parse_suite(lx);
    if (is_keyword(lx, "elif")) {
        n->u.if_stmt.orelse = parse_if_or_elif(lx);
    } else if (eat_keyword(lx, "else")) {
        n->u.if_stmt.orelse = parse_suite(lx);
    }
    return n;
}

static struct py_node *parse_while(struct py_lexer *lx) {
    py_lexer_next(lx); /* 'while' */
    struct py_node *n = py_node_new(PY_WHILE);
    n->u.while_stmt.test = parse_expr(lx);
    n->u.while_stmt.body = parse_suite(lx);
    return n;
}

static struct py_node *parse_for(struct py_lexer *lx) {
    py_lexer_next(lx); /* 'for' */
    struct py_node *n = py_node_new(PY_FOR);
    if (lx->cur.type != PTOK_IDENT) {
        syntax_error(lx, "expected loop variable name");
    } else {
        n->u.for_stmt.var = py_strdup(lx->cur.text);
        py_lexer_next(lx);
    }
    if (!eat_keyword(lx, "in")) syntax_error(lx, "expected 'in'");
    n->u.for_stmt.iter = parse_expr(lx);
    n->u.for_stmt.body = parse_suite(lx);
    return n;
}

static struct py_node *parse_def(struct py_lexer *lx) {
    py_lexer_next(lx); /* 'def' */
    struct py_node *n = py_node_new(PY_FUNC_DEF);
    if (lx->cur.type != PTOK_IDENT) {
        syntax_error(lx, "expected function name");
    } else {
        n->u.func.name = py_strdup(lx->cur.text);
        py_lexer_next(lx);
    }
    expect_punct(lx, "(");
    struct py_node *phead = NULL, *ptail = NULL;
    while (lx->cur.type == PTOK_IDENT) {
        struct py_node *p = py_node_new(PY_IDENT);
        p->u.ident.name = py_strdup(lx->cur.text);
        py_lexer_next(lx);
        append_chain(&phead, &ptail, p);
        if (!eat_punct(lx, ",")) break;
    }
    expect_punct(lx, ")");
    n->u.func.params = phead;
    n->u.func.body = parse_suite(lx);
    return n;
}

static struct py_node *parse_statement(struct py_lexer *lx) {
    while (lx->cur.type == PTOK_NEWLINE) py_lexer_next(lx); /* stray blank line */
    if (is_keyword(lx, "if")) return parse_if_or_elif(lx);
    if (is_keyword(lx, "while")) return parse_while(lx);
    if (is_keyword(lx, "for")) return parse_for(lx);
    if (is_keyword(lx, "def")) return parse_def(lx);
    return parse_simple_statement_line(lx);
}

struct py_node *py_parse_program(struct py_lexer *lx) {
    /* Error state is reset once per py_run(), before py_lexer_init() runs
     * (which itself lexes the first token and so can already fail on a
     * pathological first line) -- resetting here would wipe that. */
    struct py_node *program = py_node_new(PY_PROGRAM);
    struct py_node *head = NULL, *tail = NULL;
    while (lx->cur.type != PTOK_EOF && !py_has_error()) {
        if (lx->cur.type == PTOK_NEWLINE || lx->cur.type == PTOK_DEDENT) { py_lexer_next(lx); continue; }
        uint32_t before = lx->pos;
        struct py_node *stmt = parse_statement(lx);
        append_chain(&head, &tail, stmt);
        if (lx->pos == before && lx->cur.type != PTOK_EOF && !py_has_error()) py_lexer_next(lx);
    }
    program->u.program.body = head;
    return py_has_error() ? NULL : program;
}
