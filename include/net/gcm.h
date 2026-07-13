#ifndef NET_GCM_H
#define NET_GCM_H

#include <stdint.h>
#include <net/aes.h>

/* AES-128-GCM (NIST SP 800-38D). 12-byte (96-bit) IV only -- the one
 * size TLS 1.2 (RFC 5288) ever uses, so no support for the generic
 * "IV via GHASH" path SP 800-38D defines for other IV lengths. */
#define GCM_IV_SIZE  12
#define GCM_TAG_SIZE 16

/* Encrypts `len` bytes of `plaintext` into `ciphertext` (may alias) and
 * produces a 16-byte authentication tag over `aad`+ciphertext. */
void gcm_encrypt(const aes128_ctx *aes, const uint8_t iv[GCM_IV_SIZE],
                  const uint8_t *aad, uint32_t aad_len,
                  const uint8_t *plaintext, uint32_t len,
                  uint8_t *ciphertext, uint8_t tag[GCM_TAG_SIZE]);

/* Decrypts `len` bytes of `ciphertext` into `plaintext` (may alias) and
 * checks the supplied tag against one computed the same way encrypt
 * did. Returns 1 if the tag matches (and plaintext is valid), 0 if it
 * doesn't (plaintext is still written either way -- caller must not
 * use it when this returns 0). */
int gcm_decrypt(const aes128_ctx *aes, const uint8_t iv[GCM_IV_SIZE],
                 const uint8_t *aad, uint32_t aad_len,
                 const uint8_t *ciphertext, uint32_t len,
                 uint8_t *plaintext, const uint8_t tag[GCM_TAG_SIZE]);

#endif
