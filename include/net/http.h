#ifndef NET_HTTP_H
#define NET_HTTP_H

#include <stdint.h>

/* Fetches `path` from `host`:`port` with a simple GET (HTTP/1.1,
 * Connection: close -- no keep-alive, no redirects, no HTTPS). Decodes
 * chunked transfer-encoding if the server uses it. Copies up to
 * body_cap bytes of the decoded body into body_out, and (if the
 * response has one) the Content-Type header value into
 * content_type_out/content_type_cap (empty string if absent -- pass
 * NULL/0 to skip). Returns 1 on success (even for non-2xx statuses --
 * check *status_out), 0 if DNS/TCP itself failed.
 *
 * Transparently cached: a prior 200 response whose Cache-Control said
 * a real max-age (and not no-store/no-cache) is served straight out of
 * an in-memory table, keyed by host:port+path, without touching the
 * network again -- see the cache in http.c for exactly what that does
 * and doesn't cover (no ETag revalidation, no Expires header, small
 * fixed number of entries). */
int http_get(const char *host, uint16_t port, const char *path,
             int *status_out, char *body_out, uint32_t body_cap, uint32_t *body_len_out,
             char *content_type_out, uint32_t content_type_cap);

#endif
