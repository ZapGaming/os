#ifndef JS_LEXER_H
#define JS_LEXER_H

#include <stdint.h>

#define JS_TOKEN_MAX_LEN 64

enum js_token_type {
    TOK_EOF,
    TOK_NUM,
    TOK_STR,
    TOK_IDENT,
    TOK_KEYWORD,
    TOK_PUNCT, /* operators/punctuation, stored verbatim in text (e.g. "===", "+=", "{") */
    TOK_TEMPLATE, /* backtick string; raw (undecoded, un-split) body in template_raw/template_len -- the
                   * parser re-scans it for ${...} interpolations since it can be longer than
                   * JS_TOKEN_MAX_LEN and needs to hand sub-ranges to the expression parser. */
};

struct js_token {
    enum js_token_type type;
    char text[JS_TOKEN_MAX_LEN];
    int32_t num_value; /* valid when type == TOK_NUM -- integer only, no FPU in this kernel */
    const char *template_raw; /* valid when type == TOK_TEMPLATE: points into the original source, not NUL-terminated */
    uint32_t template_len;
};

/* Shared with js/parser.c for decoding template-literal text chunks the
 * same way string literals are decoded. */
char js_lexer_decode_escape(char c);

struct js_lexer {
    const char *src;
    uint32_t len;
    uint32_t pos;
    struct js_token cur;
};

void js_lexer_init(struct js_lexer *lx, const char *src, uint32_t len);
/* Advances past the current token and lexes the next one into lx->cur. */
void js_lexer_next(struct js_lexer *lx);

#endif
