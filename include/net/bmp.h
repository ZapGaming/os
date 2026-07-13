#ifndef NET_BMP_H
#define NET_BMP_H

#include <stdint.h>

/* Clamp so a hostile or just-huge image can't blow the kernel heap. Kept
 * modest (not just "big but bounded") because decoded pixels stay
 * resident for as long as the page is displayed, on top of everything
 * else already sharing the 8MB kheap arena -- fine for the icons/photos
 * an ordinary page inlines, not meant for a full-screen hero image. */
#define BMP_MAX_DIMENSION 256

struct bmp_image {
    int width, height;
    uint32_t *pixels; /* kmalloc'd, width*height, top-to-bottom, 0xRRGGBB */
};

/* Decodes a BMP file from `data` (length `len`) into `out`. Only the
 * common uncompressed case is supported: a BITMAPFILEHEADER followed by
 * a 40-byte BITMAPINFOHEADER with 24 or 32 bits/pixel and BI_RGB (no
 * compression, no color table, no OS/2 variants). That covers ordinary
 * web images saved as .bmp; anything else (RLE, 16-bit, indexed-color,
 * or of course any other image format entirely -- JPEG/PNG/GIF need a
 * real decompressor this kernel doesn't have) is rejected rather than
 * misread. Returns 1 on success (out->pixels is then kmalloc'd -- free
 * with bmp_free()), 0 on any format it doesn't handle. */
int bmp_decode(const uint8_t *data, uint32_t len, struct bmp_image *out);
void bmp_free(struct bmp_image *img);

#endif
