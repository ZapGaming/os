#ifndef ZAPOS_SHIM_STDIO_H
#define ZAPOS_SHIM_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct { int dummy; } FILE;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Real, non-NULL-but-inert handles: enough for code that checks
 * "did I get a valid stream" without ever actually reading/writing
 * through it for real (there's no console/terminal device to be one
 * on this port -- every I/O op below just fails or discards). */
extern FILE *stdin, *stdout, *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
int fflush(FILE *f);
int feof(FILE *f);
int remove(const char *path);
int fileno(FILE *f);
int rename(const char *old, const char *new_name);

int printf(const char *fmt, ...);
int putchar(int c);
int puts(const char *s);
int fprintf(FILE *f, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);

#endif
