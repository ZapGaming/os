#ifndef NET_HTTP_H
#define NET_HTTP_H

#include <stdint.h>

/* Fetches `path` from `host`:`port` with a simple GET (HTTP/1.1,
 * Connection: close -- no keep-alive). `use_tls` selects net/tls.c's
 * TLS 1.2 client instead of a plain net/tcp.c connection -- see
 * include/net/tls.h for that client's scope (no certificate validation
 * of any kind: "for interoperability, not security"). Follows 3xx
 * redirects (bounded hop count, scheme included -- an http -> https
 * forced redirect switches to TLS mid-chain automatically), sends/
 * stores cookies via a small per-host jar, and decodes chunked
 * transfer-encoding and gzip/deflate content-encoding, all
 * transparently. Copies up to body_cap bytes of the final decoded body
 * into body_out, and (if the response has one) the Content-Type header
 * value into content_type_out/content_type_cap (empty string if absent
 * -- pass NULL/0 to skip). Returns 1 on success (even for non-2xx
 * statuses -- check *status_out), 0 if DNS/TCP/TLS itself failed.
 *
 * Transparently cached: a prior 200 response whose Cache-Control said
 * a real max-age (and not no-store/no-cache) is served straight out of
 * an in-memory table, keyed by host:port+path, without touching the
 * network again -- see the cache in http.c for exactly what that does
 * and doesn't cover (no ETag revalidation, no Expires header, small
 * fixed number of entries). */
int http_get(int use_tls, const char *host, uint16_t port, const char *path,
             int *status_out, char *body_out, uint32_t body_cap, uint32_t *body_len_out,
             char *content_type_out, uint32_t content_type_cap);

#endif
