#include <net/dom.h>
#include <kernel/kheap.h>
#include <string.h>

#define DOM_MAX_DEPTH 32
#define DOM_TEXT_CHUNK 4096

static char to_lower_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int tag_is(const char *tag, const char *name) {
    return strcmp(tag, name) == 0;
}

/* Elements that never have a matching close tag / children in the
 * pragmatic subset we care about. */
static int is_void_element(const char *tag) {
    return tag_is(tag, "br") || tag_is(tag, "hr") || tag_is(tag, "img") ||
           tag_is(tag, "meta") || tag_is(tag, "link") || tag_is(tag, "input") ||
           tag_is(tag, "area") || tag_is(tag, "base") || tag_is(tag, "col") ||
           tag_is(tag, "embed") || tag_is(tag, "source") || tag_is(tag, "track") ||
           tag_is(tag, "wbr");
}

static struct dom_node *new_node(enum dom_node_type type) {
    struct dom_node *n = kmalloc(sizeof(struct dom_node));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->type = type;
    return n;
}

static void append_child(struct dom_node *parent, struct dom_node *child) {
    if (!parent || !child) return;
    if (parent->last_child) parent->last_child->next = child;
    else parent->children = child;
    parent->last_child = child;
}

/* Decodes a `&...;` entity starting at data[i] (data[i] == '&'). Returns
 * the decoded character in *out and the number of source bytes consumed
 * (including '&' and the trailing ';'); returns 0 if unrecognized. */
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

/* Parses a `name="value"` / `name='value'` / bare `name` attribute list
 * up to '>' or "/>" and copies out the handful of attributes the layout
 * engine cares about. `*i` starts right after the tag name and is left
 * pointing at '>' (not consumed). */
static void parse_attrs(const char *html, uint32_t len, uint32_t *i, struct dom_node *node) {
    while (*i < len && html[*i] != '>') {
        while (*i < len && (html[*i] == ' ' || html[*i] == '\t' || html[*i] == '\n' ||
                             html[*i] == '\r' || html[*i] == '/')) (*i)++;
        if (*i >= len || html[*i] == '>') break;

        char name[24];
        int nlen = 0;
        while (*i < len && html[*i] != '=' && html[*i] != '>' && html[*i] != ' ' &&
               html[*i] != '\t' && html[*i] != '\n' && nlen < 23) {
            name[nlen++] = to_lower_ch(html[*i]);
            (*i)++;
        }
        name[nlen] = 0;

        while (*i < len && (html[*i] == ' ' || html[*i] == '\t')) (*i)++;

        char value[DOM_MAX_STYLE];
        int vlen = 0;
        if (*i < len && html[*i] == '=') {
            (*i)++;
            while (*i < len && (html[*i] == ' ' || html[*i] == '\t')) (*i)++;
            char quote = 0;
            if (*i < len && (html[*i] == '"' || html[*i] == '\'')) { quote = html[*i]; (*i)++; }
            while (*i < len) {
                if (quote ? html[*i] == quote : (html[*i] == ' ' || html[*i] == '>' || html[*i] == '\t')) break;
                if (vlen < (int)sizeof(value) - 1) value[vlen++] = html[*i];
                (*i)++;
            }
            if (quote && *i < len && html[*i] == quote) (*i)++;
        }
        value[vlen] = 0;

        if (nlen > 0) {
            if (tag_is(name, "id")) strncpy(node->id, value, DOM_MAX_ID - 1);
            else if (tag_is(name, "class")) strncpy(node->class_name, value, DOM_MAX_CLASS - 1);
            else if (tag_is(name, "href")) strncpy(node->href, value, DOM_MAX_HREF - 1);
            else if (tag_is(name, "style")) strncpy(node->style, value, DOM_MAX_STYLE - 1);
        }
    }
}

static void skip_raw_element(const char *data, uint32_t len, uint32_t *i, const char *close_tag) {
    uint32_t close_len = (uint32_t)strlen(close_tag);
    while (*i < len) {
        if (data[*i] == '<' && *i + close_len <= len) {
            int match = 1;
            for (uint32_t k = 0; k < close_len; k++) {
                if (to_lower_ch(data[*i + k]) != close_tag[k]) { match = 0; break; }
            }
            if (match) return;
        }
        (*i)++;
    }
}

/* Appends one decoded character to a text node under construction,
 * growing its kmalloc'd buffer in fixed-size chunks. */
static void text_append(char **buf, int *len, int *cap, char c) {
    if (*len + 1 >= *cap) {
        int new_cap = *cap + DOM_TEXT_CHUNK;
        char *grown = kmalloc((size_t)new_cap);
        if (!grown) return; /* out of heap -- drop the rest of this text run */
        if (*buf) { memcpy(grown, *buf, (size_t)*len); kfree(*buf); }
        *buf = grown;
        *cap = new_cap;
    }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = 0;
}

struct dom_node *dom_parse(const char *html, uint32_t len, char *title_out, int title_cap) {
    struct dom_node *root = new_node(DOM_ELEMENT);
    if (!root) return NULL;
    strcpy(root->tag, "body");

    struct dom_node *stack[DOM_MAX_DEPTH];
    int depth = 0;
    stack[depth] = root;

    int in_title = 0;
    int title_len = 0;
    if (title_out && title_cap > 0) title_out[0] = 0;

    char *text_buf = NULL;
    int text_len = 0, text_cap = 0;

    for (uint32_t i = 0; i < len; ) {
        char c = html[i];

        if (c == '<') {
            /* Flush any accumulated text run as a text node under the
             * current top-of-stack element before handling the tag. */
            if (text_len > 0) {
                struct dom_node *tn = new_node(DOM_TEXT);
                if (tn) { tn->text = text_buf; append_child(stack[depth], tn); }
                else kfree(text_buf);
                text_buf = NULL; text_len = 0; text_cap = 0;
            } else if (text_buf) {
                kfree(text_buf);
                text_buf = NULL; text_cap = 0;
            }

            uint32_t tag_start = i + 1;
            int closing = 0;
            uint32_t j = tag_start;
            if (j < len && html[j] == '!') { /* comment / doctype -- skip to '>' */
                while (j < len && html[j] != '>') j++;
                if (j < len) j++;
                i = j;
                continue;
            }
            if (j < len && html[j] == '/') { closing = 1; j++; }

            char tag[DOM_MAX_TAG];
            int tlen = 0;
            while (j < len && html[j] != '>' && html[j] != ' ' && html[j] != '\t' &&
                   html[j] != '\n' && html[j] != '\r' && html[j] != '/' && tlen < DOM_MAX_TAG - 1) {
                tag[tlen++] = to_lower_ch(html[j]);
                j++;
            }
            tag[tlen] = 0;

            if (tlen == 0) { /* stray '<' */
                while (j < len && html[j] != '>') j++;
                if (j < len) j++;
                i = j;
                continue;
            }

            if (closing) {
                while (j < len && html[j] != '>') j++;
                if (j < len) j++;
                i = j;
                if (tag_is(tag, "title")) { in_title = 0; continue; }
                /* Pop stack until we find (and pop) a matching open tag. */
                for (int d = depth; d >= 1; d--) {
                    if (tag_is(stack[d]->tag, tag)) { depth = d - 1; break; }
                }
                continue;
            }

            struct dom_node *node = new_node(DOM_ELEMENT);
            if (!node) { while (j < len && html[j] != '>') j++; if (j < len) j++; i = j; continue; }
            strcpy(node->tag, tag);
            parse_attrs(html, len, &j, node);
            if (j < len && html[j] == '>') j++;
            i = j;

            if (tag_is(tag, "script") || tag_is(tag, "style")) {
                char close_seq[10];
                strcpy(close_seq, "</");
                strcat(close_seq, tag);
                skip_raw_element(html, len, &i, close_seq);
                /* not attached to the tree -- purely skipped */
                continue;
            }

            append_child(stack[depth], node);

            if (tag_is(tag, "title")) { in_title = 1; title_len = 0; continue; }

            if (!is_void_element(tag) && depth < DOM_MAX_DEPTH - 1) {
                depth++;
                stack[depth] = node;
            }
            continue;
        }

        char decoded = (c == '\n' || c == '\t' || c == '\r') ? ' ' : c;
        int consumed = 1;
        if (c == '&') {
            char e;
            int n = decode_entity(html, len, i, &e);
            if (n > 0) { decoded = e; consumed = n; }
        }

        if (in_title) {
            if (title_out && title_len < title_cap - 1 &&
                !(decoded == ' ' && title_len > 0 && title_out[title_len - 1] == ' ')) {
                title_out[title_len++] = decoded;
                title_out[title_len] = 0;
            }
        } else {
            text_append(&text_buf, &text_len, &text_cap, decoded);
        }
        i += (uint32_t)consumed;
    }

    if (text_len > 0) {
        struct dom_node *tn = new_node(DOM_TEXT);
        if (tn) { tn->text = text_buf; append_child(stack[depth], tn); }
        else kfree(text_buf);
    } else if (text_buf) {
        kfree(text_buf);
    }

    return root;
}

void dom_free(struct dom_node *node) {
    if (!node) return;
    struct dom_node *child = node->children;
    while (child) {
        struct dom_node *next = child->next;
        dom_free(child);
        child = next;
    }
    if (node->text) kfree(node->text);
    kfree(node);
}
