/* sqGenericSSL.c — real OpenSSL TLS backend for the SqueakSSL plugin (Linux /
 * generic non-Apple; Apple uses sqMacSSL.c via Security.framework).
 *
 * Implements the SqueakSSL.h contract over the OpenSSL **memory-BIO** model: the
 * Smalltalk image owns the TCP socket and passes ciphertext in/out through these
 * functions, which run the TLS state machine over two in-memory BIOs (no
 * blocking socket I/O in the VM).  Replaces the former no-op stub that returned
 * SQSSL_GENERIC_ERROR for everything (which made every HTTPS handshake fail with
 * ZdcPluginMissing).  Requires linking -lssl -lcrypto (see CMakeLists.txt).
 */
#include "sq.h"
#include "SqueakSSL.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Gated diagnostics: set SQSSL_DEBUG=1 to trace the handshake to stderr. */
#define SSLDBG(...) do { if (getenv("SQSSL_DEBUG")) { \
    fprintf(stderr, "[sqSSL] " __VA_ARGS__); fflush(stderr); } } while (0)

typedef struct sqSSL {
    int   state;        /* SQSSL_UNUSED / CONNECTING / ACCEPTING / CONNECTED */
    int   certFlags;    /* SQSSL_* certificate status bits */
    int   loglevel;
    char *certName;     /* local cert name (server use; unused for plain client) */
    char *peerName;     /* peer cert subject CN (lazily filled by getString PEERNAME) */
    char *serverName;   /* SNI / verification hostname (setString SERVERNAME) */

    SSL_CTX *ctx;
    SSL     *ssl;
    BIO     *bioIn;     /* network -> SSL: image writes incoming ciphertext here */
    BIO     *bioOut;    /* SSL -> network: image reads outgoing ciphertext here  */
} sqSSL;

/* Growable 1-based handle table (handle 0 == failure, per SqueakSSL.c). */
static sqSSL **handles = NULL;
static int     handleCount = 0;

static sqSSL *sslFromHandle(sqInt h) {
    if (h < 1 || h > handleCount) return NULL;
    return handles[h - 1];
}

static void initOpenSSLOnce(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
}

/* Drain pending outgoing ciphertext from bioOut into dstBuf. */
static sqInt copyOut(sqSSL *ssl, char *dstBuf, sqInt dstLen) {
    int pending = BIO_ctrl_pending(ssl->bioOut);
    if (pending <= 0) return 0;
    if (pending > dstLen) return SQSSL_BUFFER_TOO_SMALL;
    int n = BIO_read(ssl->bioOut, dstBuf, (int)dstLen);
    return (n < 0) ? 0 : n;
}

sqInt sqCreateSSL(void) {
    initOpenSSLOnce();
    sqSSL *ssl = (sqSSL *)calloc(1, sizeof(sqSSL));
    if (!ssl) return 0;
    ssl->ctx = SSL_CTX_new(TLS_method());
    if (!ssl->ctx) { free(ssl); return 0; }
    SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_SSLv3 | SSL_OP_ALL);
    /* Cap at TLS 1.2 for now: this mem-BIO backend doesn't yet drive TLS 1.3's
       post-handshake messages (NewSessionTicket arrives after the handshake and
       the image's connect/decrypt split mishandles it). TLS 1.2 with modern AEAD
       ciphers is universally supported and fully secure; lifting the cap is a
       follow-up once the post-handshake read path is correct. */
    SSL_CTX_set_max_proto_version(ssl->ctx, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(ssl->ctx);     /* system CA store */
    ssl->ssl = SSL_new(ssl->ctx);
    if (!ssl->ssl) { SSL_CTX_free(ssl->ctx); free(ssl); return 0; }
    ssl->bioIn  = BIO_new(BIO_s_mem());
    ssl->bioOut = BIO_new(BIO_s_mem());
    if (!ssl->bioIn || !ssl->bioOut) {
        if (ssl->bioIn)  BIO_free(ssl->bioIn);
        if (ssl->bioOut) BIO_free(ssl->bioOut);
        SSL_free(ssl->ssl); SSL_CTX_free(ssl->ctx); free(ssl); return 0;
    }
    SSL_set_bio(ssl->ssl, ssl->bioIn, ssl->bioOut); /* SSL owns the BIOs now */
    ssl->state = SQSSL_UNUSED;

    for (int i = 0; i < handleCount; i++) {
        if (handles[i] == NULL) { handles[i] = ssl; return i + 1; }
    }
    sqSSL **grown = (sqSSL **)realloc(handles, (handleCount + 1) * sizeof(sqSSL *));
    if (!grown) { SSL_free(ssl->ssl); SSL_CTX_free(ssl->ctx); free(ssl); return 0; }
    handles = grown;
    handles[handleCount] = ssl;
    SSLDBG("create: handle=%d ssl=%p ctx=%p\n", handleCount + 1, (void *)ssl, (void *)ssl->ctx);
    return ++handleCount;   /* 1-based */
}

sqInt sqDestroySSL(sqInt handle) {
    sqSSL *ssl = sslFromHandle(handle);
    if (!ssl) return 0;
    if (ssl->ssl) SSL_free(ssl->ssl);           /* also frees the owned BIOs */
    if (ssl->ctx) SSL_CTX_free(ssl->ctx);
    free(ssl->peerName); free(ssl->serverName); free(ssl->certName);
    free(ssl);
    handles[handle - 1] = NULL;
    return 1;
}

sqInt sqConnectSSL(sqInt handle, char *srcBuf, sqInt srcLen, char *dstBuf, sqInt dstLen) {
    sqSSL *ssl = sslFromHandle(handle);
    SSLDBG("connect: handle=%ld ssl=%p state=%d srcLen=%ld dstLen=%ld sni=%s\n",
           (long)handle, (void *)ssl, ssl ? ssl->state : -1, (long)srcLen, (long)dstLen,
           (ssl && ssl->serverName) ? ssl->serverName : "(none)");
    if (!ssl) return SQSSL_INVALID_STATE;
    if (ssl->state == SQSSL_CONNECTED) {
        /* Handshake done, but the peer sent more right after it (TLS 1.3
           NewSessionTicket) and the image fed it to connect once more. Process it
           now (SSL_read drives post-handshake handling) so it doesn't sit unread
           in the BIO and confuse the later app-data read. No app data is expected
           yet, so a WANT_READ here is the normal, successful outcome. */
        if (srcLen > 0) BIO_write(ssl->bioIn, srcBuf, (int)srcLen);
        char tmp[16384];
        int r = SSL_read(ssl->ssl, tmp, sizeof tmp);
        SSLDBG("connect: post-handshake SSL_read=%d err=%d (fed %ld)\n",
               r, (r <= 0) ? SSL_get_error(ssl->ssl, r) : 0, (long)srcLen);
        return SQSSL_OK;
    }
    if (ssl->state != SQSSL_UNUSED && ssl->state != SQSSL_CONNECTING)
        return SQSSL_INVALID_STATE;
    if (ssl->state == SQSSL_UNUSED) {
        ssl->state = SQSSL_CONNECTING;
        SSL_set_connect_state(ssl->ssl);
        if (ssl->serverName && *ssl->serverName) {
            SSL_set_tlsext_host_name(ssl->ssl, ssl->serverName);            /* SNI */
            X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl->ssl), ssl->serverName, 0);
        }
    }
    if (srcLen > 0) BIO_write(ssl->bioIn, srcBuf, (int)srcLen);
    int rc = SSL_connect(ssl->ssl);
    int err = (rc <= 0) ? SSL_get_error(ssl->ssl, rc) : 0;
    SSLDBG("connect: SSL_connect rc=%d err=%d pendingOut=%d\n",
           rc, err, (int)BIO_ctrl_pending(ssl->bioOut));
    if (rc <= 0) {
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            if (getenv("SQSSL_DEBUG")) {
                char eb[256]; ERR_error_string_n(ERR_get_error(), eb, sizeof eb);
                fprintf(stderr, "[sqSSL] connect FATAL rc=%d err=%d openssl=%s\n", rc, err, eb);
            }
            return SQSSL_GENERIC_ERROR;
        }
        sqInt out = copyOut(ssl, dstBuf, dstLen);   /* handshake bytes to send */
        SSLDBG("connect: WANT more, returning out=%ld\n", (long)out);
        return (out == 0) ? SQSSL_NEED_MORE_DATA : out;
    }
    ssl->state = SQSSL_CONNECTED;
    long v = SSL_get_verify_result(ssl->ssl);
    ssl->certFlags = (v == X509_V_OK) ? 0 : SQSSL_OTHER_ISSUE;
    SSLDBG("connect: CONNECTED verify=%ld\n", v);
    return copyOut(ssl, dstBuf, dstLen);
}

sqInt sqAcceptSSL(sqInt handle, char *srcBuf, sqInt srcLen, char *dstBuf, sqInt dstLen) {
    sqSSL *ssl = sslFromHandle(handle);
    if (!ssl) return SQSSL_INVALID_STATE;
    if (ssl->state != SQSSL_UNUSED && ssl->state != SQSSL_ACCEPTING)
        return SQSSL_INVALID_STATE;
    if (ssl->state == SQSSL_UNUSED) {
        ssl->state = SQSSL_ACCEPTING;
        SSL_set_accept_state(ssl->ssl);
    }
    if (srcLen > 0) BIO_write(ssl->bioIn, srcBuf, (int)srcLen);
    int rc = SSL_accept(ssl->ssl);
    if (rc <= 0) {
        int err = SSL_get_error(ssl->ssl, rc);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
            return SQSSL_GENERIC_ERROR;
        sqInt out = copyOut(ssl, dstBuf, dstLen);
        return (out == 0) ? SQSSL_NEED_MORE_DATA : out;
    }
    ssl->state = SQSSL_CONNECTED;
    return copyOut(ssl, dstBuf, dstLen);
}

sqInt sqEncryptSSL(sqInt handle, char *srcBuf, sqInt srcLen, char *dstBuf, sqInt dstLen) {
    sqSSL *ssl = sslFromHandle(handle);
    if (!ssl || ssl->state != SQSSL_CONNECTED) return SQSSL_INVALID_STATE;
    int n = SSL_write(ssl->ssl, srcBuf, (int)srcLen);
    sqInt out = (n >= (int)srcLen) ? copyOut(ssl, dstBuf, dstLen) : SQSSL_GENERIC_ERROR;
    SSLDBG("encrypt: srcLen=%ld SSL_write=%d -> out=%ld\n", (long)srcLen, n, (long)out);
    return out;
}

sqInt sqDecryptSSL(sqInt handle, char *srcBuf, sqInt srcLen, char *dstBuf, sqInt dstLen) {
    sqSSL *ssl = sslFromHandle(handle);
    if (!ssl || ssl->state != SQSSL_CONNECTED) return SQSSL_INVALID_STATE;
    if (srcLen > 0) BIO_write(ssl->bioIn, srcBuf, (int)srcLen);
    int n = SSL_read(ssl->ssl, dstBuf, (int)dstLen);
    int err = (n <= 0) ? SSL_get_error(ssl->ssl, n) : 0;
    SSLDBG("decrypt: srcLen=%ld SSL_read=%d err=%d head=[%.50s]\n",
           (long)srcLen, n, err, (n > 0) ? dstBuf : "");
    if (n < 0) {
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_ZERO_RETURN) return 0;
        return SQSSL_GENERIC_ERROR;
    }
    return n;
}

char *sqGetStringPropertySSL(sqInt handle, int propID) {
    sqSSL *ssl = sslFromHandle(handle);
    if (!ssl) return NULL;
    switch (propID) {
    case SQSSL_PROP_PEERNAME: {
        X509 *cert = SSL_get1_peer_certificate(ssl->ssl);
        if (!cert) return NULL;
        char buf[256]; buf[0] = 0;
        X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, buf, sizeof(buf));
        X509_free(cert);
        free(ssl->peerName);
        ssl->peerName = strdup(buf);
        return ssl->peerName;
    }
    case SQSSL_PROP_SERVERNAME: return ssl->serverName;
    default: return NULL;
    }
}

sqInt sqSetStringPropertySSL(sqInt handle, int propID, char *propName, sqInt propLen) {
    sqSSL *ssl = sslFromHandle(handle);
    if (!ssl) return 0;
    char *val = NULL;
    if (propName && propLen > 0) {
        val = (char *)malloc(propLen + 1);
        if (!val) return 0;
        memcpy(val, propName, propLen);
        val[propLen] = '\0';
    }
    switch (propID) {
    case SQSSL_PROP_SERVERNAME: free(ssl->serverName); ssl->serverName = val; return 1;
    case SQSSL_PROP_CERTNAME:   free(ssl->certName);   ssl->certName   = val; return 1;
    default: free(val); return 0;
    }
}

sqInt sqGetIntPropertySSL(sqInt handle, sqInt propID) {
    sqSSL *ssl = sslFromHandle(handle);
    if (!ssl) return 0;
    switch (propID) {
    case SQSSL_PROP_VERSION:   return SQSSL_VERSION;
    case SQSSL_PROP_SSLSTATE:  return ssl->state;
    case SQSSL_PROP_CERTSTATE: return ssl->certFlags;
    case SQSSL_PROP_LOGLEVEL:  return ssl->loglevel;
    default: return 0;
    }
}

sqInt sqSetIntPropertySSL(sqInt handle, sqInt propID, sqInt propValue) {
    sqSSL *ssl = sslFromHandle(handle);
    if (!ssl) return 0;
    if (propID == SQSSL_PROP_LOGLEVEL) { ssl->loglevel = (int)propValue; return 1; }
    return 0;
}
