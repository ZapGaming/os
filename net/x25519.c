#include <net/x25519.h>
#include <string.h>

/* X25519 (RFC 7748 Section 5) over GF(2^255-19), implemented with plain
 * 32-bit-limb bignum arithmetic (8 limbs = 256 bits per field element,
 * little-endian limb order: v[0] is the least-significant 32 bits).
 * No __int128, no floating point -- every multiply is a 32x32->64
 * hardware MUL via a uint64_t intermediate, which gcc emits inline on
 * -m32 without any libgcc call. Verified against the RFC 7748 5.2 test
 * vectors and randomized cross-checks against an independent bignum
 * implementation (see the host-side test harness used during
 * development) before being wired into the TLS handshake. */

typedef uint32_t fe[8];

/* p = 2^255 - 19, little-endian 32-bit limbs. */
static const uint32_t FE_P[8] = {
    0xFFFFFFEDu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu
};

/* p - 2, used as the exponent for modular inversion via Fermat's
 * little theorem (a^(p-2) == a^-1 mod p). */
static const uint32_t FE_P_MINUS_2[8] = {
    0xFFFFFFEBu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu
};

static void fe_copy(fe r, const fe a) { memcpy(r, a, sizeof(fe)); }
static void fe_zero(fe r) { memset(r, 0, sizeof(fe)); }
static void fe_one(fe r) { memset(r, 0, sizeof(fe)); r[0] = 1; }

/* 1 if a >= b, 0 otherwise (both 8-limb, little-endian). */
static int bn8_geq(const uint32_t *a, const uint32_t *b) {
    for (int i = 7; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return 0;
    }
    return 1;
}

/* r = a - b (8 limbs each), returns the borrow (0 or 1). */
static uint32_t bn8_sub(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    int64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        int64_t d = (int64_t)a[i] - (int64_t)b[i] - borrow;
        if (d < 0) { d += ((int64_t)1 << 32); borrow = 1; } else borrow = 0;
        r[i] = (uint32_t)d;
    }
    return (uint32_t)borrow;
}

/* r = a + b (8 limbs each), returns the carry (0 or 1). */
static uint32_t bn8_add(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t s = (uint64_t)a[i] + (uint64_t)b[i] + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
    return (uint32_t)carry;
}

/* fe_add/fe_sub assume both inputs are already fully reduced (< p);
 * a+b < 2p < 2^256 always (since p < 2^255), so at most one conditional
 * subtraction of p is ever needed to bring the sum back into [0, p). */
static void fe_add(fe r, const fe a, const fe b) {
    fe t;
    bn8_add(t, a, b);
    if (bn8_geq(t, FE_P)) bn8_sub(t, t, FE_P);
    fe_copy(r, t);
}

static void fe_sub(fe r, const fe a, const fe b) {
    fe t;
    uint32_t borrow = bn8_sub(t, a, b);
    if (borrow) bn8_add(t, t, FE_P); /* a - b was negative; (a-b)+p is in [0,p) */
    fe_copy(r, t);
}

/* out = in >> bits, where `in` has in_limbs limbs and `out` has
 * out_limbs limbs (out_limbs may be smaller than what's needed to hold
 * every bit of `in`, in which case the high bits are simply discarded --
 * exactly what a right shift should do). Generic multi-limb shift used
 * only by the field-multiplication reduction step below. */
static void bn_shr(uint32_t *out, const uint32_t *in, int in_limbs, int out_limbs, int bits) {
    int limb_shift = bits / 32;
    int bit_shift = bits % 32;
    for (int i = 0; i < out_limbs; i++) {
        int idx = i + limb_shift;
        uint32_t lo = (idx < in_limbs) ? in[idx] : 0;
        uint32_t hi = (idx + 1 < in_limbs) ? in[idx + 1] : 0;
        out[i] = bit_shift ? ((lo >> bit_shift) | (hi << (32 - bit_shift))) : lo;
    }
}

/* out (limbs+1 limbs) = in (limbs limbs) * k, k a small (<2^32) constant. */
static void bn_mul_small(uint32_t *out, const uint32_t *in, int limbs, uint32_t k) {
    uint64_t carry = 0;
    for (int i = 0; i < limbs; i++) {
        uint64_t p = (uint64_t)in[i] * (uint64_t)k + carry;
        out[i] = (uint32_t)p;
        carry = p >> 32;
    }
    out[limbs] = (uint32_t)carry;
}

/* out (out_limbs limbs) = a (a_limbs) + b (b_limbs), zero-extending the
 * shorter operand. Caller must size out_limbs generously enough that no
 * carry is ever lost off the top -- every call site below documents why
 * its chosen size suffices. */
static void bn_add_generic(uint32_t *out, int out_limbs,
                            const uint32_t *a, int a_limbs,
                            const uint32_t *b, int b_limbs) {
    uint64_t carry = 0;
    for (int i = 0; i < out_limbs; i++) {
        uint32_t av = (i < a_limbs) ? a[i] : 0;
        uint32_t bv = (i < b_limbs) ? b[i] : 0;
        uint64_t s = (uint64_t)av + (uint64_t)bv + carry;
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
}

/* Reduces a 512-bit product (16 limbs) mod p = 2^255-19 down to a fully
 * reduced 8-limb field element, using the identity 2^255 = 19 (mod p):
 * splitting t = T1*2^255 + T0 gives t = T0 + 19*T1 (mod p), and doing
 * that twice shrinks the high part enough (257 bits -> ~7 bits -> gone)
 * that only a final small number of conditional subtractions of p
 * remain. Sizes below are generous relative to the tight bounds derived
 * from a,b < p < 2^255 (so t < 2^510), leaving headroom rather than
 * relying on exact bit counts. */
static void fe_reduce_from_512(fe r, const uint32_t t[16]) {
    uint32_t T1[9];
    bn_shr(T1, t, 16, 9, 255);
    uint32_t T0[8];
    memcpy(T0, t, 7 * sizeof(uint32_t));
    T0[7] = t[7] & 0x7FFFFFFFu;

    uint32_t m1[10];
    bn_mul_small(m1, T1, 9, 19);
    uint32_t S[10];
    bn_add_generic(S, 10, T0, 8, m1, 10);

    uint32_t S1[3];
    bn_shr(S1, S, 10, 3, 255);
    uint32_t S0[8];
    memcpy(S0, S, 7 * sizeof(uint32_t));
    S0[7] = S[7] & 0x7FFFFFFFu;

    uint32_t m2[4];
    bn_mul_small(m2, S1, 3, 19);
    uint32_t R[8];
    bn_add_generic(R, 8, S0, 8, m2, 4);

    fe_copy(r, R);
    while (bn8_geq(r, FE_P)) {
        uint32_t tmp[8];
        bn8_sub(tmp, r, FE_P);
        fe_copy(r, tmp);
    }
}

static void fe_mul(fe r, const fe a, const fe b) {
    uint32_t t[16];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t prod = (uint64_t)a[i] * (uint64_t)b[j] + (uint64_t)t[i + j] + carry;
            t[i + j] = (uint32_t)prod;
            carry = prod >> 32;
        }
        int k = i + 8;
        while (carry) {
            uint64_t s = (uint64_t)t[k] + carry;
            t[k] = (uint32_t)s;
            carry = s >> 32;
            k++;
        }
    }
    fe_reduce_from_512(r, t);
}

/* a^(p-2) mod p == a^-1 mod p (Fermat's little theorem), via a plain
 * left-to-right square-and-multiply over all 256 bits of the exponent
 * (leading zero bits just square the running result of 1, which stays
 * 1 -- harmless, and simpler/safer than special-casing the exponent's
 * exact bit length). */
static void fe_invert(fe r, const fe a) {
    fe result;
    fe_one(result);
    for (int bit = 255; bit >= 0; bit--) {
        fe_mul(result, result, result);
        int limb = bit / 32, off = bit % 32;
        if ((FE_P_MINUS_2[limb] >> off) & 1u) fe_mul(result, result, a);
    }
    fe_copy(r, result);
}

/* swap == 1 or 0; branchless conditional swap via an all-ones/all-zeros mask. */
static void fe_cswap(uint32_t swap, fe a, fe b) {
    uint32_t mask = (uint32_t)0 - swap;
    for (int i = 0; i < 8; i++) {
        uint32_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

static void fe_from_bytes(fe r, const uint8_t b[32]) {
    for (int i = 0; i < 8; i++) {
        r[i] = (uint32_t)b[4 * i] | ((uint32_t)b[4 * i + 1] << 8) |
               ((uint32_t)b[4 * i + 2] << 16) | ((uint32_t)b[4 * i + 3] << 24);
    }
    /* RFC 7748 5: "implementations MUST mask the most significant bit
     * in the final byte" when decoding a u-coordinate for X25519. */
    r[7] &= 0x7FFFFFFFu;
    /* That only guarantees < 2^255, not < p (p = 2^255-19) -- reduce
     * the rare non-canonical inputs in [p, 2^255) down to canonical
     * form so every later fe_add/fe_sub's "already reduced" assumption
     * holds. */
    while (bn8_geq(r, FE_P)) {
        uint32_t tmp[8];
        bn8_sub(tmp, r, FE_P);
        fe_copy(r, tmp);
    }
}

static void fe_to_bytes(uint8_t b[32], const fe r) {
    for (int i = 0; i < 8; i++) {
        b[4 * i]     = (uint8_t)(r[i]);
        b[4 * i + 1] = (uint8_t)(r[i] >> 8);
        b[4 * i + 2] = (uint8_t)(r[i] >> 16);
        b[4 * i + 3] = (uint8_t)(r[i] >> 24);
    }
}

/* C zero-initializes every element an aggregate initializer doesn't
 * mention, so only the first byte needs to be written here. */
const uint8_t x25519_base_point[32] = { 9 };

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]) {
    uint8_t k[32];
    memcpy(k, scalar, 32);
    /* decodeScalar25519 clamping (RFC 7748 5). */
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    fe x1, x2, z2, x3, z3;
    fe_from_bytes(x1, u);
    fe_one(x2);
    fe_zero(z2);
    fe_copy(x3, x1);
    fe_one(z3);

    uint32_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        uint32_t kt = (k[t / 8] >> (t % 8)) & 1u;
        swap ^= kt;
        fe_cswap(swap, x2, x3);
        fe_cswap(swap, z2, z3);
        swap = kt;

        fe A, B, AA, BB, E, C, D, DA, CB, sum, diff, diffsq, a24E, aa_plus;
        fe_add(A, x2, z2);
        fe_sub(B, x2, z2);
        fe_mul(AA, A, A);
        fe_mul(BB, B, B);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul(DA, D, A);
        fe_mul(CB, C, B);

        fe_add(sum, DA, CB);
        fe_mul(x3, sum, sum);              /* x3 = (DA+CB)^2 */
        fe_sub(diff, DA, CB);
        fe_mul(diffsq, diff, diff);
        fe_mul(z3, x1, diffsq);            /* z3 = x1*(DA-CB)^2 */

        fe_mul(x2, AA, BB);                /* x2 = AA*BB */

        fe a24 = { 121665u, 0, 0, 0, 0, 0, 0, 0 }; /* (486662-2)/4 */
        fe_mul(a24E, a24, E);
        fe_add(aa_plus, AA, a24E);
        fe_mul(z2, E, aa_plus);             /* z2 = E*(AA + a24*E) */
    }
    fe_cswap(swap, x2, x3);
    fe_cswap(swap, z2, z3);

    fe z2inv, result;
    fe_invert(z2inv, z2);
    fe_mul(result, x2, z2inv);
    fe_to_bytes(out, result);
}
