#include <net/css.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

static char to_lower_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* No strstr() in this kernel's minimal freestanding string.h -- only
 * needed here (to spot "var(" inside a declaration value), so it's a
 * local helper rather than a new addition to the shared string lib. */
static char *find_substr(char *hay, const char *needle) {
    int nlen = (int)strlen(needle);
    for (char *p = hay; *p; p++) {
        int i = 0;
        for (; i < nlen && p[i] && p[i] == needle[i]; i++) { }
        if (i == nlen) return p;
    }
    return NULL;
}

/* The built-in "browser" defaults -- parsed through the same
 * css_parse_into() a page's own <style> rules go through, so there's
 * only one cascade implementation. Page rules are appended after this
 * in the sheet, so they win on any property they also set. */
static const char *default_css =
    "html,body,div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,hr,br,section,article,header,"
    "footer,nav,blockquote,pre,table,tr,form { display: block; }"
    "a,b,strong,i,em,span,small,code,label { display: inline; }"
    "body { color: #000000; }"
    "h1,h2,h3,h4,h5,h6 { font-weight: bold; color: #F2C14E; margin-top: 14px; margin-bottom: 8px; }"
    "p,ul,ol,blockquote,pre,table { margin-top: 8px; margin-bottom: 8px; }"
    "li { margin-top: 2px; margin-bottom: 2px; padding-left: 8px; }"
    "blockquote { padding-left: 16px; }"
    "a { color: #62D8FF; }"
    "b,strong { font-weight: bold; color: #FFFFFF; }"
    "hr { margin-top: 10px; margin-bottom: 10px; }";

void css_stylesheet_init(struct css_stylesheet *sheet) {
    sheet->head = NULL;
    sheet->tail = NULL;
    css_parse_into(sheet, default_css, (uint32_t)strlen(default_css));
}

void css_stylesheet_free(struct css_stylesheet *sheet) {
    struct css_rule *r = sheet->head;
    while (r) {
        struct css_rule *next = r->next;
        kfree(r);
        r = next;
    }
    sheet->head = sheet->tail = NULL;
}

static void trim(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && is_ws(s[len - 1])) s[--len] = 0;
    int start = 0;
    while (s[start] && is_ws(s[start])) start++;
    if (start > 0) {
        int i = 0;
        for (; s[start + i]; i++) s[i] = s[start + i];
        s[i] = 0;
    }
}

/* A selector token may be a descendant chain like "div p.note" -- we
 * only support simple selectors, so take the last space-separated
 * piece (the actual matched element), which is the common case anyway
 * (most real-world "div p" style rules still do the right thing for
 * plain <p> elements under our tag-only fallback). */
static void simplify_selector(char *sel) {
    trim(sel);
    char *last_space = NULL;
    for (char *p = sel; *p; p++) if (*p == ' ') last_space = p;
    if (last_space) {
        int i = 0;
        for (char *p = last_space + 1; *p; p++) sel[i++] = *p;
        sel[i] = 0;
    }
    for (char *p = sel; *p; p++) *p = to_lower_ch(*p);
}

static void skip_ws_comments(const char *text, uint32_t len, uint32_t *i) {
    for (;;) {
        while (*i < len && is_ws(text[*i])) (*i)++;
        if (*i + 1 < len && text[*i] == '/' && text[*i + 1] == '*') {
            *i += 2;
            while (*i + 1 < len && !(text[*i] == '*' && text[*i + 1] == '/')) (*i)++;
            *i += 2;
            continue;
        }
        break;
    }
}

void css_parse_into(struct css_stylesheet *sheet, const char *text, uint32_t len) {
    uint32_t i = 0;
    while (i < len) {
        skip_ws_comments(text, len, &i);
        if (i >= len) break;

        char selectors_raw[512];
        int slen = 0;
        while (i < len && text[i] != '{' && slen < (int)sizeof(selectors_raw) - 1) {
            selectors_raw[slen++] = text[i++];
        }
        selectors_raw[slen] = 0;
        if (i >= len) break; /* no rule body -- trailing garbage */
        i++; /* consume '{' */

        /* Sized to hold a whole rule body in one piece (up to ~1.5KB
         * seen in real minified Tailwind/Next.js output, e.g. the
         * universal `*,:before,:after{...}` reset that defines dozens
         * of --tw-* custom properties at once) -- NOT just "however
         * many declarations we'll keep" (CSS_MAX_DECLS). If this
         * buffer were sized to the latter and a real body ran past it,
         * the raw-copy loop below would stop mid-body without ever
         * consuming the rule's closing '}', desyncing the outer parse
         * loop and corrupting every rule after it in the stylesheet --
         * not just truncating the one oversized rule. */
        char decls_raw[4096];
        int dlen = 0;
        while (i < len && text[i] != '}' && dlen < (int)sizeof(decls_raw) - 1) {
            decls_raw[dlen++] = text[i++];
        }
        decls_raw[dlen] = 0;
        if (i < len) i++; /* consume '}' */

        struct css_rule *rule = kmalloc(sizeof(struct css_rule));
        if (!rule) { serial_printf("css: out of memory parsing rule\n"); continue; }
        memset(rule, 0, sizeof(*rule));

        /* Split selectors_raw on ',' */
        int start = 0;
        for (int p = 0; p <= slen && rule->selector_count < CSS_MAX_GROUPS; p++) {
            if (p == slen || selectors_raw[p] == ',') {
                char token[CSS_MAX_SELECTOR];
                int tl = p - start;
                if (tl > (int)sizeof(token) - 1) tl = (int)sizeof(token) - 1;
                if (tl > 0) {
                    memcpy(token, selectors_raw + start, (size_t)tl);
                    token[tl] = 0;
                    simplify_selector(token);
                    if (token[0]) strcpy(rule->selectors[rule->selector_count++], token);
                }
                start = p + 1;
            }
        }

        /* Split decls_raw on ';', each on the first ':' */
        start = 0;
        for (int p = 0; p <= dlen && rule->decl_count < CSS_MAX_DECLS; p++) {
            if (p == dlen || decls_raw[p] == ';') {
                char decl[80];
                int tl = p - start;
                if (tl > (int)sizeof(decl) - 1) tl = (int)sizeof(decl) - 1;
                if (tl > 0) {
                    memcpy(decl, decls_raw + start, (size_t)tl);
                    decl[tl] = 0;
                    char *colon = strchr(decl, ':');
                    if (colon) {
                        *colon = 0;
                        char *prop = decl;
                        char *value = colon + 1;
                        trim(prop);
                        trim(value);
                        for (char *q = prop; *q; q++) *q = to_lower_ch(*q);
                        if (prop[0] && value[0]) {
                            struct css_decl *d = &rule->decls[rule->decl_count++];
                            strncpy(d->prop, prop, CSS_PROP_LEN - 1);
                            strncpy(d->value, value, CSS_VALUE_LEN - 1);
                        }
                    }
                }
                start = p + 1;
            }
        }

        if (rule->selector_count == 0 || rule->decl_count == 0) {
            kfree(rule);
            continue;
        }

        if (sheet->tail) sheet->tail->next = rule;
        else sheet->head = rule;
        sheet->tail = rule;
    }
}

/* Scans raw HTML text (not the DOM) for <style>...</style> blocks --
 * simpler and more robust than trying to thread style content out
 * through the DOM tree, since dom_parse() just discards it. */
void css_extract_style_blocks(struct css_stylesheet *sheet, const char *html, uint32_t len) {
    for (uint32_t i = 0; i + 7 <= len; i++) {
        if (html[i] != '<') continue;
        int match = 1;
        const char *open = "<style";
        for (int k = 0; k < 6; k++) {
            if (i + (uint32_t)k >= len || to_lower_ch(html[i + (uint32_t)k]) != open[k]) { match = 0; break; }
        }
        if (!match) continue;

        uint32_t j = i + 6;
        while (j < len && html[j] != '>') j++;
        if (j >= len) break;
        j++; /* consume '>' */

        uint32_t content_start = j;
        while (j < len) {
            if (html[j] == '<' && j + 8 <= len) {
                int close_match = 1;
                const char *close = "</style>";
                for (int k = 0; k < 8; k++) {
                    if (to_lower_ch(html[j + (uint32_t)k]) != close[k]) { close_match = 0; break; }
                }
                if (close_match) break;
            }
            j++;
        }
        css_parse_into(sheet, html + content_start, j - content_start);
        i = j;
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct named_color { const char *name; uint32_t rgb; };
static const struct named_color named_colors[] = {
    {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xE05252}, {"green", 0x2FBF71},
    {"blue", 0x3E6FF0}, {"yellow", 0xF2C14E}, {"gray", 0x8A8FA8}, {"grey", 0x8A8FA8},
    {"silver", 0xC0C0C0}, {"orange", 0xE0954C}, {"cyan", 0x62D8FF}, {"magenta", 0xE05CE0},
    {"purple", 0xB05CE0}, {"navy", 0x1B2040}, {"teal", 0x3ED0D8}, {"lime", 0x8FE3A8},
    {"maroon", 0x7A1F1F}, {"olive", 0x7A7A1F}, {"pink", 0xF0A8C8}, {"brown", 0x8A5A3A},
};

static uint32_t parse_color(const char *value, int *has_color_out) {
    *has_color_out = 1;
    if (value[0] == '#') {
        const char *h = value + 1;
        int hl = (int)strlen(h);
        if (hl >= 6) {
            int r = hex_val(h[0]) * 16 + hex_val(h[1]);
            int g = hex_val(h[2]) * 16 + hex_val(h[3]);
            int b = hex_val(h[4]) * 16 + hex_val(h[5]);
            if (r >= 0 && g >= 0 && b >= 0) return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        } else if (hl >= 3) {
            int r = hex_val(h[0]), g = hex_val(h[1]), b = hex_val(h[2]);
            if (r >= 0 && g >= 0 && b >= 0) return ((uint32_t)(r * 17) << 16) | ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
        }
        *has_color_out = 0;
        return 0;
    }
    if (strncmp(value, "rgb", 3) == 0) {
        const char *p = strchr(value, '(');
        if (p) {
            p++;
            int comp[3] = {0, 0, 0};
            for (int c = 0; c < 3; c++) {
                while (*p == ' ') p++;
                int v = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                comp[c] = v;
                while (*p == ' ' || *p == ',') p++;
            }
            return ((uint32_t)comp[0] << 16) | ((uint32_t)comp[1] << 8) | (uint32_t)comp[2];
        }
    }
    if (strcmp(value, "transparent") == 0 || strcmp(value, "none") == 0 || strcmp(value, "inherit") == 0) {
        *has_color_out = 0;
        return 0;
    }
    for (unsigned t = 0; t < sizeof(named_colors) / sizeof(named_colors[0]); t++) {
        if (strcmp(value, named_colors[t].name) == 0) return named_colors[t].rgb;
    }
    *has_color_out = 0;
    return 0;
}

/* Splits `s` on top-level commas only (depth tracked through nested
 * parens, so a color stop like "rgb(0, 0, 0)" doesn't get sliced in
 * the middle) into up to `max_tokens` trimmed tokens. Used to pull the
 * optional direction and however many color stops out of a
 * linear-gradient()'s argument list without a real CSS value tokenizer. */
static int split_top_level(const char *s, char tokens[][40], int max_tokens) {
    int count = 0;
    int depth = 0;
    int start = 0;
    char buf[200];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    int len = (int)strlen(buf);
    for (int i = 0; i <= len && count < max_tokens; i++) {
        char c = buf[i];
        if (c == '(') depth++;
        else if (c == ')') depth--;
        if ((c == ',' && depth == 0) || c == 0) {
            int tl = i - start;
            if (tl > 39) tl = 39;
            if (tl < 0) tl = 0;
            memcpy(tokens[count], buf + start, (size_t)tl);
            tokens[count][tl] = 0;
            trim(tokens[count]);
            count++;
            start = i + 1;
        }
    }
    return count;
}

/* Cuts a trailing " <percentage-or-length>" position off a gradient
 * color-stop token (e.g. "red 20%" -> "red", "#fff 0%" -> "#fff") --
 * but not off an rgb()/rgba() stop, whose own internal spaces (after
 * each comma) never start at index 0, so nothing there gets cut. */
static void strip_trailing_position(char *s) {
    if (strncmp(s, "rgb", 3) == 0) return;
    char *sp = strchr(s, ' ');
    if (sp) *sp = 0;
}

/* Parses a `linear-gradient(...)` function's argument list: an
 * optional leading direction (`to <side>...` or `Ndeg`) followed by
 * two or more comma-separated color stops. Only the first and last
 * stop, and a horizontal-vs-vertical axis for the direction (any
 * diagonal angle rounds to whichever it's closer to), survive -- see
 * struct css_computed's comment for why. Returns 0 if `value` isn't a
 * linear-gradient() at all, or neither color stop parses. */
static int parse_linear_gradient(const char *value, uint32_t *c1, uint32_t *c2, int *horizontal) {
    char *start = find_substr((char *)value, "linear-gradient(");
    if (!start) return 0;
    char *open = start + 16; /* "linear-gradient(" is 16 chars, already past the '(' */

    char *close = open;
    int depth = 1;
    while (*close && depth > 0) {
        if (*close == '(') depth++;
        else if (*close == ')') { depth--; if (depth == 0) break; }
        close++;
    }

    char inner[200];
    int ilen = (int)(close - open);
    if (ilen > (int)sizeof(inner) - 1) ilen = (int)sizeof(inner) - 1;
    if (ilen < 0) ilen = 0;
    memcpy(inner, open, (size_t)ilen);
    inner[ilen] = 0;

    char tokens[8][40];
    int n = split_top_level(inner, tokens, 8);
    if (n < 2) return 0;

    int has_direction = (strncmp(tokens[0], "to ", 3) == 0) || find_substr(tokens[0], "deg") != NULL;
    int color_start = has_direction ? 1 : 0;
    *horizontal = 0;
    if (has_direction) {
        if (find_substr(tokens[0], "right") || find_substr(tokens[0], "left")) *horizontal = 1;
        char *deg = find_substr(tokens[0], "deg");
        if (deg) {
            int neg = 0, val = 0;
            char *q = tokens[0];
            if (*q == '-') { neg = 1; q++; }
            while (*q >= '0' && *q <= '9') { val = val * 10 + (*q - '0'); q++; }
            if (neg) val = -val;
            int mod = ((val % 360) + 360) % 360;
            if ((mod > 45 && mod < 135) || (mod > 225 && mod < 315)) *horizontal = 1;
        }
    }
    if (n - color_start < 1) return 0;

    strip_trailing_position(tokens[color_start]);
    strip_trailing_position(tokens[n - 1]);
    int has1, has2;
    *c1 = parse_color(tokens[color_start], &has1);
    *c2 = parse_color(tokens[n - 1], &has2);
    return has1 && has2;
}

static int parse_px(const char *value) {
    int neg = 0;
    const char *p = value;
    if (*p == '-') { neg = 1; p++; }
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return neg ? -v : v;
}

/* Global custom-property table -- see css_resolve_custom_properties()
 * in css.h for why this is document-wide rather than per-element. */
#define CSS_MAX_CUSTOM_PROPS 128
#define CSS_CUSTOM_NAME_LEN  40
#define CSS_CUSTOM_VALUE_LEN 160

struct css_custom_prop { char name[CSS_CUSTOM_NAME_LEN]; char value[CSS_CUSTOM_VALUE_LEN]; };
static struct css_custom_prop custom_props[CSS_MAX_CUSTOM_PROPS];
static int custom_prop_count = 0;

static void custom_add_or_update(const char *name, const char *value) {
    for (int i = 0; i < custom_prop_count; i++) {
        if (strcmp(custom_props[i].name, name) == 0) {
            strncpy(custom_props[i].value, value, CSS_CUSTOM_VALUE_LEN - 1);
            custom_props[i].value[CSS_CUSTOM_VALUE_LEN - 1] = 0;
            return;
        }
    }
    if (custom_prop_count >= CSS_MAX_CUSTOM_PROPS) return;
    struct css_custom_prop *p = &custom_props[custom_prop_count++];
    strncpy(p->name, name, CSS_CUSTOM_NAME_LEN - 1);
    p->name[CSS_CUSTOM_NAME_LEN - 1] = 0;
    strncpy(p->value, value, CSS_CUSTOM_VALUE_LEN - 1);
    p->value[CSS_CUSTOM_VALUE_LEN - 1] = 0;
}

static int custom_lookup(const char *name, char *out, int out_cap) {
    for (int i = 0; i < custom_prop_count; i++) {
        if (strcmp(custom_props[i].name, name) == 0) {
            strncpy(out, custom_props[i].value, (size_t)out_cap - 1);
            out[out_cap - 1] = 0;
            return 1;
        }
    }
    return 0;
}

/* Replaces the first "var(--name)" or "var(--name, fallback)" found in
 * `buf` with its resolved value (or the literal fallback if the name
 * isn't in the table, or empty text if there's neither) and reports
 * whether it found one to replace at all -- called in a bounded loop
 * by css_resolve_var_refs() below so one level of var-inside-var
 * indirection still resolves without ever looping forever on a
 * reference this table can't satisfy. */
static void resolve_one_var(char *buf, int cap, int *found) {
    char *start = find_substr(buf, "var(");
    if (!start) { *found = 0; return; }
    *found = 1;

    char *inner = start + 4;
    int depth = 1;
    char *p = inner;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') { depth--; if (depth == 0) break; }
        p++;
    }
    char *close = p; /* the matching ')', or the trailing NUL if malformed */

    char inner_buf[CSS_CUSTOM_VALUE_LEN];
    int ilen = (int)(close - inner);
    if (ilen < 0) ilen = 0;
    if (ilen > (int)sizeof(inner_buf) - 1) ilen = (int)sizeof(inner_buf) - 1;
    memcpy(inner_buf, inner, (size_t)ilen);
    inner_buf[ilen] = 0;

    char *comma = strchr(inner_buf, ',');
    char *name = inner_buf;
    char *fallback = NULL;
    if (comma) { *comma = 0; fallback = comma + 1; trim(fallback); }
    trim(name);
    for (char *q = name; *q; q++) *q = to_lower_ch(*q);

    char resolved[CSS_CUSTOM_VALUE_LEN];
    if (!custom_lookup(name, resolved, sizeof(resolved))) {
        if (fallback) { strncpy(resolved, fallback, sizeof(resolved) - 1); resolved[sizeof(resolved) - 1] = 0; }
        else resolved[0] = 0;
    }

    char tmp[256];
    int tp = 0;
    for (const char *q = buf; q < start && tp < (int)sizeof(tmp) - 1; q++) tmp[tp++] = *q;
    for (const char *q = resolved; *q && tp < (int)sizeof(tmp) - 1; q++) tmp[tp++] = *q;
    const char *after = (*close == ')') ? close + 1 : close;
    for (const char *q = after; *q && tp < (int)sizeof(tmp) - 1; q++) tmp[tp++] = *q;
    tmp[tp] = 0;

    strncpy(buf, tmp, (size_t)cap - 1);
    buf[cap - 1] = 0;
}

static void css_resolve_var_refs(char *buf, int cap) {
    for (int iter = 0; iter < 6; iter++) {
        int found;
        resolve_one_var(buf, cap, &found);
        if (!found) break;
    }
}

static int is_global_selector(const char *sel) {
    return strcmp(sel, "*") == 0 || strcmp(sel, ":root") == 0 || strcmp(sel, ":host") == 0 ||
           strcmp(sel, "html") == 0 || strcmp(sel, "body") == 0;
}

void css_resolve_custom_properties(const struct css_stylesheet *sheet) {
    custom_prop_count = 0;
    for (const struct css_rule *rule = sheet->head; rule; rule = rule->next) {
        int is_global = 0;
        for (int g = 0; g < rule->selector_count; g++) {
            if (is_global_selector(rule->selectors[g])) { is_global = 1; break; }
        }
        if (!is_global) continue;
        for (int d = 0; d < rule->decl_count; d++) {
            const char *prop = rule->decls[d].prop;
            if (prop[0] == '-' && prop[1] == '-') custom_add_or_update(prop, rule->decls[d].value);
        }
    }
    /* One extra pass so a custom property whose own value references
     * another one (e.g. --font-mono: var(--font-geist-mono), ...)
     * resolves too, as far as this table can take it. */
    for (int i = 0; i < custom_prop_count; i++) {
        css_resolve_var_refs(custom_props[i].value, CSS_CUSTOM_VALUE_LEN);
    }
}

static void apply_decl(struct css_computed *out, const char *prop, const char *raw_value) {
    char value[CSS_VALUE_LEN];
    strncpy(value, raw_value, CSS_VALUE_LEN - 1);
    value[CSS_VALUE_LEN - 1] = 0;
    if (find_substr(value, "var(")) css_resolve_var_refs(value, CSS_VALUE_LEN);

    if (strcmp(prop, "color") == 0) {
        int has; uint32_t c = parse_color(value, &has);
        if (has) out->color = c;
    } else if (strcmp(prop, "background-color") == 0 || strcmp(prop, "background") == 0) {
        uint32_t g1, g2; int horiz;
        if (find_substr(value, "linear-gradient(") && parse_linear_gradient(value, &g1, &g2, &horiz)) {
            out->has_background = 1;
            out->background_color = g1;
            out->has_gradient = 1;
            out->gradient_color2 = g2;
            out->gradient_horizontal = horiz;
        } else {
            int has; uint32_t c = parse_color(value, &has);
            out->has_background = has;
            if (has) out->background_color = c;
            out->has_gradient = 0;
        }
    } else if (strcmp(prop, "border-radius") == 0) {
        /* Tailwind's "rounded-full" utility (real-world usage for a
         * pill-shaped button/badge, exactly the common case this is
         * worth handling) emits scientific notation --
         * "border-radius:3.40282e38px", float's max value, to
         * guarantee a full capsule regardless of element size --
         * which parse_px() can't read (it stops at the first non-
         * digit, so this would otherwise silently parse as just "3").
         * fb_fill_rounded_rect() already clamps to min(w,h)/2, so any
         * suitably large sentinel here produces the same true capsule
         * shape a real browser would draw. */
        if (strchr(value, 'e') || strchr(value, 'E')) out->border_radius = 999999;
        else out->border_radius = parse_px(value);
    } else if (strcmp(prop, "font-weight") == 0) {
        out->bold = (strcmp(value, "bold") == 0 || strcmp(value, "bolder") == 0 || parse_px(value) >= 700);
    } else if (strcmp(prop, "display") == 0) {
        if (strcmp(value, "none") == 0) out->display = CSS_DISPLAY_NONE;
        else if (strcmp(value, "inline") == 0) out->display = CSS_DISPLAY_INLINE;
        else if (strcmp(value, "flex") == 0 || strcmp(value, "inline-flex") == 0) out->display = CSS_DISPLAY_FLEX;
        else out->display = CSS_DISPLAY_BLOCK;
    } else if (strcmp(prop, "flex-direction") == 0) {
        out->flex_row = strncmp(value, "column", 6) != 0;
    } else if (strcmp(prop, "gap") == 0 || strcmp(prop, "column-gap") == 0) {
        out->gap = parse_px(value);
    } else if (strcmp(prop, "justify-content") == 0) {
        if (strcmp(value, "center") == 0) out->justify = CSS_JUSTIFY_CENTER;
        else if (strcmp(value, "flex-end") == 0 || strcmp(value, "end") == 0) out->justify = CSS_JUSTIFY_END;
        else if (strcmp(value, "space-between") == 0 || strcmp(value, "space-around") == 0 ||
                 strcmp(value, "space-evenly") == 0) out->justify = CSS_JUSTIFY_BETWEEN;
        else out->justify = CSS_JUSTIFY_START;
    } else if (strcmp(prop, "margin") == 0) {
        out->margin_top = out->margin_bottom = parse_px(value);
    } else if (strcmp(prop, "margin-top") == 0) {
        out->margin_top = parse_px(value);
    } else if (strcmp(prop, "margin-bottom") == 0) {
        out->margin_bottom = parse_px(value);
    } else if (strcmp(prop, "padding") == 0) {
        out->padding_top = out->padding_bottom = out->padding_left = parse_px(value);
    } else if (strcmp(prop, "padding-top") == 0) {
        out->padding_top = parse_px(value);
    } else if (strcmp(prop, "padding-bottom") == 0) {
        out->padding_bottom = parse_px(value);
    } else if (strcmp(prop, "padding-left") == 0) {
        out->padding_left = parse_px(value);
    } else if (strcmp(prop, "width") == 0) {
        if (strcmp(value, "auto") != 0) out->width = parse_px(value);
    } else if (strcmp(prop, "height") == 0) {
        if (strcmp(value, "auto") != 0) out->height = parse_px(value);
    } else if (strcmp(prop, "float") == 0) {
        if (strcmp(value, "left") == 0) out->cssfloat = CSS_FLOAT_LEFT;
        else if (strcmp(value, "right") == 0) out->cssfloat = CSS_FLOAT_RIGHT;
        else out->cssfloat = CSS_FLOAT_NONE;
    }
}

/* Does `sel` (already lowercased/simplified) match `node`? Supports a
 * bare tag, "*", ".class" (checks each space-separated class token on
 * the node), "#id", and "tag.class". */
static int selector_matches(const char *sel, const struct dom_node *node) {
    if (strcmp(sel, "*") == 0) return 1;

    if (sel[0] == '#') return node->id[0] && strcmp(node->id, sel + 1) == 0;

    const char *class_part = NULL;
    char tag_part[CSS_MAX_SELECTOR];
    if (sel[0] == '.') {
        class_part = sel + 1;
        tag_part[0] = 0;
    } else {
        const char *dot = strchr(sel, '.');
        if (dot) {
            int tl = (int)(dot - sel);
            if (tl > (int)sizeof(tag_part) - 1) tl = (int)sizeof(tag_part) - 1;
            memcpy(tag_part, sel, (size_t)tl);
            tag_part[tl] = 0;
            class_part = dot + 1;
        } else {
            strcpy(tag_part, sel);
        }
    }

    if (tag_part[0] && strcmp(tag_part, node->tag) != 0) return 0;

    if (class_part && class_part[0]) {
        const char *p = node->class_name;
        int clen = (int)strlen(class_part);
        while (*p) {
            while (*p == ' ') p++;
            const char *start = p;
            while (*p && *p != ' ') p++;
            if ((int)(p - start) == clen && memcmp(start, class_part, (size_t)clen) == 0) return 1;
        }
        return 0;
    }

    return 1;
}

void css_compute_style(const struct dom_node *node, const struct css_stylesheet *sheet,
                        const struct css_computed *parent, struct css_computed *out) {
    memset(out, 0, sizeof(*out));
    out->color = parent ? parent->color : 0xF2F4FF;
    out->bold = parent ? parent->bold : 0;
    out->display = CSS_DISPLAY_INLINE;
    out->has_background = 0;
    out->width = -1;
    out->height = -1;
    out->cssfloat = CSS_FLOAT_NONE;
    out->flex_row = 1;
    out->gap = 0;
    out->justify = CSS_JUSTIFY_START;

    for (const struct css_rule *rule = sheet->head; rule; rule = rule->next) {
        int matched = 0;
        for (int g = 0; g < rule->selector_count; g++) {
            if (selector_matches(rule->selectors[g], node)) { matched = 1; break; }
        }
        if (!matched) continue;
        for (int d = 0; d < rule->decl_count; d++) {
            apply_decl(out, rule->decls[d].prop, rule->decls[d].value);
        }
    }

    if (node->style[0]) {
        /* Reuse the declaration-list splitter by wrapping the inline
         * style text as a one-off, throwaway "rule" body. */
        char buf[DOM_MAX_STYLE];
        strncpy(buf, node->style, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        int len = (int)strlen(buf);
        int start = 0;
        for (int p = 0; p <= len; p++) {
            if (p == len || buf[p] == ';') {
                char decl[80];
                int tl = p - start;
                if (tl > (int)sizeof(decl) - 1) tl = (int)sizeof(decl) - 1;
                if (tl > 0) {
                    memcpy(decl, buf + start, (size_t)tl);
                    decl[tl] = 0;
                    char *colon = strchr(decl, ':');
                    if (colon) {
                        *colon = 0;
                        char *prop = decl, *value = colon + 1;
                        trim(prop); trim(value);
                        for (char *q = prop; *q; q++) *q = to_lower_ch(*q);
                        if (prop[0] && value[0]) apply_decl(out, prop, value);
                    }
                }
                start = p + 1;
            }
        }
    }
}
