/*
 * tls.h — the TLS the transport needs, behind an interface with two backends.
 *
 * Two reasons this is not just a direct OpenSSL call.
 *
 * Portability: OpenSSL is painful to build and ship on Windows and adds a heavy dependency on
 * Linux. mbedtls builds anywhere from source in seconds, which matters for a project whose point
 * is running the same stack on three platforms.
 *
 * And verification: ~/rplay hit frame corruption on AirPlay that turned out to be the crypto
 * implementation, and switching to mbedtls fixed it. That is a different situation from this one
 * -- there the video payload itself was encrypted with unauthenticated AES, whereas here TLS only
 * carries the tunnel and is authenticated, so a crypto fault would drop the connection rather
 * than silently corrupt frames. But "would" is an argument, and swapping the implementation is a
 * measurement. This makes that measurement a build flag rather than a rewrite.
 *
 *   make TLS=openssl   (default)
 *   make TLS=mbedtls
 */
#ifndef RP_TLS_H
#define RP_TLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rp_tls_ctx  rp_tls_ctx;    /* client identity, reusable across connections */
typedef struct rp_tls_conn rp_tls_conn;   /* one upgraded socket */

/* Build a client context from the PEM identity in the usbmuxd pair record.
 *
 * lockdown negotiates very old TLS and presents a certificate we cannot chain, so both backends
 * must lower their minimum version and skip verification. That is not a shortcut: the pairing
 * itself is the trust anchor, and the connection is over USB to a device we already paired with.
 */
rp_tls_ctx *rp_tls_ctx_new(const uint8_t *cert_pem, size_t cert_len,
                           const uint8_t *key_pem, size_t key_len);
void rp_tls_ctx_free(rp_tls_ctx *ctx);

/* Upgrade a connected socket. The socket stays owned by the caller. */
rp_tls_conn *rp_tls_connect(rp_tls_ctx *ctx, int fd);
void rp_tls_close(rp_tls_conn *c);

/* Return bytes moved, 0 on clean close, negative on error. */
long rp_tls_read(rp_tls_conn *c, void *buf, size_t len);
long rp_tls_write(rp_tls_conn *c, const void *buf, size_t len);

/* Which backend was compiled in, for the startup banner — so a run can never be attributed to
 * the wrong one. */
const char *rp_tls_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* RP_TLS_H */
