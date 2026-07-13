#ifndef NET_X25519_H
#define NET_X25519_H

#include <stdint.h>

/* X25519 (RFC 7748 Section 5): Montgomery-ladder scalar multiplication
 * over Curve25519 / GF(2^255-19). `scalar` and `u` are 32-byte little-
 * endian values; `out` receives the 32-byte little-endian result.
 * `scalar` is clamped internally per decodeScalar25519 (RFC 7748 5),
 * exactly as the RFC's X25519() function specifies -- callers pass the
 * raw bytes (private key or test-vector scalar) unclamped. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]);

/* RFC 7748 5.2's fixed base point u=9, as a 32-byte little-endian value. */
extern const uint8_t x25519_base_point[32];

#endif
