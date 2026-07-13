#ifndef ZAPOS_SHIM_LIMITS_H
#define ZAPOS_SHIM_LIMITS_H
/* Replaces (rather than chains to) gcc's own freestanding limits.h,
 * which #include_next's a real system limits.h for constants like
 * LONG_MAX -- fatal under -nostdinc, since there's no "next" one. */

#define CHAR_BIT   8
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define UCHAR_MAX  255
#define CHAR_MIN   0
#define CHAR_MAX   255

#define SHRT_MIN   (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535

#define INT_MIN    (-2147483647 - 1)
#define INT_MAX    2147483647
#define UINT_MAX   4294967295U

#define LONG_MIN   (-2147483647L - 1)
#define LONG_MAX   2147483647L
#define ULONG_MAX  4294967295UL

#define LLONG_MIN  (-9223372036854775807LL - 1)
#define LLONG_MAX  9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

#define PATH_MAX   4096

#endif
