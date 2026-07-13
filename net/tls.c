#include <net/tls.h>
#include <net/tcp.h>
#include <net/sha256.h>
#include <net/hmac.h>
#include <net/x25519.h>
#include <net/aes.h>
#include <net/gcm.h>
#include <kernel/pit.h>
#include <kernel/serial.h>
#include <string.h>

/* See include/net/tls.h for the full scope disclosure (TLS 1.2 only,
 * one cipher suite, one curve, no certificate/signature validation of
 * any kind). This file is the record layer + handshake state machine;
 * net/sha256.c, net/hmac.c, net/x25519.c, net/aes.c and net/gcm.c are
 * the independently-verified crypto primitives it's built on. */

/* -------------------------------------------------------------------
 * Wire constants
 * ------------------------------------------------------------------- */

#define TLS_VERSION 0x0303

enum {
    TLS_CT_CHANGE_CIPHER_SPEC = 20,
    TLS_CT_ALERT              = 21,
    TLS_CT_HANDSHAKE          = 22,
    TLS_CT_APPLICATION_DATA   = 23,
};

enum {
    HS_CLIENT_HELLO       = 1,
    HS_SERVER_HELLO       = 2,
    HS_CERTIFICATE        = 11,
    HS_SERVER_KEY_EXCHANGE = 12,
    HS_SERVER_HELLO_DONE  = 14,
    HS_CLIENT_KEY_EXCHANGE = 16,
    HS_FINISHED           = 20,
};

#define TLS_CIPHER_SUITE 0xC02F /* TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 */
#define TLS_CURVE_X25519 0x001d

#define TLS_MAX_PLAINTEXT  16384
#define GCM_NONCE_EXPLICIT 8
#define TLS_MAX_CIPHERTEXT (GCM_NONCE_EXPLICIT + TLS_MAX_PLAINTEXT + GCM_TAG_SIZE)
#define TLS_RECORD_HDR_LEN 5
#define TLS_RAW_BUF_SIZE   (TLS_RECORD_HDR_LEN + TLS_MAX_CIPHERTEXT)

#define HS_REASM_SIZE      24576 /* generous room for a multi-record Certificate chain */
#define APP_RECV_BUF_SIZE  32768 /* matches net/tcp.c's own TCP_RECV_BUF_SIZE */

#define TLS_IO_TIMEOUT_TICKS 500 /* 5s per blocking wait, at 100Hz -- matches tcp.c's style */

/* -------------------------------------------------------------------
 * Connection state -- static/BSS, not on the stack: per-task kernel
 * stacks are only 16KB (kernel/scheduler.h's TASK_STACK_SIZE), and
 * this subsystem's buffers (a full 16KB+ TLS record, a handshake
 * reassembly buffer, a decrypted-application-data queue) are each
 * bigger than that on their own. Single connection at a time, exactly
 * like net/tcp.c's own `conn` -- this OS's browser never needs more.
 * ------------------------------------------------------------------- */

static struct {
    int connected;         /* handshake completed successfully */
    int peer_done;         /* peer alert seen, or underlying TCP is done */
    int client_encrypted;  /* we've sent our ChangeCipherSpec */
    int server_encrypted;  /* we've received the server's ChangeCipherSpec */

    aes128_ctx client_aes, server_aes;
    uint8_t client_iv[4], server_iv[4];
    uint64_t client_seq, server_seq;

    sha256_ctx transcript; /* running hash of every handshake message, both directions */

    uint8_t raw_buf[TLS_RAW_BUF_SIZE]; /* raw bytes from tcp_recv(), possibly a partial record */
    uint32_t raw_len;

    uint8_t hs_reasm[HS_REASM_SIZE]; /* reassembled handshake-message plaintext, handshake phase only */
    uint32_t hs_reasm_len;

    uint8_t app_recv_buf[APP_RECV_BUF_SIZE]; /* decrypted application data awaiting tls_recv() */
    uint32_t app_recv_len;

    /* rec_scratch holds one record's raw (still-ciphertext, if
     * encryption is active) payload; msg_scratch holds the decrypted
     * (or, pre-CCS, as-received) result. These are kept as two
     * distinct buffers -- rather than decrypting in place over
     * rec_scratch -- so callers never have to reason about whether
     * gcm_decrypt()'s particular internal processing order happens to
     * make a given in/out aliasing safe. */
    uint8_t rec_scratch[TLS_MAX_CIPHERTEXT];
    uint8_t msg_scratch[TLS_MAX_PLAINTEXT];

    uint8_t hs_body[HS_REASM_SIZE]; /* one reassembled handshake message's body, handshake phase only */

    uint8_t send_out[TLS_RECORD_HDR_LEN + TLS_MAX_CIPHERTEXT]; /* one outgoing record, header included */
} tls;

/* -------------------------------------------------------------------
 * A small hash-based PRNG for the ephemeral X25519 private key and the
 * ClientHello random. This is NOT a cryptographically vetted RNG -- it
 * mixes RDTSC jitter (sampled across several PIT-interrupt-driven HLTs
 * at boot, then ratcheted forward with a running counter and a fresh
 * RDTSC reading on every call) through SHA-256. Entirely in keeping
 * with this subsystem's disclosed "interoperability, not security"
 * scope: a weak ephemeral key only weakens the *forward secrecy* of a
 * connection whose server identity was never authenticated in the
 * first place.
 * ------------------------------------------------------------------- */

static inline uint64_t tls_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Cooperative "wait for the next interrupt" primitive. TLS_HOST_TEST is
 * defined only by the host-side integration test harness used during
 * this subsystem's development (to drive this exact file, unmodified
 * apart from this one swap, against a real TLS server over a BSD
 * socket) -- `hlt` is a privileged instruction that would just crash a
 * userspace process, so the host build substitutes a short sleep with
 * the same "yield, then re-check" role. The freestanding kernel build
 * always takes the real `hlt`. */
#ifdef TLS_HOST_TEST
#include <time.h>
static inline void tls_halt(void) {
    struct timespec ts = { 0, 1000000 }; /* 1ms */
    nanosleep(&ts, NULL);
}
#else
static inline void tls_halt(void) { __asm__ volatile ("hlt"); }
#endif

static uint8_t rng_pool[32];
static int rng_seeded = 0;
static uint32_t rng_counter = 0;

static void rng_seed_once(void) {
    if (rng_seeded) return;
    uint8_t acc[32];
    memset(acc, 0, sizeof(acc));
    for (int i = 0; i < 32; i++) {
        tls_halt(); /* wakes on the next interrupt (PIT, NIC, ...) -- real jitter */
        uint64_t tsc = tls_rdtsc();
        uint32_t ticks = pit_ticks();
        uint8_t mix[40];
        memcpy(mix, acc, 32);
        memcpy(mix + 32, &tsc, 4);
        memcpy(mix + 36, &ticks, 4);
        sha256(mix, sizeof(mix), acc);
    }
    memcpy(rng_pool, acc, 32);
    rng_seeded = 1;
}

static void tls_random_bytes(uint8_t *out, uint32_t len) {
    rng_seed_once();
    while (len > 0) {
        uint64_t tsc = tls_rdtsc();
        rng_counter++;
        uint8_t input[40];
        memcpy(input, rng_pool, 32);
        memcpy(input + 32, &rng_counter, 4);
        memcpy(input + 36, &tsc, 4);
        uint8_t block[32];
        sha256(input, sizeof(input), block);
        memcpy(rng_pool, block, 32); /* ratchet forward */
        uint32_t take = len < 32 ? len : 32;
        memcpy(out, block, take);
        out += take;
        len -= take;
    }
}

/* -------------------------------------------------------------------
 * TLS 1.2 PRF (RFC 5246 Section 5), P_SHA256 built on HMAC-SHA256.
 * Verified on the host against an independent Python/hmac
 * reimplementation before being transplanted here unchanged -- see
 * this project's development notes; the three cases checked were
 * 48-, 12- and 100-byte outputs (48 = master_secret's size, 12 =
 * Finished's verify_data size, 100 to exercise more than 3 HMAC
 * iterations).
 * ------------------------------------------------------------------- */

#define PRF_MAX_SEED_LEN 128

static void tls_prf(const uint8_t *secret, uint32_t secret_len,
                     const uint8_t *seed, uint32_t seed_len,
                     uint8_t *out, uint32_t out_len) {
    uint8_t a[32];
    hmac_sha256(secret, secret_len, seed, seed_len, a);

    uint8_t input[32 + PRF_MAX_SEED_LEN];
    while (out_len > 0) {
        memcpy(input, a, 32);
        memcpy(input + 32, seed, seed_len);
        uint8_t block[32];
        hmac_sha256(secret, secret_len, input, 32 + seed_len, block);
        uint32_t take = out_len < 32 ? out_len : 32;
        memcpy(out, block, take);
        out += take;
        out_len -= take;

        uint8_t a_next[32];
        hmac_sha256(secret, secret_len, a, 32, a_next);
        memcpy(a, a_next, 32);
    }
}

/* -------------------------------------------------------------------
 * Record layer
 * ------------------------------------------------------------------- */

/* Blocks (cooperatively, like tcp.c's own send/connect loops) until at
 * least `need` bytes are sitting in tls.raw_buf, pulling more from
 * tcp_recv() as needed. Returns 0 on timeout or once the underlying
 * TCP connection is done (FIN + drained, or a hard TCP failure). */
static int fill_raw(uint32_t need) {
    if (need > sizeof(tls.raw_buf)) return 0;
    uint32_t start = pit_ticks();
    while (tls.raw_len < need) {
        int got = tcp_recv(tls.raw_buf + tls.raw_len, (uint16_t)(sizeof(tls.raw_buf) - tls.raw_len));
        if (got > 0) {
            tls.raw_len += (uint32_t)got;
            start = pit_ticks();
        } else if (got < 0) {
            tls.peer_done = 1;
            return 0;
        } else {
            if (pit_ticks() - start > TLS_IO_TIMEOUT_TICKS) return 0;
            tls_halt();
        }
    }
    return 1;
}

/* Reads exactly one raw TLS record (header + full fragment) off the
 * wire -- the fragment is still ciphertext if encryption is active on
 * this direction; read_plaintext_record() below handles that. */
static int read_record(uint8_t *type_out, uint16_t *ver_out, uint8_t *payload, uint32_t *payload_len) {
    if (!fill_raw(TLS_RECORD_HDR_LEN)) return 0;
    uint8_t type = tls.raw_buf[0];
    uint16_t ver = (uint16_t)((tls.raw_buf[1] << 8) | tls.raw_buf[2]);
    uint16_t len = (uint16_t)((tls.raw_buf[3] << 8) | tls.raw_buf[4]);
    if (len > TLS_MAX_CIPHERTEXT) return 0; /* not a record we could have sent/expect */
    if (!fill_raw((uint32_t)TLS_RECORD_HDR_LEN + len)) return 0;

    memcpy(payload, tls.raw_buf + TLS_RECORD_HDR_LEN, len);
    *payload_len = len;
    *type_out = type;
    if (ver_out) *ver_out = ver;

    uint32_t consumed = (uint32_t)TLS_RECORD_HDR_LEN + len;
    memmove(tls.raw_buf, tls.raw_buf + consumed, tls.raw_len - consumed);
    tls.raw_len -= consumed;
    return 1;
}

/* Reads one record and, if the server->client direction is currently
 * encrypted, GCM-decrypts and tag-checks it (a failed tag is always
 * fatal -- see include/net/tls.h). `payload` receives the plaintext
 * (or the as-received bytes, before ChangeCipherSpec). */
static int read_plaintext_record(uint8_t *type_out, uint8_t *payload, uint32_t *payload_len) {
    uint16_t ver;
    uint32_t raw_len;
    if (!read_record(type_out, &ver, tls.rec_scratch, &raw_len)) return 0;

    if (!tls.server_encrypted) {
        /* Pre-CCS records should never exceed the 2^14 plaintext
         * fragment limit; `payload` (tls.msg_scratch at every call
         * site) is only sized for that, not read_record()'s more
         * permissive post-CCS ciphertext bound. */
        if (raw_len > TLS_MAX_PLAINTEXT) return 0;
        memcpy(payload, tls.rec_scratch, raw_len);
        *payload_len = raw_len;
        return 1;
    }

    if (raw_len < GCM_NONCE_EXPLICIT + GCM_TAG_SIZE) return 0;
    uint32_t ct_len = raw_len - GCM_NONCE_EXPLICIT - GCM_TAG_SIZE;

    uint8_t nonce[GCM_IV_SIZE];
    memcpy(nonce, tls.server_iv, 4);
    memcpy(nonce + 4, tls.rec_scratch, GCM_NONCE_EXPLICIT);

    uint8_t aad[13];
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(tls.server_seq >> (56 - 8 * i));
    aad[8] = *type_out;
    aad[9] = (uint8_t)(ver >> 8);
    aad[10] = (uint8_t)ver;
    aad[11] = (uint8_t)(ct_len >> 8);
    aad[12] = (uint8_t)ct_len;

    int ok = gcm_decrypt(&tls.server_aes, nonce, aad, sizeof(aad),
                          tls.rec_scratch + GCM_NONCE_EXPLICIT, ct_len,
                          payload, tls.rec_scratch + GCM_NONCE_EXPLICIT + ct_len);
    tls.server_seq++;
    if (!ok) {
        serial_printf("tls: GCM tag mismatch on incoming record -- dropping connection\n");
        return 0;
    }
    *payload_len = ct_len;
    return 1;
}

/* Builds and sends one record of `type` carrying `payload` (encrypting
 * it first if the client->server direction is currently encrypted).
 * Writes directly into the static tls.send_out rather than a stack
 * buffer -- a full-size record is bigger than this kernel's entire
 * 16KB per-task stack (kernel/scheduler.h's TASK_STACK_SIZE). */
static int send_record(uint8_t type, const uint8_t *payload, uint32_t payload_len) {
    uint8_t *out = tls.send_out;

    if (!tls.client_encrypted) {
        out[0] = type;
        out[1] = (uint8_t)(TLS_VERSION >> 8);
        out[2] = (uint8_t)TLS_VERSION;
        out[3] = (uint8_t)(payload_len >> 8);
        out[4] = (uint8_t)payload_len;
        memcpy(out + TLS_RECORD_HDR_LEN, payload, payload_len);
        return tcp_send(out, (uint16_t)(TLS_RECORD_HDR_LEN + payload_len));
    }

    uint8_t nonce_explicit[GCM_NONCE_EXPLICIT];
    for (int i = 0; i < 8; i++) nonce_explicit[i] = (uint8_t)(tls.client_seq >> (56 - 8 * i));

    uint8_t nonce[GCM_IV_SIZE];
    memcpy(nonce, tls.client_iv, 4);
    memcpy(nonce + 4, nonce_explicit, GCM_NONCE_EXPLICIT);

    uint8_t aad[13];
    memcpy(aad, nonce_explicit, 8);
    aad[8] = type;
    aad[9] = (uint8_t)(TLS_VERSION >> 8);
    aad[10] = (uint8_t)TLS_VERSION;
    aad[11] = (uint8_t)(payload_len >> 8);
    aad[12] = (uint8_t)payload_len;

    uint32_t frag_len = GCM_NONCE_EXPLICIT + payload_len + GCM_TAG_SIZE;
    out[0] = type;
    out[1] = (uint8_t)(TLS_VERSION >> 8);
    out[2] = (uint8_t)TLS_VERSION;
    out[3] = (uint8_t)(frag_len >> 8);
    out[4] = (uint8_t)frag_len;
    memcpy(out + TLS_RECORD_HDR_LEN, nonce_explicit, GCM_NONCE_EXPLICIT);

    /* Ciphertext lands straight in out[]; `payload` is always a
     * distinct buffer from tls.send_out at every call site below, so
     * this is a plain (non-aliased) in/out pair for gcm_encrypt(). */
    uint8_t tag[GCM_TAG_SIZE];
    gcm_encrypt(&tls.client_aes, nonce, aad, sizeof(aad), payload, payload_len,
                out + TLS_RECORD_HDR_LEN + GCM_NONCE_EXPLICIT, tag);
    tls.client_seq++;

    memcpy(out + TLS_RECORD_HDR_LEN + GCM_NONCE_EXPLICIT + payload_len, tag, GCM_TAG_SIZE);

    return tcp_send(out, (uint16_t)(TLS_RECORD_HDR_LEN + frag_len));
}

/* -------------------------------------------------------------------
 * Handshake message framing (on top of the record layer above).
 * Reading reassembles a message across as many records as it takes
 * (a Certificate chain can outrun one 16KB record); sending is always
 * one message per record here since every message we send is small.
 * Every message that crosses the wire in either direction is fed into
 * `tls.transcript` exactly once, in wire order.
 * ------------------------------------------------------------------- */

static int send_handshake(uint8_t hs_type, const uint8_t *body, uint32_t body_len) {
    uint8_t msg[4 + 512]; /* every handshake message we send fits comfortably in 512 bytes */
    if (body_len > 512) return 0;
    msg[0] = hs_type;
    msg[1] = (uint8_t)(body_len >> 16);
    msg[2] = (uint8_t)(body_len >> 8);
    msg[3] = (uint8_t)body_len;
    memcpy(msg + 4, body, body_len);

    sha256_update(&tls.transcript, msg, 4 + body_len);
    return send_record(TLS_CT_HANDSHAKE, msg, 4 + body_len);
}

static int read_handshake_message(uint8_t *type_out, uint8_t *body, uint32_t *body_len, uint32_t max_body) {
    for (;;) {
        if (tls.hs_reasm_len >= 4) {
            uint32_t mlen = ((uint32_t)tls.hs_reasm[1] << 16) | ((uint32_t)tls.hs_reasm[2] << 8) | tls.hs_reasm[3];
            if (tls.hs_reasm_len >= 4 + mlen) {
                if (mlen > max_body) return 0;
                *type_out = tls.hs_reasm[0];
                memcpy(body, tls.hs_reasm + 4, mlen);
                *body_len = mlen;

                sha256_update(&tls.transcript, tls.hs_reasm, 4 + mlen);
                memmove(tls.hs_reasm, tls.hs_reasm + 4 + mlen, tls.hs_reasm_len - (4 + mlen));
                tls.hs_reasm_len -= (4 + mlen);
                return 1;
            }
        }

        uint8_t rec_type;
        uint32_t rec_len;
        if (!read_plaintext_record(&rec_type, tls.msg_scratch, &rec_len)) return 0;
        if (rec_type == TLS_CT_ALERT) {
            serial_printf("tls: received an alert during the handshake -- failing\n");
            return 0;
        }
        if (rec_type != TLS_CT_HANDSHAKE) return 0;
        if (tls.hs_reasm_len + rec_len > sizeof(tls.hs_reasm)) {
            serial_printf("tls: handshake reassembly buffer overflow\n");
            return 0;
        }
        memcpy(tls.hs_reasm + tls.hs_reasm_len, tls.msg_scratch, rec_len);
        tls.hs_reasm_len += rec_len;
    }
}

/* -------------------------------------------------------------------
 * Handshake state machine
 * ------------------------------------------------------------------- */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static int send_client_hello(const char *sni_hostname, uint8_t client_random[32]) {
    tls_random_bytes(client_random, 32);

    uint32_t host_len = (uint32_t)strlen(sni_hostname);
    if (host_len > 128) return 0; /* nothing realistic needs a hostname this long */

    /* Fixed fields (41 bytes) + 2-byte extensions-length + SNI
     * (9+host_len) + supported_groups (8) + ec_point_formats (6) +
     * signature_algorithms (12) = 78 + host_len <= 206 for the 128-byte
     * cap above; rounded up generously. */
    uint8_t body[320];
    uint32_t p = 0;

    put_u16(body + p, TLS_VERSION); p += 2;
    memcpy(body + p, client_random, 32); p += 32;
    body[p++] = 0; /* empty session_id */
    put_u16(body + p, 2); p += 2;      /* cipher_suites length */
    put_u16(body + p, TLS_CIPHER_SUITE); p += 2;
    body[p++] = 1; /* compression_methods length */
    body[p++] = 0; /* null compression */

    /* Extensions, length-prefixed as a whole and each individually. */
    uint32_t ext_len_pos = p;
    p += 2; /* filled in once we know the total */
    uint32_t ext_start = p;

    /* server_name (SNI) */
    put_u16(body + p, 0x0000); p += 2;
    put_u16(body + p, (uint16_t)(host_len + 5)); p += 2; /* extension_data length */
    put_u16(body + p, (uint16_t)(host_len + 3)); p += 2; /* server_name_list length */
    body[p++] = 0; /* name_type = host_name */
    put_u16(body + p, (uint16_t)host_len); p += 2;
    memcpy(body + p, sni_hostname, host_len); p += host_len;

    /* supported_groups: x25519 only */
    put_u16(body + p, 0x000a); p += 2;
    put_u16(body + p, 4); p += 2; /* extension_data length */
    put_u16(body + p, 2); p += 2; /* list length */
    put_u16(body + p, TLS_CURVE_X25519); p += 2;

    /* ec_point_formats: uncompressed only */
    put_u16(body + p, 0x000b); p += 2;
    put_u16(body + p, 2); p += 2; /* extension_data length */
    body[p++] = 1;  /* list length */
    body[p++] = 0;  /* uncompressed */

    /* signature_algorithms: enough for a server to be willing to pick
     * an ECDHE_RSA suite (RFC 5246 7.4.1.4.1) -- we never verify any
     * signature ourselves, this is purely to satisfy that requirement. */
    put_u16(body + p, 0x000d); p += 2;
    put_u16(body + p, 8); p += 2; /* extension_data length */
    put_u16(body + p, 6); p += 2; /* list length */
    put_u16(body + p, 0x0401); p += 2; /* rsa_pkcs1_sha256 */
    put_u16(body + p, 0x0501); p += 2; /* rsa_pkcs1_sha384 */
    put_u16(body + p, 0x0601); p += 2; /* rsa_pkcs1_sha512 */

    put_u16(body + ext_len_pos, (uint16_t)(p - ext_start));

    return send_handshake(HS_CLIENT_HELLO, body, p);
}

static int parse_server_hello(const uint8_t *body, uint32_t len, uint8_t server_random[32]) {
    if (len < 2 + 32 + 1) return 0;
    uint32_t p = 0;
    uint16_t server_version = (uint16_t)((body[0] << 8) | body[1]);
    if (server_version != TLS_VERSION) {
        serial_printf("tls: server picked protocol version 0x%04x, not TLS 1.2\n", server_version);
        return 0;
    }
    p += 2;
    memcpy(server_random, body + p, 32); p += 32;

    uint8_t session_id_len = body[p++];
    if (p + session_id_len + 2 + 1 > len) return 0;
    p += session_id_len;

    uint16_t cipher_suite = (uint16_t)((body[p] << 8) | body[p + 1]); p += 2;
    if (cipher_suite != TLS_CIPHER_SUITE) {
        serial_printf("tls: server picked cipher suite 0x%04x, not the one we offered\n", cipher_suite);
        return 0;
    }
    p += 1; /* compression_method, always null */

    /* Any remaining bytes are extensions -- nothing here is needed for
     * this client's scope (no ALPN, no renegotiation_info use, etc.),
     * so they're intentionally left unparsed. */
    (void)p;
    return 1;
}

/* Parses just the ECDHE parameters (RFC 8422 5.4): curve_type,
 * namedcurve, and the server's public value. The digitally-signed
 * signature that follows is never inspected -- see include/net/tls.h. */
static int parse_server_key_exchange(const uint8_t *body, uint32_t len, uint8_t server_pub[32]) {
    if (len < 1 + 2 + 1 + 32) return 0;
    if (body[0] != 3 /* named_curve */) return 0;
    uint16_t curve = (uint16_t)((body[1] << 8) | body[2]);
    if (curve != TLS_CURVE_X25519) {
        serial_printf("tls: server's ServerKeyExchange named curve 0x%04x != x25519\n", curve);
        return 0;
    }
    uint8_t point_len = body[3];
    if (point_len != 32) return 0;
    memcpy(server_pub, body + 4, 32);
    return 1;
}

/* Snapshots the running transcript hash without disturbing it (the
 * ctx is a plain value struct, so a bytewise copy is a valid,
 * independently-finalizable clone). */
static void transcript_hash_now(uint8_t out[32]) {
    sha256_ctx snap = tls.transcript;
    sha256_final(&snap, out);
}

int tls_connect(uint32_t remote_ip, uint16_t remote_port, const char *sni_hostname) {
    memset(&tls, 0, sizeof(tls));

    if (!tcp_connect(remote_ip, remote_port)) return 0;
    sha256_init(&tls.transcript);

    uint8_t client_random[32], server_random[32];
    if (!send_client_hello(sni_hostname, client_random)) { tcp_close(); return 0; }

    /* tls.hs_body (static, not stack -- see its declaration) holds
     * whichever handshake message body was most recently reassembled. */
    uint8_t type;
    uint32_t body_len;
    uint8_t *body = tls.hs_body;

    if (!read_handshake_message(&type, body, &body_len, sizeof(tls.hs_body)) || type != HS_SERVER_HELLO) {
        serial_printf("tls: expected ServerHello\n"); tcp_close(); return 0;
    }
    if (!parse_server_hello(body, body_len, server_random)) { tcp_close(); return 0; }

    if (!read_handshake_message(&type, body, &body_len, sizeof(tls.hs_body)) || type != HS_CERTIFICATE) {
        serial_printf("tls: expected Certificate\n"); tcp_close(); return 0;
    }
    /* Deliberately not parsed further -- see include/net/tls.h. */

    if (!read_handshake_message(&type, body, &body_len, sizeof(tls.hs_body)) || type != HS_SERVER_KEY_EXCHANGE) {
        serial_printf("tls: expected ServerKeyExchange\n"); tcp_close(); return 0;
    }
    uint8_t server_pub[32];
    if (!parse_server_key_exchange(body, body_len, server_pub)) { tcp_close(); return 0; }

    if (!read_handshake_message(&type, body, &body_len, sizeof(tls.hs_body)) || type != HS_SERVER_HELLO_DONE) {
        serial_printf("tls: expected ServerHelloDone\n"); tcp_close(); return 0;
    }

    /* Generate our ephemeral X25519 keypair and the shared secret. */
    uint8_t client_priv[32], client_pub[32], shared_secret[32];
    tls_random_bytes(client_priv, 32);
    x25519(client_pub, client_priv, x25519_base_point);
    x25519(shared_secret, client_priv, server_pub);

    uint8_t cke_body[1 + 32];
    cke_body[0] = 32;
    memcpy(cke_body + 1, client_pub, 32);
    if (!send_handshake(HS_CLIENT_KEY_EXCHANGE, cke_body, sizeof(cke_body))) { tcp_close(); return 0; }

    /* master_secret = PRF(shared_secret, "master secret", client_random || server_random)
     * -- note client_random comes first here, but server_random comes
     * first in the key_block derivation just below; RFC 5246 6.3
     * really does swap the order between the two, it's not a typo. */
    static const uint8_t master_secret_label[] = "master secret";
    uint8_t seed64[64];
    memcpy(seed64, client_random, 32);
    memcpy(seed64 + 32, server_random, 32);
    uint8_t label_seed[13 + 64];
    memcpy(label_seed, master_secret_label, 13);
    memcpy(label_seed + 13, seed64, 64);
    uint8_t master_secret[48];
    tls_prf(shared_secret, 32, label_seed, 13 + 64, master_secret, 48);

    /* key_block = PRF(master_secret, "key expansion", server_random || client_random)
     * RFC 5288's GCM suites carry no MAC key (mac_key_length = 0), so
     * the layout is just: client_write_key[16], server_write_key[16],
     * client_write_IV[4], server_write_IV[4] (the fixed halves of the
     * GCM nonce -- RFC 5288 Section 3). */
    static const uint8_t key_expansion_label[] = "key expansion";
    memcpy(seed64, server_random, 32);
    memcpy(seed64 + 32, client_random, 32);
    uint8_t label_seed2[13 + 64];
    memcpy(label_seed2, key_expansion_label, 13);
    memcpy(label_seed2 + 13, seed64, 64);
    uint8_t key_block[40];
    tls_prf(master_secret, 48, label_seed2, 13 + 64, key_block, 40);

    aes128_init(&tls.client_aes, key_block);
    aes128_init(&tls.server_aes, key_block + 16);
    memcpy(tls.client_iv, key_block + 32, 4);
    memcpy(tls.server_iv, key_block + 36, 4);

    /* ChangeCipherSpec -- its own record type, not a handshake message
     * (not fed into the transcript hash). Everything the client sends
     * from here on, starting with its own Finished, is GCM-encrypted. */
    uint8_t ccs_payload = 1;
    if (!send_record(TLS_CT_CHANGE_CIPHER_SPEC, &ccs_payload, 1)) { tcp_close(); return 0; }
    tls.client_encrypted = 1;

    uint8_t handshake_hash[32];
    transcript_hash_now(handshake_hash);
    static const uint8_t client_finished_label[] = "client finished"; /* 15 bytes, no NUL */
    uint8_t client_verify_data[12];
    uint8_t cf_label_seed[15 + 32];
    memcpy(cf_label_seed, client_finished_label, 15);
    memcpy(cf_label_seed + 15, handshake_hash, 32);
    tls_prf(master_secret, 48, cf_label_seed, 15 + 32, client_verify_data, 12);
    if (!send_handshake(HS_FINISHED, client_verify_data, 12)) { tcp_close(); return 0; }

    /* Snapshot the transcript *now* -- after our own Finished but
     * before reading the server's -- since that's the exact hash the
     * server's Finished is defined over (RFC 5246 7.4.9: everything up
     * to but not including the Finished message being verified).
     * read_handshake_message() below folds the server's Finished bytes
     * into tls.transcript as soon as it reassembles them, so taking
     * this snapshot any later would wrongly include them. */
    transcript_hash_now(handshake_hash);

    /* Server's ChangeCipherSpec, read directly (not a handshake
     * message either) -- everything received after this is decrypted. */
    uint8_t rec_type;
    uint32_t rec_len;
    if (!read_plaintext_record(&rec_type, tls.msg_scratch, &rec_len) ||
        rec_type != TLS_CT_CHANGE_CIPHER_SPEC || rec_len != 1 || tls.msg_scratch[0] != 1) {
        serial_printf("tls: expected server ChangeCipherSpec\n"); tcp_close(); return 0;
    }
    tls.server_encrypted = 1;

    if (!read_handshake_message(&type, body, &body_len, sizeof(tls.hs_body)) ||
        type != HS_FINISHED || body_len != 12) {
        serial_printf("tls: expected server Finished\n"); tcp_close(); return 0;
    }
    static const uint8_t server_finished_label[] = "server finished"; /* 15 bytes, no NUL */
    uint8_t server_verify_data[12];
    uint8_t sf_label_seed[15 + 32];
    memcpy(sf_label_seed, server_finished_label, 15);
    memcpy(sf_label_seed + 15, handshake_hash, 32);
    tls_prf(master_secret, 48, sf_label_seed, 15 + 32, server_verify_data, 12);
    if (memcmp(server_verify_data, body, 12) != 0) {
        serial_printf("tls: server Finished verify_data mismatch -- our transcript hash must be wrong\n");
        tcp_close(); return 0;
    }

    tls.connected = 1;
    serial_printf("tls: handshake complete with %u.%u.%u.%u:%u (SNI \"%s\")\n",
                  (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
                  (remote_ip >> 8) & 0xFF, remote_ip & 0xFF, remote_port, sni_hostname);
    return 1;
}

int tls_send(const void *data, uint16_t len) {
    if (!tls.connected) return 0;
    const uint8_t *p = (const uint8_t *)data;
    uint16_t remaining = len;
    while (remaining > 0) {
        uint16_t chunk = remaining > TLS_MAX_PLAINTEXT ? TLS_MAX_PLAINTEXT : remaining;
        if (!send_record(TLS_CT_APPLICATION_DATA, p, chunk)) return 0;
        p += chunk;
        remaining = (uint16_t)(remaining - chunk);
    }
    return 1;
}

/* Pulls whatever records are available without blocking, decrypting
 * application-data records into tls.app_recv_buf and silently
 * consuming (but still tag-checking, since the GCM sequence number
 * must advance for every record regardless of type) anything else,
 * treating an alert as the end of the data stream. */
static void tls_pump(void) {
    for (;;) {
        if (tls.raw_len >= TLS_RECORD_HDR_LEN) {
            uint16_t frag_len = (uint16_t)((tls.raw_buf[3] << 8) | tls.raw_buf[4]);
            if (tls.raw_len >= (uint32_t)TLS_RECORD_HDR_LEN + frag_len) {
                /* A full record is already buffered -- read_plaintext_record()
                 * (via read_record()/fill_raw()) will find enough bytes
                 * without ever needing to call tcp_recv() or hlt, so this
                 * stays non-blocking. */
                uint8_t rec_type;
                uint32_t payload_len;
                if (!read_plaintext_record(&rec_type, tls.msg_scratch, &payload_len)) {
                    tls.peer_done = 1;
                    return;
                }
                if (rec_type == TLS_CT_APPLICATION_DATA) {
                    uint32_t space = sizeof(tls.app_recv_buf) - tls.app_recv_len;
                    uint32_t take = payload_len < space ? payload_len : space;
                    memcpy(tls.app_recv_buf + tls.app_recv_len, tls.msg_scratch, take);
                    tls.app_recv_len += take;
                    /* if `take` < payload_len the excess is dropped -- the
                     * buffer is sized to match net/tcp.c's own receive
                     * buffer, so this only bites if the caller falls far
                     * behind on draining it via tls_recv(), same tradeoff
                     * tcp.c makes. */
                } else if (rec_type == TLS_CT_ALERT) {
                    tls.peer_done = 1;
                    return;
                }
                /* Anything else (a stray ChangeCipherSpec, etc.) is just
                 * discarded after its tag has already been checked above. */
                continue; /* there may be another full record already buffered */
            }
        }

        /* Not enough buffered for a full record -- try to top up from
         * TCP without blocking, and stop for now if that yields nothing. */
        if (tls.raw_len >= sizeof(tls.raw_buf)) return; /* can't happen: a record never exceeds raw_buf's size */
        int got = tcp_recv(tls.raw_buf + tls.raw_len, (uint16_t)(sizeof(tls.raw_buf) - tls.raw_len));
        if (got > 0) {
            tls.raw_len += (uint32_t)got;
            continue;
        }
        if (got < 0) tls.peer_done = 1;
        return;
    }
}

int tls_recv(void *buf, uint16_t max_len) {
    if (!tls.connected) return -1;
    tls_pump();
    if (tls.app_recv_len == 0) {
        return tls.peer_done ? -1 : 0;
    }
    uint32_t copy_len = tls.app_recv_len < max_len ? tls.app_recv_len : max_len;
    memcpy(buf, tls.app_recv_buf, copy_len);
    memmove(tls.app_recv_buf, tls.app_recv_buf + copy_len, tls.app_recv_len - copy_len);
    tls.app_recv_len -= copy_len;
    return (int)copy_len;
}

void tls_close(void) {
    tcp_close();
    tls.connected = 0;
}
