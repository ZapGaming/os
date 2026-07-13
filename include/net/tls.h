#ifndef NET_TLS_H
#define NET_TLS_H

#include <stdint.h>

/* A from-scratch TLS 1.2 client, scoped down hard for tractability:
 *
 *   - TLS 1.2 (0x0303) only, exactly one cipher suite
 *     (TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 0xC02F) and exactly one
 *     ECDHE curve (X25519, RFC 7748) -- if a server doesn't support
 *     that suite/curve combination, the handshake simply fails.
 *
 *   - DELIBERATE SECURITY SCOPE CUT, disclosed prominently here: the
 *     server's Certificate message is parsed only enough to skip over
 *     it (its length is self-delimiting), and the ServerKeyExchange
 *     signature over the ECDHE parameters is never checked. In other
 *     words, this client authenticates *nothing* about the server --
 *     no chain validation, no hostname check, no expiry check, no
 *     signature verification. It gets you an encrypted (AES-128-GCM,
 *     with the authentication tag on every record actually checked)
 *     channel to *something* claiming to be the requested host, which
 *     is enough to defeat passive eavesdropping and keep the browser's
 *     HTTPS fetches from being rejected outright, but it is trivially
 *     defeated by an active on-path attacker. This is explicitly "for
 *     interoperability, not security" -- do not use it for anything
 *     where that distinction matters.
 *
 *   - No renegotiation, no session resumption, no alerts sent on our
 *     side (a fatal condition just fails the connection outright), and
 *     receiving an alert record during the handshake is treated as
 *     fatal without inspecting its level/description.
 *
 * Layered directly on top of net/tcp.h's single-connection client TCP
 * (tcp_connect/tcp_send/tcp_recv/tcp_close) -- this file owns no
 * sockets or packets of its own. */

/* Performs the full handshake over a fresh TCP connection to
 * remote_ip:remote_port: ClientHello (with SNI set to sni_hostname) ->
 * ServerHello/Certificate/ServerKeyExchange/ServerHelloDone ->
 * ClientKeyExchange/ChangeCipherSpec/Finished -> server's
 * ChangeCipherSpec/Finished. Blocking, like tcp_connect(). Returns 1 if
 * every step succeeded (including the server Finished's verify_data
 * matching what we computed), 0 if anything failed at any stage. */
int tls_connect(uint32_t remote_ip, uint16_t remote_port, const char *sni_hostname);

/* Like tcp_send(): blocking, encrypts and splits `data` into as many
 * TLS records as needed (each up to 16384 bytes of plaintext). Returns
 * 1 on success, 0 if any underlying tcp_send() failed. */
int tls_send(const void *data, uint16_t len);

/* Like tcp_recv(): non-blocking. Returns the number of decrypted
 * (and tag-verified) application-data bytes copied into buf (0 if none
 * are available yet), or -1 once the connection is over (peer alert or
 * TCP FIN) and every already-decrypted byte has been drained. Any
 * record whose GCM tag fails to verify is treated as a fatal error
 * (also reported as -1) -- this client never hands the caller data it
 * couldn't authenticate at the record layer, even though it never
 * authenticated the server's identity at the handshake layer. */
int tls_recv(void *buf, uint16_t max_len);

/* Closes the underlying TCP connection (no close_notify alert is sent
 * first -- see the "no alerts sent" note above) and resets all
 * connection state so a fresh tls_connect() can be made. */
void tls_close(void);

#endif
