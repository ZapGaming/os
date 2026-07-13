#include <py/lexer.h>
#include <py/error.h>
#include <string.h>

static const char *keywords[] = {
    "True", "False", "None", "and", "or", "not", "if", "elif", "else",
    "while", "for", "in", "def", "return", "break", "continue", "pass",
};

static int is_keyword_text(const char *text) {
    for (unsigned i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strcmp(text, keywords[i]) == 0) return 1;
    }
    return 0;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

static char decode_escape(char c) {
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        default: return c; /* \\, \', \", and anything else pass through literally */
    }
}

void py_lexer_init(struct py_lexer *lx, const char *src, uint32_t len) {
    lx->src = src;
    lx->len = len;
    lx->pos = 0;
    lx->indent_stack[0] = 0;
    lx->indent_sp = 0;
    lx->pending_dedents = 0;
    lx->paren_depth = 0;
    lx->at_line_start = 1;
    lx->error = 0;
    lx->error_msg[0] = 0;
    py_lexer_next(lx);
}

static void indent_error(struct py_lexer *lx, const char *msg) {
    lx->error = 1;
    py_error_set("IndentationError: ", msg, NULL);
    lx->cur.type = PTOK_EOF;
    lx->cur.text[0] = 0;
}

/* Consumes leading whitespace/blank/comment-only lines and figures out
 * the indentation of the next real line of code, pushing/popping
 * lx->indent_stack as needed. Leaves lx->pos at the first
 * non-whitespace character of that line (or at lx->len on EOF).
 * Returns 1 if it already produced a token (INDENT/DEDENT/EOF) that
 * the caller should return immediately; 0 if the caller should fall
 * through to ordinary token lexing at the (now correctly positioned)
 * lx->pos. */
static int handle_line_start(struct py_lexer *lx) {
    for (;;) {
        int col = 0;
        while (lx->pos < lx->len && (lx->src[lx->pos] == ' ' || lx->src[lx->pos] == '\t')) {
            if (lx->src[lx->pos] == '\t') col = ((col / 8) + 1) * 8;
            else col++;
            lx->pos++;
        }
        if (lx->pos >= lx->len) {
            lx->at_line_start = 0;
            if (lx->indent_sp > 0) {
                lx->pending_dedents = lx->indent_sp - 1;
                lx->indent_sp = 0;
                lx->cur.type = PTOK_DEDENT;
                lx->cur.text[0] = 0;
                return 1;
            }
            lx->cur.type = PTOK_EOF;
            lx->cur.text[0] = 0;
            return 1;
        }
        char c = lx->src[lx->pos];
        if (c == '\r') { lx->pos++; continue; }
        if (c == '\n') { lx->pos++; continue; } /* blank line */
        if (c == '#') { while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++; continue; }

        /* Real content line. */
        lx->at_line_start = 0;
        if (col > lx->indent_stack[lx->indent_sp]) {
            if (lx->indent_sp + 1 >= PY_INDENT_MAX) {
                indent_error(lx, "too many nested indentation levels");
                return 1;
            }
            lx->indent_sp++;
            lx->indent_stack[lx->indent_sp] = col;
            lx->cur.type = PTOK_INDENT;
            lx->cur.text[0] = 0;
            return 1;
        }
        if (col < lx->indent_stack[lx->indent_sp]) {
            int pops = 0;
            while (lx->indent_sp > 0 && lx->indent_stack[lx->indent_sp] > col) { lx->indent_sp--; pops++; }
            if (lx->indent_stack[lx->indent_sp] != col) {
                indent_error(lx, "unindent does not match any outer indentation level");
                return 1;
            }
            lx->pending_dedents = pops - 1;
            lx->cur.type = PTOK_DEDENT;
            lx->cur.text[0] = 0;
            return 1;
        }
        return 0; /* col == current indent: no INDENT/DEDENT, lex the token normally */
    }
}

void py_lexer_next(struct py_lexer *lx) {
    if (py_has_error()) { lx->cur.type = PTOK_EOF; lx->cur.text[0] = 0; return; }

    if (lx->pending_dedents > 0) {
        lx->pending_dedents--;
        lx->cur.type = PTOK_DEDENT;
        lx->cur.text[0] = 0;
        return;
    }

    if (lx->at_line_start) {
        if (handle_line_start(lx)) return;
    }

    /* Skip inline whitespace/comments; if we hit a newline that ends
     * the logical line (i.e. we're not inside unmatched brackets),
     * that's the NEWLINE token. Inside brackets, newlines are just
     * whitespace, so keep looping. */
    for (;;) {
        while (lx->pos < lx->len && (lx->src[lx->pos] == ' ' || lx->src[lx->pos] == '\t' || lx->src[lx->pos] == '\r')) {
            lx->pos++;
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '#') {
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '\n') {
            if (lx->paren_depth > 0) { lx->pos++; continue; }
            lx->pos++;
            lx->cur.type = PTOK_NEWLINE;
            lx->cur.text[0] = 0;
            lx->at_line_start = 1;
            return;
        }
        break;
    }

    if (lx->pos >= lx->len) {
        if (lx->paren_depth > 0) {
            py_error_set("SyntaxError: ", "unexpected end of input (unmatched bracket)", NULL);
            lx->cur.type = PTOK_EOF;
            return;
        }
        /* Mid-line EOF with no trailing newline in the source: synthesize
         * one NEWLINE so the last statement terminates normally, then let
         * the next call's handle_line_start() drive DEDENTs/EOF. */
        lx->cur.type = PTOK_NEWLINE;
        lx->cur.text[0] = 0;
        lx->at_line_start = 1;
        return;
    }

    char c = lx->src[lx->pos];

    if (is_digit(c)) {
        int64_t value = 0;
        int i = 0;
        while (lx->pos < lx->len && is_digit(lx->src[lx->pos]) && i < PY_TOKEN_MAX_LEN - 1) {
            value = value * 10 + (lx->src[lx->pos] - '0');
            lx->cur.text[i++] = lx->src[lx->pos];
            lx->pos++;
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '.' &&
            !(lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '.')) {
            lx->pos++;
            double frac = 0.0, scale = 0.1;
            while (lx->pos < lx->len && is_digit(lx->src[lx->pos]) && i < PY_TOKEN_MAX_LEN - 1) {
                frac += (double)(lx->src[lx->pos] - '0') * scale;
                scale *= 0.1;
                lx->cur.text[i++] = lx->src[lx->pos];
                lx->pos++;
            }
            lx->cur.text[i] = 0;
            lx->cur.type = PTOK_FLOAT;
            lx->cur.float_value = (double)value + frac;
            return;
        }
        lx->cur.text[i] = 0;
        lx->cur.type = PTOK_INT;
        lx->cur.int_value = value;
        return;
    }

    if (c == '"' || c == '\'') {
        char quote = c;
        lx->pos++;
        int i = 0;
        while (lx->pos < lx->len && lx->src[lx->pos] != quote && i < PY_TOKEN_MAX_LEN - 1) {
            char ch = lx->src[lx->pos];
            if (ch == '\\' && lx->pos + 1 < lx->len) {
                lx->pos++;
                ch = decode_escape(lx->src[lx->pos]);
            }
            lx->cur.text[i++] = ch;
            lx->pos++;
        }
        lx->cur.text[i] = 0;
        if (lx->pos < lx->len) lx->pos++; /* closing quote */
        lx->cur.type = PTOK_STR;
        return;
    }

    if (is_ident_start(c)) {
        int i = 0;
        while (lx->pos < lx->len && is_ident_char(lx->src[lx->pos]) && i < PY_TOKEN_MAX_LEN - 1) {
            lx->cur.text[i++] = lx->src[lx->pos];
            lx->pos++;
        }
        lx->cur.text[i] = 0;
        lx->cur.type = is_keyword_text(lx->cur.text) ? PTOK_KEYWORD : PTOK_IDENT;
        return;
    }

    static const char *three[] = { "**=", "//=" };
    static const char *two[] = { "**", "//", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=" };

    for (unsigned t = 0; t < sizeof(three) / sizeof(three[0]); t++) {
        uint32_t l = (uint32_t)strlen(three[t]);
        if (lx->pos + l <= lx->len && memcmp(lx->src + lx->pos, three[t], l) == 0) {
            strcpy(lx->cur.text, three[t]);
            lx->cur.type = PTOK_PUNCT;
            lx->pos += l;
            return;
        }
    }
    for (unsigned t = 0; t < sizeof(two) / sizeof(two[0]); t++) {
        uint32_t l = (uint32_t)strlen(two[t]);
        if (lx->pos + l <= lx->len && memcmp(lx->src + lx->pos, two[t], l) == 0) {
            strcpy(lx->cur.text, two[t]);
            lx->cur.type = PTOK_PUNCT;
            lx->pos += l;
            return;
        }
    }

    lx->cur.text[0] = c;
    lx->cur.text[1] = 0;
    lx->cur.type = PTOK_PUNCT;
    lx->pos++;
    if (c == '(' || c == '[' || c == '{') lx->paren_depth++;
    else if (c == ')' || c == ']' || c == '}') { if (lx->paren_depth > 0) lx->paren_depth--; }
}
