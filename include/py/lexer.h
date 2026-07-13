#ifndef PY_LEXER_H
#define PY_LEXER_H

#include <stdint.h>

#define PY_TOKEN_MAX_LEN 64
#define PY_INDENT_MAX 32 /* max nested indentation levels tracked at once */

enum py_token_type {
    PTOK_EOF,
    PTOK_NEWLINE, /* end of a logical (non-blank, non-comment-only) line */
    PTOK_INDENT,
    PTOK_DEDENT,
    PTOK_INT,
    PTOK_FLOAT,
    PTOK_STR,
    PTOK_IDENT,
    PTOK_KEYWORD,
    PTOK_PUNCT, /* operators/punctuation, stored verbatim in text */
};

struct py_token {
    enum py_token_type type;
    char text[PY_TOKEN_MAX_LEN];
    int64_t int_value;   /* valid when type == PTOK_INT */
    double float_value;  /* valid when type == PTOK_FLOAT */
};

/* Indentation-based lexer, the standard "stack of indent-column-widths"
 * technique: whenever a new logical line's leading whitespace is deeper
 * than the current top of the stack we push and emit INDENT; whenever
 * it's shallower we pop (possibly emitting several DEDENTs) until the
 * stack top matches again. Blank lines and comment-only lines are
 * invisible to this: they never move the stack and never produce a
 * NEWLINE. Tabs are expanded to the next multiple of 8 columns (the
 * common, simple choice -- this lexer does not try to detect mixed
 * tabs/spaces ambiguity beyond that). A line inside unmatched
 * ( [ { is a "continuation" line: newlines there are treated as
 * ordinary whitespace so multi-line list literals and call argument
 * lists work without any special escaping. */
struct py_lexer {
    const char *src;
    uint32_t len;
    uint32_t pos;
    struct py_token cur;

    int indent_stack[PY_INDENT_MAX];
    int indent_sp;         /* index of current top; indent_stack[0] == 0 always */
    int pending_dedents;   /* extra DEDENT tokens still owed before resuming normal lexing */
    int paren_depth;       /* unmatched ( [ { count */
    int at_line_start;     /* next py_lexer_next() call must (re)compute indentation first */

    int error;             /* set on lexical errors (e.g. inconsistent indentation) */
    char error_msg[64];
};

void py_lexer_init(struct py_lexer *lx, const char *src, uint32_t len);
void py_lexer_next(struct py_lexer *lx);

#endif
