#include <net/http.h>
#include <net/dns.h>
#include <net/tcp.h>
#include <net/gzip.h>
#include <kernel/pit.h>
#include <kernel/kheap.h>
#include <kernel/serial.h>
#include <string.h>

static int ci_starts_with(const char *s, const char *prefix);
static uint32_t parse_uint(const char *s, const char *end);

#define HTTP_MAX_REDIRECTS 5

/* A flat, per-host cookie jar -- no Path/Expires/HttpOnly/Secure/
 * SameSite handling, no per-path scoping, just "these name=value pairs
 * go out with every request to this host" (close enough for the sites
 * that actually need cookies to function at all, e.g. session/consent
 * cookies). Replaced by name on a new Set-Cookie for the same host. */
#define COOKIE_JAR_ENTRIES 32
#define COOKIE_HOST_LEN    64
#define COOKIE_NAME_LEN    64
#define COOKIE_VALUE_LEN   192

struct cookie_entry {
    char host[COOKIE_HOST_LEN];
    char name[COOKIE_NAME_LEN];
    char value[COOKIE_VALUE_LEN];
    int valid;
};

static struct cookie_entry cookie_jar[COOKIE_JAR_ENTRIES];

static void cookie_store(const char *host, const char *name, uint32_t name_len,
                          const char *value, uint32_t value_len) {
    if (name_len == 0 || name_len >= COOKIE_NAME_LEN || value_len >= COOKIE_VALUE_LEN) return;

    int slot = -1;
    for (int i = 0; i < COOKIE_JAR_ENTRIES; i++) {
        if (cookie_jar[i].valid && strcmp(cookie_jar[i].host, host) == 0 &&
            strncmp(cookie_jar[i].name, name, name_len) == 0 && cookie_jar[i].name[name_len] == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < COOKIE_JAR_ENTRIES; i++) {
            if (!cookie_jar[i].valid) { slot = i; break; }
        }
    }
    if (slot < 0) return; /* jar full -- drop it rather than evict, cookies are best-effort here */

    strncpy(cookie_jar[slot].host, host, COOKIE_HOST_LEN - 1);
    cookie_jar[slot].host[COOKIE_HOST_LEN - 1] = 0;
    memcpy(cookie_jar[slot].name, name, name_len);
    cookie_jar[slot].name[name_len] = 0;
    memcpy(cookie_jar[slot].value, value, value_len);
    cookie_jar[slot].value[value_len] = 0;
    cookie_jar[slot].valid = 1;
}

/* Parses one "Name=Value" pair out of a Set-Cookie header value
 * (stopping at the first ';' -- every other attribute: Path, Expires,
 * Max-Age, Domain, Secure, HttpOnly, SameSite -- is ignored) and stores
 * it for `host`. */
static void cookie_parse_set_cookie(const char *host, const char *value, const char *end) {
    const char *p = value;
    const char *eq = NULL;
    while (p < end && *p != ';') {
        if (!eq && *p == '=') eq = p;
        p++;
    }
    if (!eq) return;
    const char *name_start = value;
    while (name_start < eq && *name_start == ' ') name_start++;
    const char *name_end = eq;
    while (name_end > name_start && name_end[-1] == ' ') name_end--;
    const char *val_start = eq + 1;
    const char *val_end = p;
    while (val_end > val_start && val_end[-1] == ' ') val_end--;

    cookie_store(host, name_start, (uint32_t)(name_end - name_start), val_start, (uint32_t)(val_end - val_start));
}

/* Builds "name1=value1; name2=value2" for every cookie stored against
 * `host`. Returns the number of bytes written (0 if none / doesn't fit). */
static uint32_t cookie_build_header(const char *host, char *out, uint32_t out_cap) {
    uint32_t pos = 0;
    int first = 1;
    for (int i = 0; i < COOKIE_JAR_ENTRIES; i++) {
        if (!cookie_jar[i].valid || strcmp(cookie_jar[i].host, host) != 0) continue;
        uint32_t name_len = (uint32_t)strlen(cookie_jar[i].name);
        uint32_t value_len = (uint32_t)strlen(cookie_jar[i].value);
        uint32_t need = name_len + 1 + value_len + (first ? 0 : 2);
        if (pos + need >= out_cap) break;
        if (!first) { out[pos++] = ';'; out[pos++] = ' '; }
        memcpy(out + pos, cookie_jar[i].name, name_len);
        pos += name_len;
        out[pos++] = '=';
        memcpy(out + pos, cookie_jar[i].value, value_len);
        pos += value_len;
        first = 0;
    }
    out[pos] = 0;
    return pos;
}

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

/* Holds the response body after Transfer-Encoding (chunked) has been
 * undone but before Content-Encoding (gzip/deflate) has -- i.e. still
 * possibly compressed. Same lazy-kmalloc/sizing convention as raw_buf. */
static uint8_t *comp_buf = NULL;

/* A small in-memory response cache, keyed by "host:port/path", so a
 * page revisited (or a stylesheet/image shared by several pages) in
 * the same boot skips the network entirely. Deliberately modest: only
 * successful (status 200) responses that fit in HTTP_CACHE_MAX_BODY
 * and whose own Cache-Control actually permits it (a real max-age,
 * not "no-store"/"no-cache"/absent) get cached; there's no ETag/
 * If-None-Match revalidation of a stale entry, and no Expires header
 * support -- once max-age's TTL passes, a stale entry is just evicted
 * and re-fetched from scratch, same as a cold cache. That covers the
 * common, high-value case (content-hashed static assets served with
 * a long max-age, e.g. Next.js's `/_next/static/...` chunks) without
 * the added complexity a fully spec-correct HTTP cache would need. */
#define HTTP_CACHE_ENTRIES     8
#define HTTP_CACHE_MAX_BODY    (256u * 1024)
#define HTTP_CACHE_KEY_LEN     160
#define HTTP_CACHE_CT_LEN      64

struct http_cache_entry {
    char key[HTTP_CACHE_KEY_LEN];
    uint8_t *body;
    uint32_t body_len;
    char content_type[HTTP_CACHE_CT_LEN];
    int status;
    uint32_t expires_tick; /* pit_ticks() value after which this entry is stale */
    uint32_t last_used;
    int valid;
};

static struct http_cache_entry http_cache[HTTP_CACHE_ENTRIES];
static uint32_t http_cache_clock = 0;

static void make_cache_key(char *out, uint32_t cap, const char *host, uint16_t port, const char *path) {
    char portbuf[8];
    int pi = 0;
    uint16_t p = port;
    char rev[8]; int ri = 0;
    if (p == 0) rev[ri++] = '0';
    while (p > 0) { rev[ri++] = (char)('0' + (p % 10)); p = (uint16_t)(p / 10); }
    while (ri > 0) portbuf[pi++] = rev[--ri];
    portbuf[pi] = 0;

    (void)cap; /* host/port/path are already bounded well under HTTP_CACHE_KEY_LEN */
    strcpy(out, host);
    strcat(out, ":");
    strcat(out, portbuf);
    strcat(out, path);
}

static struct http_cache_entry *cache_find(const char *key) {
    for (int i = 0; i < HTTP_CACHE_ENTRIES; i++) {
        if (http_cache[i].valid && strcmp(http_cache[i].key, key) == 0) {
            if (pit_ticks() < http_cache[i].expires_tick) return &http_cache[i];
            /* Stale -- free it now rather than waiting for eviction to
             * bother, so a repeated miss on the same URL doesn't leak. */
            kfree(http_cache[i].body);
            http_cache[i].body = NULL;
            http_cache[i].valid = 0;
        }
    }
    return NULL;
}

/* Case-insensitively finds `token` (e.g. "no-store") as a whole
 * comma-separated directive inside a Cache-Control value. */
static int cache_control_has(const char *value, const char *end, const char *token) {
    uint32_t tok_len = (uint32_t)strlen(token);
    const char *p = value;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',')) p++;
        const char *tok_start = p;
        if ((uint32_t)(end - p) >= tok_len && ci_starts_with(p, token)) {
            const char *after = p + tok_len;
            if (after >= end || *after == ',' || *after == ' ' || *after == '=') return 1;
        }
        while (p < end && *p != ',') p++;
        if (p == tok_start) p++;
    }
    return 0;
}

static int cache_control_max_age(const char *value, const char *end) {
    const char *p = value;
    while (p < end) {
        if ((uint32_t)(end - p) >= 8 && ci_starts_with(p, "max-age=")) {
            return (int)parse_uint(p + 8, end);
        }
        p++;
    }
    return -1;
}

static void cache_store(const char *key, int status, const char *content_type,
                         const uint8_t *body, uint32_t body_len, uint32_t max_age_secs) {
    if (status != 200 || body_len == 0 || body_len > HTTP_CACHE_MAX_BODY || max_age_secs == 0) return;

    int slot = -1;
    for (int i = 0; i < HTTP_CACHE_ENTRIES; i++) {
        if (!http_cache[i].valid) { slot = i; break; }
    }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < HTTP_CACHE_ENTRIES; i++) {
            if (http_cache[i].last_used < oldest) { oldest = http_cache[i].last_used; slot = i; }
        }
    }
    if (slot < 0) return;

    uint8_t *copy = (uint8_t *)kmalloc(body_len);
    if (!copy) return;
    memcpy(copy, body, body_len);

    if (http_cache[slot].valid) kfree(http_cache[slot].body);
    strncpy(http_cache[slot].key, key, HTTP_CACHE_KEY_LEN - 1);
    http_cache[slot].key[HTTP_CACHE_KEY_LEN - 1] = 0;
    http_cache[slot].body = copy;
    http_cache[slot].body_len = body_len;
    strncpy(http_cache[slot].content_type, content_type ? content_type : "", HTTP_CACHE_CT_LEN - 1);
    http_cache[slot].content_type[HTTP_CACHE_CT_LEN - 1] = 0;
    http_cache[slot].status = status;
    http_cache[slot].expires_tick = pit_ticks() + max_age_secs * 100u; /* PIT runs at 100Hz */
    http_cache[slot].last_used = http_cache_clock++;
    http_cache[slot].valid = 1;
}

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

/* Like find_header(), but starts searching after `from` (NULL = start
 * of headers) so every occurrence of a repeatable header (e.g.
 * Set-Cookie, which a response can carry several of) can be visited by
 * calling this in a loop with each previous return value. */
static const char *find_header_next(const char *headers, uint32_t len, const char *name, const char *from) {
    uint32_t name_len = (uint32_t)strlen(name);
    const char *p = from ? from : headers;
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

/* Parses a Location header value into an (updated) host/port/path.
 * Handles absolute ("http://host[:port]/path"), scheme-relative
 * ("//host/path"), and root-relative ("/path") targets -- the common
 * real-world cases. Returns 0 (can't/won't follow) for an https target
 * (no TLS client exists yet) or an opaque relative path (this doesn't
 * attempt dot-segment resolution against the current URL), 1 otherwise. */
static int parse_location(const char *loc, uint32_t loc_len, char *host, uint32_t host_cap,
                           uint16_t *port, char *path, uint32_t path_cap) {
    const char *p = loc;
    const char *end = loc + loc_len;

    if (loc_len >= 8 && ci_starts_with(p, "https://")) return 0;

    if (loc_len >= 7 && ci_starts_with(p, "http://")) {
        p += 7;
    } else if (loc_len >= 2 && p[0] == '/' && p[1] == '/') {
        p += 2; /* scheme-relative -- assume http, same reasoning as above */
    } else if (loc_len >= 1 && p[0] == '/') {
        uint32_t n = (uint32_t)(end - p);
        if (n >= path_cap) n = path_cap - 1;
        memcpy(path, p, n);
        path[n] = 0;
        return 1; /* root-relative: host/port unchanged */
    } else {
        return 0;
    }

    const char *host_start = p;
    while (p < end && *p != '/' && *p != ':') p++;
    uint32_t hn = (uint32_t)(p - host_start);
    if (hn >= host_cap) hn = host_cap - 1;
    memcpy(host, host_start, hn);
    host[hn] = 0;

    if (p < end && *p == ':') {
        p++;
        *port = (uint16_t)parse_uint(p, end);
        while (p < end && *p != '/') p++;
    } else {
        *port = 80;
    }

    if (p < end) {
        uint32_t pn = (uint32_t)(end - p);
        if (pn >= path_cap) pn = path_cap - 1;
        memcpy(path, p, pn);
        path[pn] = 0;
    } else {
        strcpy(path, "/");
    }
    return 1;
}

static int is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

int http_get(const char *host, uint16_t port, const char *path,
             int *status_out, char *body_out, uint32_t body_cap, uint32_t *body_len_out,
             char *content_type_out, uint32_t content_type_cap) {
    *status_out = 0;
    *body_len_out = 0;
    if (content_type_out && content_type_cap) content_type_out[0] = 0;

    char cache_key[HTTP_CACHE_KEY_LEN];
    make_cache_key(cache_key, sizeof(cache_key), host, port, path);
    struct http_cache_entry *hit = cache_find(cache_key);
    if (hit) {
        *status_out = hit->status;
        uint32_t n = hit->body_len < body_cap ? hit->body_len : body_cap;
        memcpy(body_out, hit->body, n);
        *body_len_out = n;
        if (content_type_out && content_type_cap) {
            strncpy(content_type_out, hit->content_type, content_type_cap - 1);
            content_type_out[content_type_cap - 1] = 0;
        }
        hit->last_used = http_cache_clock++;
        serial_printf("http: %s%s -> CACHED status=%d body=%u bytes\n", host, path, hit->status, n);
        return 1;
    }

    if (!raw_buf) raw_buf = (uint8_t *)kmalloc(HTTP_RAW_BUF_SIZE);
    if (!comp_buf) comp_buf = (uint8_t *)kmalloc(HTTP_RAW_BUF_SIZE);
    if (!raw_buf || !comp_buf) {
        serial_printf("http: out of memory allocating %u-byte receive buffer\n", HTTP_RAW_BUF_SIZE);
        return 0;
    }

    char cur_host[128];
    char cur_path[512];
    uint16_t cur_port = port;
    strncpy(cur_host, host, sizeof(cur_host) - 1); cur_host[sizeof(cur_host) - 1] = 0;
    strncpy(cur_path, path, sizeof(cur_path) - 1); cur_path[sizeof(cur_path) - 1] = 0;

    uint32_t total = 0, header_end = 0;

    for (int hop = 0; ; hop++) {
        uint32_t ip;
        if (!dns_resolve(cur_host, &ip)) return 0;
        if (!tcp_connect(ip, cur_port)) return 0;

        char cookie_hdr[512];
        uint32_t cookie_hdr_len = cookie_build_header(cur_host, cookie_hdr, sizeof(cookie_hdr));

        char req[1600];
        strcpy(req, "GET ");
        strcat(req, cur_path);
        strcat(req, " HTTP/1.1\r\nHost: ");
        strcat(req, cur_host);
        strcat(req, "\r\nUser-Agent: ZapOS/1.0\r\nAccept-Encoding: gzip, deflate\r\n");
        if (cookie_hdr_len > 0) {
            strcat(req, "Cookie: ");
            strcat(req, cookie_hdr);
            strcat(req, "\r\n");
        }
        strcat(req, "Connection: close\r\n\r\n");

        if (!tcp_send(req, (uint16_t)strlen(req))) {
            tcp_close();
            return 0;
        }

        total = 0;
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

        header_end = 0;
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

        const char *p = (const char *)raw_buf;
        while (p < (const char *)raw_buf + header_end && *p != ' ') p++;
        if (*p == ' ') p++;
        *status_out = (int)parse_uint(p, (const char *)raw_buf + header_end);

        const char *hdr_end = (const char *)raw_buf + header_end;
        const char *sc = find_header_next((const char *)raw_buf, header_end, "set-cookie", NULL);
        while (sc) {
            const char *line_end = sc;
            while (line_end < hdr_end && *line_end != '\r' && *line_end != '\n') line_end++;
            cookie_parse_set_cookie(cur_host, sc, line_end);
            sc = find_header_next((const char *)raw_buf, header_end, "set-cookie", sc);
        }

        if (is_redirect_status(*status_out) && hop < HTTP_MAX_REDIRECTS) {
            const char *loc = find_header((const char *)raw_buf, header_end, "location");
            if (loc) {
                const char *line_end = loc;
                while (line_end < hdr_end && *line_end != '\r' && *line_end != '\n') line_end++;
                char next_host[128], next_path[512];
                uint16_t next_port;
                strcpy(next_host, cur_host);
                next_port = cur_port;
                if (parse_location(loc, (uint32_t)(line_end - loc), next_host, sizeof(next_host),
                                    &next_port, next_path, sizeof(next_path))) {
                    strcpy(cur_host, next_host);
                    cur_port = next_port;
                    strcpy(cur_path, next_path);
                    serial_printf("http: %d redirect -> %s:%u%s\n", *status_out, cur_host, cur_port, cur_path);
                    continue;
                }
            }
        }
        break;
    }

    const uint8_t *body = raw_buf + header_end;
    uint32_t body_avail = total - header_end;

    const char *chunked = find_header((const char *)raw_buf, header_end, "transfer-encoding");
    int is_chunked = chunked && ci_starts_with(chunked, "chunked");

    char content_type_buf[HTTP_CACHE_CT_LEN];
    content_type_buf[0] = 0;
    {
        const char *ct = find_header((const char *)raw_buf, header_end, "content-type");
        if (ct) {
            const char *line_end = ct;
            const char *hdr_end = (const char *)raw_buf + header_end;
            while (line_end < hdr_end && *line_end != '\r' && *line_end != '\n') line_end++;
            uint32_t n = (uint32_t)(line_end - ct);
            if (n > sizeof(content_type_buf) - 1) n = sizeof(content_type_buf) - 1;
            memcpy(content_type_buf, ct, n);
            content_type_buf[n] = 0;
        }
    }
    if (content_type_out && content_type_cap) {
        strncpy(content_type_out, content_type_buf, content_type_cap - 1);
        content_type_out[content_type_cap - 1] = 0;
    }

    /* Decode Transfer-Encoding (chunked) into comp_buf first -- it may
     * still be Content-Encoding-compressed at this point, hence the
     * name: the compressed-but-de-chunked body. */
    uint32_t comp_len;
    if (is_chunked) {
        comp_len = decode_chunked(body, body_avail, comp_buf, HTTP_RAW_BUF_SIZE);
    } else {
        const char *cl = find_header((const char *)raw_buf, header_end, "content-length");
        uint32_t content_length = cl ? parse_uint(cl, (const char *)raw_buf + header_end) : body_avail;
        comp_len = content_length < body_avail ? content_length : body_avail;
        if (comp_len > HTTP_RAW_BUF_SIZE) comp_len = HTTP_RAW_BUF_SIZE;
        memcpy(comp_buf, body, comp_len);
    }

    const char *enc = find_header((const char *)raw_buf, header_end, "content-encoding");
    int is_gzip = enc && ci_starts_with(enc, "gzip");
    int is_deflate = enc && ci_starts_with(enc, "deflate");

    uint32_t body_len;
    if (is_gzip) {
        gzip_decompress(comp_buf, comp_len, (uint8_t *)body_out, body_cap, &body_len);
    } else if (is_deflate) {
        deflate_decompress(comp_buf, comp_len, (uint8_t *)body_out, body_cap, &body_len);
    } else {
        body_len = comp_len < body_cap ? comp_len : body_cap;
        memcpy(body_out, comp_buf, body_len);
    }

    *body_len_out = body_len;
    serial_printf("http: %s%s -> status=%d body=%u bytes%s%s\n",
                  host, path, *status_out, body_len, is_chunked ? " (chunked)" : "",
                  is_gzip ? " (gzip)" : is_deflate ? " (deflate)" : "");

    const char *cc = find_header((const char *)raw_buf, header_end, "cache-control");
    if (cc) {
        const char *hdr_end = (const char *)raw_buf + header_end;
        const char *line_end = cc;
        while (line_end < hdr_end && *line_end != '\r' && *line_end != '\n') line_end++;
        if (!cache_control_has(cc, line_end, "no-store") && !cache_control_has(cc, line_end, "no-cache")) {
            int max_age = cache_control_max_age(cc, line_end);
            if (max_age > 0) {
                cache_store(cache_key, *status_out, content_type_buf, (const uint8_t *)body_out,
                            body_len, (uint32_t)max_age);
            }
        }
    }
    return 1;
}
