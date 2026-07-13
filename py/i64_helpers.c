/* Software 64-bit integer divide/modulo helpers for this 32-bit target.
 *
 * py/interp.c's PY_INT (a real int64_t, unlike js/js.h's deliberately
 * 32-bit-only numbers) makes gcc emit calls to __divdi3/__moddi3/
 * __divmoddi4/etc. for any 64-bit division or modulo -- normally
 * satisfied by libgcc, but this system only has an x86_64 libgcc.a (no
 * 32-bit multilib installed), which can't link into a -m elf_i386
 * binary. userprogs/doom/doomlibc.c hit the identical problem for
 * DOOM's fixed-point math and solved it the same way: a plain
 * bit-at-a-time software division, implemented once here instead of
 * pulling in any prebuilt archive. */

typedef unsigned long long u64;
typedef long long i64;

static u64 udiv64(u64 num, u64 den, u64 *rem) {
    u64 q = 0, r = 0;
    if (den == 0) { if (rem) *rem = 0; return 0; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((num >> i) & 1ULL);
        if (r >= den) {
            r -= den;
            q |= (1ULL << i);
        }
    }
    if (rem) *rem = r;
    return q;
}

u64 __udivdi3(u64 a, u64 b) {
    return udiv64(a, b, 0);
}

u64 __umoddi3(u64 a, u64 b) {
    u64 rem;
    udiv64(a, b, &rem);
    return rem;
}

u64 __udivmoddi4(u64 a, u64 b, u64 *rem) {
    return udiv64(a, b, rem);
}

i64 __divdi3(i64 a, i64 b) {
    int neg = 0;
    u64 ua, ub;
    if (a < 0) { ua = (u64)(-a); neg = !neg; } else ua = (u64)a;
    if (b < 0) { ub = (u64)(-b); neg = !neg; } else ub = (u64)b;
    u64 q = udiv64(ua, ub, 0);
    return neg ? -(i64)q : (i64)q;
}

i64 __moddi3(i64 a, i64 b) {
    u64 ua = a < 0 ? (u64)(-a) : (u64)a;
    u64 ub = b < 0 ? (u64)(-b) : (u64)b;
    u64 rem;
    udiv64(ua, ub, &rem);
    return a < 0 ? -(i64)rem : (i64)rem;
}

i64 __divmoddi4(i64 a, i64 b, i64 *rem) {
    i64 q = __divdi3(a, b);
    if (rem) *rem = a - q * b;
    return q;
}
