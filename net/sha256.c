#include <net/sha256.h>
#include <string.h>

/* FIPS 180-4 SHA-256 round constants (first 32 bits of the fractional
 * parts of the cube roots of the first 64 primes). */
static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

/* Processes exactly one 64-byte block, updating ctx->state. */
static void sha256_process_block(sha256_ctx *ctx, const uint8_t block[SHA256_BLOCK_SIZE]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

void sha256_update(sha256_ctx *ctx, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->total_len += len;

    if (ctx->buf_len > 0) {
        uint32_t need = SHA256_BLOCK_SIZE - ctx->buf_len;
        uint32_t take = len < need ? len : need;
        memcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += take;
        p += take;
        len -= take;
        if (ctx->buf_len == SHA256_BLOCK_SIZE) {
            sha256_process_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }

    while (len >= SHA256_BLOCK_SIZE) {
        sha256_process_block(ctx, p);
        p += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
    }

    if (len > 0) {
        memcpy(ctx->buf, p, len);
        ctx->buf_len = len;
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]) {
    uint64_t bit_len = ctx->total_len * 8;

    /* Append the mandatory 0x80 byte directly into buf (not via
     * sha256_update(), which would double-count it in total_len). */
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len == SHA256_BLOCK_SIZE) {
        sha256_process_block(ctx, ctx->buf);
        ctx->buf_len = 0;
    }

    /* Zero-pad up to a 56-byte boundary (leaving 8 bytes for the
     * length), processing a block in between if the 0x80 byte above
     * already left less than 8 bytes of room in the current block. */
    if (ctx->buf_len > 56) {
        memset(ctx->buf + ctx->buf_len, 0, SHA256_BLOCK_SIZE - ctx->buf_len);
        sha256_process_block(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    memset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);

    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[i] = (uint8_t)(bit_len >> (56 - 8 * i));
    memcpy(ctx->buf + 56, len_bytes, 8);
    sha256_process_block(ctx, ctx->buf);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST_SIZE]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}
