#include <py/error.h>
#include <stdint.h>
#include <string.h>

#define PY_ERROR_BUF_SIZE 128

static int had_error = 0;
static char error_buf[PY_ERROR_BUF_SIZE];

static void bounded_append(char *dst, uint32_t *pos, const char *s) {
    if (!s) return;
    uint32_t len = (uint32_t)strlen(s);
    uint32_t space = PY_ERROR_BUF_SIZE - 1 - *pos;
    if (len > space) len = space;
    memcpy(dst + *pos, s, len);
    *pos += len;
    dst[*pos] = 0;
}

void py_error_reset(void) {
    had_error = 0;
    error_buf[0] = 0;
}

void py_error_set(const char *prefix, const char *detail1, const char *detail2) {
    if (had_error) return; /* first error wins */
    had_error = 1;
    uint32_t pos = 0;
    error_buf[0] = 0;
    bounded_append(error_buf, &pos, prefix);
    bounded_append(error_buf, &pos, detail1);
    bounded_append(error_buf, &pos, detail2);
}

int py_has_error(void) { return had_error; }

const char *py_error_message(void) { return error_buf; }
