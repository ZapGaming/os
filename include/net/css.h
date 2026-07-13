#ifndef NET_CSS_H
#define NET_CSS_H

#include <stdint.h>
#include <net/dom.h>

#define CSS_MAX_SELECTOR 40
#define CSS_MAX_GROUPS   32
#define CSS_MAX_DECLS    12
#define CSS_PROP_LEN     24
#define CSS_VALUE_LEN    56

struct css_decl {
    char prop[CSS_PROP_LEN];
    char value[CSS_VALUE_LEN];
};

/* One `selector, selector { decls } ` rule. Selectors are a pragmatic
 * subset only: a bare tag name, `.class`, `#id`, or `tag.class` -- no
 * descendant/child combinators, no pseudo-classes. */
struct css_rule {
    char selectors[CSS_MAX_GROUPS][CSS_MAX_SELECTOR];
    int selector_count;
    struct css_decl decls[CSS_MAX_DECLS];
    int decl_count;
    struct css_rule *next;
};

struct css_stylesheet {
    struct css_rule *head;
    struct css_rule *tail;
};

enum css_display {
    CSS_DISPLAY_BLOCK,
    CSS_DISPLAY_INLINE,
    CSS_DISPLAY_NONE,
};

enum css_float {
    CSS_FLOAT_NONE,
    CSS_FLOAT_LEFT,
    CSS_FLOAT_RIGHT,
};

/* A resolved (cascaded + inherited) style for one element, ready for
 * the layout engine to consume. */
struct css_computed {
    uint32_t color;
    uint32_t background_color;
    int has_background;
    int bold;
    enum css_display display;
    enum css_float cssfloat;
    int width, height;   /* pixels; -1 means unset/auto */
    int margin_top, margin_bottom;
    int padding_top, padding_bottom, padding_left;
};

void css_stylesheet_init(struct css_stylesheet *sheet);
void css_stylesheet_free(struct css_stylesheet *sheet);

/* Parses `text` (length `len`) as a CSS rule list and appends the
 * parsed rules to `sheet` (kmalloc'd nodes). */
void css_parse_into(struct css_stylesheet *sheet, const char *text, uint32_t len);

/* Scans raw HTML for <style>...</style> blocks and feeds their content
 * to css_parse_into. Independent of dom_parse (which just skips <style>
 * element content entirely when building the tree). */
void css_extract_style_blocks(struct css_stylesheet *sheet, const char *html, uint32_t len);

/* Resolves the cascaded style for `node` given its parent's already-
 * resolved style (NULL for the root). Applies, in order: UA/page rules
 * in `sheet` (later rules win on a per-property basis), then the
 * element's own inline `style=""` attribute (highest priority). */
void css_compute_style(const struct dom_node *node, const struct css_stylesheet *sheet,
                        const struct css_computed *parent, struct css_computed *out);

#endif
