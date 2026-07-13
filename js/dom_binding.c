#include <js/dom_binding.h>
#include <js/lexer.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

#define ONCLICK_MAX 32
#define TEXT_CONTENT_MAX 512

static struct dom_node *g_document_root = NULL;
static int g_needs_relayout = 0;

static struct {
    struct dom_node *node;
    js_value handler;
} onclick_table[ONCLICK_MAX];
static int onclick_count = 0;

void js_dom_reset(void) {
    onclick_count = 0;
    g_needs_relayout = 0;
    g_document_root = NULL;
}

int js_dom_needs_relayout(void) { return g_needs_relayout; }
void js_dom_clear_relayout_flag(void) { g_needs_relayout = 0; }

void js_dom_register_onclick(struct dom_node *node, js_value handler) {
    for (int i = 0; i < onclick_count; i++) {
        if (onclick_table[i].node == node) { onclick_table[i].handler = handler; return; }
    }
    if (onclick_count < ONCLICK_MAX) {
        onclick_table[onclick_count].node = node;
        onclick_table[onclick_count].handler = handler;
        onclick_count++;
    } else {
        serial_printf("js: onclick handler table full (max %d elements)\n", ONCLICK_MAX);
    }
}

static int js_dom_get_onclick(struct dom_node *node, js_value *out) {
    for (int i = 0; i < onclick_count; i++) {
        if (onclick_table[i].node == node) { *out = onclick_table[i].handler; return 1; }
    }
    return 0;
}

/* Bubbles up from `node` through its ancestors (no capture phase, no
 * stopPropagation -- just fires the first handler found, innermost
 * first) since that covers the common case of a handler on a wrapping
 * <div>/<button> around plain text. */
int js_dom_dispatch_click(struct dom_node *node) {
    for (struct dom_node *n = node; n; n = n->parent) {
        js_value handler;
        if (js_dom_get_onclick(n, &handler)) {
            js_call(handler, js_wrap_dom_node(n), NULL, 0);
            return 1;
        }
    }
    return 0;
}

js_value js_wrap_dom_node(struct dom_node *node) {
    struct js_object *obj = js_new_object(JS_OBJ_DOM_ELEMENT);
    obj->dom_node = node;
    return js_make_object(obj);
}

static struct dom_node *find_by_id(struct dom_node *node, const char *id) {
    for (struct dom_node *c = node->children; c; c = c->next) {
        if (c->type == DOM_ELEMENT) {
            if (strcmp(c->id, id) == 0) return c;
            struct dom_node *found = find_by_id(c, id);
            if (found) return found;
        }
    }
    return NULL;
}

/* Converts a JS style property like "backgroundColor" to CSS's
 * "background-color" (insert '-' before each uppercase letter, then
 * lowercase it) -- covers the common camelCase style-API names without
 * a lookup table. */
static void camel_to_kebab(const char *camel, char *out, int cap) {
    int o = 0;
    for (const char *p = camel; *p && o < cap - 2; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            out[o++] = '-';
            out[o++] = (char)(*p - 'A' + 'a');
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

/* Replaces an existing "prop:value;" pair in node->style, or appends a
 * new one if not present -- keeps the inline style buffer as a normal
 * ';'-separated declaration list the CSS cascade already understands. */
static void dom_style_set(struct dom_node *node, const char *prop, const char *value) {
    char existing[DOM_MAX_STYLE];
    strncpy(existing, node->style, sizeof(existing) - 1);
    existing[sizeof(existing) - 1] = 0;

    char rebuilt[DOM_MAX_STYLE];
    rebuilt[0] = 0;
    int found = 0;
    int len = (int)strlen(existing);
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || existing[i] == ';') {
            if (i > start) {
                char decl[80];
                int dl = i - start;
                if (dl > (int)sizeof(decl) - 1) dl = (int)sizeof(decl) - 1;
                memcpy(decl, existing + start, (size_t)dl);
                decl[dl] = 0;
                char *colon = strchr(decl, ':');
                int is_match = 0;
                if (colon) {
                    char name[40];
                    int nl = (int)(colon - decl);
                    if (nl > (int)sizeof(name) - 1) nl = (int)sizeof(name) - 1;
                    memcpy(name, decl, (size_t)nl);
                    name[nl] = 0;
                    is_match = strcmp(name, prop) == 0;
                }
                if (is_match) {
                    found = 1;
                    strcat(rebuilt, prop);
                    strcat(rebuilt, ":");
                    strcat(rebuilt, value);
                    strcat(rebuilt, ";");
                } else if (strlen(rebuilt) + (size_t)dl + 2 < sizeof(rebuilt)) {
                    strcat(rebuilt, decl);
                    strcat(rebuilt, ";");
                }
            }
            start = i + 1;
        }
    }
    if (!found && strlen(rebuilt) + strlen(prop) + strlen(value) + 3 < sizeof(rebuilt)) {
        strcat(rebuilt, prop);
        strcat(rebuilt, ":");
        strcat(rebuilt, value);
        strcat(rebuilt, ";");
    }
    strncpy(node->style, rebuilt, DOM_MAX_STYLE - 1);
    node->style[DOM_MAX_STYLE - 1] = 0;
}

js_value js_dom_get_prop(struct js_object *obj, const char *name, int *handled) {
    *handled = 1;
    if (obj->kind == JS_OBJ_DOM_ELEMENT) {
        if (strcmp(name, "id") == 0) return js_make_str(js_strdup(obj->dom_node->id));
        if (strcmp(name, "className") == 0) return js_make_str(js_strdup(obj->dom_node->class_name));
        if (strcmp(name, "textContent") == 0 || strcmp(name, "innerText") == 0) {
            char buf[TEXT_CONTENT_MAX];
            dom_text_content(obj->dom_node, buf, sizeof(buf));
            return js_make_str(js_strdup(buf));
        }
        if (strcmp(name, "innerHTML") == 0) return js_make_str(""); /* getter not modeled -- see README */
        if (strcmp(name, "style") == 0) {
            struct js_object *style = js_new_object(JS_OBJ_DOM_STYLE);
            style->dom_node = obj->dom_node;
            return js_make_object(style);
        }
        if (strcmp(name, "onclick") == 0) {
            js_value v;
            if (js_dom_get_onclick(obj->dom_node, &v)) return v;
            return js_undefined();
        }
    }
    *handled = 0;
    return js_undefined();
}

void js_dom_set_prop(struct js_object *obj, const char *name, js_value value, int *handled) {
    *handled = 1;
    if (obj->kind == JS_OBJ_DOM_ELEMENT) {
        if (strcmp(name, "textContent") == 0 || strcmp(name, "innerText") == 0) {
            dom_set_text_content(obj->dom_node, js_to_string(value));
            g_needs_relayout = 1;
            return;
        }
        if (strcmp(name, "innerHTML") == 0) {
            const char *html = js_to_string(value);
            char dummy_title[8];
            struct dom_node *fragment_root = dom_parse(html, (uint32_t)strlen(html), dummy_title, sizeof(dummy_title));
            struct dom_node *new_children = fragment_root->children;
            kfree(fragment_root);
            dom_replace_children(obj->dom_node, new_children);
            g_needs_relayout = 1;
            return;
        }
        if (strcmp(name, "onclick") == 0) {
            js_dom_register_onclick(obj->dom_node, value);
            return;
        }
        *handled = 0;
        return;
    }
    if (obj->kind == JS_OBJ_DOM_STYLE) {
        char kebab[48];
        camel_to_kebab(name, kebab, sizeof(kebab));
        dom_style_set(obj->dom_node, kebab, js_to_string(value));
        g_needs_relayout = 1;
        return;
    }
    *handled = 0;
}

static js_value native_get_element_by_id(js_value this_val, js_value *args, int argc) {
    (void)this_val;
    if (argc < 1 || !g_document_root) return js_null_value();
    struct dom_node *found = find_by_id(g_document_root, js_to_string(args[0]));
    return found ? js_wrap_dom_node(found) : js_null_value();
}

static js_value native_console_log(js_value this_val, js_value *args, int argc) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        serial_write(i > 0 ? " " : "");
        serial_write(js_to_string(args[i]));
    }
    serial_write("\n");
    return js_undefined();
}

static js_value native_math_abs(js_value this_val, js_value *args, int argc) {
    (void)this_val;
    int32_t n = argc > 0 ? js_to_num(args[0]) : 0;
    return js_make_num(n < 0 ? -n : n);
}
static js_value native_math_max(js_value this_val, js_value *args, int argc) {
    (void)this_val;
    int32_t best = argc > 0 ? js_to_num(args[0]) : 0;
    for (int i = 1; i < argc; i++) { int32_t n = js_to_num(args[i]); if (n > best) best = n; }
    return js_make_num(best);
}
static js_value native_math_min(js_value this_val, js_value *args, int argc) {
    (void)this_val;
    int32_t best = argc > 0 ? js_to_num(args[0]) : 0;
    for (int i = 1; i < argc; i++) { int32_t n = js_to_num(args[i]); if (n < best) best = n; }
    return js_make_num(best);
}
static js_value native_math_floor(js_value this_val, js_value *args, int argc) {
    (void)this_val;
    /* Numbers are already integers in this engine -- see js.h. */
    return js_make_num(argc > 0 ? js_to_num(args[0]) : 0);
}

static struct js_object *make_native(js_native_fn fn) {
    struct js_object *obj = js_new_object(JS_OBJ_NATIVE);
    obj->native_fn = fn;
    return obj;
}

struct js_env *js_make_global_env(struct dom_node *document_root) {
    g_document_root = document_root;
    struct js_env *env = js_env_new(NULL);

    struct js_object *document = js_new_object(JS_OBJ_PLAIN);
    js_set_prop(document, "getElementById", js_make_object(make_native(native_get_element_by_id)));
    js_env_declare(env, "document", js_make_object(document), 0);

    struct js_object *console = js_new_object(JS_OBJ_PLAIN);
    js_set_prop(console, "log", js_make_object(make_native(native_console_log)));
    js_env_declare(env, "console", js_make_object(console), 0);

    struct js_object *math = js_new_object(JS_OBJ_PLAIN);
    js_set_prop(math, "abs", js_make_object(make_native(native_math_abs)));
    js_set_prop(math, "max", js_make_object(make_native(native_math_max)));
    js_set_prop(math, "min", js_make_object(make_native(native_math_min)));
    js_set_prop(math, "floor", js_make_object(make_native(native_math_floor)));
    js_env_declare(env, "Math", js_make_object(math), 0);

    return env;
}

static char to_lower_ch(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static int has_src_attr(const char *tag_start, const char *tag_end) {
    for (const char *p = tag_start; p + 4 <= tag_end; p++) {
        if (to_lower_ch(p[0]) == 's' && to_lower_ch(p[1]) == 'r' && to_lower_ch(p[2]) == 'c' && p[3] == '=') return 1;
    }
    return 0;
}

void js_run_inline_scripts(const char *html, uint32_t len, struct js_env *env) {
    for (uint32_t i = 0; i + 7 <= len; i++) {
        if (html[i] != '<') continue;
        int match = 1;
        const char *open = "<script";
        for (int k = 0; k < 7; k++) {
            if (i + (uint32_t)k >= len || to_lower_ch(html[i + (uint32_t)k]) != open[k]) { match = 0; break; }
        }
        if (!match) continue;

        uint32_t tag_start = i + 7;
        uint32_t j = tag_start;
        while (j < len && html[j] != '>') j++;
        if (j >= len) break;
        int external = has_src_attr(html + tag_start, html + j);
        j++; /* consume '>' */

        uint32_t body_start = j;
        while (j < len) {
            if (html[j] == '<' && j + 9 <= len) {
                int close_match = 1;
                const char *close = "</script>";
                for (int k = 0; k < 9; k++) {
                    if (to_lower_ch(html[j + (uint32_t)k]) != close[k]) { close_match = 0; break; }
                }
                if (close_match) break;
            }
            j++;
        }

        if (!external) {
            struct js_lexer lx;
            js_lexer_init(&lx, html + body_start, j - body_start);
            struct js_node *program = js_parse_program(&lx);
            if (program) js_run_program(program, env);
            else serial_printf("js: skipping a script with a parse error\n");
        } else {
            serial_printf("js: skipping external <script src=...> (no script fetching)\n");
        }

        i = j;
    }
}
