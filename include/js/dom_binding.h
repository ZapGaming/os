#ifndef JS_DOM_BINDING_H
#define JS_DOM_BINDING_H

#include <js/js.h>
#include <net/dom.h>

/* Wraps `node` as a JS_OBJ_DOM_ELEMENT value (a fresh wrapper each call
 * -- cheap, since it's just an arena-allocated struct pointing at the
 * same underlying dom_node). */
js_value js_wrap_dom_node(struct dom_node *node);

/* DOM-aware property get/set, consulted by the interpreter before
 * falling back to plain js_get_prop/js_set_prop. Sets *handled to 1 if
 * this call was actually a DOM special case (innerHTML, textContent,
 * style.*, onclick, or document.getElementById) -- 0 means "not mine,
 * use the generic path." Setting textContent/innerHTML/style marks the
 * page dirty (js_dom_needs_relayout()) so the browser knows to redo
 * layout before the next redraw. */
js_value js_dom_get_prop(struct js_object *obj, const char *name, int *handled);
void js_dom_set_prop(struct js_object *obj, const char *name, js_value value, int *handled);

/* Builds the global environment (document/console/Math) for `document_root`.
 * document_root must stay alive for as long as this environment and any
 * values captured from it are in use. */
struct js_env *js_make_global_env(struct dom_node *document_root);

/* Registers `handler` (a JS_OBJ_FUNCTION value) as the onclick for `node` --
 * called when the interpreter sees `element.onclick = fn`. Looked up by
 * js_dom_dispatch_click() when the browser detects a click landed on
 * that element. Returns 1 if a handler was found and invoked. */
void js_dom_register_onclick(struct dom_node *node, js_value handler);
int js_dom_dispatch_click(struct dom_node *node);

/* True if a DOM mutation (innerHTML/textContent/style) happened since
 * the last js_dom_clear_relayout_flag() call. */
int js_dom_needs_relayout(void);
void js_dom_clear_relayout_flag(void);

/* Resets the onclick handler table -- call alongside js_arena_reset()
 * on every navigation, since the handlers (and the arena they live in)
 * are about to become invalid. */
void js_dom_reset(void);

/* Scans raw HTML for <script>...</script> blocks (a `src=` attribute
 * marks an external script -- skipped, no fetching happens) and runs
 * each inline body against `env` in document order. Independent of
 * dom_parse(), which discards <script> content entirely when building
 * the tree -- same pattern as css_extract_style_blocks(). */
void js_run_inline_scripts(const char *html, uint32_t len, struct js_env *env);

/* Redirects console.log's output to `sink` instead of the serial log --
 * used by the terminal's "js <file>" command so a script's console.log
 * lands in its own scrollback. Pass NULL to go back to serial (the
 * default; the browser never needs to call this itself). */
void js_set_console_sink(void (*sink)(const char *));

/* Lets WebAssembly.instantiate(url) (see js/dom_binding.c) actually
 * fetch bytes -- this engine has no fetch()/ArrayBuffer of its own, so
 * the browser (which already knows how to resolve a URL against the
 * current page and fetch it over HTTP) registers this once. `*out_data`
 * must be kmalloc'd; the caller frees it after use. Returns 1 on
 * success, 0 on any fetch/resolve failure. */
typedef int (*js_binary_fetch_fn)(const char *url, uint8_t **out_data, uint32_t *out_len);
void js_set_binary_fetcher(js_binary_fetch_fn fn);

/* Polls net/websocket.c's one connection (if `new WebSocket(url)` --
 * see js/dom_binding.c -- has ever registered one for the current
 * page) for a newly-arrived message or close, and dispatches
 * .onmessage/.onclose synchronously, in-frame -- same timing model as
 * js_dom_dispatch_click() above, just fired by gui/compositor.c's
 * gui_run() once per frame instead of by a mouse click. Also fires
 * .onopen, exactly once, on the first call after a successful
 * ws_connect() (see js/dom_binding.c for why that can't happen any
 * earlier). A no-op when there's no WebSocket for the current page, so
 * the caller doesn't need to gate the call itself. */
void js_dom_ws_poll(void);

#endif
