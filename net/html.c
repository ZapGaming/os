#include <net/html.h>
#include <string.h>

static char to_lower_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int tag_is(const char *tag, const char *name) {
    return strcmp(tag, name) == 0;
}

static int is_heading(const char *tag) {
    return tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == 0;
}

static int is_flush_on_open(const char *tag) {
    return tag_is(tag, "p") || tag_is(tag, "div") || tag_is(tag, "li") || is_heading(tag) ||
           tag_is(tag, "br") || tag_is(tag, "ul") || tag_is(tag, "ol") || tag_is(tag, "tr");
}

static int is_flush_on_close(const char *tag) {
    return tag_is(tag, "p") || tag_is(tag, "div") || tag_is(tag, "li") || is_heading(tag) || tag_is(tag, "tr");
}

struct parser_state {
    struct html_doc *out;
    char cur[HTML_MAX_LINE_LEN];
    int cur_len;
    enum html_style cur_style;
    int bold_depth, link_depth, heading_depth;
    int in_title;
    int list_prefix_pending;
};

static void flush_line(struct parser_state *st) {
    while (st->cur_len > 0 && st->cur[st->cur_len - 1] == ' ') st->cur_len--;
    if (st->cur_len == 0) return;
    if (st->out->line_count >= HTML_MAX_LINES) return;

    struct html_line *line = &st->out->lines[st->out->line_count++];
    memcpy(line->text, st->cur, (size_t)st->cur_len);
    line->text[st->cur_len] = 0;
    line->style = st->cur_style;
    st->cur_len = 0;
}

static void emit_blank(struct parser_state *st) {
    if (st->out->line_count == 0) return;
    if (st->out->line_count >= HTML_MAX_LINES) return;
    struct html_line *prev = &st->out->lines[st->out->line_count - 1];
    if (prev->text[0] == 0) return; /* already blank, don't stack blanks */
    struct html_line *line = &st->out->lines[st->out->line_count++];
    line->text[0] = 0;
    line->style = HTML_STYLE_NORMAL;
}

static void update_style(struct parser_state *st) {
    if (st->heading_depth > 0) st->cur_style = HTML_STYLE_HEADING;
    else if (st->link_depth > 0) st->cur_style = HTML_STYLE_LINK;
    else if (st->bold_depth > 0) st->cur_style = HTML_STYLE_BOLD;
    else st->cur_style = HTML_STYLE_NORMAL;
}

static void append_char(struct parser_state *st, char c) {
    if (c == ' ' && st->cur_len > 0 && st->cur[st->cur_len - 1] == ' ') return; /* collapse whitespace */

    /* Don't silently truncate long runs of text -- flush_line() doesn't
     * touch cur_style, so this just wraps onto a fresh same-styled line. */
    if (st->cur_len >= HTML_MAX_LINE_LEN - 1) flush_line(st);

    if (st->list_prefix_pending) {
        st->cur[st->cur_len++] = '-';
        st->cur[st->cur_len++] = ' ';
        st->list_prefix_pending = 0;
    }
    st->cur[st->cur_len++] = c;
}

/* Decodes a `&...;` entity starting at data[i] (data[i] == '&'). Returns
 * the decoded character in *out and the number of source bytes consumed
 * (including '&' and the trailing ';'); returns 0 consumed if this isn't
 * a recognized entity (caller should just emit '&' literally then). */
static int decode_entity(const char *data, uint32_t len, uint32_t i, char *out) {
    struct { const char *name; char ch; } table[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'},
        {"apos;", '\''}, {"#39;", '\''}, {"nbsp;", ' '},
    };
    for (unsigned t = 0; t < sizeof(table) / sizeof(table[0]); t++) {
        uint32_t nlen = (uint32_t)strlen(table[t].name);
        if (i + 1 + nlen <= len && memcmp(data + i + 1, table[t].name, nlen) == 0) {
            *out = table[t].ch;
            return (int)(1 + nlen);
        }
    }
    return 0;
}

static void skip_raw_element(const char *data, uint32_t len, uint32_t *i, const char *close_tag) {
    uint32_t close_len = (uint32_t)strlen(close_tag);
    while (*i < len) {
        if (data[*i] == '<' && *i + close_len <= len) {
            int match = 1;
            for (uint32_t k = 0; k < close_len; k++) {
                if (to_lower_ch(data[*i + k]) != close_tag[k]) { match = 0; break; }
            }
            if (match) return; /* leave *i pointing at the closing tag's '<' */
        }
        (*i)++;
    }
}

void html_parse(const char *html, uint32_t len, struct html_doc *out) {
    memset(out, 0, sizeof(*out));
    struct parser_state st;
    memset(&st, 0, sizeof(st));
    st.out = out;

    int title_len = 0;

    for (uint32_t i = 0; i < len; ) {
        char c = html[i];

        if (c == '<') {
            uint32_t tag_start = i + 1;
            int closing = 0;
            uint32_t j = tag_start;
            if (j < len && html[j] == '/') { closing = 1; j++; }

            char tag[16];
            int tlen = 0;
            while (j < len && html[j] != '>' && html[j] != ' ' && html[j] != '\t' &&
                   html[j] != '\n' && html[j] != '/' && tlen < 15) {
                tag[tlen++] = to_lower_ch(html[j]);
                j++;
            }
            tag[tlen] = 0;

            while (j < len && html[j] != '>') j++;
            if (j < len) j++; /* consume '>' */
            i = j;

            if (tlen == 0) continue;

            if (tag_is(tag, "script") || tag_is(tag, "style")) {
                if (!closing) {
                    char close_seq[10];
                    strcpy(close_seq, "</");
                    strcat(close_seq, tag);
                    skip_raw_element(html, len, &i, close_seq);
                }
                continue;
            }

            if (tag_is(tag, "title")) { st.in_title = !closing; continue; }
            if (tag_is(tag, "b") || tag_is(tag, "strong")) {
                flush_line(&st); /* commit text accumulated under the old style first */
                st.bold_depth += closing ? -1 : 1;
                if (st.bold_depth < 0) st.bold_depth = 0;
                update_style(&st);
                continue;
            }
            if (tag_is(tag, "a")) {
                flush_line(&st);
                st.link_depth += closing ? -1 : 1;
                if (st.link_depth < 0) st.link_depth = 0;
                update_style(&st);
                continue;
            }
            if (is_heading(tag)) {
                if (!closing) { flush_line(&st); emit_blank(&st); st.heading_depth++; }
                else { st.heading_depth--; if (st.heading_depth < 0) st.heading_depth = 0; flush_line(&st); emit_blank(&st); }
                update_style(&st);
                continue;
            }
            if (tag_is(tag, "li")) {
                if (!closing) { flush_line(&st); st.list_prefix_pending = 1; }
                else flush_line(&st);
                continue;
            }
            if (is_flush_on_open(tag) && !closing) {
                flush_line(&st);
                if (tag_is(tag, "p") || tag_is(tag, "div")) emit_blank(&st);
                continue;
            }
            if (is_flush_on_close(tag) && closing) {
                flush_line(&st);
                if (tag_is(tag, "p") || tag_is(tag, "div")) emit_blank(&st);
                continue;
            }
            continue;
        }

        if (c == '&') {
            char decoded;
            int consumed = decode_entity(html, len, i, &decoded);
            if (consumed > 0) {
                if (st.in_title) {
                    if (title_len < HTML_MAX_TITLE - 1) out->title[title_len++] = decoded;
                } else {
                    append_char(&st, decoded);
                }
                i += (uint32_t)consumed;
                continue;
            }
        }

        char emit = (c == '\n' || c == '\t' || c == '\r') ? ' ' : c;
        if (st.in_title) {
            if (title_len < HTML_MAX_TITLE - 1 && !(emit == ' ' && title_len > 0 && out->title[title_len - 1] == ' ')) {
                out->title[title_len++] = emit;
            }
        } else {
            append_char(&st, emit);
        }
        i++;
    }

    flush_line(&st);
    out->title[title_len] = 0;
}
