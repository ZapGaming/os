#include <string.h>

void *memset(void *dst, int val, size_t len) {
    unsigned char *d = dst;
    for (size_t i = 0; i < len; i++) d[i] = (unsigned char)val;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t len) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < len; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t len) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        for (size_t i = 0; i < len; i++) d[i] = s[i];
    } else {
        for (size_t i = len; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t len) {
    const unsigned char *pa = a, *pb = b;
    for (size_t i = 0; i < len; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

char *strcpy(char *dst, const char *src) {
    char *ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

char *strcat(char *dst, const char *src) {
    char *ret = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++));
    return ret;
}
