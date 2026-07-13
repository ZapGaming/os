#include <net/png.h>
#include <net/gzip.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Undoes one of PNG's five per-scanline filters (RFC 2083 6.2) in
 * place, byte by byte. `bpp` is bytes-per-pixel for the byte-distance
 * arguments (Sub/Paeth look one full pixel to the left; Up/Paeth look
 * at the previous decoded row) -- always equal to `channels` in this
 * decoder's 8-bit-depth-only scope, never fractional. */
static void unfilter_row(uint8_t filter, uint8_t *dst, const uint8_t *src,
                          const uint8_t *prev_row, uint32_t stride, int bpp) {
    for (uint32_t x = 0; x < stride; x++) {
        int a = (x >= (uint32_t)bpp) ? dst[x - bpp] : 0;
        int b = prev_row ? prev_row[x] : 0;
        int c = (prev_row && x >= (uint32_t)bpp) ? prev_row[x - bpp] : 0;
        int raw = src[x];
        int val;
        switch (filter) {
            case 1: val = raw + a; break;
            case 2: val = raw + b; break;
            case 3: val = raw + (a + b) / 2; break;
            case 4: {
                int p = a + b - c;
                int pa = p > a ? p - a : a - p;
                int pb = p > b ? p - b : b - p;
                int pc = p > c ? p - c : c - p;
                int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
                val = raw + pred;
                break;
            }
            default: val = raw; break;
        }
        dst[x] = (uint8_t)val;
    }
}

int png_decode(const uint8_t *data, uint32_t len, struct bmp_image *out) {
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (len < 8 || memcmp(data, sig, 8) != 0) return 0;

    int width = 0, height = 0, bit_depth = 0, color_type = -1;
    uint8_t palette[256 * 3];
    int palette_count = 0;

    uint8_t *idat_buf = NULL;
    uint32_t idat_len = 0, idat_cap = 0;
    int ok = 1;

    uint32_t pos = 8;
    while (pos + 8 <= len) {
        uint32_t chunk_len = be32(data + pos);
        const uint8_t *chunk_type = data + pos + 4;
        const uint8_t *chunk_data = data + pos + 8;
        if ((uint64_t)pos + 8 + (uint64_t)chunk_len + 4 > len) break;

        if (memcmp(chunk_type, "IHDR", 4) == 0) {
            if (chunk_len < 13) { ok = 0; break; }
            width = (int)be32(chunk_data);
            height = (int)be32(chunk_data + 4);
            bit_depth = chunk_data[8];
            color_type = chunk_data[9];
            uint8_t compression = chunk_data[10];
            uint8_t filter_method = chunk_data[11];
            uint8_t interlace = chunk_data[12];
            if (compression != 0 || filter_method != 0 || interlace != 0) {
                serial_printf("png: unsupported (interlaced image or non-standard filter/compression method)\n");
                ok = 0; break;
            }
            if (width <= 0 || height <= 0 || width > BMP_MAX_DIMENSION || height > BMP_MAX_DIMENSION) { ok = 0; break; }
        } else if (memcmp(chunk_type, "PLTE", 4) == 0) {
            palette_count = (int)(chunk_len / 3);
            if (palette_count > 256) palette_count = 256;
            memcpy(palette, chunk_data, (size_t)palette_count * 3);
        } else if (memcmp(chunk_type, "IDAT", 4) == 0) {
            if (idat_len + chunk_len > idat_cap) {
                uint32_t new_cap = idat_cap == 0 ? (chunk_len + 4096) : (idat_cap * 2 + chunk_len);
                uint8_t *nb = (uint8_t *)kmalloc(new_cap);
                if (!nb) { ok = 0; break; }
                if (idat_buf) { memcpy(nb, idat_buf, idat_len); kfree(idat_buf); }
                idat_buf = nb;
                idat_cap = new_cap;
            }
            memcpy(idat_buf + idat_len, chunk_data, chunk_len);
            idat_len += chunk_len;
        } else if (memcmp(chunk_type, "IEND", 4) == 0) {
            pos += 8 + chunk_len + 4;
            break;
        }
        pos += 8 + chunk_len + 4;
    }

    if (!ok || width == 0 || height == 0 || !idat_buf) {
        if (idat_buf) kfree(idat_buf);
        return 0;
    }
    if (bit_depth != 8 ||
        (color_type != 0 && color_type != 2 && color_type != 3 && color_type != 4 && color_type != 6) ||
        (color_type == 3 && palette_count == 0)) {
        kfree(idat_buf);
        return 0;
    }

    int channels = (color_type == 0) ? 1 : (color_type == 2) ? 3 : (color_type == 3) ? 1 : (color_type == 4) ? 2 : 4;
    uint32_t stride = (uint32_t)width * (uint32_t)channels;
    uint32_t raw_size = (stride + 1) * (uint32_t)height;

    uint8_t *raw = (uint8_t *)kmalloc(raw_size);
    if (!raw) { kfree(idat_buf); return 0; }
    uint32_t got = 0;
    deflate_decompress(idat_buf, idat_len, raw, raw_size, &got);
    kfree(idat_buf);
    if (got < raw_size) { kfree(raw); return 0; }

    uint8_t *pixels_raw = (uint8_t *)kmalloc((uint32_t)width * (uint32_t)height * (uint32_t)channels);
    if (!pixels_raw) { kfree(raw); return 0; }

    const uint8_t *prev_row = NULL;
    for (int y = 0; y < height; y++) {
        uint8_t filter = raw[(uint32_t)y * (stride + 1)];
        const uint8_t *src = raw + (uint32_t)y * (stride + 1) + 1;
        uint8_t *dst = pixels_raw + (uint32_t)y * stride;
        unfilter_row(filter, dst, src, prev_row, stride, channels);
        prev_row = dst;
    }
    kfree(raw);

    uint32_t *pixels = (uint32_t *)kmalloc((uint32_t)width * (uint32_t)height * sizeof(uint32_t));
    if (!pixels) { kfree(pixels_raw); return 0; }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const uint8_t *px = pixels_raw + ((uint32_t)y * (uint32_t)width + (uint32_t)x) * (uint32_t)channels;
            uint8_t r, g, b;
            if (color_type == 0) {
                r = g = b = px[0];
            } else if (color_type == 2) {
                r = px[0]; g = px[1]; b = px[2];
            } else if (color_type == 3) {
                uint8_t idx = px[0];
                if (idx < palette_count) { r = palette[idx * 3]; g = palette[idx * 3 + 1]; b = palette[idx * 3 + 2]; }
                else { r = g = b = 0; }
            } else if (color_type == 4) {
                int gray = px[0], alpha = px[1];
                r = g = b = (uint8_t)((gray * alpha + 255 * (255 - alpha) + 127) / 255);
            } else {
                int rr = px[0], gg = px[1], bb = px[2], alpha = px[3];
                r = (uint8_t)((rr * alpha + 255 * (255 - alpha) + 127) / 255);
                g = (uint8_t)((gg * alpha + 255 * (255 - alpha) + 127) / 255);
                b = (uint8_t)((bb * alpha + 255 * (255 - alpha) + 127) / 255);
            }
            pixels[(uint32_t)y * (uint32_t)width + (uint32_t)x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    kfree(pixels_raw);

    out->width = width;
    out->height = height;
    out->pixels = pixels;
    return 1;
}
