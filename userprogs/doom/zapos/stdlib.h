#ifndef ZAPOS_SHIM_STDLIB_H
#define ZAPOS_SHIM_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

void exit(int code);
void abort(void);

int atoi(const char *s);
long atol(const char *s);
/* No atof(): nothing in this build's DOOM source calls it (the one
 * call site, in m_config.c's float config handling, is gone -- see
 * m_config.c), and it would return a double, which can't exist in
 * compiled code at all under -mgeneral-regs-only. */

int abs(int x);
long labs(long x);

int rand(void);
void srand(unsigned int seed);

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

char *getenv(const char *name);
int system(const char *cmd);

#endif
