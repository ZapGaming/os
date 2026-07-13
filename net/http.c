#include <net/http.h>
#include <net/dns.h>
#include <net/tcp.h>
#include <kernel/pit.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

/* Sized to hold a whole response (headers + body) in one shot -- big
 * enough for a short downloaded clip, not big enough to stream an
 * arbitrarily large file (no chunked-to-disk streaming exists yet).
 * Allocated lazily on first use rather than as a permanent chunk of
 * kernel BSS. Kept well under half the 8MB kheap arena on purpose: the
 * browser's own per-fetch body buffer (roughly this size too) and any
 * audio file already loaded by the File Manager all have to coexist
 * in the same heap, and kmalloc() returning NULL here used to fail
 * completely silently -- see the log line below. */
#define HTTP_RAW_BUF_SIZE (2u * 1024 * 1024)

static uint8_t *raw_buf = NULL;

static int ci_char_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}

static int ci_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s || !ci_char_eq(*s, *prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}

/* Case-insensitively finds a header line "name: value" within
 * [headers, headers+len) and returns a pointer to the value (skipping
 * leading spaces), or NULL. Not null-terminated -- caller must respect
 * the line's extent (up to the next \r\n). */
static const char *find_header(const char *headers, uint32_t len, const char *name) {
    uint32_t name_len = (uint32_t)strlen(name);
    const char *p = headers;
    const char *end = headers + len;
    while (p < end) {
        if ((uint32_t)(end - p) >= name_len + 1 && ci_starts_with(p, name) && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (v < end && *v == ' ') v++;
            return v;
        }
        while (p < end && *p != '\n') p++;
        p++;
    }
    return NULL;
}

static uint32_t parse_uint(const char *s, const char *end) {
    uint32_t val = 0;
    while (s < end && *s >= '0' && *s <= '9') { val = val * 10 + (uint32_t)(*s - '0'); s++; }
    return val;
}

static uint32_t decode_chunked(const uint8_t *data, uint32_t len, uint8_t *out, uint32_t out_cap) {
    uint32_t pos = 0, outlen = 0;
    while (pos < len) {
        uint32_t size = 0;
        while (pos < len && data[pos] != '\r' && data[pos] != '\n') {
            char c = (char)data[pos];
            uint32_t digit;
            if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
            else break;
            size = size * 16 + digit;
            pos++;
        }
        while (pos < len && data[pos] != '\n') pos++;
        pos++;
        if (size == 0 || pos > len) break;

        uint32_t copy = size;
        if (pos + copy > len) copy = len - pos;
        if (outlen + copy > out_cap) copy = out_cap - outlen;
        memcpy(out + outlen, data + pos, copy);
        outlen += copy;
        pos += size;
        if (pos < len && data[pos] == '\r') pos++;
        if (pos < len && data[pos] == '\n') pos++;
    }
    return outlen;
}

int http_get(const char *host, uint16_t port, const char *path,
             int *status_out, char *body_out, uint32_t body_cap, uint32_t *body_len_out,
             char *content_type_out, uint32_t content_type_cap) {
    *status_out = 0;
    *body_len_out = 0;
    if (content_type_out && content_type_cap) content_type_out[0] = 0;

    if (!raw_buf) raw_buf = (uint8_t *)kmalloc(HTTP_RAW_BUF_SIZE);
    if (!raw_buf) {
        serial_printf("http: out of memory allocating %u-byte receive buffer\n", HTTP_RAW_BUF_SIZE);
        return 0;
    }

    uint32_t ip;
    if (!dns_resolve(host, &ip)) return 0;
    if (!tcp_connect(ip, port)) return 0;

    char req[512];
    strcpy(req, "GET ");
    strcat(req, path);
    strcat(req, " HTTP/1.1\r\nHost: ");
    strcat(req, host);
    strcat(req, "\r\nUser-Agent: ZapOS/1.0\r\nConnection: close\r\n\r\n");
    int req_len = (int)strlen(req);

    if (!tcp_send(req, (uint16_t)req_len)) {
        tcp_close();
        return 0;
    }

    uint32_t total = 0;
    for (;;) {
        int got = tcp_recv(raw_buf + total, (uint16_t)(HTTP_RAW_BUF_SIZE - total > 4096 ? 4096 : HTTP_RAW_BUF_SIZE - total));
        if (got > 0) {
            total += (uint32_t)got;
        } else if (got < 0) {
            break;
        } else {
            pit_sleep(20);
        }
        if (total >= HTTP_RAW_BUF_SIZE - 4096) {
            break;
        }
    }
    tcp_close();

    /* find end of headers */
    uint32_t header_end = 0;
    for (uint32_t i = 0; i + 3 < total; i++) {
        if (raw_buf[i] == '\r' && raw_buf[i + 1] == '\n' && raw_buf[i + 2] == '\r' && raw_buf[i + 3] == '\n') {
            header_end = i + 4;
            break;
        }
    }
    if (header_end == 0) {
        serial_printf("http: malformed response (no header terminator)\n");
        return 1; /* connected fine, just nothing sensible to show */
    }

    /* status line: "HTTP/1.1 200 OK\r\n" */
    const char *p = (const char *)raw_buf;
    while (p < (const char *)raw_buf + header_end && *p != ' ') p++;
    if (*p == ' ') p++;
    *status_out = (int)parse_uint(p, (const char *)raw_buf + header_end);

    const uint8_t *body = raw_buf + header_end;
    uint32_t body_avail = total - header_end;

    const char *chunked = find_header((const char *)raw_buf, header_end, "transfer-encoding");
    int is_chunked = chunked && ci_starts_with(chunked, "chunked");

    if (content_type_out && content_type_cap) {
        const char *ct = find_header((const char *)raw_buf, header_end, "content-type");
        if (ct) {
            const char *line_end = ct;
            const char *hdr_end = (const char *)raw_buf + header_end;
            while (line_end < hdr_end && *line_end != '\r' && *line_end != '\n') line_end++;
            uint32_t n = (uint32_t)(line_end - ct);
            if (n > content_type_cap - 1) n = content_type_cap - 1;
            memcpy(content_type_out, ct, n);
            content_type_out[n] = 0;
        }
    }

    uint32_t body_len;
    if (is_chunked) {
        body_len = decode_chunked(body, body_avail, (uint8_t *)body_out, body_cap);
    } else {
        const char *cl = find_header((const char *)raw_buf, header_end, "content-length");
        uint32_t content_length = cl ? parse_uint(cl, (const char *)raw_buf + header_end) : body_avail;
        body_len = content_length < body_avail ? content_length : body_avail;
        if (body_len > body_cap) body_len = body_cap;
        memcpy(body_out, body, body_len);
    }

    *body_len_out = body_len;
    serial_printf("http: %s%s -> status=%d body=%u bytes%s\n",
                  host, path, *status_out, body_len, is_chunked ? " (chunked)" : "");
    return 1;
}
