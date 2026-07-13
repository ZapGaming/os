#ifndef NET_DOM_H
#define NET_DOM_H

#include <stdint.h>

#define DOM_MAX_TAG    16
#define DOM_MAX_ID     32
#define DOM_MAX_CLASS  64
#define DOM_MAX_HREF   192
#define DOM_MAX_STYLE  160
#define DOM_MAX_TITLE  128

enum dom_node_type {
    DOM_ELEMENT,
    DOM_TEXT,
};

/* A DOM tree node. Elements and text nodes share one struct for
 * simplicity -- `tag[0] == 0` marks a text node, in which case `text`
 * is the only field that matters besides the tree links. Everything is
 * kmalloc'd; the whole tree is freed with dom_free(). */
struct dom_node {
    enum dom_node_type type;
    char tag[DOM_MAX_TAG];      /* lowercase, e.g. "p", "a", "h1" */
    char id[DOM_MAX_ID];
    char class_name[DOM_MAX_CLASS]; /* raw space-separated class list */
    char href[DOM_MAX_HREF];    /* only meaningful for <a> */
    char style[DOM_MAX_STYLE];  /* raw inline style="" attribute text */
    char *text;                 /* kmalloc'd decoded text, DOM_TEXT only */

    struct dom_node *children;
    struct dom_node *last_child; /* parse-time bookkeeping only */
    struct dom_node *next;       /* next sibling */
};

/* Parses `html` (length `len`) into a DOM tree rooted at a synthetic
 * top-level node (tag "body"). Skips <script>/<style> element content
 * (the CSS inside <style> is extracted separately by css_extract, not
 * built into the tree). Decodes the common HTML entities. Extracts
 * <title> text (if present) into title_out. Returns the root node;
 * free it with dom_free() when done. */
struct dom_node *dom_parse(const char *html, uint32_t len, char *title_out, int title_cap);

void dom_free(struct dom_node *node);

#endif
