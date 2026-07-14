/* Minimal SVG-subset vector icon renderer.
 *
 * Design convention: every icon below is authored as SVG source text on
 * a fixed 0..24 unit design grid. The outer <svg ...> / </svg> wrapper
 * (and any viewBox/width/height attributes on it) is completely ignored
 * -- coordinates in every child element are simply assumed to already
 * be on that 0..24 grid. There is no general XML parser here, just a
 * hand-rolled scanner over a small, fixed subset of body elements:
 *
 *   <rect x="" y="" width="" height="" rx=""/>   (rx optional, default 0)
 *   <circle cx="" cy="" r=""/>
 *   <polygon points="x1,y1 x2,y2 ..."/>
 *   <path d="M/m L/l H/h V/v C/c Z/z ..."/>       (multiple M..Z subpaths OK)
 *
 * All attribute values are assumed double-quoted. `fill`/`stroke`/etc
 * are ignored entirely -- every icon is a single-color silhouette, so
 * only coverage (alpha) is rasterized; the caller supplies the tint at
 * draw time (see svgicon_draw()).
 *
 * Every shape/subpath in one icon is combined into ONE scanline fill
 * pass using the EVEN-ODD fill rule, which is what lets two overlapping
 * shapes punch a hole in each other (see ICON_BROWSER's globe: the
 * equator/meridian "lines" are lens-shaped cutouts even-odd'd out of
 * the solid disc, since a single alpha-mask silhouette has no second
 * color to draw contrasting lines with).
 *
 * No floating point anywhere (this file compiles under the kernel's
 * -mgeneral-regs-only/-mno-sse/-mno-80387 flags same as everything else
 * in gui/) -- all coordinate math is fixed-point:
 *   - "grid-Q8": parsed attribute values, i.e. design-grid units * 256.
 *   - "px-Q4": supersample-pixel coordinates * 16, used only for the
 *     scanline intersection test (kept at a coarser fixed-point scale
 *     than grid-Q8 specifically so intermediate products can't overflow
 *     a 32-bit int -- see grid_to_pxq4()).
 * Curves (circle, rounded-rect corners, path 'C') are flattened into
 * straight line segments up front (12-16 segments per curve, via a
 * hardcoded quarter-circle cosine/sine table for circle/rounded-corner
 * arcs and De Casteljau lerps for cubic beziers) before the single
 * shared scanline-polygon-fill routine ever runs.
 *
 * Anti-aliasing: each icon is rasterized at 4x the requested output
 * resolution into a temporary 0/255 coverage buffer, then box-
 * downsampled 4x4 -> 1 to produce the final smooth-edged alpha mask. */

#include <gui/svgicon.h>
#include <gui/framebuffer.h>
#include <string.h>

/* ---- fixed-point helpers -------------------------------------------- */

typedef struct { int32_t x, y; } ptq8_t; /* grid units * 256 */

/* cos/sin * 256 for angles 0, 22.5, 45, 67.5, 90 degrees -- the only
 * trig this file needs (circle + rounded-rect-corner flattening), so
 * it's cheaper to hardcode this one quarter-circle table than to carry
 * a general fixed-point sin/cos implementation. */
static const int32_t QCOS[5] = {256, 236, 181, 98, 0};
static const int32_t QSIN[5] = {0, 98, 181, 236, 256};

/* ---- point/subpath accumulation -------------------------------------
 * One rasterize pass builds up a flat point buffer plus a list of
 * subpath start offsets into it; edges are then formed by connecting
 * consecutive points within each subpath, wrapping the last point back
 * to the first (every subpath is implicitly closed). */

#define MAX_POINTS 512
#define MAX_SUBPATHS 32

static ptq8_t g_pts[MAX_POINTS];
static int g_npts;
static int g_sub_start[MAX_SUBPATHS];
static int g_nsub;

static void begin_subpath(void) {
    if (g_nsub < MAX_SUBPATHS) g_sub_start[g_nsub++] = g_npts;
}

static void add_point(int32_t x, int32_t y) {
    if (g_npts > 0 && g_npts < MAX_POINTS) {
        ptq8_t *last = &g_pts[g_npts - 1];
        if (last->x == x && last->y == y) return; /* dedupe consecutive dup */
    }
    if (g_npts < MAX_POINTS) {
        g_pts[g_npts].x = x;
        g_pts[g_npts].y = y;
        g_npts++;
    }
}

/* ---- number/attribute scanning --------------------------------------
 * All attribute values are assumed double-quoted (per this file's
 * documented subset); numbers may have a leading sign and an optional
 * '.' fractional part. Returned in grid-Q8 (value * 256). */

static int is_sep(char c) {
    return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r';
}

static const char *skip_seps(const char *s) {
    while (*s && is_sep(*s)) s++;
    return s;
}

static int32_t parse_num_q8(const char **sp) {
    const char *s = skip_seps(*sp);
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') { s++; }
    int32_t intpart = 0;
    while (*s >= '0' && *s <= '9') { intpart = intpart * 10 + (*s - '0'); s++; }
    int32_t frac = 0, denom = 1;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { frac = frac * 10 + (*s - '0'); denom *= 10; s++; }
    }
    int32_t value_q8 = intpart * 256 + (frac * 256) / denom;
    if (neg) value_q8 = -value_q8;
    *sp = s;
    return value_q8;
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

/* Finds `name="` inside [tag, tag_end), requiring a non-word-char (or
 * tag start) immediately before the match so searching for "x" can't
 * accidentally match inside "rx" or similar. Returns a pointer to the
 * first character of the value (just after the opening quote), or NULL. */
static const char *find_attr(const char *tag, const char *tag_end, const char *name) {
    size_t nlen = strlen(name);
    for (const char *p = tag; p + nlen + 1 < tag_end; p++) {
        if (p != tag && is_word_char(p[-1])) continue;
        if ((size_t)(tag_end - p) > nlen + 1 &&
            memcmp(p, name, nlen) == 0 && p[nlen] == '=' && p[nlen + 1] == '"') {
            return p + nlen + 2;
        }
    }
    return NULL;
}

static int32_t get_attr_num(const char *tag, const char *tag_end, const char *name, int32_t def_q8) {
    const char *v = find_attr(tag, tag_end, name);
    if (!v) return def_q8;
    return parse_num_q8(&v);
}

static int get_attr_str(const char *tag, const char *tag_end, const char *name, char *buf, int bufsz) {
    const char *v = find_attr(tag, tag_end, name);
    if (!v) return 0;
    int i = 0;
    while (v < tag_end && *v != '"' && i < bufsz - 1) buf[i++] = *v++;
    buf[i] = 0;
    return 1;
}

/* ---- shape flatteners ------------------------------------------------ */

static void add_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rx) {
    if (rx <= 0 || w <= 0 || h <= 0) {
        begin_subpath();
        add_point(x, y);
        add_point(x + w, y);
        add_point(x + w, y + h);
        add_point(x, y + h);
        return;
    }
    int32_t maxr = (w < h ? w : h) / 2;
    if (rx > maxr) rx = maxr;

    begin_subpath();
    int32_t cx, cy;

    /* top-right corner: sweeps from the top edge's direction (0,-1) to
     * the right edge's direction (1,0). */
    cx = x + w - rx; cy = y + rx;
    for (int i = 0; i < 5; i++) add_point(cx + (rx * QSIN[i]) / 256, cy - (rx * QCOS[i]) / 256);
    /* bottom-right: right-edge direction (1,0) -> bottom-edge (0,1). */
    cx = x + w - rx; cy = y + h - rx;
    for (int i = 0; i < 5; i++) add_point(cx + (rx * QCOS[i]) / 256, cy + (rx * QSIN[i]) / 256);
    /* bottom-left: (0,1) -> (-1,0). */
    cx = x + rx; cy = y + h - rx;
    for (int i = 0; i < 5; i++) add_point(cx - (rx * QSIN[i]) / 256, cy + (rx * QCOS[i]) / 256);
    /* top-left: (-1,0) -> (0,-1), closing the loop. */
    cx = x + rx; cy = y + rx;
    for (int i = 0; i < 5; i++) add_point(cx - (rx * QCOS[i]) / 256, cy - (rx * QSIN[i]) / 256);
}

static void add_circle(int32_t cx, int32_t cy, int32_t r) {
    if (r <= 0) return;
    begin_subpath();
    /* quadrant 0 (0..90deg): rightmost point sweeping to topmost. */
    for (int i = 0; i < 5; i++) add_point(cx + (r * QCOS[i]) / 256, cy - (r * QSIN[i]) / 256);
    /* quadrant 1 (90..180): topmost -> leftmost. */
    for (int i = 0; i < 5; i++) add_point(cx - (r * QSIN[i]) / 256, cy - (r * QCOS[i]) / 256);
    /* quadrant 2 (180..270): leftmost -> bottommost. */
    for (int i = 0; i < 5; i++) add_point(cx - (r * QCOS[i]) / 256, cy + (r * QSIN[i]) / 256);
    /* quadrant 3 (270..360): bottommost -> rightmost (closing). */
    for (int i = 0; i < 5; i++) add_point(cx + (r * QSIN[i]) / 256, cy + (r * QCOS[i]) / 256);
}

static void add_polygon_from_str(const char *s) {
    begin_subpath();
    for (;;) {
        s = skip_seps(s);
        if (!*s) break;
        int32_t x = parse_num_q8(&s);
        s = skip_seps(s);
        int32_t y = parse_num_q8(&s);
        add_point(x, y);
    }
}

static int32_t lerp_q8(int32_t a, int32_t b, int32_t t256) {
    return a + ((b - a) * t256) / 256;
}

/* De Casteljau subdivision of a cubic bezier (p0..p3, all grid-Q8) into
 * 12 line segments (p0 is assumed already present in the point list --
 * this appends the 12 following points, the last of which is p3). */
static void flatten_cubic(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           int32_t x2, int32_t y2, int32_t x3, int32_t y3) {
    const int N = 12;
    for (int i = 1; i <= N; i++) {
        int32_t t = (i * 256) / N;
        int32_t ax = lerp_q8(x0, x1, t), ay = lerp_q8(y0, y1, t);
        int32_t bx = lerp_q8(x1, x2, t), by = lerp_q8(y1, y2, t);
        int32_t cx = lerp_q8(x2, x3, t), cy = lerp_q8(y2, y3, t);
        int32_t dx = lerp_q8(ax, bx, t), dy = lerp_q8(ay, by, t);
        int32_t ex = lerp_q8(bx, cx, t), ey = lerp_q8(by, cy, t);
        add_point(lerp_q8(dx, ex, t), lerp_q8(dy, ey, t));
    }
}

static void add_path_from_str(const char *s) {
    int32_t curx = 0, cury = 0, startx = 0, starty = 0;
    for (;;) {
        s = skip_seps(s);
        if (!*s) break;
        char cmd = *s;
        if (!((cmd >= 'A' && cmd <= 'Z') || (cmd >= 'a' && cmd <= 'z'))) break;
        s++;
        int rel = (cmd >= 'a' && cmd <= 'z');
        char cu = rel ? (char)(cmd - 32) : cmd;

        switch (cu) {
        case 'M': {
            int32_t x = parse_num_q8(&s);
            int32_t y = parse_num_q8(&s);
            if (rel) { x += curx; y += cury; }
            curx = x; cury = y; startx = x; starty = y;
            begin_subpath();
            add_point(x, y);
            break;
        }
        case 'L': {
            int32_t x = parse_num_q8(&s);
            int32_t y = parse_num_q8(&s);
            if (rel) { x += curx; y += cury; }
            curx = x; cury = y;
            add_point(x, y);
            break;
        }
        case 'H': {
            int32_t x = parse_num_q8(&s);
            if (rel) x += curx;
            curx = x;
            add_point(x, cury);
            break;
        }
        case 'V': {
            int32_t y = parse_num_q8(&s);
            if (rel) y += cury;
            cury = y;
            add_point(curx, y);
            break;
        }
        case 'C': {
            int32_t x1 = parse_num_q8(&s), y1 = parse_num_q8(&s);
            int32_t x2 = parse_num_q8(&s), y2 = parse_num_q8(&s);
            int32_t x = parse_num_q8(&s), y = parse_num_q8(&s);
            if (rel) { x1 += curx; y1 += cury; x2 += curx; y2 += cury; x += curx; y += cury; }
            flatten_cubic(curx, cury, x1, y1, x2, y2, x, y);
            curx = x; cury = y;
            break;
        }
        case 'Z':
            curx = startx; cury = starty;
            break;
        default:
            return; /* unsupported command -- stop parsing defensively */
        }
    }
}

/* ---- top-level tag scanner ------------------------------------------- */

static void parse_svg(const char *src) {
    g_npts = 0;
    g_nsub = 0;
    const char *p = src;
    while (*p) {
        if (*p != '<') { p++; continue; }
        const char *tag_start = p + 1;
        const char *gt = tag_start;
        while (*gt && *gt != '>') gt++;
        if (!*gt) break;
        const char *tag_end = gt;

        if (tag_start[0] != '/') {
            if (strncmp(tag_start, "rect", 4) == 0 && !is_word_char(tag_end > tag_start + 4 ? tag_start[4] : 0)) {
                int32_t x = get_attr_num(tag_start, tag_end, "x", 0);
                int32_t y = get_attr_num(tag_start, tag_end, "y", 0);
                int32_t w = get_attr_num(tag_start, tag_end, "width", 0);
                int32_t h = get_attr_num(tag_start, tag_end, "height", 0);
                int32_t rx = get_attr_num(tag_start, tag_end, "rx", 0);
                add_rect(x, y, w, h, rx);
            } else if (strncmp(tag_start, "circle", 6) == 0 && !is_word_char(tag_start[6])) {
                int32_t cx = get_attr_num(tag_start, tag_end, "cx", 0);
                int32_t cy = get_attr_num(tag_start, tag_end, "cy", 0);
                int32_t r = get_attr_num(tag_start, tag_end, "r", 0);
                add_circle(cx, cy, r);
            } else if (strncmp(tag_start, "polygon", 7) == 0 && !is_word_char(tag_start[7])) {
                char buf[256];
                if (get_attr_str(tag_start, tag_end, "points", buf, sizeof(buf))) {
                    add_polygon_from_str(buf);
                }
            } else if (strncmp(tag_start, "path", 4) == 0 && !is_word_char(tag_start[4])) {
                char buf[512];
                if (get_attr_str(tag_start, tag_end, "d", buf, sizeof(buf))) {
                    add_path_from_str(buf);
                }
            }
            /* anything else (the outer <svg ...> wrapper, <g>, comments,
             * etc.) is silently ignored, per this file's documented
             * subset. */
        }
        p = gt + 1;
    }
}

/* ---- scanline fill (even-odd) + supersampling ------------------------ */

#define SS 4 /* supersample factor */

static uint8_t ss_buf[(SVGICON_MAX_SIZE * SS) * (SVGICON_MAX_SIZE * SS)];

typedef struct { int32_t x0, y0, x1, y1; } edge_t; /* px-Q4 */

#define MAX_EDGES MAX_POINTS
static edge_t g_edges[MAX_EDGES];
static int g_nedges;

/* grid-Q8 -> supersample-pixel-Q4: px_q4 = grid_q8 * out_ss / (24*16).
 * Kept at Q4 (not Q8) specifically so the intersection-test multiply in
 * rasterize_fill() below can't overflow a 32-bit int (see its comment). */
static int32_t grid_to_pxq4(int32_t grid_q8, int32_t out_ss) {
    return (grid_q8 * out_ss) / (24 * 16);
}

static void build_edges(int32_t out_ss) {
    g_nedges = 0;
    for (int s = 0; s < g_nsub; s++) {
        int start = g_sub_start[s];
        int end = (s + 1 < g_nsub) ? g_sub_start[s + 1] : g_npts;
        int n = end - start;
        if (n < 2) continue;
        for (int i = 0; i < n && g_nedges < MAX_EDGES; i++) {
            const ptq8_t *A = &g_pts[start + i];
            const ptq8_t *B = &g_pts[start + (i + 1) % n];
            g_edges[g_nedges].x0 = grid_to_pxq4(A->x, out_ss);
            g_edges[g_nedges].y0 = grid_to_pxq4(A->y, out_ss);
            g_edges[g_nedges].x1 = grid_to_pxq4(B->x, out_ss);
            g_edges[g_nedges].y1 = grid_to_pxq4(B->y, out_ss);
            g_nedges++;
        }
    }
}

/* Fills ss_buf (out_ss x out_ss, 0/255 coverage) from g_pts/g_nsub via a
 * standard even-odd scanline pass: for each supersample row, intersect
 * every non-horizontal edge at the row's pixel-center y, sort the hits,
 * and fill alternating spans between them. */
static void rasterize_fill(int32_t out_ss) {
    build_edges(out_ss);
    memset(ss_buf, 0, (size_t)(out_ss * out_ss));

    int32_t xs[128];
    for (int32_t row = 0; row < out_ss; row++) {
        int32_t yc = row * 16 + 8; /* px-Q4 pixel-center sample point */
        int nx = 0;
        for (int e = 0; e < g_nedges; e++) {
            int32_t y0 = g_edges[e].y0, y1 = g_edges[e].y1;
            if (y0 == y1) continue;
            int32_t ylo = y0 < y1 ? y0 : y1;
            int32_t yhi = y0 < y1 ? y1 : y0;
            if (yc < ylo || yc >= yhi) continue;

            int32_t x0 = g_edges[e].x0, x1 = g_edges[e].x1;
            /* (x1-x0) and (yc-y0) are both bounded by the px-Q4 canvas
             * extent (<= SVGICON_MAX_SIZE*SS*16 = 4096), so this product
             * stays comfortably inside int32 (<= ~16.7M). */
            int32_t xq4 = x0 + ((x1 - x0) * (yc - y0)) / (y1 - y0);
            if (nx < 128) xs[nx++] = xq4;
        }
        for (int i = 1; i < nx; i++) {
            int32_t v = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = v;
        }
        for (int k = 0; k + 1 < nx; k += 2) {
            int32_t xstart = xs[k] / 16;
            int32_t xend = xs[k + 1] / 16;
            if (xstart < 0) xstart = 0;
            if (xend > out_ss) xend = out_ss;
            for (int32_t c = xstart; c < xend; c++) ss_buf[row * out_ss + c] = 255;
        }
    }
}

static void downsample(uint8_t *out_alpha, int out_size) {
    int32_t out_ss = out_size * SS;
    for (int y = 0; y < out_size; y++) {
        for (int x = 0; x < out_size; x++) {
            int sum = 0;
            for (int dy = 0; dy < SS; dy++) {
                const uint8_t *row = &ss_buf[(y * SS + dy) * out_ss + x * SS];
                for (int dx = 0; dx < SS; dx++) sum += row[dx];
            }
            out_alpha[y * out_size + x] = (uint8_t)(sum / (SS * SS));
        }
    }
}

void svgicon_rasterize(const char *svg_src, uint8_t *out_alpha, int out_size) {
    if (!svg_src || !out_alpha || out_size <= 0) return;
    if (out_size > SVGICON_MAX_SIZE) out_size = SVGICON_MAX_SIZE;
    parse_svg(svg_src);
    rasterize_fill(out_size * SS);
    downsample(out_alpha, out_size);
}

/* ---- built-in icon designs (24x24 grid, ~2 unit margins) -------------- */

/* ICON_ABOUT: classic "i" info glyph -- a dot and a rounded stem below
 * it, kept as two simple filled shapes (no ring needed; the dock/
 * titlebar badge already supplies a colored background around this). */
static const char SVG_ABOUT[] =
    "<svg>"
    "<circle cx=\"12\" cy=\"7\" r=\"2.5\"/>"
    "<rect x=\"9.5\" y=\"10.5\" width=\"5\" height=\"9\" rx=\"2.5\"/>"
    "</svg>";

/* ICON_SYSTEM: a rounded-rect chip body with 8 pins, 2 per edge. */
static const char SVG_SYSTEM[] =
    "<svg>"
    "<rect x=\"6\" y=\"6\" width=\"12\" height=\"12\" rx=\"2\"/>"
    "<rect x=\"9\" y=\"3\" width=\"1.8\" height=\"3\"/>"
    "<rect x=\"13.2\" y=\"3\" width=\"1.8\" height=\"3\"/>"
    "<rect x=\"9\" y=\"18\" width=\"1.8\" height=\"3\"/>"
    "<rect x=\"13.2\" y=\"18\" width=\"1.8\" height=\"3\"/>"
    "<rect x=\"3\" y=\"9\" width=\"3\" height=\"1.8\"/>"
    "<rect x=\"3\" y=\"13.2\" width=\"3\" height=\"1.8\"/>"
    "<rect x=\"18\" y=\"9\" width=\"3\" height=\"1.8\"/>"
    "<rect x=\"18\" y=\"13.2\" width=\"3\" height=\"1.8\"/>"
    "</svg>";

/* ICON_ROADMAP: a flag -- vertical pole plus a triangular flag. */
static const char SVG_ROADMAP[] =
    "<svg>"
    "<rect x=\"6\" y=\"3\" width=\"2\" height=\"18\"/>"
    "<polygon points=\"8,4 19,7.5 8,11\"/>"
    "</svg>";

/* ICON_PROCESS: 5 vertical bars of varying height, bottom-aligned --
 * a bar-chart / activity-monitor glyph. */
static const char SVG_PROCESS[] =
    "<svg>"
    "<rect x=\"4\" y=\"14\" width=\"2.6\" height=\"6\"/>"
    "<rect x=\"7.2\" y=\"10\" width=\"2.6\" height=\"10\"/>"
    "<rect x=\"10.4\" y=\"6\" width=\"2.6\" height=\"14\"/>"
    "<rect x=\"13.6\" y=\"11\" width=\"2.6\" height=\"9\"/>"
    "<rect x=\"16.8\" y=\"8\" width=\"2.6\" height=\"12\"/>"
    "</svg>";

/* ICON_NETWORK: a signal-strength glyph -- a dot plus 3 ascending bars
 * (fewer bars than ICON_PROCESS, and ascending rather than varying, so
 * it reads as "signal" rather than "activity"). */
static const char SVG_NETWORK[] =
    "<svg>"
    "<circle cx=\"4.5\" cy=\"19\" r=\"1.6\"/>"
    "<rect x=\"8\" y=\"15\" width=\"2.4\" height=\"5\"/>"
    "<rect x=\"11.4\" y=\"11\" width=\"2.4\" height=\"9\"/>"
    "<rect x=\"14.8\" y=\"7\" width=\"2.4\" height=\"13\"/>"
    "</svg>";

/* ICON_FILES: a folder -- a back rect plus a smaller tab rect flush
 * against its top edge (touching, not overlapping, so even-odd fills
 * both solid rather than punching a hole where they'd otherwise overlap). */
static const char SVG_FILES[] =
    "<svg>"
    "<rect x=\"3\" y=\"8\" width=\"18\" height=\"12\"/>"
    "<rect x=\"3\" y=\"5\" width=\"8\" height=\"3\"/>"
    "</svg>";

/* ICON_BROWSER: a globe -- a circle plus a horizontal "equator" lens and
 * a vertical "meridian" lens, each built from two cubic beziers and
 * touching the circle exactly at its poles, so even-odd punches them out
 * as thin cutout lines across the solid disc (the only way to render
 * contrasting "lines" with a single alpha-mask silhouette). */
static const char SVG_BROWSER[] =
    "<svg>"
    "<circle cx=\"12\" cy=\"12\" r=\"9\"/>"
    "<path d=\"M3,12 C7,9.3 17,9.3 21,12 C17,14.7 7,14.7 3,12 Z\"/>"
    "<path d=\"M12,3 C9.3,7 9.3,17 12,21 C14.7,17 14.7,7 12,3 Z\"/>"
    "</svg>";

/* ICON_TERMINAL: the classic prompt glyph -- a thick ">" chevron plus a
 * short underscore. */
static const char SVG_TERMINAL[] =
    "<svg>"
    "<polygon points=\"6,3 15,11 6,19 8.7,19 17.7,11 8.7,3\"/>"
    "<rect x=\"7\" y=\"20.5\" width=\"10\" height=\"1.8\"/>"
    "</svg>";

static const char *const ICON_SVG[ICON_COUNT] = {
    SVG_ABOUT, SVG_SYSTEM, SVG_ROADMAP, SVG_PROCESS,
    SVG_NETWORK, SVG_FILES, SVG_BROWSER, SVG_TERMINAL,
};

/* ---- cache + public draw API ------------------------------------------ */

static uint8_t icon_cache[ICON_COUNT][SVGICON_CACHE_SIZE * SVGICON_CACHE_SIZE];
static int icon_cache_ready;

void svgicon_init(void) {
    for (int i = 0; i < ICON_COUNT; i++) {
        svgicon_rasterize(ICON_SVG[i], icon_cache[i], SVGICON_CACHE_SIZE);
    }
    icon_cache_ready = 1;
}

const uint8_t *svgicon_get_mask(enum svg_icon_id id, int *size_out) {
    if (!icon_cache_ready || id < 0 || id >= ICON_COUNT) {
        if (size_out) *size_out = 0;
        return NULL;
    }
    if (size_out) *size_out = SVGICON_CACHE_SIZE;
    return icon_cache[id];
}

void svgicon_draw(enum svg_icon_id id, int x, int y, int size, uint32_t color, uint8_t alpha_scale) {
    if (!icon_cache_ready || id < 0 || id >= ICON_COUNT || size <= 0) return;
    const uint8_t *mask = icon_cache[id];
    for (int dy = 0; dy < size; dy++) {
        int sy = dy * SVGICON_CACHE_SIZE / size;
        const uint8_t *row = &mask[sy * SVGICON_CACHE_SIZE];
        for (int dx = 0; dx < size; dx++) {
            int sx = dx * SVGICON_CACHE_SIZE / size;
            uint8_t a = row[sx];
            if (a == 0) continue;
            uint8_t final_a = (uint8_t)(((int)a * (int)alpha_scale) / 255);
            fb_blend_pixel(x + dx, y + dy, color, final_a);
        }
    }
}
