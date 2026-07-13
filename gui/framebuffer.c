#include <gui/framebuffer.h>
#include <gui/font8x8.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

static uint8_t *fb_mem;
static uint8_t *back_buffer;
static uint32_t fb_pitch;
static uint32_t fb_w, fb_h;
static uint8_t fb_bpp;

void fb_init(uint32_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp) {
    fb_mem   = (uint8_t *)(uintptr_t)addr;
    fb_pitch = pitch;
    fb_w     = width;
    fb_h     = height;
    fb_bpp   = bpp;

    back_buffer = (uint8_t *)kmalloc(pitch * height);
    if (back_buffer) memset(back_buffer, 0, pitch * height);

    serial_printf("fb: %ux%u bpp=%d pitch=%d addr=%x backbuf=%x\n",
                  width, height, bpp, pitch, addr, (uint32_t)(uintptr_t)back_buffer);
}

uint32_t fb_width(void)  { return fb_w; }
uint32_t fb_height(void) { return fb_h; }

void fb_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h || !back_buffer) return;
    uint32_t *px = (uint32_t *)(back_buffer + y * fb_pitch + x * (fb_bpp / 8));
    *px = color;
}

uint32_t fb_get_pixel(int x, int y) {
    if (x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h || !back_buffer) return 0;
    uint32_t *px = (uint32_t *)(back_buffer + y * fb_pitch + x * (fb_bpp / 8));
    return *px & 0xFFFFFF;
}

void fb_blend_pixel(int x, int y, uint32_t color, uint8_t alpha) {
    if (alpha == 0) return;
    if (alpha == 255) { fb_put_pixel(x, y, color); return; }

    uint32_t bg = fb_get_pixel(x, y);
    uint32_t br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    uint32_t fr = (color >> 16) & 0xFF, fg = (color >> 8) & 0xFF, fb_ = color & 0xFF;

    uint32_t r = (fr * alpha + br * (255 - alpha)) / 255;
    uint32_t g = (fg * alpha + bgc * (255 - alpha)) / 255;
    uint32_t b = (fb_ * alpha + bb * (255 - alpha)) / 255;

    fb_put_pixel(x, y, RGB(r, g, b));
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            fb_put_pixel(i, j, color);
        }
    }
}

void fb_blit_rgb(int x, int y, int w, int h, const uint32_t *pixels, int src_w, int src_h) {
    if (!pixels || src_w <= 0 || src_h <= 0 || w <= 0 || h <= 0) return;
    for (int dy = 0; dy < h; dy++) {
        int sy = dy * src_h / h;
        for (int dx = 0; dx < w; dx++) {
            int sx = dx * src_w / w;
            fb_put_pixel(x + dx, y + dy, pixels[sy * src_w + sx]);
        }
    }
}

void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int i = x; i < x + w; i++) {
        fb_put_pixel(i, y, color);
        fb_put_pixel(i, y + h - 1, color);
    }
    for (int j = y; j < y + h; j++) {
        fb_put_pixel(x, j, color);
        fb_put_pixel(x + w - 1, j, color);
    }
}

void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        fb_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void swap_int(int *a, int *b) { int t = *a; *a = *b; *b = t; }

void fb_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    if (y0 > y1) { swap_int(&x0, &x1); swap_int(&y0, &y1); }
    if (y1 > y2) { swap_int(&x1, &x2); swap_int(&y1, &y2); }
    if (y0 > y1) { swap_int(&x0, &x1); swap_int(&y0, &y1); }

    int total_h = y2 - y0;
    if (total_h == 0) return;

    for (int y = y0; y <= y2; y++) {
        int second_half = (y > y1) || (y1 == y0);
        int seg_h = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_h == 0) continue;

        int ax = x0 + (x2 - x0) * (y - y0) / total_h;
        int bx = second_half
                     ? x1 + (x2 - x1) * (y - y1) / seg_h
                     : x0 + (x1 - x0) * (y - y0) / seg_h;

        if (ax > bx) swap_int(&ax, &bx);
        for (int x = ax; x <= bx; x++) fb_put_pixel(x, y, color);
    }
}

void fb_fill_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bottom >> 16) & 0xFF, bg = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;

    for (int j = 0; j < h; j++) {
        int r = tr + (br - tr) * j / (h > 1 ? h - 1 : 1);
        int g = tg + (bg - tg) * j / (h > 1 ? h - 1 : 1);
        int b = tb + (bb - tb) * j / (h > 1 ? h - 1 : 1);
        uint32_t color = RGB(r, g, b);
        for (int i = 0; i < w; i++) {
            fb_put_pixel(x + i, y + j, color);
        }
    }
}

void fb_fill_gradient_h(int x, int y, int w, int h, uint32_t left, uint32_t right) {
    int lr = (left >> 16) & 0xFF, lg = (left >> 8) & 0xFF, lb = left & 0xFF;
    int rr = (right >> 16) & 0xFF, rg = (right >> 8) & 0xFF, rb = right & 0xFF;

    for (int i = 0; i < w; i++) {
        int r = lr + (rr - lr) * i / (w > 1 ? w - 1 : 1);
        int g = lg + (rg - lg) * i / (w > 1 ? w - 1 : 1);
        int b = lb + (rb - lb) * i / (w > 1 ? w - 1 : 1);
        uint32_t color = RGB(r, g, b);
        for (int j = 0; j < h; j++) {
            fb_put_pixel(x + i, y + j, color);
        }
    }
}

/* Filled rectangle with uniformly rounded corners -- row by row, inset
 * from the left/right edges near the top and bottom rows by however
 * far the circle of radius `radius` centered on that corner has
 * curved in by that row (integer approximation of sqrt, no float math
 * anywhere in this kernel). Clamps `radius` to at most half of
 * whichever of w/h is smaller, same as real CSS border-radius. */
void fb_fill_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color) {
    if (radius <= 0 || w <= 0 || h <= 0) { fb_fill_rect(x, y, w, h, color); return; }
    int max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;

    for (int j = 0; j < h; j++) {
        int inset = 0;
        int dy = -1;
        if (j < radius) dy = radius - 1 - j;
        else if (j >= h - radius) dy = j - (h - radius);
        if (dy >= 0) {
            /* Largest dx with dx^2 + dy^2 <= radius^2 -- an integer
             * sqrt via linear search down from the top, always at
             * most `radius` iterations (a handful of pixels for
             * anything this browser renders). */
            int dx = radius;
            while (dx > 0 && dx * dx + dy * dy > radius * radius) dx--;
            inset = radius - dx;
        }
        fb_fill_rect(x + inset, y + j, w - 2 * inset, 1, color);
    }
}

void fb_draw_char(int x, int y, char c, uint32_t fg, int scale) {
    if ((unsigned char)c >= 128) return;
    if (scale < 1) scale = 1;
    const uint8_t *glyph = font8x8_basic[(int)c];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                if (scale == 1) {
                    fb_put_pixel(x + col, y + row, fg);
                } else {
                    fb_fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
                }
            }
        }
    }
}

void fb_draw_string(int x, int y, const char *s, uint32_t fg, int scale) {
    int cx = x;
    int step = 8 * (scale < 1 ? 1 : scale);
    while (*s) {
        if (*s == '\n') {
            cx = x;
            y += step;
        } else {
            fb_draw_char(cx, y, *s, fg, scale);
            cx += step;
        }
        s++;
    }
}

int fb_text_width(const char *s, int scale) {
    int step = 8 * (scale < 1 ? 1 : scale);
    int len = 0;
    while (*s) { len++; s++; }
    return len * step;
}

void fb_swap_buffers(void) {
    if (!back_buffer || !fb_mem) return;
    memcpy(fb_mem, back_buffer, fb_pitch * fb_h);
}
