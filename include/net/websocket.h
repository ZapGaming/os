#ifndef NET_WEBSOCKET_H
#define NET_WEBSOCKET_H

#include <stdint.h>

/* RFC 6455 WebSocket *client* support -- this OS's browser can open an
 * outbound ws:// connection from a page's <script>, it never accepts
 * one. Layered directly on net/tcp.c's single global TCP connection, so
 * it inherits that file's own scope cut: one WebSocket connection at a
 * time, not a connection table. Opening a new one while one is already
 * open closes the previous one first (see ws_connect()).
 *
 * Scope cuts, spelled out rather than left implicit:
 *   - No wss:// (TLS-over-WebSocket). net/tls.c's client is shaped
 *     around http.c's "send one request, read one whole response, then
 *     the connection is done" model; a WebSocket handshake instead has
 *     to send one request, read one response, and then keep the
 *     connection hanging open indefinitely for arbitrarily many further
 *     frames in both directions. Threading that through the TLS
 *     record layer is real follow-on work, not a quick extension --
 *     out of scope here. ws:// only.
 *   - No per-connection table (see above) -- consistent with tcp.c's
 *     own single global `conn`, not a new limitation this file invents.
 *   - Messages are capped at WS_MAX_MESSAGE bytes (fragmented messages
 *     across several continuation frames ARE reassembled up to that
 *     cap, they just can't exceed it) -- there is no engine-side
 *     mechanism to hand JS a message any other way than one flat
 *     buffer, and this kernel's kmalloc arena isn't sized to buffer
 *     an unbounded one anyway.
 *   - Binary frames (opcode 0x2) are accepted and delivered exactly
 *     like text frames -- as a raw byte buffer -- because this engine
 *     has no ArrayBuffer/typed-array type to hand them to JS as
 *     anything else. js/dom_binding.c turns that buffer into a plain
 *     JS string, which is non-standard (real MessageEvent.data for a
 *     binary frame is a Blob/ArrayBuffer) and lossy if the payload
 *     contains a NUL byte (the string simply ends there, same as any
 *     other C-string boundary in this engine). Documented, not fixed:
 *     this engine has no primitive that could carry an exact byte
 *     count alongside the bytes. */

#define WS_MAX_MESSAGE 4096

enum ws_event {
    WS_EVENT_NONE,    /* nothing new since the last ws_poll() */
    WS_EVENT_MESSAGE, /* a complete text/binary message (opcode 0x1/0x2, reassembled across any continuation frames) */
    WS_EVENT_CLOSE,   /* the server sent a Close frame, or the TCP connection dropped -- connection is already torn down */
};

/* Parses "ws://host[:port]/path" (default port 80, default path "/";
 * no wss://, see the scope note above -- returns 0 immediately for
 * one), resolves the host, connects via tcp_connect(), and performs
 * the RFC 6455 client handshake: sends the HTTP Upgrade request with a
 * random Sec-WebSocket-Key, reads the response, and verifies the
 * server's Sec-WebSocket-Accept against the expected SHA-1+base64
 * value. Blocking, same cooperative-wait model as tcp_connect()/
 * http_get() (busy-loops on tcp_recv() with pit_sleep() between tries,
 * bounded by a timeout). If a connection is already open, it is closed
 * first. Returns 1 once the connection is fully established and ready
 * for ws_send_text()/ws_poll(), 0 on any failure (bad/unsupported URL,
 * DNS/TCP failure, non-101 response, or an accept-key mismatch). */
int ws_connect(const char *url);

/* True once ws_connect() has succeeded and neither side has sent/seen a
 * Close frame (and the TCP connection hasn't otherwise dropped). */
int ws_is_open(void);

/* Sends `text` as one unfragmented, masked text frame (opcode 0x1) --
 * client-to-server frames are always masked with a fresh random 32-bit
 * key, per RFC 6455 5.3. Returns 1 on success, 0 if not connected or
 * the underlying tcp_send() failed. */
int ws_send_text(const char *text);

/* Sends a masked Close frame (opcode 0x8, no status code/reason
 * payload) if still open, then tears down the underlying TCP
 * connection. Safe to call when already closed (no-op). */
void ws_close(void);

/* Non-blocking poll for one incoming event, meant to be called once per
 * frame (see gui/compositor.c's gui_run(), which polls at its own
 * ~16ms cadence) -- mirrors tcp_recv()'s "nothing yet" contract via
 * WS_EVENT_NONE rather than blocking. Internally drains whatever
 * tcp_recv() has buffered and parses as many complete frames as are
 * available, silently handling Ping (replies with Pong) and Pong
 * (ignored) control frames along the way; returns as soon as it has a
 * WS_EVENT_MESSAGE or WS_EVENT_CLOSE to report, or WS_EVENT_NONE once
 * it runs out of buffered bytes without completing either.
 *
 * On WS_EVENT_MESSAGE, copies up to `cap` bytes of the message payload
 * into `out` and sets *out_len to how many (NOT NUL-terminated --
 * caller adds that if it wants a C string, see the binary-frame note
 * above re: embedded NULs). On WS_EVENT_CLOSE the connection is already
 * closed by the time this returns (ws_is_open() will read false). */
enum ws_event ws_poll(char *out, uint32_t cap, uint32_t *out_len);

#endif
