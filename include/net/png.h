#ifndef NET_PNG_H
#define NET_PNG_H

#include <stdint.h>
#include <net/bmp.h>

/* Decodes a PNG file into `out`, same struct/ownership convention as
 * bmp_decode() (kmalloc'd 0xRRGGBB pixels, top-to-bottom -- free with
 * bmp_free()). Scope: 8-bit depth only (16-bit rejected), non-
 * interlaced only (Adam7 rejected), color types 0/2/3/4/6 (grayscale,
 * RGB, palette+PLTE, grayscale+alpha, RGBA). Alpha (color type 4/6, or
 * a palette's tRNS) is composited against opaque white rather than
 * kept -- this codebase's pixel format has no alpha channel, same
 * scope cut BMP decoding already makes for anything beyond BI_RGB.
 * Chunk CRCs are not verified (consistent with this codebase's gzip
 * decoder also not verifying its own trailing CRC32 -- a corrupt image
 * fails to decode cleanly either way, just not via a checksum check).
 * Returns 0 for anything outside that scope, 1 on success. */
int png_decode(const uint8_t *data, uint32_t len, struct bmp_image *out);

#endif
