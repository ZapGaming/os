/* RFC 6455 WebSocket client, layered on net/tcp.c's single global TCP
 * connection -- see include/net/websocket.h for the scope this is held
 * to (ws:// only, one connection at a time, messages capped at
 * WS_MAX_MESSAGE, binary frames delivered as a raw byte buffer since
 * this engine has no ArrayBuffer) and why.
 *
 * Three things happen here that net/http.c's request/response model
 * has no equivalent of, which is why this isn't just "http_get() with
 * different headers":
 *   1. The handshake response has to be read byte-by-byte looking for
 *      the header terminator, same as http.c, but the connection is
 *      then kept open indefinitely afterward instead of being closed --
 *      there's no "whole response" to wait for.
 *   2. Everything client-to-server has to be masked with a fresh random
 *      32-bit key per frame (RFC 6455 5.3); everything server-to-client
 *      arrives unmasked. Getting this backwards in either direction is
 *      a protocol violation a real server will reject/drop.
 *   3. Frame payload lengths use a 3-tier variable-width encoding
 *      (7 bits inline, or a 16-bit, or a 64-bit extended length) that
 *      HTTP's plain Content-Length header never needed. */
#include <net/websocket.h>
#include <net/tcp.h>
#include <net/dns.h>
#include <net/sha1.h>
#include <kernel/pit.h>
#include <kernel/serial.h>
#include <string.h>

/* Raw bytes read from tcp_recv() but not yet resolved into a complete
 * frame -- sized for one full WS_MAX_MESSAGE-sized frame (header plus
 * payload) with extra slack, since one round of draining tcp_recv()
 * inside ws_poll() may well pull in more than one frame's worth of
 * bytes at once (a chatty server, or several small frames coalesced
 * into one TCP segment). */
#define WS_RAW_BUF_SIZE (WS_MAX_MESSAGE + 8192)

#define WS_HANDSHAKE_TIMEOUT_TICKS 500 /* 5s at the PIT's 100Hz -- generous, matching http.c's own waits */

/* One connection at a time -- see the file header comment and
 * include/net/websocket.h. Mirrors net/tcp.c's own static `conn`. */
static struct {
    int open;

    uint8_t raw_buf[WS_RAW_BUF_SIZE];
    uint32_t raw_len;

    uint8_t msg_buf[WS_MAX_MESSAGE];
    uint32_t msg_len;
    int reassembling;   /* 1 while a fragmented (FIN=0) message is being reassembled */
    int msg_opcode;     /* opcode of the first fragment of the message being reassembled */

    uint32_t prng_state;
} ws;

/* ---- xorshift32, seeded from pit_ticks() --------------------------
 * Used for both the Sec-WebSocket-Key nonce and every outgoing frame's
 * masking key. Neither needs to be cryptographically unpredictable --
 * RFC 6455's own rationale for the mask is defeating naive proxy
 * caches/legacy intermediaries misinterpreting WebSocket traffic as
 * something else, not a security boundary -- so this from-scratch PRNG
 * (there is no arc4random/getrandom in this freestanding kernel) is
 * good enough, same spirit as tcp.c seeding its initial sequence number
 * from pit_ticks() rather than a real RNG. */
static uint32_t ws_rand(void) {
    if (ws.prng_state == 0) ws.prng_state = pit_ticks() * 2654435761u + 0x9E3779B9u;
    ws.prng_state ^= ws.prng_state << 13;
    ws.prng_state ^= ws.prng_state >> 17;
    ws.prng_state ^= ws.prng_state << 5;
    return ws.prng_state;
}

/* ---- base64 (encode only -- nothing here ever needs to decode it) --*/
static void base64_encode(const uint8_t *data, uint32_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | (uint32_t)data[i + 2];
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = tbl[(n >> 6) & 0x3F];
        out[o++] = tbl[n & 0x3F];
        i += 3;
    }
    uint32_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = tbl[(n >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = 0;
}

/* ---- tiny local header-parsing helpers, duplicated rather than shared
 * with net/http.c's own (static-to-that-file) equivalents -- same
 * per-file convention this codebase already uses elsewhere (e.g.
 * net/dhcp.c/net/http.c each parsing their own small integers rather
 * than sharing one utility). ------------------------------------------*/
static int ws_ci_char_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}
static int ws_ci_starts_with(const char *s, const char *end, const char *prefix) {
    while (*prefix) {
        if (s >= end || !ws_ci_char_eq(*s, *prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}
/* Case-insensitively finds "name: value" within [headers, headers+len)
 * and returns a pointer to the value (past any leading spaces); not
 * NUL-terminated, caller must stop at the line's own \r\n. */
static const char *ws_find_header(const char *headers, uint32_t len, const char *name) {
    uint32_t name_len = (uint32_t)strlen(name);
    const char *p = headers;
    const char *end = headers + len;
    while (p < end) {
        if ((uint32_t)(end - p) >= name_len + 1 && ws_ci_starts_with(p, end, name) && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (v < end && *v == ' ') v++;
            return v;
        }
        while (p < end && *p != '\n') p++;
        p++;
    }
    return NULL;
}

/* Splits "host[:port]/path" (the scheme has already been stripped by
 * the caller) exactly like gui/compositor.c's br_parse_url() does for
 * http(s) URLs -- same host/port/path defaulting conventions, just
 * ws:// instead. */
static int ws_parse_url(const char *url, char *host, int host_cap, uint16_t *port, char *path, int path_cap) {
    const char *p = url;
    if (p[0] == 'w' && p[1] == 's' && p[2] == ':' && p[3] == '/' && p[4] == '/') {
        p += 5;
    } else if (p[0] == 'w' && p[1] == 's' && p[2] == 's' && p[3] == ':' && p[4] == '/' && p[5] == '/') {
        serial_printf("ws: wss:// (TLS-over-WebSocket) is not supported -- see include/net/websocket.h\n");
        return 0;
    } else {
        serial_printf("ws: URL must start with ws:// (got: %s)\n", url);
        return 0;
    }

    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < host_cap - 1) host[i++] = *p++;
    host[i] = 0;
    if (i == 0) return 0;

    *port = 80;
    if (*p == ':') {
        p++;
        int prt = 0;
        while (*p >= '0' && *p <= '9') { prt = prt * 10 + (*p - '0'); p++; }
        if (prt > 0 && prt < 65536) *port = (uint16_t)prt;
    }

    if (*p == 0) strcpy(path, "/");
    else {
        strncpy(path, p, (size_t)path_cap - 1);
        path[path_cap - 1] = 0;
    }
    return 1;
}

/* Builds and sends one frame: FIN=1 (this file never fragments its own
 * outgoing frames -- there's no need to, everything it ever sends fits
 * comfortably in one frame), masked as required for every client-to-
 * server frame. Used for text messages (ws_send_text), Pong replies to
 * a Ping, and the Close frame ws_close() sends. */
static int ws_send_frame(uint8_t opcode, const void *payload, uint32_t len) {
    if (!ws.open) return 0;

    uint8_t header[14]; /* 1 (opcode/FIN) + up to 9 (length) + 4 (mask) */
    uint32_t hlen = 0;
    header[hlen++] = (uint8_t)(0x80 | opcode);

    if (len <= 125) {
        header[hlen++] = (uint8_t)(0x80 | len);
    } else if (len <= 0xFFFF) {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (uint8_t)(len >> 8);
        header[hlen++] = (uint8_t)len;
    } else {
        header[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) header[hlen++] = (uint8_t)(((uint64_t)len) >> (8 * i));
    }

    uint8_t mask[4];
    uint32_t r = ws_rand();
    mask[0] = (uint8_t)(r >> 24); mask[1] = (uint8_t)(r >> 16);
    mask[2] = (uint8_t)(r >> 8);  mask[3] = (uint8_t)r;
    memcpy(header + hlen, mask, 4);
    hlen += 4;

    if (!tcp_send(header, (uint16_t)hlen)) return 0;
    if (len == 0) return 1;

    /* Masked into a scratch buffer, not the caller's own -- ws_send_text()'s
     * caller (JS's ws.send(str), see js/dom_binding.c) owns that string and
     * shouldn't see it come back XOR'd. Chunked for anything bigger than
     * one stack buffer, same convention tcp_send() itself uses above
     * TCP_MAX_SEGMENT. */
    uint8_t chunk[1024];
    const uint8_t *p = (const uint8_t *)payload;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t take = len - sent < sizeof(chunk) ? len - sent : (uint32_t)sizeof(chunk);
        for (uint32_t i = 0; i < take; i++) chunk[i] = (uint8_t)(p[sent + i] ^ mask[(sent + i) % 4]);
        if (!tcp_send(chunk, (uint16_t)take)) return 0;
        sent += take;
    }
    return 1;
}

int ws_send_text(const char *text) {
    return ws_send_frame(0x1, text, (uint32_t)strlen(text));
}

void ws_close(void) {
    if (ws.open) ws_send_frame(0x8, NULL, 0);
    ws.open = 0;
    tcp_close();
    ws.raw_len = 0;
    ws.msg_len = 0;
    ws.reassembling = 0;
}

int ws_is_open(void) { return ws.open; }

int ws_connect(const char *url) {
    if (ws.open) ws_close();

    char host[64], path[192];
    uint16_t port;
    if (!ws_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) return 0;

    uint32_t ip;
    if (!dns_resolve(host, &ip)) {
        serial_printf("ws: DNS resolution failed for %s\n", host);
        return 0;
    }
    if (!tcp_connect(ip, port)) {
        serial_printf("ws: TCP connect failed to %s:%u\n", host, port);
        return 0;
    }

    /* Sec-WebSocket-Key: 16 random bytes, base64-encoded to 24 chars
     * (see ws_rand()'s comment above for why xorshift32 is fine here). */
    ws.prng_state ^= pit_ticks() * 2654435761u + 0x9E3779B9u;
    uint8_t key_bytes[16];
    for (int i = 0; i < 16; i++) {
        uint32_t r = ws_rand();
        key_bytes[i] = (uint8_t)(r >> (8 * (i % 4)));
    }
    char key_b64[25];
    base64_encode(key_bytes, 16, key_b64);

    char req[512];
    strcpy(req, "GET ");
    strcat(req, path);
    strcat(req, " HTTP/1.1\r\nHost: ");
    strcat(req, host);
    strcat(req, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    strcat(req, key_b64);
    strcat(req, "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: ZapOS/1.0\r\n\r\n");

    if (!tcp_send(req, (uint16_t)strlen(req))) {
        serial_printf("ws: sending handshake request failed\n");
        tcp_close();
        return 0;
    }

    /* Read the handshake response the same way http.c reads a response's
     * headers -- byte-by-byte via non-blocking tcp_recv(), looking for
     * the \r\n\r\n terminator -- but stop there instead of reading a
     * body: a 101 response has none, and the connection stays open for
     * frames afterward instead of being closed. */
    char resp[1024];
    uint32_t resp_len = 0;
    uint32_t header_end = 0;
    uint32_t start = pit_ticks();
    for (;;) {
        int got = tcp_recv(resp + resp_len, (uint16_t)(sizeof(resp) - resp_len > 512 ? 512 : sizeof(resp) - resp_len));
        if (got > 0) {
            resp_len += (uint32_t)got;
            for (uint32_t i = 0; i + 3 < resp_len; i++) {
                if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
                    header_end = i + 4;
                    break;
                }
            }
            if (header_end) break;
        } else if (got < 0) {
            serial_printf("ws: connection closed during handshake\n");
            tcp_close();
            return 0;
        }
        if (resp_len >= sizeof(resp) - 1) {
            serial_printf("ws: handshake response headers too large\n");
            tcp_close();
            return 0;
        }
        if (pit_ticks() - start > WS_HANDSHAKE_TIMEOUT_TICKS) {
            serial_printf("ws: handshake timed out waiting for a response\n");
            tcp_close();
            return 0;
        }
        pit_sleep(10);
    }

    const char *p = resp;
    const char *hdr_end_p = resp + header_end;
    while (p < hdr_end_p && *p != ' ') p++;
    if (p < hdr_end_p) p++;
    int status = 0;
    while (p < hdr_end_p && *p >= '0' && *p <= '9') { status = status * 10 + (*p - '0'); p++; }
    if (status != 101) {
        serial_printf("ws: server did not upgrade (status %d, expected 101)\n", status);
        tcp_close();
        return 0;
    }

    const char *accept = ws_find_header(resp, header_end, "sec-websocket-accept");
    if (!accept) {
        serial_printf("ws: 101 response has no Sec-WebSocket-Accept header\n");
        tcp_close();
        return 0;
    }
    const char *accept_end = accept;
    while (accept_end < hdr_end_p && *accept_end != '\r' && *accept_end != '\n') accept_end++;

    char accept_src[24 + 36 + 1];
    strcpy(accept_src, key_b64);
    strcat(accept_src, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"); /* RFC 6455 1.3's fixed GUID */
    uint8_t digest[SHA1_DIGEST_SIZE];
    sha1(accept_src, (uint32_t)strlen(accept_src), digest);
    char expected[32];
    base64_encode(digest, SHA1_DIGEST_SIZE, expected);

    uint32_t accept_len = (uint32_t)(accept_end - accept);
    if (accept_len != strlen(expected) || memcmp(accept, expected, accept_len) != 0) {
        serial_printf("ws: Sec-WebSocket-Accept mismatch -- handshake rejected\n");
        tcp_close();
        return 0;
    }

    /* Anything the server already sent past the header terminator in the
     * same read is real frame data, not part of the handshake -- keep it
     * rather than dropping it on the floor. */
    ws.raw_len = 0;
    if (resp_len > header_end) {
        uint32_t leftover = resp_len - header_end;
        if (leftover > sizeof(ws.raw_buf)) leftover = (uint32_t)sizeof(ws.raw_buf);
        memcpy(ws.raw_buf, resp + header_end, leftover);
        ws.raw_len = leftover;
    }

    ws.open = 1;
    ws.msg_len = 0;
    ws.reassembling = 0;
    serial_printf("ws: connected to %s:%u%s\n", host, port, path);
    return 1;
}

struct ws_frame {
    int fin;
    uint8_t opcode;
    uint8_t *payload; /* points inside ws.raw_buf; unmasked in place already if it was masked */
    uint32_t payload_len;
    uint32_t frame_len; /* total bytes (header+mask+payload) this frame occupies in raw_buf */
};

/* Parses one frame starting at buf[0] per RFC 6455 5.2 (FIN/opcode,
 * the 7-bit/16-bit/64-bit payload-length tiers, and an optional mask --
 * server frames are required by spec to arrive unmasked, but this
 * unmasks anyway if the bit happens to be set, for robustness rather
 * than strict spec-policing). Returns 1 with `out` filled in if a whole
 * frame is present, 0 if buf doesn't hold a complete frame yet (wait
 * for more bytes), or -1 if the frame's declared length exceeds
 * WS_MAX_MESSAGE (a fatal desync -- there's no partial-frame delivery
 * in this protocol, so the only options are buffer it whole or give up). */
static int ws_parse_frame(uint8_t *buf, uint32_t len, struct ws_frame *out) {
    if (len < 2) return 0;
    int fin = (buf[0] & 0x80) != 0;
    uint8_t opcode = (uint8_t)(buf[0] & 0x0F);
    int masked = (buf[1] & 0x80) != 0;
    uint64_t plen = buf[1] & 0x7F;
    uint32_t pos = 2;

    if (plen == 126) {
        if (len < pos + 2) return 0;
        plen = ((uint32_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;
    } else if (plen == 127) {
        if (len < pos + 8) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | buf[pos + i];
        pos += 8;
    }

    if (plen > WS_MAX_MESSAGE) return -1;

    uint32_t mask_off = pos;
    if (masked) pos += 4;

    if (len < pos + plen) return 0; /* frame not fully buffered yet */

    if (masked) {
        for (uint32_t i = 0; i < plen; i++) buf[pos + i] ^= buf[mask_off + (i % 4)];
    }

    out->fin = fin;
    out->opcode = opcode;
    out->payload = buf + pos;
    out->payload_len = (uint32_t)plen;
    out->frame_len = pos + (uint32_t)plen;
    return 1;
}

enum ws_event ws_poll(char *out, uint32_t cap, uint32_t *out_len) {
    if (!ws.open) return WS_EVENT_NONE;

    /* Drain whatever tcp_recv() has buffered -- it's already non-
     * blocking (see include/net/tcp.h), so this never stalls the
     * caller's per-frame loop. */
    for (;;) {
        if (ws.raw_len >= sizeof(ws.raw_buf)) break;
        uint32_t space = (uint32_t)sizeof(ws.raw_buf) - ws.raw_len;
        int got = tcp_recv(ws.raw_buf + ws.raw_len, (uint16_t)(space > 1400 ? 1400 : space));
        if (got > 0) {
            ws.raw_len += (uint32_t)got;
            continue;
        }
        if (got < 0) {
            /* Peer dropped the TCP connection without a clean Close
             * frame -- still a close from this browser's point of view. */
            ws.open = 0;
            tcp_close();
            return WS_EVENT_CLOSE;
        }
        break; /* got == 0: nothing more buffered right now */
    }

    for (;;) {
        struct ws_frame frame;
        int r = ws_parse_frame(ws.raw_buf, ws.raw_len, &frame);
        if (r == 0) return WS_EVENT_NONE; /* incomplete -- wait for more bytes next poll */
        if (r < 0) {
            serial_printf("ws: frame exceeds WS_MAX_MESSAGE (%d bytes), closing\n", WS_MAX_MESSAGE);
            ws_close();
            return WS_EVENT_CLOSE;
        }

        memmove(ws.raw_buf, ws.raw_buf + frame.frame_len, ws.raw_len - frame.frame_len);
        ws.raw_len -= frame.frame_len;

        switch (frame.opcode) {
            case 0x8: /* close */
                ws_close();
                return WS_EVENT_CLOSE;

            case 0x9: /* ping -- reply with pong echoing the same payload */
                ws_send_frame(0xA, frame.payload, frame.payload_len);
                continue;

            case 0xA: /* pong */
                continue;

            case 0x0: /* continuation of a fragmented message */
                if (!ws.reassembling) continue; /* stray continuation frame -- ignore */
                if (ws.msg_len + frame.payload_len <= WS_MAX_MESSAGE) {
                    memcpy(ws.msg_buf + ws.msg_len, frame.payload, frame.payload_len);
                    ws.msg_len += frame.payload_len;
                }
                if (frame.fin) {
                    ws.reassembling = 0;
                    uint32_t n = ws.msg_len < cap ? ws.msg_len : cap;
                    memcpy(out, ws.msg_buf, n);
                    *out_len = n;
                    return WS_EVENT_MESSAGE;
                }
                continue;

            case 0x1: /* text */
            case 0x2: /* binary -- delivered identically, see the file-level scope note */
                if (frame.fin) {
                    uint32_t n = frame.payload_len < cap ? frame.payload_len : cap;
                    memcpy(out, frame.payload, n);
                    *out_len = n;
                    return WS_EVENT_MESSAGE;
                }
                ws.reassembling = 1;
                ws.msg_opcode = frame.opcode;
                ws.msg_len = frame.payload_len <= WS_MAX_MESSAGE ? frame.payload_len : WS_MAX_MESSAGE;
                memcpy(ws.msg_buf, frame.payload, ws.msg_len);
                continue;

            default:
                continue; /* unknown/reserved opcode -- ignore rather than fail the whole connection */
        }
    }
}
