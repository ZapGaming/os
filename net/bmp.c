#include <net/bmp.h>
#include <kernel/kheap.h>

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static int32_t rd_i32(const uint8_t *p) {
    return (int32_t)rd_u32(p);
}

int bmp_decode(const uint8_t *data, uint32_t len, struct bmp_image *out) {
    if (len < 54 || data[0] != 'B' || data[1] != 'M') return 0;

    uint32_t pixel_offset = rd_u32(data + 10);
    uint32_t header_size = rd_u32(data + 14);
    if (header_size != 40) return 0; /* only BITMAPINFOHEADER */

    int32_t width = rd_i32(data + 18);
    int32_t height_raw = rd_i32(data + 22);
    uint16_t bpp = rd_u16(data + 28);
    uint32_t compression = rd_u32(data + 30);

    if (width <= 0 || width > BMP_MAX_DIMENSION) return 0;
    int top_down = height_raw < 0;
    int32_t height = top_down ? -height_raw : height_raw;
    if (height <= 0 || height > BMP_MAX_DIMENSION) return 0;
    if (compression != 0) return 0; /* BI_RGB only, no RLE/BITFIELDS */
    if (bpp != 24 && bpp != 32) return 0;

    uint32_t bytes_per_px = bpp / 8;
    uint32_t row_stride = ((uint32_t)width * bytes_per_px + 3u) & ~3u;
    uint64_t needed = (uint64_t)pixel_offset + (uint64_t)row_stride * (uint64_t)height;
    if (needed > len) return 0;

    uint32_t *pixels = kmalloc(sizeof(uint32_t) * (uint32_t)width * (uint32_t)height);
    if (!pixels) return 0;

    for (int32_t y = 0; y < height; y++) {
        int32_t src_row = top_down ? y : (height - 1 - y);
        const uint8_t *row = data + pixel_offset + (uint32_t)src_row * row_stride;
        uint32_t *dst = pixels + (uint32_t)y * (uint32_t)width;
        for (int32_t x = 0; x < width; x++) {
            const uint8_t *px = row + (uint32_t)x * bytes_per_px;
            uint8_t b = px[0], g = px[1], r = px[2];
            dst[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    out->width = width;
    out->height = height;
    out->pixels = pixels;
    return 1;
}

void bmp_free(struct bmp_image *img) {
    if (img->pixels) kfree(img->pixels);
    img->pixels = NULL;
    img->width = img->height = 0;
}
