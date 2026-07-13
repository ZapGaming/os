#ifndef NET_SHA256_H
#define NET_SHA256_H

#include <stdint.h>

/* SHA-256, FIPS 180-4. Pure integer implementation (32-bit words only --
 * no floating point, matching every other primitive in this directory). */

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

typedef struct {
    uint32_t state[8];
    uint64_t total_len;      /* total bytes absorbed so far */
    uint8_t  buf[SHA256_BLOCK_SIZE];
    uint32_t buf_len;        /* bytes currently held in buf */
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, uint32_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot convenience wrapper around init/update/final. */
void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST_SIZE]);

#endif
