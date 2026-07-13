//
// A minimal freestanding libc for the ZapOS DOOM port -- just enough
// of what real DOOM/doomgeneric source actually calls (see the zapos/
// shim headers for the declared surface) to link and run. No FPU/SSE
// exists in this build at all, so nothing here ever touches a float
// or double.

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "zapos_syscalls.h"

int errno;

/* ---------------- 64-bit division helper ---------------- */

/* m_fixed.c's FixedDiv does a genuine 64/32-bit signed division
 * (promoted to 64/64 by C's usual arithmetic conversions) -- on a
 * 32-bit target that's normally satisfied by libgcc's __divdi3, but
 * this toolchain has no 32-bit libgcc.a installed (only the x86_64
 * one, which can't link into a -m32 binary). Software long division
 * is plenty fast enough at DOOM's call volume. */
static unsigned long long udiv64(unsigned long long n, unsigned long long d) {
    unsigned long long q = 0, r = 0;
    if (d == 0) return 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) {
            r -= d;
            q |= (1ULL << i);
        }
    }
    return q;
}

long long __divdi3(long long a, long long b) {
    int neg = 0;
    unsigned long long ua, ub;
    if (a < 0) { ua = (unsigned long long)(-a); neg ^= 1; } else ua = (unsigned long long)a;
    if (b < 0) { ub = (unsigned long long)(-b); neg ^= 1; } else ub = (unsigned long long)b;
    unsigned long long q = udiv64(ua, ub);
    return neg ? -(long long)q : (long long)q;
}

/* ---------------- malloc/free ---------------- */

/* DOOM's own zone allocator (z_zone.c) asks for one big block (16MB
 * by default, see i_system.c's I_ZoneBase) and manages it internally
 * from then on -- this only needs to be a plain, general-purpose
 * allocator underneath that, not anything sophisticated. Same design
 * as the kernel's own kernel/kheap.c: a first-fit free list over one
 * static arena, since this ELF has no notion of growing its own heap
 * at runtime (no brk/mmap syscall exists). Sized well above DOOM's
 * default 16MB zone to leave room for the embedded WAD's lump cache
 * and everything else layered on top. */
#define HEAP_SIZE (48u * 1024 * 1024)
static unsigned char heap_arena[HEAP_SIZE] __attribute__((aligned(16)));

struct block_header {
    size_t size;
    int free;
    struct block_header *next;
};

static struct block_header *heap_head;
static int heap_ready = 0;

static void heap_init(void) {
    heap_head = (struct block_header *)heap_arena;
    heap_head->size = HEAP_SIZE - sizeof(struct block_header);
    heap_head->free = 1;
    heap_head->next = NULL;
    heap_ready = 1;
}

static void split_block(struct block_header *block, size_t size) {
    size_t remaining = block->size - size;
    if (remaining <= sizeof(struct block_header) + 16) return;
    struct block_header *nb = (struct block_header *)((unsigned char *)(block + 1) + size);
    nb->size = remaining - sizeof(struct block_header);
    nb->free = 1;
    nb->next = block->next;
    block->size = size;
    block->next = nb;
}

void *malloc(size_t size) {
    if (!heap_ready) heap_init();
    if (size == 0) return NULL;
    size = (size + 15) & ~(size_t)15;

    for (struct block_header *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            return (void *)(b + 1);
        }
    }
    sys_write("doomlibc: malloc() out of memory\n");
    return NULL;
}

static void coalesce(void) {
    for (struct block_header *b = heap_head; b && b->next; b = b->next) {
        if (b->free && b->next->free) {
            b->size += sizeof(struct block_header) + b->next->size;
            b->next = b->next->next;
        }
    }
}

void free(void *ptr) {
    if (!ptr) return;
    struct block_header *b = (struct block_header *)ptr - 1;
    b->free = 1;
    coalesce();
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    struct block_header *b = (struct block_header *)ptr - 1;
    if (b->size >= size) return ptr;
    void *n = malloc(size);
    if (n) {
        memcpy(n, ptr, b->size);
        free(ptr);
    }
    return n;
}

/* ---------------- process control ---------------- */

void exit(int code) {
    (void)code;
    sys_exit();
    for (;;) { }
}

void abort(void) {
    sys_write("doomlibc: abort()\n");
    sys_exit();
    for (;;) { }
}

int system(const char *cmd) { (void)cmd; return -1; }

/* ---------------- string/mem ---------------- */

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i-- > 0;) d[i] = s[i];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a, *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) if (pa[i] != pb[i]) return pa[i] - pb[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = 0;
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

int strcasecmp(const char *a, const char *b) {
    while (*a && lc(*a) == lc(*b)) { a++; b++; }
    return (unsigned char)lc(*a) - (unsigned char)lc(*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = lc(a[i]), cb = lc(b[i]);
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        if (!a[i]) return 0;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (; *s; s++) if (*s == (char)c) last = s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *)hay;
    for (; *hay; hay++) {
        if (strncmp(hay, needle, nlen) == 0) return (char *)hay;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strerror(int errnum) {
    (void)errnum;
    return "error";
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size > 0) {
        size_t cn = n < size - 1 ? n : size - 1;
        memcpy(dst, src, cn);
        dst[cn] = 0;
    }
    return n;
}

size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dlen = strlen(dst);
    if (dlen >= size) return dlen + strlen(src);
    return dlen + strlcpy(dst + dlen, src, size - dlen);
}

/* ---------------- ints ---------------- */

int atoi(const char *s) {
    int sign = 1, v = 0;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * sign;
}

long atol(const char *s) {
    long sign = 1, v = 0;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * sign;
}

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static unsigned int rand_state = 12345;
int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 1) & 0x7fffffff);
}
void srand(unsigned int seed) { rand_state = seed; }

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    /* Simple insertion sort -- doom never sorts anything performance-
     * critical (a handful of high-score-ish tables at most), so O(n^2)
     * is fine and this avoids needing a real partitioning quicksort. */
    unsigned char *arr = (unsigned char *)base;
    unsigned char tmp[64];
    for (size_t i = 1; i < nmemb; i++) {
        size_t j = i;
        while (j > 0 && compar(arr + (j - 1) * size, arr + j * size) > 0) {
            memcpy(tmp, arr + j * size, size);
            memcpy(arr + j * size, arr + (j - 1) * size, size);
            memcpy(arr + (j - 1) * size, tmp, size);
            j--;
        }
    }
}

char *getenv(const char *name) { (void)name; return NULL; }

/* ---------------- filesystem (all fail gracefully -- no syscall exists) ---------------- */

int mkdir(const char *path, int mode) { (void)path; (void)mode; return 0; }
int access(const char *path, int mode) { (void)path; (void)mode; return -1; }
char *getcwd(char *buf, unsigned long size) { if (size > 0 && buf) buf[0] = 0; return buf; }
int unlink(const char *path) { (void)path; return -1; }
int isatty(int fd) { (void)fd; return 0; }

static FILE stdin_obj, stdout_obj, stderr_obj;
FILE *stdin = &stdin_obj, *stdout = &stdout_obj, *stderr = &stderr_obj;

FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int fclose(FILE *f) { (void)f; return 0; }
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) { (void)ptr; (void)size; (void)nmemb; (void)f; return 0; }
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) { (void)ptr; (void)f; return size * nmemb; }
int fseek(FILE *f, long offset, int whence) { (void)f; (void)offset; (void)whence; return -1; }
long ftell(FILE *f) { (void)f; return -1; }
int fflush(FILE *f) { (void)f; return 0; }
int feof(FILE *f) { (void)f; return 1; }
int remove(const char *path) { (void)path; return -1; }
int fileno(FILE *f) { (void)f; return -1; }
int rename(const char *old, const char *new_name) { (void)old; (void)new_name; return -1; }
int sscanf(const char *str, const char *fmt, ...) { (void)str; (void)fmt; return 0; }

/* ---------------- printf family ---------------- */

static void out_char(char *buf, size_t size, size_t *pos, char c) {
    if (*pos < size) buf[*pos] = c;
    (*pos)++;
}

static void out_str(char *buf, size_t size, size_t *pos, const char *s, int min_width, int zero_pad, int precision) {
    size_t len = strlen(s);
    if (precision >= 0 && (size_t)precision < len) len = (size_t)precision;
    int pad = min_width - (int)len;
    for (int i = 0; i < pad; i++) out_char(buf, size, pos, zero_pad ? '0' : ' ');
    for (size_t i = 0; i < len; i++) out_char(buf, size, pos, s[i]);
}

/* precision on an integer conversion (e.g. "%.3d") means "minimum
 * digit count, zero-padded" per C99 -- distinct from min_width, which
 * pads the whole field with spaces (or zeros via the '0' flag, but
 * only when no explicit precision was given). hu_stuff.c's "STCFN%.3d"
 * needs this to look up the right font lump name (STCFN033, not
 * STCFN33) -- without it, W_CacheLumpName fails outright. */
static void out_uint(char *buf, size_t size, size_t *pos, unsigned long v, int base, int upper,
                      int min_width, int zero_pad, int precision) {
    char tmp[32];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; }

    int zeros = (precision >= 0 && precision > n) ? precision - n : 0;
    int pad = min_width - (n + zeros);
    char pad_char = (precision < 0 && zero_pad) ? '0' : ' ';
    for (int i = 0; i < pad; i++) out_char(buf, size, pos, pad_char);
    for (int i = 0; i < zeros; i++) out_char(buf, size, pos, '0');
    while (n > 0) out_char(buf, size, pos, tmp[--n]);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { out_char(buf, size, &pos, *p); continue; }
        p++;
        if (*p == 0) break;

        int zero_pad = 0, min_width = 0, precision = -1;
        int is_long = 0;

        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') { min_width = min_width * 10 + (*p - '0'); p++; }
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') { precision = precision * 10 + (*p - '0'); p++; }
        }
        while (*p == 'l') { is_long = 1; p++; }

        switch (*p) {
            case 'd': case 'i': {
                long v = is_long ? va_arg(ap, long) : va_arg(ap, int);
                if (v < 0) { out_char(buf, size, &pos, '-'); v = -v; }
                out_uint(buf, size, &pos, (unsigned long)v, 10, 0, min_width, zero_pad, precision);
                break;
            }
            case 'u':
                out_uint(buf, size, &pos, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int),
                         10, 0, min_width, zero_pad, precision);
                break;
            case 'x':
                out_uint(buf, size, &pos, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int),
                         16, 0, min_width, zero_pad, precision);
                break;
            case 'X':
                out_uint(buf, size, &pos, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int),
                         16, 1, min_width, zero_pad, precision);
                break;
            case 'o':
                out_uint(buf, size, &pos, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int),
                         8, 0, min_width, zero_pad, precision);
                break;
            case 'p':
                out_str(buf, size, &pos, "0x", 0, 0, -1);
                out_uint(buf, size, &pos, (unsigned long)va_arg(ap, void *), 16, 0, 0, 0, -1);
                break;
            case 'c':
                out_char(buf, size, &pos, (char)va_arg(ap, int));
                break;
            case 's':
                out_str(buf, size, &pos, va_arg(ap, const char *), min_width, 0, precision);
                break;
            case '%':
                out_char(buf, size, &pos, '%');
                break;
            default:
                out_char(buf, size, &pos, '%');
                out_char(buf, size, &pos, *p);
                break;
        }
    }
    if (size > 0) buf[pos < size ? pos : size - 1] = 0;
    return (int)pos;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* printf/fprintf/vprintf/vfprintf all route to sys_write() -- there's
 * no console/terminal device, just the kernel's own serial log, which
 * is exactly what SYS_WRITE already reaches (see kernel/syscall.c). */
#define PRINTF_BUF_SIZE 1024
static char printf_buf[PRINTF_BUF_SIZE];

int vprintf(const char *fmt, va_list ap) {
    int r = vsnprintf(printf_buf, PRINTF_BUF_SIZE, fmt, ap);
    sys_write(printf_buf);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    (void)f;
    return vprintf(fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int putchar(int c) {
    char buf[2] = { (char)c, 0 };
    sys_write(buf);
    return c;
}

int puts(const char *s) {
    sys_write(s);
    sys_write("\n");
    return 0;
}
