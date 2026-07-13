#ifndef NET_CSS_H
#define NET_CSS_H

#include <stdint.h>
#include <net/dom.h>

#define CSS_MAX_SELECTOR 144
#define CSS_MAX_GROUPS   32
#define CSS_MAX_DECLS    24
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
    CSS_DISPLAY_FLEX,
};

enum css_float {
    CSS_FLOAT_NONE,
    CSS_FLOAT_LEFT,
    CSS_FLOAT_RIGHT,
};

/* Real flexbox has main/cross-axis alignment, wrap, grow/shrink
 * weighting, order, etc. This is the deliberately-simplified subset
 * (see net/layout.c's layout_flex_row): row direction gets its own
 * layout pass, column falls back to normal block stacking (close
 * enough without wrap/grow to matter visually most of the time); no
 * flex-wrap, no flex-grow/shrink weighting beyond "explicit width, or
 * an equal share of whatever's left." */
enum css_justify {
    CSS_JUSTIFY_START,
    CSS_JUSTIFY_CENTER,
    CSS_JUSTIFY_END,
    CSS_JUSTIFY_BETWEEN,
};

/* A resolved (cascaded + inherited) style for one element, ready for
 * the layout engine to consume. */
struct css_computed {
    uint32_t color;
    uint32_t background_color;
    int has_background;
    /* A linear-gradient() background -- only its first/last color stop
     * and a horizontal-vs-vertical axis (any angle rounds to whichever
     * of those two it's closer to) survive; middle stops and the exact
     * angle don't. background_color above holds the first stop. */
    int has_gradient;
    uint32_t gradient_color2;
    int gradient_horizontal;
    int border_radius;   /* pixels; one uniform radius, not four corners */
    int bold;
    enum css_display display;
    enum css_float cssfloat;
    int width, height;   /* pixels; -1 means unset/auto */
    int margin_top, margin_bottom;
    int padding_top, padding_bottom, padding_left;
    int flex_row;        /* 1 = row (default), 0 = column -- only meaningful if display == CSS_DISPLAY_FLEX */
    int gap;
    enum css_justify justify;
};

void css_stylesheet_init(struct css_stylesheet *sheet);
void css_stylesheet_free(struct css_stylesheet *sheet);

/* Scans `sheet` for custom-property declarations (`--name: value`) on
 * "*", ":root", ":host", "html", or "body" rules and populates a
 * global lookup table `var(--name)` references resolve against
 * (module-static, one page's worth at a time -- call this again for
 * every fresh stylesheet, which css_compute_style() implicitly relies
 * on already having been done before it runs). This is NOT real
 * per-element custom-property cascading/inheritance -- it's a single
 * document-wide table, which matches how generated CSS (Tailwind,
 * Next.js, etc.) actually defines theme variables in practice (once,
 * at the root) even though it isn't spec-accurate for a page that
 * overrides a variable deeper in the tree. */
void css_resolve_custom_properties(const struct css_stylesheet *sheet);

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
