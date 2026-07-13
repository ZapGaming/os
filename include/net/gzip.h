#ifndef NET_GZIP_H
#define NET_GZIP_H

#include <stdint.h>

/* From-scratch DEFLATE (RFC 1951) decoder -- stored, fixed-Huffman, and
 * dynamic-Huffman blocks. Decodes into a caller-provided buffer, same
 * bounded-output convention as http.c's decode_chunked(): fills up to
 * out_cap bytes and stops there rather than overflowing, even if the
 * compressed stream has more to give. Returns bytes written, or 0 on a
 * malformed stream. */
uint32_t inflate_raw(const uint8_t *src, uint32_t src_len, uint8_t *out, uint32_t out_cap);

/* RFC 1952 gzip wrapper: skips the 10-byte header (plus optional
 * FEXTRA/FNAME/FCOMMENT/FHCRC fields per the flags byte) and hands the
 * rest to inflate_raw(). Does not verify the trailing CRC32/ISIZE.
 * Returns 1 on success (even if output got truncated at out_cap), 0 if
 * the header itself is malformed. */
int gzip_decompress(const uint8_t *src, uint32_t src_len, uint8_t *out, uint32_t out_cap, uint32_t *out_len);

/* "Content-Encoding: deflate" -- inconsistently either a raw DEFLATE
 * stream or one wrapped in a 2-byte zlib (RFC 1950) header; detects
 * which and skips it if present, then otherwise behaves like
 * inflate_raw(). Returns 1 on success (even if truncated at out_cap). */
int deflate_decompress(const uint8_t *src, uint32_t src_len, uint8_t *out, uint32_t out_cap, uint32_t *out_len);

#endif
