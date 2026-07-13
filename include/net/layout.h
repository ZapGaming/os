#ifndef NET_LAYOUT_H
#define NET_LAYOUT_H

#include <stdint.h>
#include <net/dom.h>
#include <net/css.h>

#define LAYOUT_MAX_ITEMS 2048
#define LAYOUT_MAX_LINKS 256
#define LAYOUT_TEXT_LEN  40
#define LAYOUT_LINE_H    16
#define LAYOUT_CHAR_W    8

enum layout_item_type {
    LAYOUT_ITEM_TEXT,
    LAYOUT_ITEM_RECT,
    LAYOUT_ITEM_HR,
    LAYOUT_ITEM_IMAGE,
};

/* One positioned, already-styled thing to draw, in document pixel
 * coordinates (y = 0 at the top of the page; the GUI subtracts its own
 * scroll offset before drawing). Backgrounds (LAYOUT_ITEM_RECT) are
 * meant to be drawn in their own pass before all text/hr items, since
 * this flat list doesn't otherwise guarantee paint order between a
 * block's background and its own text. */
struct layout_item {
    enum layout_item_type type;
    int x, y, w, h;
    uint32_t color;
    char text[LAYOUT_TEXT_LEN]; /* LAYOUT_ITEM_TEXT only */
    int link_id;                /* index into doc->links, -1 if none */
    const struct dom_node *owner; /* nearest enclosing element, for JS onclick dispatch; NULL if none */
    const uint32_t *pixels;     /* LAYOUT_ITEM_IMAGE only; w*h, top-to-bottom, 0xRRGGBB */
    /* LAYOUT_ITEM_RECT only, from a linear-gradient()/border-radius
     * background (see struct css_computed) -- color above is the
     * gradient's first stop. */
    int has_gradient;
    uint32_t color2;
    int gradient_horizontal;
    int radius;
};

/* Implemented by the GUI/browser layer (gui/compositor.c), which is the
 * only thing that actually fetches and decodes images -- net/layout.c
 * itself never touches the network. Returns 1 and fills out_w/out_h/
 * out_pixels if `node` (an <img> element) has an already-loaded image,
 * 0 if it doesn't (fetch/decode failed, or the format isn't supported --
 * see net/bmp.h for what is). */
int layout_get_image(const struct dom_node *node, int *out_w, int *out_h, const uint32_t **out_pixels);

struct layout_link {
    char href[DOM_MAX_HREF];
};

struct layout_doc {
    struct layout_item *items;
    int item_count;
    struct layout_link *links;
    int link_count;
    char title[DOM_MAX_TITLE];
    int content_height; /* total laid-out page height in px, for scrolling */
};

void layout_doc_alloc(struct layout_doc *doc);
void layout_doc_free(struct layout_doc *doc);

/* Walks `root` (as returned by dom_parse) with styles resolved from
 * `sheet`, laying it out as a single column `viewport_width` px wide
 * starting at document x=0/y=0. `doc` must already be allocated via
 * layout_doc_alloc(); its item/link counts are reset here. */
void layout_run(const struct dom_node *root, const struct css_stylesheet *sheet,
                 int viewport_width, struct layout_doc *doc);

#endif
