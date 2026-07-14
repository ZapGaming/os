#ifndef CC_LEXER_H
#define CC_LEXER_H

#include <stdint.h>

#define CC_TOKEN_MAX_LEN 64
#define CC_STRING_MAX_LEN 512

enum cc_token_type {
    CTOK_EOF,
    CTOK_NUM,
    CTOK_STR,
    CTOK_IDENT,
    CTOK_KEYWORD,
    CTOK_PUNCT, /* operators/punctuation, verbatim text e.g. "==", "+=", "{" (only a few multi-char ones are actually lexed -- see cc/lexer.c) */
};

struct cc_token {
    enum cc_token_type type;
    char text[CC_TOKEN_MAX_LEN];  /* ident/keyword/punct text */
    int32_t num_value;            /* valid when type == CTOK_NUM */
    char str_value[CC_STRING_MAX_LEN]; /* decoded (escapes resolved) string body, valid when type == CTOK_STR */
    uint32_t str_len;
    int line; /* for error messages */
};

struct cc_lexer {
    const char *src;
    uint32_t len;
    uint32_t pos;
    int line;
    struct cc_token cur;
    char errmsg[128];
    int error;
};

void cc_lexer_init(struct cc_lexer *lx, const char *src, uint32_t len);
/* Advances past the current token and lexes the next one into lx->cur.
 * On a lex error, lx->error is set and lx->cur becomes CTOK_EOF so the
 * parser unwinds instead of looping forever. */
void cc_lexer_next(struct cc_lexer *lx);

#endif
