#ifndef GUI_FRAMEBUFFER_H
#define GUI_FRAMEBUFFER_H

#include <stdint.h>

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

void fb_init(uint32_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp);

uint32_t fb_width(void);
uint32_t fb_height(void);

void fb_put_pixel(int x, int y, uint32_t color);
uint32_t fb_get_pixel(int x, int y);

/* alpha in [0,255]; blends `color` over whatever is already at (x,y) */
void fb_blend_pixel(int x, int y, uint32_t color, uint8_t alpha);

void fb_fill_rect(int x, int y, int w, int h, uint32_t color);

/* Nearest-neighbor blit of an RGB pixel buffer (src_w*src_h, top-to-
 * bottom, 0xRRGGBB) into the dest rect (x,y,w,h) -- scaled if w/h don't
 * match src_w/src_h. Per-pixel bounds-checked via fb_put_pixel, same as
 * everything else here, so it clips safely against window edges. */
void fb_blit_rgb(int x, int y, int w, int h, const uint32_t *pixels, int src_w, int src_h);
void fb_draw_rect(int x, int y, int w, int h, uint32_t color);
void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void fb_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);

/* Vertical linear gradient from `top` to `bottom` over the rect. */
void fb_fill_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

void fb_draw_char(int x, int y, char c, uint32_t fg, int scale);
void fb_draw_string(int x, int y, const char *s, uint32_t fg, int scale);
int  fb_text_width(const char *s, int scale);

/* Back buffer support: draw into an off-screen buffer, then blit it to the
 * real framebuffer in one shot to avoid visible tearing/flicker. */
void fb_swap_buffers(void);

#endif
