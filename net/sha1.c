#include <net/sha1.h>
#include <string.h>

static inline uint32_t rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32 - n));
}

/* Processes exactly one 64-byte block, updating ctx->state. Structure
 * mirrors net/sha256.c's sha256_process_block() -- same buffering
 * scheme, just a different message schedule and round function, per
 * FIPS 180-4's SHA-1 definition. */
static void sha1_process_block(sha1_ctx *ctx, const uint8_t block[SHA1_BLOCK_SIZE]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2];
    uint32_t d = ctx->state[3], e = ctx->state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }

        uint32_t temp = rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl(b, 30); b = a; a = temp;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

void sha1_init(sha1_ctx *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

void sha1_update(sha1_ctx *ctx, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->total_len += len;

    if (ctx->buf_len > 0) {
        uint32_t need = SHA1_BLOCK_SIZE - ctx->buf_len;
        uint32_t take = len < need ? len : need;
        memcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += take;
        p += take;
        len -= take;
        if (ctx->buf_len == SHA1_BLOCK_SIZE) {
            sha1_process_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }

    while (len >= SHA1_BLOCK_SIZE) {
        sha1_process_block(ctx, p);
        p += SHA1_BLOCK_SIZE;
        len -= SHA1_BLOCK_SIZE;
    }

    if (len > 0) {
        memcpy(ctx->buf, p, len);
        ctx->buf_len = len;
    }
}

void sha1_final(sha1_ctx *ctx, uint8_t out[SHA1_DIGEST_SIZE]) {
    uint64_t bit_len = ctx->total_len * 8;

    /* Append the mandatory 0x80 byte directly into buf (not via
     * sha1_update(), which would double-count it in total_len). */
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len == SHA1_BLOCK_SIZE) {
        sha1_process_block(ctx, ctx->buf);
        ctx->buf_len = 0;
    }

    /* Zero-pad up to a 56-byte boundary (leaving 8 bytes for the
     * length), processing a block in between if the 0x80 byte above
     * already left less than 8 bytes of room in the current block. */
    if (ctx->buf_len > 56) {
        memset(ctx->buf + ctx->buf_len, 0, SHA1_BLOCK_SIZE - ctx->buf_len);
        sha1_process_block(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    memset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);

    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[i] = (uint8_t)(bit_len >> (56 - 8 * i));
    memcpy(ctx->buf + 56, len_bytes, 8);
    sha1_process_block(ctx, ctx->buf);

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST_SIZE]) {
    sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}
