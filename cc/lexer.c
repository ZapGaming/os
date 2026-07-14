/* Hand-written lexer for the C subset cc/ compiles -- see
 * include/cc/lexer.h. Modeled directly on js/lexer.c's structure
 * (single lookahead token in lx->cur, cc_lexer_next() advances it) but
 * for C token shapes: no template literals, decimal/hex integer
 * literals only (no floats -- see the doc comment on why in
 * include/cc/codegen.h). */
#include <cc/lexer.h>
#include <string.h>

static const char *keywords[] = {
    "int", "void", "if", "else", "while", "for", "return",
    "break", "continue", NULL
};

static int is_keyword(const char *s) {
    for (int i = 0; keywords[i]; i++) if (strcmp(s, keywords[i]) == 0) return 1;
    return 0;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static char peekc(struct cc_lexer *lx) { return lx->pos < lx->len ? lx->src[lx->pos] : 0; }
static char peekc2(struct cc_lexer *lx) { return lx->pos + 1 < lx->len ? lx->src[lx->pos + 1] : 0; }

static void skip_ws_and_comments(struct cc_lexer *lx) {
    for (;;) {
        char c = peekc(lx);
        if (c == '\n') { lx->line++; lx->pos++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { lx->pos++; continue; }
        if (c == '/' && peekc2(lx) == '/') {
            while (lx->pos < lx->len && peekc(lx) != '\n') lx->pos++;
            continue;
        }
        if (c == '/' && peekc2(lx) == '*') {
            lx->pos += 2;
            while (lx->pos < lx->len && !(peekc(lx) == '*' && peekc2(lx) == '/')) {
                if (peekc(lx) == '\n') lx->line++;
                lx->pos++;
            }
            if (lx->pos < lx->len) lx->pos += 2;
            continue;
        }
        break;
    }
}

static char decode_escape(char c) {
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        default: return c;
    }
}

static void lex_error(struct cc_lexer *lx, const char *msg) {
    if (lx->error) return;
    lx->error = 1;
    size_t i = 0;
    for (; msg[i] && i < sizeof(lx->errmsg) - 1; i++) lx->errmsg[i] = msg[i];
    lx->errmsg[i] = 0;
    lx->cur.type = CTOK_EOF;
}

void cc_lexer_init(struct cc_lexer *lx, const char *src, uint32_t len) {
    lx->src = src;
    lx->len = len;
    lx->pos = 0;
    lx->line = 1;
    lx->error = 0;
    lx->errmsg[0] = 0;
    cc_lexer_next(lx);
}

void cc_lexer_next(struct cc_lexer *lx) {
    if (lx->error) { lx->cur.type = CTOK_EOF; return; }
    skip_ws_and_comments(lx);
    lx->cur.line = lx->line;

    if (lx->pos >= lx->len) { lx->cur.type = CTOK_EOF; lx->cur.text[0] = 0; return; }

    char c = peekc(lx);

    if (is_digit(c)) {
        uint32_t start = lx->pos;
        int32_t val = 0;
        if (c == '0' && (peekc2(lx) == 'x' || peekc2(lx) == 'X')) {
            lx->pos += 2;
            while (lx->pos < lx->len) {
                char h = peekc(lx);
                int digit;
                if (h >= '0' && h <= '9') digit = h - '0';
                else if (h >= 'a' && h <= 'f') digit = 10 + (h - 'a');
                else if (h >= 'A' && h <= 'F') digit = 10 + (h - 'A');
                else break;
                val = val * 16 + digit;
                lx->pos++;
            }
        } else {
            while (lx->pos < lx->len && is_digit(peekc(lx))) {
                val = val * 10 + (peekc(lx) - '0');
                lx->pos++;
            }
        }
        (void)start;
        lx->cur.type = CTOK_NUM;
        lx->cur.num_value = val;
        return;
    }

    if (is_alpha(c)) {
        uint32_t i = 0;
        while (lx->pos < lx->len && is_alnum(peekc(lx)) && i < CC_TOKEN_MAX_LEN - 1) {
            lx->cur.text[i++] = peekc(lx);
            lx->pos++;
        }
        lx->cur.text[i] = 0;
        lx->cur.type = is_keyword(lx->cur.text) ? CTOK_KEYWORD : CTOK_IDENT;
        return;
    }

    if (c == '"') {
        lx->pos++;
        uint32_t i = 0;
        while (lx->pos < lx->len && peekc(lx) != '"') {
            char ch = peekc(lx);
            if (ch == '\n') { lex_error(lx, "unterminated string literal"); return; }
            if (ch == '\\') {
                lx->pos++;
                ch = decode_escape(peekc(lx));
            }
            if (i < CC_STRING_MAX_LEN - 1) lx->cur.str_value[i++] = ch;
            lx->pos++;
        }
        if (lx->pos >= lx->len) { lex_error(lx, "unterminated string literal"); return; }
        lx->pos++; /* closing quote */
        lx->cur.str_value[i] = 0;
        lx->cur.str_len = i;
        lx->cur.type = CTOK_STR;
        return;
    }

    if (c == '\'') {
        lx->pos++;
        char ch = peekc(lx);
        if (ch == '\\') { lx->pos++; ch = decode_escape(peekc(lx)); }
        lx->pos++;
        if (peekc(lx) != '\'') { lex_error(lx, "unterminated character literal"); return; }
        lx->pos++;
        lx->cur.type = CTOK_NUM;
        lx->cur.num_value = (unsigned char)ch;
        return;
    }

    /* Punctuation -- longest match first among the few multi-char
     * operators this subset supports. */
    static const char *multi[] = {
        "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "++", "--", NULL
    };
    for (int i = 0; multi[i]; i++) {
        size_t mlen = strlen(multi[i]);
        if (lx->pos + mlen <= lx->len && strncmp(lx->src + lx->pos, multi[i], mlen) == 0) {
            strcpy(lx->cur.text, multi[i]);
            lx->cur.type = CTOK_PUNCT;
            lx->pos += mlen;
            return;
        }
    }

    static const char single[] = "+-*/%<>=!&|^~()[]{};,.";
    if (strchr(single, c)) {
        lx->cur.text[0] = c;
        lx->cur.text[1] = 0;
        lx->cur.type = CTOK_PUNCT;
        lx->pos++;
        return;
    }

    lex_error(lx, "unexpected character");
}
