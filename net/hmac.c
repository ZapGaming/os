#include <net/hmac.h>
#include <net/sha256.h>
#include <string.h>

/* RFC 2104 HMAC, instantiated with SHA-256 (block size 64 bytes). */
void hmac_sha256(const uint8_t *key, uint32_t key_len,
                  const uint8_t *data, uint32_t data_len,
                  uint8_t out[32]) {
    uint8_t key_block[SHA256_BLOCK_SIZE];
    memset(key_block, 0, sizeof(key_block));

    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, key_block); /* digest shrinks it to 32 bytes */
    } else {
        memcpy(key_block, key, key_len);
    }

    uint8_t ipad[SHA256_BLOCK_SIZE], opad[SHA256_BLOCK_SIZE];
    for (uint32_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = (uint8_t)(key_block[i] ^ 0x36);
        opad[i] = (uint8_t)(key_block[i] ^ 0x5c);
    }

    uint8_t inner_digest[SHA256_DIGEST_SIZE];
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner_digest);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, inner_digest, sizeof(inner_digest));
    sha256_final(&ctx, out);
}
