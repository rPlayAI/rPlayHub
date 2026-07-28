/* OpenSSL backend for tls.h. Default on macOS, where it ships with the system. */
#include "tls.h"

#include <stdio.h>
#include <stdlib.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

struct rp_tls_ctx  { SSL_CTX *ctx; };
struct rp_tls_conn { SSL *ssl; };

const char *rp_tls_backend(void) { return "openssl"; }

rp_tls_ctx *rp_tls_ctx_new(const uint8_t *cert_pem, size_t cert_len,
                           const uint8_t *key_pem, size_t key_len)
{
    SSL_library_init();
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;

    /* lockdown speaks TLS 1.0 with a certificate we cannot chain. The pairing is the trust
     * anchor here, not the certificate, and the link is USB to a device already paired. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION);
    SSL_CTX_set_security_level(ctx, 0);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    BIO *cb = BIO_new_mem_buf(cert_pem, (int)cert_len);
    X509 *x = PEM_read_bio_X509(cb, NULL, NULL, NULL);
    BIO *kb = BIO_new_mem_buf(key_pem, (int)key_len);
    EVP_PKEY *pk = PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL);

    int ok = x && pk && SSL_CTX_use_certificate(ctx, x) == 1 &&
             SSL_CTX_use_PrivateKey(ctx, pk) == 1;
    if (!ok) {
        fprintf(stderr, "tls(openssl): could not load the host identity\n");
        ERR_print_errors_fp(stderr);
    }
    if (x) X509_free(x);
    if (pk) EVP_PKEY_free(pk);
    BIO_free(cb);
    BIO_free(kb);
    if (!ok) { SSL_CTX_free(ctx); return NULL; }

    rp_tls_ctx *out = calloc(1, sizeof *out);
    if (!out) { SSL_CTX_free(ctx); return NULL; }
    out->ctx = ctx;
    return out;
}

void rp_tls_ctx_free(rp_tls_ctx *ctx)
{
    if (!ctx) return;
    SSL_CTX_free(ctx->ctx);
    free(ctx);
}

rp_tls_conn *rp_tls_connect(rp_tls_ctx *ctx, int fd)
{
    if (!ctx) return NULL;
    SSL *ssl = SSL_new(ctx->ctx);
    if (!ssl) return NULL;
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "tls(openssl): handshake failed\n");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return NULL;
    }
    rp_tls_conn *c = calloc(1, sizeof *c);
    if (!c) { SSL_free(ssl); return NULL; }
    c->ssl = ssl;
    return c;
}

void rp_tls_close(rp_tls_conn *c)
{
    if (!c) return;
    SSL_free(c->ssl);
    free(c);
}

long rp_tls_read(rp_tls_conn *c, void *buf, size_t len)
{
    return c ? SSL_read(c->ssl, buf, (int)len) : -1;
}

long rp_tls_write(rp_tls_conn *c, const void *buf, size_t len)
{
    return c ? SSL_write(c->ssl, buf, (int)len) : -1;
}
