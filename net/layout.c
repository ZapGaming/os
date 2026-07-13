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
    const struct dom_node *owner;
};

struct layout_ctx {
    struct layout_doc *doc;
    const struct css_stylesheet *sheet;
    struct inline_word *words;
    int word_count;
    int items_dropped;
    int links_dropped;
};

static int layout_children(struct layout_ctx *ctx, const struct dom_node *parent,
                            const struct css_computed *parent_style, int x, int width,
                            int cursor_y, int link_id, const struct dom_node *owner);
static int layout_flex_row(struct layout_ctx *ctx, const struct dom_node *parent,
                            const struct css_computed *parent_style, int x, int width, int y,
                            int link_id);

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
                      uint32_t color, const char *text, int link_id, const struct dom_node *owner) {
    if (ctx->doc->item_count >= LAYOUT_MAX_ITEMS) { ctx->items_dropped++; return; }
    struct layout_item *it = &ctx->doc->items[ctx->doc->item_count++];
    it->type = type;
    it->x = x; it->y = y; it->w = w; it->h = h;
    it->color = color;
    it->link_id = link_id;
    it->owner = owner;
    it->pixels = NULL;
    if (text) { strncpy(it->text, text, LAYOUT_TEXT_LEN - 1); it->text[LAYOUT_TEXT_LEN - 1] = 0; }
    else it->text[0] = 0;
}

static void add_image_item(struct layout_ctx *ctx, int x, int y, int w, int h,
                            const uint32_t *pixels, int link_id, const struct dom_node *owner) {
    if (ctx->doc->item_count >= LAYOUT_MAX_ITEMS) { ctx->items_dropped++; return; }
    struct layout_item *it = &ctx->doc->items[ctx->doc->item_count++];
    it->type = LAYOUT_ITEM_IMAGE;
    it->x = x; it->y = y; it->w = w; it->h = h;
    it->color = 0;
    it->link_id = link_id;
    it->owner = owner;
    it->pixels = pixels;
    it->text[0] = 0;
}

static int register_link(struct layout_ctx *ctx, const char *href) {
    if (!href[0]) return -1;
    if (ctx->doc->link_count >= LAYOUT_MAX_LINKS) { ctx->links_dropped++; return -1; }
    int id = ctx->doc->link_count++;
    strncpy(ctx->doc->links[id].href, href, DOM_MAX_HREF - 1);
    ctx->doc->links[id].href[DOM_MAX_HREF - 1] = 0;
    return id;
}

static void push_word(struct layout_ctx *ctx, const char *text, int tlen, uint32_t color, int link_id,
                       const struct dom_node *owner) {
    if (ctx->word_count >= LAYOUT_MAX_WORDS) return;
    if (tlen > LAYOUT_WORD_LEN - 1) tlen = LAYOUT_WORD_LEN - 1;
    struct inline_word *w = &ctx->words[ctx->word_count++];
    memcpy(w->text, text, (size_t)tlen);
    w->text[tlen] = 0;
    w->color = color;
    w->link_id = link_id;
    w->owner = owner;
}

static void flow_text(struct layout_ctx *ctx, const char *text, uint32_t color, int link_id,
                       const struct dom_node *owner) {
    if (!text) return;
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        push_word(ctx, start, (int)(p - start), color, link_id, owner);
    }
}

/* Word-wraps and emits the accumulated flow as TEXT items starting at
 * (x, cursor_y), one item per word (so each word keeps its own color,
 * link id, and owning element -- needed for real per-word click hit-
 * testing, both for links and for JS onclick handlers). Clears the
 * flow and returns the cursor_y just past the last line. */
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
        add_item(ctx, LAYOUT_ITEM_TEXT, pen_x, y, wlen * LAYOUT_CHAR_W, LAYOUT_LINE_H,
                 w->color, w->text, w->link_id, w->owner);
        pen_x += wlen * LAYOUT_CHAR_W;
        col += wlen;
        any_on_line = 1;
    }
    ctx->word_count = 0;
    return y + LAYOUT_LINE_H;
}

/* A deliberately simplified flexbox, used only for `flex-direction:
 * row` (column direction is close enough to normal block stacking
 * that layout_children() below just handles it directly instead).
 * Every visible child keeps its own explicit CSS width if it set one;
 * whatever width is left over is split equally among the children
 * that didn't -- there's no real flex-grow/shrink weighting, no wrap
 * (a row that overflows just overflows), and `align-items` isn't
 * implemented (children are always top-aligned within the row, at
 * their own natural height). justify-content only visibly matters
 * when there's leftover space to distribute, i.e. when at least one
 * child has an explicit width smaller than its equal share. */
static int layout_flex_row(struct layout_ctx *ctx, const struct dom_node *parent,
                            const struct css_computed *parent_style, int x, int width, int y,
                            int link_id) {
    int n = 0, explicit_sum = 0, auto_count = 0;
    for (const struct dom_node *c = parent->children; c; c = c->next) {
        if (c->type == DOM_TEXT) continue;
        struct css_computed cs;
        css_compute_style(c, ctx->sheet, parent_style, &cs);
        if (cs.display == CSS_DISPLAY_NONE) continue;
        n++;
        if (cs.width > 0) explicit_sum += cs.width; else auto_count++;
    }
    if (n == 0) return y;

    int gap = parent_style->gap;
    int gap_total = gap * (n - 1);
    int remaining = width - explicit_sum - gap_total;
    if (remaining < 0) remaining = 0;
    int auto_w = auto_count > 0 ? remaining / auto_count : 0;
    if (auto_w < LAYOUT_CHAR_W) auto_w = LAYOUT_CHAR_W;

    int total_used = explicit_sum + auto_count * auto_w + gap_total;
    int extra = width - total_used;
    if (extra < 0) extra = 0;
    int start_x = x, extra_gap = 0;
    if (parent_style->justify == CSS_JUSTIFY_CENTER) start_x = x + extra / 2;
    else if (parent_style->justify == CSS_JUSTIFY_END) start_x = x + extra;
    else if (parent_style->justify == CSS_JUSTIFY_BETWEEN && n > 1) extra_gap = extra / (n - 1);

    int cx = start_x, max_bottom = y;
    for (const struct dom_node *c = parent->children; c; c = c->next) {
        if (c->type == DOM_TEXT) continue;
        struct css_computed cs;
        css_compute_style(c, ctx->sheet, parent_style, &cs);
        if (cs.display == CSS_DISPLAY_NONE) continue;

        int child_link_id = link_id;
        if (strcmp(c->tag, "a") == 0 && c->href[0]) child_link_id = register_link(ctx, c->href);

        int child_w = cs.width > 0 ? cs.width : auto_w;

        int cy = y + cs.margin_top;
        int block_top = cy;
        cy += cs.padding_top;

        int rect_idx = -1;
        if (cs.has_background) {
            int before = ctx->doc->item_count;
            add_item(ctx, LAYOUT_ITEM_RECT, cx, block_top, child_w, 0, cs.background_color, NULL, -1, c);
            if (ctx->doc->item_count > before) rect_idx = before;
        }

        int inner_x = cx + cs.padding_left;
        int inner_w = child_w - cs.padding_left;
        if (inner_w < LAYOUT_CHAR_W) inner_w = LAYOUT_CHAR_W;

        int child_bottom = (cs.display == CSS_DISPLAY_FLEX && cs.flex_row)
                                ? layout_flex_row(ctx, c, &cs, inner_x, inner_w, cy, child_link_id)
                                : layout_children(ctx, c, &cs, inner_x, inner_w, cy, child_link_id, c);
        child_bottom = flush_flow(ctx, inner_x, inner_w, child_bottom);
        child_bottom += cs.padding_bottom;
        if (cs.height > 0 && block_top + cs.padding_top + cs.height > child_bottom) {
            child_bottom = block_top + cs.padding_top + cs.height;
        }
        if (rect_idx >= 0) ctx->doc->items[rect_idx].h = child_bottom - block_top;

        int full_bottom = child_bottom + cs.margin_bottom;
        if (full_bottom > max_bottom) max_bottom = full_bottom;

        cx += child_w + gap + extra_gap;
    }
    return max_bottom;
}

static int layout_children(struct layout_ctx *ctx, const struct dom_node *parent,
                            const struct css_computed *parent_style, int x, int width,
                            int cursor_y, int link_id, const struct dom_node *owner) {
    /* Simplified float tracking, scoped to this one block formatting
     * context: at most one active float per side. A float carves out
     * horizontal space from x/width for whatever follows it in normal
     * flow, until the flow's cursor_y passes the float's own bottom
     * ("clears" it) -- floats don't advance cursor_y themselves, since
     * they sit beside flow content instead of pushing it down. A second
     * same-side float that starts before the first clears just stacks
     * below it rather than beside it (real CSS packs same-side floats
     * side by side if they fit; that's out of scope here). */
    int lf_w = 0, lf_bottom = cursor_y;
    int rf_w = 0, rf_bottom = cursor_y;

    for (const struct dom_node *child = parent->children; child; child = child->next) {
        if (cursor_y >= lf_bottom) lf_w = 0;
        if (cursor_y >= rf_bottom) rf_w = 0;
        int eff_x = x + lf_w;
        int eff_width = width - lf_w - rf_w;
        if (eff_width < LAYOUT_CHAR_W) eff_width = LAYOUT_CHAR_W;

        if (child->type == DOM_TEXT) {
            flow_text(ctx, child->text, parent_style->color, link_id, owner);
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
            cursor_y = flush_flow(ctx, eff_x, eff_width, cursor_y);
            continue;
        }

        if (strcmp(child->tag, "img") == 0) {
            cursor_y = flush_flow(ctx, eff_x, eff_width, cursor_y);

            int nat_w = 0, nat_h = 0;
            const uint32_t *pixels = NULL;
            layout_get_image(child, &nat_w, &nat_h, &pixels);
            int img_w = style.width > 0 ? style.width : (nat_w > 0 ? nat_w : 32);
            int img_h = style.height > 0 ? style.height :
                        (nat_h > 0 && nat_w > 0 ? (img_w * nat_h) / nat_w : 32);

            if (style.cssfloat != CSS_FLOAT_NONE) {
                int fy = (style.cssfloat == CSS_FLOAT_LEFT) ? (lf_w > 0 ? lf_bottom : cursor_y)
                                                             : (rf_w > 0 ? rf_bottom : cursor_y);
                int fx = (style.cssfloat == CSS_FLOAT_LEFT) ? x : x + width - img_w;
                add_image_item(ctx, fx, fy, img_w, img_h, pixels, child_link_id, child);
                if (style.cssfloat == CSS_FLOAT_LEFT) { lf_w = img_w; lf_bottom = fy + img_h; }
                else { rf_w = img_w; rf_bottom = fy + img_h; }
            } else {
                cursor_y += style.margin_top;
                add_image_item(ctx, eff_x, cursor_y, img_w, img_h, pixels, child_link_id, child);
                cursor_y += img_h + style.margin_bottom;
            }
            continue;
        }

        if (style.display == CSS_DISPLAY_INLINE && style.cssfloat == CSS_FLOAT_NONE) {
            /* Stays in the same paragraph flow as its siblings. */
            cursor_y = layout_children(ctx, child, &style, eff_x, eff_width, cursor_y, child_link_id, child);
            continue;
        }

        /* Block-level (floats are always block, regardless of `display`,
         * per CSS -- and everything else non-inline): flush whatever
         * inline content preceded it, then lay this element out as its
         * own box. */
        cursor_y = flush_flow(ctx, eff_x, eff_width, cursor_y);

        if (strcmp(child->tag, "hr") == 0) {
            cursor_y += style.margin_top;
            add_item(ctx, LAYOUT_ITEM_HR, eff_x, cursor_y, eff_width, 2, style.color, NULL, -1, child);
            cursor_y += 2 + style.margin_bottom;
            continue;
        }

        if (style.cssfloat != CSS_FLOAT_NONE) {
            int float_w = style.width > 0 ? style.width : eff_width / 2;
            if (float_w < LAYOUT_CHAR_W) float_w = LAYOUT_CHAR_W;
            int fy = (style.cssfloat == CSS_FLOAT_LEFT) ? (lf_w > 0 ? lf_bottom : cursor_y)
                                                         : (rf_w > 0 ? rf_bottom : cursor_y);
            int fx = (style.cssfloat == CSS_FLOAT_LEFT) ? x : x + width - float_w;

            int block_top = fy + style.margin_top;
            int fcursor = block_top + style.padding_top;

            int rect_idx = -1;
            if (style.has_background) {
                int before_count = ctx->doc->item_count;
                add_item(ctx, LAYOUT_ITEM_RECT, fx, block_top, float_w, 0, style.background_color, NULL, -1, child);
                if (ctx->doc->item_count > before_count) rect_idx = before_count;
            }

            int fchild_x = fx + style.padding_left;
            int fchild_w = float_w - style.padding_left;
            if (fchild_w < LAYOUT_CHAR_W) fchild_w = LAYOUT_CHAR_W;

            if (strcmp(child->tag, "li") == 0) push_word(ctx, "-", 1, style.color, -1, child);

            fcursor = layout_children(ctx, child, &style, fchild_x, fchild_w, fcursor, child_link_id, child);
            fcursor = flush_flow(ctx, fchild_x, fchild_w, fcursor);
            fcursor += style.padding_bottom;
            if (style.height > 0 && block_top + style.padding_top + style.height > fcursor) {
                fcursor = block_top + style.padding_top + style.height;
            }
            if (rect_idx >= 0) ctx->doc->items[rect_idx].h = fcursor - block_top;

            int float_bottom = fcursor + style.margin_bottom;
            if (style.cssfloat == CSS_FLOAT_LEFT) { lf_w = float_w; lf_bottom = float_bottom; }
            else { rf_w = float_w; rf_bottom = float_bottom; }
            continue;
        }

        int box_width = eff_width;
        if (style.width > 0 && style.width < box_width) box_width = style.width;

        cursor_y += style.margin_top;
        int block_top = cursor_y;
        cursor_y += style.padding_top;

        /* Reserve this block's own background rect's array slot *before*
         * laying out its children, so it ends up earlier in the item
         * list than anything nested inside it -- the draw loop paints
         * a pass front-to-back in array order, so "earlier" means
         * "underneath." Its real height isn't known until the children
         * are laid out, so it's patched in place afterward via the
         * same index rather than appended fresh (which previously put
         * every container's background *after*, and therefore visually
         * on top of, its own children's backgrounds -- e.g. a <body>
         * with a background color completely hid any background color
         * on a child <div>, since both covered the same screen area
         * and the body's rect was always emitted last). */
        int rect_idx = -1;
        if (style.has_background) {
            int before_count = ctx->doc->item_count;
            add_item(ctx, LAYOUT_ITEM_RECT, eff_x, block_top, box_width, 0, style.background_color, NULL, -1, child);
            if (ctx->doc->item_count > before_count) rect_idx = before_count;
        }

        int child_x = eff_x + style.padding_left;
        int child_width = box_width - style.padding_left;
        if (child_width < LAYOUT_CHAR_W) child_width = LAYOUT_CHAR_W;

        if (strcmp(child->tag, "li") == 0) push_word(ctx, "-", 1, style.color, -1, child);

        cursor_y = (style.display == CSS_DISPLAY_FLEX && style.flex_row)
                       ? layout_flex_row(ctx, child, &style, child_x, child_width, cursor_y, child_link_id)
                       : layout_children(ctx, child, &style, child_x, child_width, cursor_y, child_link_id, child);
        cursor_y = flush_flow(ctx, child_x, child_width, cursor_y);

        cursor_y += style.padding_bottom;

        if (style.height > 0 && block_top + style.padding_top + style.height > cursor_y) {
            cursor_y = block_top + style.padding_top + style.height;
        }

        if (rect_idx >= 0) ctx->doc->items[rect_idx].h = cursor_y - block_top;

        cursor_y += style.margin_bottom;
    }

    if (lf_bottom > cursor_y) cursor_y = lf_bottom;
    if (rf_bottom > cursor_y) cursor_y = rf_bottom;
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

    int cursor_y = layout_children(&ctx, root, &root_style, 0, viewport_width, 0, -1, root);
    cursor_y = flush_flow(&ctx, 0, viewport_width, cursor_y);
    doc->content_height = cursor_y;

    kfree(ctx.words);

    if (ctx.items_dropped > 0) serial_printf("layout: dropped %d render items (page too large)\n", ctx.items_dropped);
    if (ctx.links_dropped > 0) serial_printf("layout: dropped %d links (page has too many)\n", ctx.links_dropped);
}
