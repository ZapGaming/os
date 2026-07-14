#include <js/lexer.h>
#include <string.h>

static const char *keywords[] = {
    "var", "let", "const", "function", "return", "if", "else", "for", "while",
    "break", "continue", "true", "false", "null", "undefined", "this", "typeof",
    "class", "new",
    /* Deliberately no "extends": this engine has no prototype chain, so
     * class inheritance is out of scope (see js/interp.c's JS_NEW). Not
     * lexing it as a keyword means `class Foo extends Bar {}` fails with
     * an honest parse error (unexpected identifier "extends" where `{`
     * was expected) instead of silently dropping the parent class. */
};

static int is_keyword(const char *text) {
    for (unsigned i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strcmp(text, keywords[i]) == 0) return 1;
    }
    return 0;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
static int is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

static void skip_ws_and_comments(struct js_lexer *lx) {
    for (;;) {
        while (lx->pos < lx->len &&
               (lx->src[lx->pos] == ' ' || lx->src[lx->pos] == '\t' ||
                lx->src[lx->pos] == '\n' || lx->src[lx->pos] == '\r')) {
            lx->pos++;
        }
        if (lx->pos + 1 < lx->len && lx->src[lx->pos] == '/' && lx->src[lx->pos + 1] == '/') {
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        if (lx->pos + 1 < lx->len && lx->src[lx->pos] == '/' && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            while (lx->pos + 1 < lx->len && !(lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/')) lx->pos++;
            lx->pos += 2;
            continue;
        }
        break;
    }
}

char js_lexer_decode_escape(char c) {
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        default: return c; /* \\, \', \", \`, \$, and anything else pass through literally */
    }
}

void js_lexer_init(struct js_lexer *lx, const char *src, uint32_t len) {
    lx->src = src;
    lx->len = len;
    lx->pos = 0;
    js_lexer_next(lx);
}

void js_lexer_next(struct js_lexer *lx) {
    skip_ws_and_comments(lx);

    if (lx->pos >= lx->len) {
        lx->cur.type = TOK_EOF;
        lx->cur.text[0] = 0;
        return;
    }

    char c = lx->src[lx->pos];

    if (is_digit(c)) {
        int32_t value = 0;
        int i = 0;
        while (lx->pos < lx->len && is_digit(lx->src[lx->pos]) && i < JS_TOKEN_MAX_LEN - 1) {
            lx->cur.text[i++] = lx->src[lx->pos];
            value = value * 10 + (lx->src[lx->pos] - '0');
            lx->pos++;
        }
        lx->cur.text[i] = 0;
        lx->cur.type = TOK_NUM;
        lx->cur.num_value = value;
        return;
    }

    if (c == '`') {
        /* Template literal: scan for the matching closing backtick without
         * decoding or splitting on ${...} here -- js/parser.c re-walks
         * template_raw/template_len to build alternating string-chunk and
         * interpolated-expression nodes (it needs a whole sub-lexer per
         * interpolation, which this single-token lexer can't hand back).
         * Brace-depth tracking only applies inside a ${...}: a bare
         * backtick nested inside one (e.g. a string literal in the
         * interpolated expression) isn't handled -- rare enough in
         * generated JS to accept as a scope limit. */
        lx->pos++;
        uint32_t start = lx->pos;
        int depth = 0;
        while (lx->pos < lx->len) {
            char ch = lx->src[lx->pos];
            if (depth == 0 && ch == '`') break;
            if (ch == '\\' && lx->pos + 1 < lx->len) { lx->pos += 2; continue; }
            if (depth == 0 && ch == '$' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '{') {
                depth = 1;
                lx->pos += 2;
                continue;
            }
            if (depth > 0) {
                if (ch == '{') depth++;
                else if (ch == '}') depth--;
            }
            lx->pos++;
        }
        lx->cur.template_raw = lx->src + start;
        lx->cur.template_len = lx->pos - start;
        lx->cur.text[0] = 0;
        if (lx->pos < lx->len) lx->pos++; /* closing backtick */
        lx->cur.type = TOK_TEMPLATE;
        return;
    }

    if (c == '"' || c == '\'') {
        char quote = c;
        lx->pos++;
        int i = 0;
        while (lx->pos < lx->len && lx->src[lx->pos] != quote) {
            char ch = lx->src[lx->pos];
            if (ch == '\\' && lx->pos + 1 < lx->len) {
                lx->pos++;
                ch = js_lexer_decode_escape(lx->src[lx->pos]);
            }
            /* A string longer than the token buffer still gets scanned
             * all the way to its real closing quote (just silently
             * truncated in `text`) -- otherwise lx->pos is left
             * mid-string, and the lexer resumes tokenizing the string's
             * own tail as if it were code. */
            if (i < JS_TOKEN_MAX_LEN - 1) lx->cur.text[i++] = ch;
            lx->pos++;
        }
        lx->cur.text[i] = 0;
        if (lx->pos < lx->len) lx->pos++; /* closing quote */
        lx->cur.type = TOK_STR;
        return;
    }

    if (is_ident_start(c)) {
        int i = 0;
        while (lx->pos < lx->len && is_ident_char(lx->src[lx->pos]) && i < JS_TOKEN_MAX_LEN - 1) {
            lx->cur.text[i++] = lx->src[lx->pos];
            lx->pos++;
        }
        lx->cur.text[i] = 0;
        lx->cur.type = is_keyword(lx->cur.text) ? TOK_KEYWORD : TOK_IDENT;
        return;
    }

    /* Punctuation -- longest match first. */
    static const char *three[] = {"===", "!==", "..."};
    static const char *two[] = {"==", "!=", "<=", ">=", "&&", "||", "++", "--",
                                 "+=", "-=", "*=", "/=", "%=", "=>"};

    for (unsigned t = 0; t < sizeof(three) / sizeof(three[0]); t++) {
        uint32_t l = (uint32_t)strlen(three[t]);
        if (lx->pos + l <= lx->len && memcmp(lx->src + lx->pos, three[t], l) == 0) {
            strcpy(lx->cur.text, three[t]);
            lx->cur.type = TOK_PUNCT;
            lx->pos += l;
            return;
        }
    }
    for (unsigned t = 0; t < sizeof(two) / sizeof(two[0]); t++) {
        uint32_t l = (uint32_t)strlen(two[t]);
        if (lx->pos + l <= lx->len && memcmp(lx->src + lx->pos, two[t], l) == 0) {
            strcpy(lx->cur.text, two[t]);
            lx->cur.type = TOK_PUNCT;
            lx->pos += l;
            return;
        }
    }

    lx->cur.text[0] = c;
    lx->cur.text[1] = 0;
    lx->cur.type = TOK_PUNCT;
    lx->pos++;
}
