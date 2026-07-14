#include <js/dom_binding.h>
#include <js/lexer.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <net/wasm.h>
#include <net/websocket.h>
#include <string.h>

#define ONCLICK_MAX 32
#define TEXT_CONTENT_MAX 512
/* Message payloads are already capped at WS_MAX_MESSAGE bytes coming
 * out of net/websocket.c's ws_poll() -- this is just the JS-visible
 * event object's own copy of that same bound, plus one byte for the
 * NUL terminator a JS string needs that the raw ws_poll() buffer
 * doesn't (see the ws.h scope note re: binary frames/embedded NULs). */
#define WS_JS_MSG_MAX WS_MAX_MESSAGE

/* WebAssembly.instantiate(url) needs raw bytes from somewhere, but this
 * engine has no fetch()/ArrayBuffer to carry them in JS -- so instead
 * of a byte source, the browser (which already knows how to resolve a
 * relative URL against the current page and fetch it) registers a
 * callback here. `*out_data` is kmalloc'd; caller (native_wasm_instantiate)
 * frees it once wasm_parse_module() has copied what it needs. */
static js_binary_fetch_fn g_binary_fetcher = NULL;
void js_set_binary_fetcher(js_binary_fetch_fn fn) { g_binary_fetcher = fn; }

/* WASM modules/instances are kmalloc'd (their own allocator, per
 * net/wasm.h), not arena-allocated, so they need explicit cleanup --
 * same reasoning as onclick_table below, and cleared alongside it in
 * js_dom_reset() since both are per-page state that outlives nothing
 * past a navigation. */
#define WASM_INSTANCE_MAX 4
static struct wasm_module wasm_modules[WASM_INSTANCE_MAX];
static struct wasm_instance wasm_instances[WASM_INSTANCE_MAX];
static int wasm_slot_used[WASM_INSTANCE_MAX];

struct js_wasm_export_binding {
    struct wasm_instance *inst;
    char name[WASM_MAX_NAME_LEN];
    int has_result;
};

/* new WebSocket(url) (see native_websocket_construct below) hands back
 * a plain JS_OBJ_PLAIN object -- .send/.close are just native-function
 * properties on it, and .onopen/.onmessage/.onclose are just ordinary
 * data properties (js_get_prop/js_set_prop already handles those with
 * no DOM-style special-casing needed, since this isn't a
 * JS_OBJ_DOM_ELEMENT). What DOES need a side reference, the same reason
 * onclick_table exists below: net/websocket.c's underlying connection
 * is a singleton with no JS-visible identity of its own, so something
 * has to remember *which* JS object's handlers to call when a frame
 * arrives -- there's only ever one live connection (see
 * include/net/websocket.h), so a single pointer suffices where onclick
 * needed a whole table keyed by DOM node.
 *
 * g_ws_registered is distinct from ws_is_open(): it also covers the
 * one-poll window where onopen still needs firing (see
 * js_dom_ws_poll()) even though the connection is already open, and it
 * drops back to 0 the moment nothing more should ever be dispatched to
 * g_ws_object again (close seen, or the page navigated away). */
static struct js_object *g_ws_object = NULL;
static int g_ws_registered = 0;
static int g_ws_open_pending = 0;

static struct dom_node *g_document_root = NULL;
static int g_needs_relayout = 0;

/* console.log normally goes straight to the serial log -- the only
 * "console" that exists when a page's script is the caller. The
 * terminal's "js <file>" command (gui/shell.c) wants that same
 * console.log to land in its own scrollback instead, so it points this
 * at a sink before running a script and clears it back to NULL (serial)
 * afterward. Never both at once: browser onclick handlers and a
 * terminal-run script both execute synchronously within the single GUI
 * task, never concurrently. */
static void (*g_console_sink)(const char *) = NULL;

void js_set_console_sink(void (*sink)(const char *)) {
    g_console_sink = sink;
}

static struct {
    struct dom_node *node;
    js_value handler;
} onclick_table[ONCLICK_MAX];
static int onclick_count = 0;

void js_dom_reset(void) {
    onclick_count = 0;
    g_needs_relayout = 0;
    g_document_root = NULL;
    for (int i = 0; i < WASM_INSTANCE_MAX; i++) {
        if (wasm_slot_used[i]) {
            wasm_free_instance(&wasm_instances[i]);
            wasm_free_module(&wasm_modules[i]);
            wasm_slot_used[i] = 0;
        }
    }
    /* A live WebSocket connection belongs to the page that opened it --
     * same lifecycle rule as onclick_table/WASM instances above, just
     * for a resource net/websocket.c owns instead of the JS arena.
     * Closing it here (rather than leaving it for the next
     * js_dom_ws_poll() to notice) matters because g_ws_object is about
     * to become a dangling pointer into an arena that js_arena_reset()
     * (called by the same navigation right alongside this) is about to
     * throw away wholesale. */
    if (ws_is_open()) ws_close();
    g_ws_object = NULL;
    g_ws_registered = 0;
    g_ws_open_pending = 0;
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

static js_value native_get_element_by_id(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)fn_obj;
    if (argc < 1 || !g_document_root) return js_null_value();
    struct dom_node *found = find_by_id(g_document_root, js_to_string(args[0]));
    return found ? js_wrap_dom_node(found) : js_null_value();
}

static js_value native_console_log(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)fn_obj;
    void (*out)(const char *) = g_console_sink ? g_console_sink : serial_write;
    for (int i = 0; i < argc; i++) {
        out(i > 0 ? " " : "");
        out(js_to_string(args[i]));
    }
    out("\n");
    return js_undefined();
}

static js_value native_math_abs(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)fn_obj;
    int32_t n = argc > 0 ? js_to_num(args[0]) : 0;
    return js_make_num(n < 0 ? -n : n);
}
static js_value native_math_max(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)fn_obj;
    int32_t best = argc > 0 ? js_to_num(args[0]) : 0;
    for (int i = 1; i < argc; i++) { int32_t n = js_to_num(args[i]); if (n > best) best = n; }
    return js_make_num(best);
}
static js_value native_math_min(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)fn_obj;
    int32_t best = argc > 0 ? js_to_num(args[0]) : 0;
    for (int i = 1; i < argc; i++) { int32_t n = js_to_num(args[i]); if (n < best) best = n; }
    return js_make_num(best);
}
static js_value native_math_floor(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)fn_obj;
    /* Numbers are already integers in this engine -- see js.h. */
    return js_make_num(argc > 0 ? js_to_num(args[0]) : 0);
}

static struct js_object *make_native(js_native_fn fn) {
    struct js_object *obj = js_new_object(JS_OBJ_NATIVE);
    obj->native_fn = fn;
    return obj;
}

/* The actual callable wrapper for one WASM export -- native_data (set
 * by native_wasm_instantiate below) says which instance and which
 * export name this particular native function object is bound to,
 * since js_native_fn has no other way to carry per-object context. */
static js_value native_wasm_export_call(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val;
    struct js_wasm_export_binding *binding = (struct js_wasm_export_binding *)fn_obj->native_data;
    int32_t wargs[WASM_MAX_PARAMS];
    int wargc = argc > WASM_MAX_PARAMS ? WASM_MAX_PARAMS : argc;
    for (int i = 0; i < wargc; i++) wargs[i] = js_to_num(args[i]);
    int32_t result = 0;
    if (!wasm_call_export(binding->inst, binding->name, wargs, wargc, &result)) {
        serial_printf("js: WebAssembly call to '%s' trapped\n", binding->name);
        return js_undefined();
    }
    return binding->has_result ? js_make_num(result) : js_undefined();
}

/* Non-standard shape, forced by what this engine actually has: real
 * WebAssembly.instantiate() takes an ArrayBuffer/Promise and returns a
 * Promise -- this engine has neither, so it takes a URL string
 * (resolved against the current page by whatever js_set_binary_fetcher()
 * registered) and returns the resolved `{instance: {exports: {...}}}`
 * object directly and synchronously. Every exported function becomes a
 * real callable JS native wrapping wasm_call_export(). */
static js_value native_wasm_instantiate(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)fn_obj;
    if (argc < 1 || args[0].type != JS_STR) {
        serial_printf("js: WebAssembly.instantiate needs a URL string (no fetch()/ArrayBuffer in this engine)\n");
        return js_undefined();
    }
    if (!g_binary_fetcher) {
        serial_printf("js: WebAssembly.instantiate: no fetcher registered\n");
        return js_undefined();
    }

    int slot = -1;
    for (int i = 0; i < WASM_INSTANCE_MAX; i++) {
        if (!wasm_slot_used[i]) { slot = i; break; }
    }
    if (slot < 0) {
        serial_printf("js: WebAssembly: too many live instances for this page (max %d)\n", WASM_INSTANCE_MAX);
        return js_undefined();
    }

    const char *url = js_to_string(args[0]);
    uint8_t *data = NULL;
    uint32_t len = 0;
    if (!g_binary_fetcher(url, &data, &len)) {
        serial_printf("js: WebAssembly.instantiate: fetch failed for %s\n", url);
        return js_undefined();
    }

    struct wasm_module *mod = &wasm_modules[slot];
    int parsed = wasm_parse_module(data, len, mod);
    kfree(data);
    if (!parsed) {
        serial_printf("js: WebAssembly.instantiate: module parse failed for %s\n", url);
        return js_undefined();
    }

    struct wasm_instance *inst = &wasm_instances[slot];
    if (!wasm_instantiate(mod, inst)) {
        wasm_free_module(mod);
        serial_printf("js: WebAssembly.instantiate: instantiation failed for %s\n", url);
        return js_undefined();
    }
    wasm_slot_used[slot] = 1;

    struct js_object *exports = js_new_object(JS_OBJ_PLAIN);
    for (uint32_t i = 0; i < mod->export_count; i++) {
        struct wasm_export *exp = &mod->exports[i];
        if (exp->kind != 0) continue; /* function exports only -- nothing to call for table/memory/global */

        struct js_wasm_export_binding *binding =
            (struct js_wasm_export_binding *)js_alloc(sizeof(struct js_wasm_export_binding));
        binding->inst = inst;
        strncpy(binding->name, exp->name, sizeof(binding->name) - 1);
        binding->name[sizeof(binding->name) - 1] = 0;
        binding->has_result = mod->types[mod->funcs[exp->index].type_idx].result_count > 0;

        struct js_object *fn = js_new_object(JS_OBJ_NATIVE);
        fn->native_fn = native_wasm_export_call;
        fn->native_data = binding;
        js_set_prop(exports, exp->name, js_make_object(fn));
    }

    struct js_object *instance_obj = js_new_object(JS_OBJ_PLAIN);
    js_set_prop(instance_obj, "exports", js_make_object(exports));

    struct js_object *result = js_new_object(JS_OBJ_PLAIN);
    js_set_prop(result, "instance", js_make_object(instance_obj));
    return js_make_object(result);
}

/* WebSocket.readyState numeric values -- same 4 values/meanings as the
 * real API, even though this engine has no async connect (see below)
 * to ever actually observe CONNECTING from JS. */
enum { WS_STATE_CONNECTING = 0, WS_STATE_OPEN = 1, WS_STATE_CLOSING = 2, WS_STATE_CLOSED = 3 };

/* ws.send(str) -- ignores `this_val` and just operates on
 * net/websocket.c's one global connection, consistent with that file's
 * own "one connection at a time" scope; there is at most one
 * JS-visible WebSocket object alive anyway (see g_ws_object above). */
static js_value native_ws_send(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)fn_obj;
    if (argc < 1) return js_undefined();
    if (!ws_is_open()) {
        serial_printf("js: WebSocket.send() called while not open -- dropped\n");
        return js_undefined();
    }
    if (!ws_send_text(js_to_string(args[0]))) {
        serial_printf("js: WebSocket.send() failed (TCP send error)\n");
    }
    return js_undefined();
}

/* ws.close() -- fires onclose synchronously, right here, rather than
 * waiting for the next js_dom_ws_poll(): a locally-initiated close
 * happens inline during JS execution (same as an onclick handler
 * running inline), it isn't something that arrived asynchronously off
 * the network for the per-frame poll to notice. Real WebSocket fires
 * onclose asynchronously even for a local close; this engine has no
 * event loop to defer it into, so "synchronously, right where the
 * script called close()" is the honest equivalent. */
static js_value native_ws_close(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)args; (void)argc; (void)fn_obj;
    if (ws_is_open()) {
        ws_close();
        if (g_ws_object) {
            js_set_prop(g_ws_object, "readyState", js_make_num(WS_STATE_CLOSED));
            js_value handler = js_get_prop(g_ws_object, "onclose");
            if (handler.type == JS_OBJ) js_call(handler, js_make_object(g_ws_object), NULL, 0);
        }
    }
    g_ws_registered = 0;
    return js_undefined();
}

/* Non-standard shape, forced by what this engine actually has -- same
 * spirit as native_wasm_instantiate() above. Real WebSocket connects
 * asynchronously (the constructor returns immediately, before any
 * network I/O has happened, and onopen fires later); this engine has
 * no event loop to run that connection attempt on in the background,
 * so ws_connect() below runs it synchronously and to completion before
 * `new WebSocket(url)` ever returns to the calling script -- by which
 * point the handshake has already succeeded or failed. onopen still
 * fires on the *next* js_dom_ws_poll() rather than from inside here,
 * though, because the script's very next line (`ws.onopen = ...`)
 * hasn't run yet at this point -- see g_ws_open_pending. */
static js_value native_websocket_construct(js_value this_val, js_value *args, int argc, struct js_object *fn_obj) {
    (void)this_val; (void)fn_obj;
    if (argc < 1 || args[0].type != JS_STR) {
        serial_printf("js: WebSocket requires a URL string argument\n");
        return js_undefined();
    }
    const char *url = js_to_string(args[0]);

    struct js_object *obj = js_new_object(JS_OBJ_PLAIN);
    js_value obj_val = js_make_object(obj);
    js_set_prop(obj, "url", js_make_str(js_strdup(url)));
    js_set_prop(obj, "onopen", js_undefined());
    js_set_prop(obj, "onmessage", js_undefined());
    js_set_prop(obj, "onclose", js_undefined());
    js_set_prop(obj, "send", js_make_object(make_native(native_ws_send)));
    js_set_prop(obj, "close", js_make_object(make_native(native_ws_close)));

    g_ws_object = obj;
    g_ws_registered = 0;
    g_ws_open_pending = 0;

    if (ws_connect(url)) {
        js_set_prop(obj, "readyState", js_make_num(WS_STATE_OPEN));
        g_ws_registered = 1;
        g_ws_open_pending = 1; /* fire onopen on the first js_dom_ws_poll() after this */
    } else {
        js_set_prop(obj, "readyState", js_make_num(WS_STATE_CLOSED));
        serial_printf("js: new WebSocket(%s) failed to connect\n", url);
    }
    return obj_val;
}

/* Called once per gui_run() frame (see gui/compositor.c) -- the exact
 * same timing model js_dom_dispatch_click() already uses for onclick,
 * just triggered by "did a WebSocket frame arrive" instead of "did a
 * mouse click happen." A no-op whenever there's no registered
 * connection, so the caller doesn't need to gate the call itself. */
void js_dom_ws_poll(void) {
    if (!g_ws_registered || !g_ws_object) return;

    if (g_ws_open_pending) {
        g_ws_open_pending = 0;
        js_value handler = js_get_prop(g_ws_object, "onopen");
        if (handler.type == JS_OBJ) js_call(handler, js_make_object(g_ws_object), NULL, 0);
    }

    if (!ws_is_open()) { g_ws_registered = 0; return; }

    static char msg_buf[WS_JS_MSG_MAX + 1];
    uint32_t msg_len = 0;
    enum ws_event ev = ws_poll(msg_buf, WS_JS_MSG_MAX, &msg_len);

    if (ev == WS_EVENT_MESSAGE) {
        /* NUL-terminated so it can become a JS string -- see
         * include/net/websocket.h's binary-frame note re: this being
         * lossy if the payload itself contains a NUL byte. There's no
         * primitive in this engine that could carry an exact byte count
         * alongside the bytes instead. */
        msg_buf[msg_len] = 0;
        struct js_object *event = js_new_object(JS_OBJ_PLAIN);
        js_set_prop(event, "data", js_make_str(js_strdup(msg_buf)));
        js_value handler = js_get_prop(g_ws_object, "onmessage");
        if (handler.type == JS_OBJ) {
            js_value event_arg = js_make_object(event);
            js_call(handler, js_make_object(g_ws_object), &event_arg, 1);
        }
    } else if (ev == WS_EVENT_CLOSE) {
        js_set_prop(g_ws_object, "readyState", js_make_num(WS_STATE_CLOSED));
        js_value handler = js_get_prop(g_ws_object, "onclose");
        if (handler.type == JS_OBJ) js_call(handler, js_make_object(g_ws_object), NULL, 0);
        g_ws_registered = 0;
    }
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

    struct js_object *wasm_ns = js_new_object(JS_OBJ_PLAIN);
    js_set_prop(wasm_ns, "instantiate", js_make_object(make_native(native_wasm_instantiate)));
    js_env_declare(env, "WebAssembly", js_make_object(wasm_ns), 0);

    /* A plain JS_OBJ_NATIVE global, not a JS_OBJ_FUNCTION -- js/interp.c's
     * JS_NEW case has a dedicated branch letting `new WebSocket(url)`
     * call this directly (see the comment there); calling it as a plain
     * WebSocket(url), with no `new` at all, works too, since js_call()
     * dispatches JS_OBJ_NATIVE the same way either path ends up. */
    js_env_declare(env, "WebSocket", js_make_object(make_native(native_websocket_construct)), 0);

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
