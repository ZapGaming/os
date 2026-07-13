#include <net/gcm.h>
#include <string.h>

/* AES-128-GCM per NIST SP 800-38D, restricted to the 96-bit IV case
 * (the only one RFC 5288/TLS 1.2 ever uses). GHASH is implemented with
 * the textbook shift-and-xor GF(2^128) multiplication (Algorithm 1 in
 * SP 800-38D): blocks are bit strings with bit 0 = the MSB of byte 0,
 * and the reduction constant R = 11100001 followed by 120 zero bits
 * (i.e. byte 0 = 0xE1, bytes 1..15 = 0). Not constant-time (a table-
 * free bit-at-a-time multiply branches on both the message bit and the
 * running value's low bit) -- fine given this subsystem's documented
 * "interoperability, not security" scope; the tag is still always
 * checked, which is what actually matters for correctness. */

static void block_xor(uint8_t r[16], const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; i++) r[i] = (uint8_t)(a[i] ^ b[i]);
}

/* Z = X . Y in GF(2^128) under the GCM reduction polynomial. Safe to
 * call with Z aliasing X and/or Y (X is copied up front; Y is copied
 * into V before Z is ever written). */
static void gf_mult(uint8_t Z[16], const uint8_t X[16], const uint8_t Y[16]) {
    uint8_t Xc[16];
    memcpy(Xc, X, 16);
    uint8_t V[16];
    memcpy(V, Y, 16);
    memset(Z, 0, 16);

    for (int i = 0; i < 128; i++) {
        int byte_i = i / 8;
        int bit_i = 7 - (i % 8); /* bit 0 of the block is the MSB of byte 0 */
        if ((Xc[byte_i] >> bit_i) & 1) {
            block_xor(Z, Z, V);
        }
        int lsb = V[15] & 1; /* "rightmost bit" per the spec's bit numbering */
        for (int j = 15; j > 0; j--) {
            V[j] = (uint8_t)((V[j] >> 1) | ((V[j - 1] & 1) << 7));
        }
        V[0] = (uint8_t)(V[0] >> 1);
        if (lsb) V[0] ^= 0xE1;
    }
}

/* GHASH_H(A || C) with the standard zero-padding of the final partial
 * block of each of A and C, followed by the 64-bit-length||64-bit-length
 * block, per SP 800-38D section 6.4. */
static void ghash(uint8_t out[16], const uint8_t H[16],
                   const uint8_t *aad, uint32_t aad_len,
                   const uint8_t *c, uint32_t c_len) {
    uint8_t Y[16];
    memset(Y, 0, 16);

    uint32_t off = 0;
    while (off < aad_len) {
        uint8_t block[16];
        memset(block, 0, 16);
        uint32_t chunk = (aad_len - off < 16) ? (aad_len - off) : 16;
        memcpy(block, aad + off, chunk);
        block_xor(Y, Y, block);
        gf_mult(Y, Y, H);
        off += chunk;
    }

    off = 0;
    while (off < c_len) {
        uint8_t block[16];
        memset(block, 0, 16);
        uint32_t chunk = (c_len - off < 16) ? (c_len - off) : 16;
        memcpy(block, c + off, chunk);
        block_xor(Y, Y, block);
        gf_mult(Y, Y, H);
        off += chunk;
    }

    uint8_t lenblock[16];
    uint64_t aad_bits = (uint64_t)aad_len * 8;
    uint64_t c_bits = (uint64_t)c_len * 8;
    for (int i = 0; i < 8; i++) lenblock[i] = (uint8_t)(aad_bits >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) lenblock[8 + i] = (uint8_t)(c_bits >> (56 - 8 * i));
    block_xor(Y, Y, lenblock);
    gf_mult(Y, Y, H);

    memcpy(out, Y, 16);
}

/* 32-bit big-endian increment of just the last 4 bytes of a 16-byte
 * counter block (SP 800-38D's incr_32, wrapping mod 2^32). */
static void inc32(uint8_t block[16]) {
    uint32_t ctr = ((uint32_t)block[12] << 24) | ((uint32_t)block[13] << 16) |
                   ((uint32_t)block[14] << 8) | (uint32_t)block[15];
    ctr++;
    block[12] = (uint8_t)(ctr >> 24);
    block[13] = (uint8_t)(ctr >> 16);
    block[14] = (uint8_t)(ctr >> 8);
    block[15] = (uint8_t)ctr;
}

static void gctr_xor(const aes128_ctx *aes, uint8_t counter[16],
                      const uint8_t *in, uint32_t len, uint8_t *out) {
    uint32_t off = 0;
    while (off < len) {
        uint8_t keystream[16];
        aes128_encrypt_block(aes, counter, keystream);
        uint32_t chunk = (len - off < 16) ? (len - off) : 16;
        for (uint32_t i = 0; i < chunk; i++) out[off + i] = (uint8_t)(in[off + i] ^ keystream[i]);
        inc32(counter);
        off += chunk;
    }
}

/* J0 = IV || 0^31 || 1, per SP 800-38D 7.1's 96-bit-IV special case. */
static void make_j0(uint8_t j0[16], const uint8_t iv[GCM_IV_SIZE]) {
    memcpy(j0, iv, GCM_IV_SIZE);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
}

void gcm_encrypt(const aes128_ctx *aes, const uint8_t iv[GCM_IV_SIZE],
                  const uint8_t *aad, uint32_t aad_len,
                  const uint8_t *plaintext, uint32_t len,
                  uint8_t *ciphertext, uint8_t tag[GCM_TAG_SIZE]) {
    uint8_t zero[16];
    memset(zero, 0, 16);
    uint8_t H[16];
    aes128_encrypt_block(aes, zero, H);

    uint8_t j0[16];
    make_j0(j0, iv);

    uint8_t counter[16];
    memcpy(counter, j0, 16);
    inc32(counter); /* encryption keystream starts at inc32(J0) */
    gctr_xor(aes, counter, plaintext, len, ciphertext);

    uint8_t S[16];
    ghash(S, H, aad, aad_len, ciphertext, len);

    uint8_t E[16];
    aes128_encrypt_block(aes, j0, E);
    for (int i = 0; i < GCM_TAG_SIZE; i++) tag[i] = (uint8_t)(E[i] ^ S[i]);
}

int gcm_decrypt(const aes128_ctx *aes, const uint8_t iv[GCM_IV_SIZE],
                 const uint8_t *aad, uint32_t aad_len,
                 const uint8_t *ciphertext, uint32_t len,
                 uint8_t *plaintext, const uint8_t tag[GCM_TAG_SIZE]) {
    uint8_t zero[16];
    memset(zero, 0, 16);
    uint8_t H[16];
    aes128_encrypt_block(aes, zero, H);

    uint8_t j0[16];
    make_j0(j0, iv);

    uint8_t S[16];
    ghash(S, H, aad, aad_len, ciphertext, len);

    uint8_t E[16];
    aes128_encrypt_block(aes, j0, E);
    uint8_t expected_tag[GCM_TAG_SIZE];
    for (int i = 0; i < GCM_TAG_SIZE; i++) expected_tag[i] = (uint8_t)(E[i] ^ S[i]);

    uint8_t diff = 0;
    for (int i = 0; i < GCM_TAG_SIZE; i++) diff |= (uint8_t)(expected_tag[i] ^ tag[i]);

    uint8_t counter[16];
    memcpy(counter, j0, 16);
    inc32(counter);
    gctr_xor(aes, counter, ciphertext, len, plaintext);

    return diff == 0;
}
