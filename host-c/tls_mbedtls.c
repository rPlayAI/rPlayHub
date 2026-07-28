/*
 * mbedtls backend for tls.h.
 *
 * The same library ~/rplay and ~/carplay-dev use, vendored at ../deps/mbedtls. Building it from
 * source is what makes the Linux and Windows ports tractable — there is no system OpenSSL to
 * find, no version skew, and no shipping question.
 */
#include "tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

struct rp_tls_ctx {
    mbedtls_x509_crt          cert;
    mbedtls_pk_context        key;
    mbedtls_entropy_context   entropy;
    mbedtls_ctr_drbg_context  drbg;
    mbedtls_ssl_config        conf;
};

struct rp_tls_conn {
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
};

const char *rp_tls_backend(void) { return "mbedtls"; }

/* PEM must be NUL-terminated for mbedtls, and the length it is given must INCLUDE that NUL.
 * Getting this wrong is the classic mbedtls PEM failure and reports only as a parse error. */
static int parse_pem(const uint8_t *pem, size_t len, unsigned char **out, size_t *out_len)
{
    unsigned char *buf = malloc(len + 1);
    if (!buf) return -1;
    memcpy(buf, pem, len);
    buf[len] = 0;
    *out = buf;
    *out_len = len + 1;
    return 0;
}

rp_tls_ctx *rp_tls_ctx_new(const uint8_t *cert_pem, size_t cert_len,
                           const uint8_t *key_pem, size_t key_len)
{
    rp_tls_ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;

    mbedtls_x509_crt_init(&c->cert);
    mbedtls_pk_init(&c->key);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_ssl_config_init(&c->conf);

    static const char *seed = "rplay-hub-cdhost";
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)seed, strlen(seed)) != 0) {
        fprintf(stderr, "tls(mbedtls): could not seed the RNG\n");
        goto fail;
    }

    unsigned char *cbuf = NULL, *kbuf = NULL;
    size_t clen = 0, klen = 0;
    if (parse_pem(cert_pem, cert_len, &cbuf, &clen) != 0) goto fail;
    if (parse_pem(key_pem, key_len, &kbuf, &klen) != 0) { free(cbuf); goto fail; }

    int rc = mbedtls_x509_crt_parse(&c->cert, cbuf, clen);
    int rk = mbedtls_pk_parse_key(&c->key, kbuf, klen, NULL, 0,
                                  mbedtls_ctr_drbg_random, &c->drbg);
    free(cbuf);
    free(kbuf);
    if (rc != 0 || rk != 0) {
        /* MEASURED against a real usbmuxd pair record: the certificate is rejected with
         * -0x23E0, which is X509_INVALID_NAME (-0x2380) plus ASN1_OUT_OF_DATA (-0x60). The
         * private key parses fine. mbedtls is stricter than OpenSSL about the subject/issuer
         * name encoding Apple uses in pair records, so this backend cannot yet complete a
         * lockdown session. Not a transport bug and not something a config flag fixes -- it
         * needs either a permissive parse or converting the identity before handing it over. */
        fprintf(stderr, "tls(mbedtls): host identity failed to parse (cert -0x%04X, key -0x%04X)\n",
                -rc, -rk);
        if (rc == -0x23E0)
            fprintf(stderr, "  the pair record's certificate uses a name encoding mbedtls "
                            "rejects; build with TLS=openssl for now\n");
        goto fail;
    }

    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto fail;

    /* Same two concessions the OpenSSL backend makes, and for the same reason: lockdown speaks
     * old TLS and presents a certificate that cannot be chained. The pairing is the trust
     * anchor, over USB, to a device we already paired with. */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    /* TLS 1.2 is the floor here, and it is not a choice: mbedtls 3.x dropped TLS 1.0/1.1
     * entirely. The OpenSSL backend allows 1.0 because lockdown historically negotiated it, so
     * this backend will fail against any device that insists on it. Worth knowing before
     * concluding that a failed handshake means the library is broken. */
    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    if (mbedtls_ssl_conf_own_cert(&c->conf, &c->cert, &c->key) != 0) {
        fprintf(stderr, "tls(mbedtls): could not install the client certificate\n");
        goto fail;
    }
    return c;

fail:
    rp_tls_ctx_free(c);
    return NULL;
}

void rp_tls_ctx_free(rp_tls_ctx *c)
{
    if (!c) return;
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    mbedtls_pk_free(&c->key);
    mbedtls_x509_crt_free(&c->cert);
    free(c);
}

rp_tls_conn *rp_tls_connect(rp_tls_ctx *ctx, int fd)
{
    if (!ctx) return NULL;
    rp_tls_conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;

    mbedtls_ssl_init(&c->ssl);
    c->net.fd = fd;                     /* the socket stays owned by the caller */

    if (mbedtls_ssl_setup(&c->ssl, &ctx->conf) != 0) { free(c); return NULL; }
    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    int rc;
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            fprintf(stderr, "tls(mbedtls): handshake failed (-0x%04x)\n", -rc);
            mbedtls_ssl_free(&c->ssl);
            free(c);
            return NULL;
        }
    }
    return c;
}

void rp_tls_close(rp_tls_conn *c)
{
    if (!c) return;
    mbedtls_ssl_free(&c->ssl);
    free(c);
}

long rp_tls_read(rp_tls_conn *c, void *buf, size_t len)
{
    if (!c) return -1;
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r;
    }
}

long rp_tls_write(rp_tls_conn *c, const void *buf, size_t len)
{
    if (!c) return -1;
    for (;;) {
        int r = mbedtls_ssl_write(&c->ssl, buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return r;
    }
}
