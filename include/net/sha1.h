#ifndef NET_SHA1_H
#define NET_SHA1_H

#include <stdint.h>

/* SHA-1, FIPS 180-4. Pure integer implementation (32-bit words only --
 * no floating point, matching every other primitive in this directory).
 * SHA-1 is cryptographically broken for signature/collision-resistance
 * purposes (hence net/tls.c and friends use SHA-256), but it's still
 * exactly what RFC 6455's WebSocket handshake requires for computing
 * Sec-WebSocket-Accept, so it's added here purely for that -- not a
 * general-purpose hash for anything security-sensitive. */

#define SHA1_DIGEST_SIZE 20
#define SHA1_BLOCK_SIZE  64

typedef struct {
    uint32_t state[5];
    uint64_t total_len;      /* total bytes absorbed so far */
    uint8_t  buf[SHA1_BLOCK_SIZE];
    uint32_t buf_len;        /* bytes currently held in buf */
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const void *data, uint32_t len);
void sha1_final(sha1_ctx *ctx, uint8_t out[SHA1_DIGEST_SIZE]);

/* One-shot convenience wrapper around init/update/final. */
void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST_SIZE]);

#endif
