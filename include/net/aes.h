#ifndef NET_AES_H
#define NET_AES_H

#include <stdint.h>

/* AES-128 (FIPS-197), forward (encrypt) direction only -- GCM only ever
 * needs AES-encrypt of counter blocks, in both the encrypt and decrypt
 * direction of the *record*, so no InvSubBytes/inverse key schedule
 * exists here at all. */

#define AES128_KEY_SIZE   16
#define AES128_BLOCK_SIZE 16
#define AES128_ROUNDS     10

typedef struct {
    /* 11 round keys of 4 32-bit words each (Nr+1 = 11 for AES-128). */
    uint32_t round_key[AES128_ROUNDS + 1][4];
} aes128_ctx;

void aes128_init(aes128_ctx *ctx, const uint8_t key[AES128_KEY_SIZE]);

/* Encrypts exactly one 16-byte block. in/out may alias. */
void aes128_encrypt_block(const aes128_ctx *ctx, const uint8_t in[AES128_BLOCK_SIZE],
                           uint8_t out[AES128_BLOCK_SIZE]);

#endif
