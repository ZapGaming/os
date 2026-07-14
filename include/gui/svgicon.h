#ifndef GUI_SVGICON_H
#define GUI_SVGICON_H

#include <stdint.h>

/* Minimal SVG-subset vector icon renderer for the GUI compositor's dock
 * icons and window titlebars (see gui/svgicon.c for the full parser/
 * rasterizer implementation and design conventions).
 *
 * Every built-in icon is a single-color silhouette authored on a fixed
 * 0..24 unit design grid; svgicon_init() rasterizes all of them once
 * into 8-bit coverage/alpha masks (cached at SVGICON_CACHE_SIZE), and
 * svgicon_draw() composites a cached mask onto the framebuffer at any
 * requested size/tint/opacity. */

enum svg_icon_id {
    ICON_ABOUT = 0,
    ICON_SYSTEM,
    ICON_ROADMAP,
    ICON_PROCESS,
    ICON_NETWORK,
    ICON_FILES,
    ICON_BROWSER,
    ICON_TERMINAL,
    ICON_COUNT
};

/* Resolution every built-in icon is rasterized+cached at internally.
 * svgicon_draw() nearest-neighbor scales the cached mask to whatever
 * `size` the caller actually requests. */
#define SVGICON_CACHE_SIZE 32

/* Largest `out_size` svgicon_rasterize() will honor (clamped silently) --
 * bounds the static scratch supersample buffer in gui/svgicon.c. Plenty
 * above SVGICON_CACHE_SIZE for both the real cache and host-side tests. */
#define SVGICON_MAX_SIZE 64

/* Rasterizes + caches every built-in icon once. Must be called before
 * svgicon_draw() (or svgicon_get_mask()) produces anything useful. */
void svgicon_init(void);

/* Draws icon `id` into the framebuffer at top-left (x,y), scaled to
 * size x size, tinted with `color`. The cached coverage mask is used as
 * a per-pixel alpha, further scaled by `alpha_scale`/255 (e.g. pass 140
 * instead of 255 to dim a closed/unfocused icon without needing a
 * second cached variant), and composited via fb_blend_pixel(). */
void svgicon_draw(enum svg_icon_id id, int x, int y, int size, uint32_t color, uint8_t alpha_scale);

/* --- Internal entry points, exposed only so the host-side test harness
 * (see gui/svgicon.c's header comment) can exercise the parser and
 * rasterizer directly -- production code only ever reaches these via
 * svgicon_init()/svgicon_draw() above. ---
 *
 * Parses `svg_src` (the hand-rolled SVG subset documented in
 * gui/svgicon.c) and rasterizes it to an out_size x out_size 8-bit
 * coverage/alpha mask written into caller-owned `out_alpha`
 * (out_size*out_size bytes, row-major top-to-bottom). `out_size` is
 * clamped to SVGICON_MAX_SIZE. */
void svgicon_rasterize(const char *svg_src, uint8_t *out_alpha, int out_size);

/* Read-only accessor for a built-in icon's cached mask (NULL, *size_out
 * = 0 if svgicon_init() hasn't run yet or `id` is out of range).
 * *size_out receives SVGICON_CACHE_SIZE on success. */
const uint8_t *svgicon_get_mask(enum svg_icon_id id, int *size_out);

#endif
