#include <net/layout.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

#define LAYOUT_WORD_LEN  40
#define LAYOUT_MAX_WORDS 4096

struct inline_word {
    char text[LAYOUT_WORD_LEN];
    uint32_t color;
    int link_id;
};

struct layout_ctx {
    struct layout_doc *doc;
    const struct css_stylesheet *sheet;
    struct inline_word *words;
    int word_count;
    int items_dropped;
    int links_dropped;
};

void layout_doc_alloc(struct layout_doc *doc) {
    doc->items = kmalloc(sizeof(struct layout_item) * LAYOUT_MAX_ITEMS);
    doc->item_count = 0;
    doc->links = kmalloc(sizeof(struct layout_link) * LAYOUT_MAX_LINKS);
    doc->link_count = 0;
    doc->title[0] = 0;
    doc->content_height = 0;
}

void layout_doc_free(struct layout_doc *doc) {
    if (doc->items) kfree(doc->items);
    if (doc->links) kfree(doc->links);
    doc->items = NULL;
    doc->links = NULL;
}

static void add_item(struct layout_ctx *ctx, enum layout_item_type type, int x, int y, int w, int h,
                      uint32_t color, const char *text, int link_id) {
    if (ctx->doc->item_count >= LAYOUT_MAX_ITEMS) { ctx->items_dropped++; return; }
    struct layout_item *it = &ctx->doc->items[ctx->doc->item_count++];
    it->type = type;
    it->x = x; it->y = y; it->w = w; it->h = h;
    it->color = color;
    it->link_id = link_id;
    if (text) { strncpy(it->text, text, LAYOUT_TEXT_LEN - 1); it->text[LAYOUT_TEXT_LEN - 1] = 0; }
    else it->text[0] = 0;
}

static int register_link(struct layout_ctx *ctx, const char *href) {
    if (!href[0]) return -1;
    if (ctx->doc->link_count >= LAYOUT_MAX_LINKS) { ctx->links_dropped++; return -1; }
    int id = ctx->doc->link_count++;
    strncpy(ctx->doc->links[id].href, href, DOM_MAX_HREF - 1);
    ctx->doc->links[id].href[DOM_MAX_HREF - 1] = 0;
    return id;
}

static void push_word(struct layout_ctx *ctx, const char *text, int tlen, uint32_t color, int link_id) {
    if (ctx->word_count >= LAYOUT_MAX_WORDS) return;
    if (tlen > LAYOUT_WORD_LEN - 1) tlen = LAYOUT_WORD_LEN - 1;
    struct inline_word *w = &ctx->words[ctx->word_count++];
    memcpy(w->text, text, (size_t)tlen);
    w->text[tlen] = 0;
    w->color = color;
    w->link_id = link_id;
}

static void flow_text(struct layout_ctx *ctx, const char *text, uint32_t color, int link_id) {
    if (!text) return;
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        push_word(ctx, start, (int)(p - start), color, link_id);
    }
}

/* Word-wraps and emits the accumulated flow as TEXT items starting at
 * (x, cursor_y), one item per word (so each word keeps its own color
 * and link id -- needed for real per-word click hit-testing). Clears
 * the flow and returns the cursor_y just past the last line. */
static int flush_flow(struct layout_ctx *ctx, int x, int width, int cursor_y) {
    if (ctx->word_count == 0) return cursor_y;
    int chars_per_line = width / LAYOUT_CHAR_W;
    if (chars_per_line < 1) chars_per_line = 1;

    int pen_x = x, col = 0, y = cursor_y, any_on_line = 0;
    for (int i = 0; i < ctx->word_count; i++) {
        struct inline_word *w = &ctx->words[i];
        int wlen = (int)strlen(w->text);
        if (any_on_line && col + 1 + wlen > chars_per_line) {
            y += LAYOUT_LINE_H;
            pen_x = x; col = 0; any_on_line = 0;
        }
        if (any_on_line) { pen_x += LAYOUT_CHAR_W; col++; }
        add_item(ctx, LAYOUT_ITEM_TEXT, pen_x, y, wlen * LAYOUT_CHAR_W, LAYOUT_LINE_H, w->color, w->text, w->link_id);
        pen_x += wlen * LAYOUT_CHAR_W;
        col += wlen;
        any_on_line = 1;
    }
    ctx->word_count = 0;
    return y + LAYOUT_LINE_H;
}

static int layout_children(struct layout_ctx *ctx, const struct dom_node *parent,
                            const struct css_computed *parent_style, int x, int width,
                            int cursor_y, int link_id) {
    for (const struct dom_node *child = parent->children; child; child = child->next) {
        if (child->type == DOM_TEXT) {
            flow_text(ctx, child->text, parent_style->color, link_id);
            continue;
        }

        struct css_computed style;
        css_compute_style(child, ctx->sheet, parent_style, &style);
        if (style.display == CSS_DISPLAY_NONE) continue;

        int child_link_id = link_id;
        if (strcmp(child->tag, "a") == 0 && child->href[0]) {
            child_link_id = register_link(ctx, child->href);
        }

        if (strcmp(child->tag, "br") == 0) {
            cursor_y = flush_flow(ctx, x, width, cursor_y);
            continue;
        }

        if (style.display == CSS_DISPLAY_INLINE) {
            /* Stays in the same paragraph flow as its siblings. */
            cursor_y = layout_children(ctx, child, &style, x, width, cursor_y, child_link_id);
            continue;
        }

        /* Block-level: flush whatever inline content preceded it, then
         * lay this element out as its own box. */
        cursor_y = flush_flow(ctx, x, width, cursor_y);

        if (strcmp(child->tag, "hr") == 0) {
            cursor_y += style.margin_top;
            add_item(ctx, LAYOUT_ITEM_HR, x, cursor_y, width, 2, style.color, NULL, -1);
            cursor_y += 2 + style.margin_bottom;
            continue;
        }

        cursor_y += style.margin_top;
        int block_top = cursor_y;
        cursor_y += style.padding_top;

        int child_x = x + style.padding_left;
        int child_width = width - style.padding_left;
        if (child_width < LAYOUT_CHAR_W) child_width = LAYOUT_CHAR_W;

        if (strcmp(child->tag, "li") == 0) push_word(ctx, "-", 1, style.color, -1);

        cursor_y = layout_children(ctx, child, &style, child_x, child_width, cursor_y, child_link_id);
        cursor_y = flush_flow(ctx, child_x, child_width, cursor_y);

        cursor_y += style.padding_bottom;

        if (style.has_background) {
            add_item(ctx, LAYOUT_ITEM_RECT, x, block_top, width, cursor_y - block_top,
                     style.background_color, NULL, -1);
        }

        cursor_y += style.margin_bottom;
    }
    return cursor_y;
}

void layout_run(const struct dom_node *root, const struct css_stylesheet *sheet,
                 int viewport_width, struct layout_doc *doc) {
    doc->item_count = 0;
    doc->link_count = 0;

    struct layout_ctx ctx;
    ctx.doc = doc;
    ctx.sheet = sheet;
    ctx.word_count = 0;
    ctx.items_dropped = 0;
    ctx.links_dropped = 0;
    ctx.words = kmalloc(sizeof(struct inline_word) * LAYOUT_MAX_WORDS);
    if (!ctx.words) {
        serial_printf("layout: out of memory for word scratch buffer\n");
        return;
    }

    struct css_computed root_style;
    css_compute_style(root, sheet, NULL, &root_style);
    root_style.display = CSS_DISPLAY_BLOCK;

    int cursor_y = layout_children(&ctx, root, &root_style, 0, viewport_width, 0, -1);
    cursor_y = flush_flow(&ctx, 0, viewport_width, cursor_y);
    doc->content_height = cursor_y;

    kfree(ctx.words);

    if (ctx.items_dropped > 0) serial_printf("layout: dropped %d render items (page too large)\n", ctx.items_dropped);
    if (ctx.links_dropped > 0) serial_printf("layout: dropped %d links (page has too many)\n", ctx.links_dropped);
}
