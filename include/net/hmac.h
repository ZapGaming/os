#ifndef NET_HMAC_H
#define NET_HMAC_H

#include <stdint.h>

/* HMAC-SHA256, RFC 2104. */
void hmac_sha256(const uint8_t *key, uint32_t key_len,
                  const uint8_t *data, uint32_t data_len,
                  uint8_t out[32]);

#endif
