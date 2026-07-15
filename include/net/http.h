#ifndef NET_HTTP_H
#define NET_HTTP_H

#include <stdint.h>

/* GET-only convenience wrapper over http_request() below -- see that
 * function's doc comment for the full mechanics (redirects, cookies,
 * chunked/gzip/deflate decoding, caching). This wrapper exists because
 * GET is overwhelmingly the common case (every caller in this repo --
 * the Browser's page/CSS/image/WASM fetches -- only ever does GET) and
 * spelling out `"GET", NULL, NULL, 0` at every call site would just be
 * noise. Signature and behavior are UNCHANGED from before http_request()
 * existed: `return http_request(use_tls, host, port, "GET", path, NULL,
 * NULL, 0, status_out, ...)` produces byte-identical request traffic and
 * identical caching behavior to the original GET-only implementation.
 *
 * Fetches `path` from `host`:`port` with a simple GET (HTTP/1.1,
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

/* General-purpose outbound HTTP(S) request -- GET, POST, or any other
 * method, with an optional caller-supplied request body and extra
 * headers. This is generic HTTP client capability: ZapOS itself has NO
 * bundled AI/API integration, ships NO API keys, and this function does
 * not itself talk to any particular web service -- it just gives a
 * caller (in the kernel, or a user app via SYS_HTTP_REQUEST, see
 * include/kernel/syscall.h) the ability to make a request with whatever
 * method/headers/body a real API (an AI provider's chat-completions
 * endpoint, a weather API, anything) requires, using the caller's own
 * endpoint and credentials.
 *
 * `method` is a NUL-terminated verb, e.g. "GET" or "POST" -- used
 * verbatim as the request line's method token, no validation against a
 * fixed list.
 *
 * `extra_headers` is NULL, or a caller-supplied block of one or more
 * already-formatted "Header-Name: value\r\n" lines (concatenated, no
 * separator needed beyond each line's own trailing \r\n), spliced into
 * the request between the standard headers this function always sends
 * (Host/User-Agent/Accept-Encoding/Cookie) and the request's end (its
 * Connection: close line and final blank line/body). This is how a
 * caller supplies e.g. "Authorization: Bearer sk-...\r\n" or
 * "Content-Type: application/json\r\n" for a real API call. Bounded
 * defensively against the internal request-header buffer's fixed
 * capacity (see net/http.c's HTTP_REQ_HDR_BUF_SIZE) -- an oversized
 * block is truncated, not overflowed.
 *
 * `body`/`body_len`: if `body` is non-NULL and `body_len > 0`, a
 * "Content-Length: <body_len>\r\n\r\n" header is appended followed by
 * exactly `body_len` raw bytes from `body` (sent as opaque bytes, NOT
 * assumed to be NUL-terminated text -- a JSON payload could in
 * principle contain any byte). If `body` is NULL (or `body_len` is 0),
 * behavior is exactly the original bare "\r\n\r\n" terminator with no
 * body and no Content-Length header.
 *
 * Redirects: 3xx responses are ONLY followed when `method` is exactly
 * "GET" (case-sensitive) -- resubmitting a POST's body to a redirect
 * target is not correct HTTP semantics, so a non-GET method that hits a
 * 301/302/303/307/308 just returns that response as-is (status code and
 * whatever body came with it), without touching Location at all.
 *
 * Caching: the in-memory response cache (see http_get()'s doc comment
 * above) is used, and populated, ONLY for `method == "GET"` -- a POST
 * (or any other non-GET method) never reads from or writes to it, so a
 * cached GET response can never leak into a POST call or vice versa.
 *
 * Every other parameter (`use_tls`, `host`, `port`, `path`, `status_out`,
 * `body_out`/`body_cap`/`body_len_out`, `content_type_out`/
 * `content_type_cap`) means exactly what it means for http_get() above.
 * Returns 1 if the request mechanically completed (even for a non-2xx
 * HTTP status -- check *status_out for that), 0 if DNS/TCP/TLS itself
 * failed -- same 0/1 convention as http_get(), which is just this
 * function called with method="GET" and no headers/body. */
int http_request(int use_tls, const char *host, uint16_t port, const char *method,
                  const char *path, const char *extra_headers, const char *body, uint32_t body_len,
                  int *status_out, char *body_out, uint32_t body_cap, uint32_t *body_len_out,
                  char *content_type_out, uint32_t content_type_cap);

#endif
