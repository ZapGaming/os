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
};

struct js_token {
    enum js_token_type type;
    char text[JS_TOKEN_MAX_LEN];
    int32_t num_value; /* valid when type == TOK_NUM -- integer only, no FPU in this kernel */
};

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
