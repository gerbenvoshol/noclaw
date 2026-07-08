/*
 * HTTP client with native TLS.
 *   macOS:  SecureTransport (Security.framework)
 *   Linux:  BearSSL
 *
 * Proper implementation: DNS resolve, TCP connect, TLS handshake,
 * HTTP/1.1 request/response with Content-Length and chunked transfer.
 */

#include "nc.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>

#ifdef __APPLE__
#include <Security/Security.h>
#include <Security/SecureTransport.h>
#else
#include <bearssl.h>
#endif

static int g_http_timeout_seconds = 30;

static int effective_http_timeout_seconds(void) {
    if (g_http_timeout_seconds < 1) return 1;
    if (g_http_timeout_seconds > 3600) return 3600;
    return g_http_timeout_seconds;
}

void nc_http_set_timeout(int timeout_seconds) {
    g_http_timeout_seconds = timeout_seconds;
}

/* ── URL parsing ──────────────────────────────────────────────── */

typedef struct {
    bool   is_https;
    char   host[256];
    char   port[8];
    char   path[1024];
} parsed_url;

static bool parse_url(const char *url, parsed_url *out) {
    memset(out, 0, sizeof(*out));

    if (strncmp(url, "https://", 8) == 0) {
        out->is_https = true;
        url += 8;
        nc_strlcpy(out->port, "443", sizeof(out->port));
    } else if (strncmp(url, "http://", 7) == 0) {
        out->is_https = false;
        url += 7;
        nc_strlcpy(out->port, "80", sizeof(out->port));
    } else {
        return false;
    }

    /* Extract host and optional port */
    const char *slash = strchr(url, '/');
    const char *colon = strchr(url, ':');

    if (colon && (!slash || colon < slash)) {
        /* Host:port */
        size_t hlen = (size_t)(colon - url);
        if (hlen >= sizeof(out->host)) return false;
        memcpy(out->host, url, hlen);
        out->host[hlen] = '\0';

        colon++;
        size_t plen = slash ? (size_t)(slash - colon) : strlen(colon);
        if (plen >= sizeof(out->port)) return false;
        memcpy(out->port, colon, plen);
        out->port[plen] = '\0';
    } else {
        size_t hlen = slash ? (size_t)(slash - url) : strlen(url);
        if (hlen >= sizeof(out->host)) return false;
        memcpy(out->host, url, hlen);
        out->host[hlen] = '\0';
    }

    if (slash)
        nc_strlcpy(out->path, slash, sizeof(out->path));
    else
        nc_strlcpy(out->path, "/", sizeof(out->path));

    return true;
}

/* ── TCP connect ──────────────────────────────────────────────── */

static int tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        nc_log(NC_LOG_ERROR, "DNS resolve failed for %s: %s", host, gai_strerror(err));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            /* Set socket timeouts to prevent indefinite hangs */
            struct timeval tv = { .tv_sec = effective_http_timeout_seconds(), .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0)
        nc_log(NC_LOG_ERROR, "TCP connect failed to %s:%s", host, port);

    return fd;
}

/* ── TLS wrapper ──────────────────────────────────────────────── */

typedef struct {
    int  fd;
    bool is_tls;
#ifdef __APPLE__
    SSLContextRef ssl_ctx;
#else
    br_ssl_client_context   sc;
    br_x509_minimal_context xc;
    br_sslio_context        ioc;
    unsigned char           iobuf[BR_SSL_BUFSIZE_MONO];
#endif
} tls_conn;

#ifdef __APPLE__

/* SecureTransport I/O callbacks */
static OSStatus tls_read_cb(SSLConnectionRef conn, void *data, size_t *len) {
    int fd = *(const int *)conn;
    ssize_t n = read(fd, data, *len);
    if (n <= 0) {
        *len = 0;
        if (n == 0) return errSSLClosedGraceful;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return errSSLWouldBlock;
        return errSSLClosedAbort;
    }
    *len = (size_t)n;
    return noErr;
}

static OSStatus tls_write_cb(SSLConnectionRef conn, const void *data, size_t *len) {
    int fd = *(const int *)conn;
    ssize_t n = write(fd, data, *len);
    if (n <= 0) {
        *len = 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return errSSLWouldBlock;
        return errSSLClosedAbort;
    }
    *len = (size_t)n;
    return noErr;
}

static bool tls_connect(tls_conn *c, const char *host) {
    if (!c->is_tls) return true;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

    c->ssl_ctx = SSLCreateContext(kCFAllocatorDefault, kSSLClientSide, kSSLStreamType);
    if (!c->ssl_ctx) return false;

    SSLSetIOFuncs(c->ssl_ctx, tls_read_cb, tls_write_cb);
    SSLSetConnection(c->ssl_ctx, &c->fd);
    SSLSetPeerDomainName(c->ssl_ctx, host, strlen(host));

    OSStatus status;
    do {
        status = SSLHandshake(c->ssl_ctx);
    } while (status == errSSLWouldBlock);

    if (status != noErr) {
        nc_log(NC_LOG_ERROR, "TLS handshake failed: %d", (int)status);
        CFRelease(c->ssl_ctx);
        c->ssl_ctx = NULL;
        return false;
    }

#pragma clang diagnostic pop
    return true;
}

static ssize_t tls_read(tls_conn *c, void *buf, size_t len) {
    if (!c->is_tls) return read(c->fd, buf, len);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    size_t processed = 0;
    OSStatus status = SSLRead(c->ssl_ctx, buf, len, &processed);
    if (status == noErr || status == errSSLClosedGraceful)
        return (ssize_t)processed;
    if (processed > 0) return (ssize_t)processed;
    return -1;
#pragma clang diagnostic pop
}

static ssize_t tls_write(tls_conn *c, const void *buf, size_t len) {
    if (!c->is_tls) return write(c->fd, buf, len);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    size_t processed = 0;
    OSStatus status = SSLWrite(c->ssl_ctx, buf, len, &processed);
    if (status == noErr) return (ssize_t)processed;
    return -1;
#pragma clang diagnostic pop
}

static bool tls_flush(tls_conn *c) {
    (void)c;
    return true;  /* SecureTransport writes are synchronous */
}

static void tls_close(tls_conn *c) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (c->ssl_ctx) {
        SSLClose(c->ssl_ctx);
        CFRelease(c->ssl_ctx);
        c->ssl_ctx = NULL;
    }
#pragma clang diagnostic pop
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
}

#else /* Linux: BearSSL */

/* ── System CA trust anchor loading ──────────────────────────── */

/* Paths to try for the system CA bundle */
static const char *ca_paths[] = {
    "/etc/ssl/certs/ca-certificates.crt",   /* Debian/Ubuntu */
    "/etc/pki/tls/certs/ca-bundle.crt",     /* RHEL/Fedora */
    "/etc/ssl/ca-bundle.pem",               /* openSUSE */
    "/etc/ssl/cert.pem",                    /* Alpine/FreeBSD */
    NULL,
};

typedef struct {
    br_x509_trust_anchor *items;
    size_t count;
    size_t cap;
    unsigned char *pool;     /* bump allocator for DN + key data */
    size_t pool_cap;
    size_t pool_pos;
} ta_list;

static ta_list g_tas = {0};

static void *ta_pool_alloc(ta_list *t, size_t n) {
    n = (n + 7) & ~(size_t)7;  /* align */
    if (t->pool_pos + n > t->pool_cap) return NULL;
    void *p = t->pool + t->pool_pos;
    t->pool_pos += n;
    return p;
}

/* X.509 decoder: accumulate DN via callback */
typedef struct { unsigned char buf[512]; size_t len; } dn_buf;

static void dn_append(void *ctx, const void *src, size_t len) {
    dn_buf *d = (dn_buf *)ctx;
    if (d->len + len <= sizeof(d->buf)) {
        memcpy(d->buf + d->len, src, len);
        d->len += len;
    }
}

static bool ta_add_from_der(ta_list *t, const unsigned char *der, size_t der_len) {
    br_x509_decoder_context dc;
    dn_buf dn = { .len = 0 };

    br_x509_decoder_init(&dc, dn_append, &dn);
    br_x509_decoder_push(&dc, der, der_len);
    if (br_x509_decoder_last_error(&dc) != 0) return false;

    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk) return false;

    /* Grow array if needed */
    if (t->count >= t->cap) return false;

    /* Copy DN */
    unsigned char *dn_copy = (unsigned char *)ta_pool_alloc(t, dn.len);
    if (!dn_copy) return false;
    memcpy(dn_copy, dn.buf, dn.len);

    br_x509_trust_anchor *ta = &t->items[t->count];
    ta->dn.data = dn_copy;
    ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey.key_type = pk->key_type;

    if (pk->key_type == BR_KEYTYPE_RSA) {
        unsigned char *n = (unsigned char *)ta_pool_alloc(t, pk->key.rsa.nlen);
        unsigned char *e = (unsigned char *)ta_pool_alloc(t, pk->key.rsa.elen);
        if (!n || !e) return false;
        memcpy(n, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(e, pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.n = n;
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = e;
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        unsigned char *q = (unsigned char *)ta_pool_alloc(t, pk->key.ec.qlen);
        if (!q) return false;
        memcpy(q, pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = q;
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
    } else {
        return false;
    }

    t->count++;
    return true;
}

/* PEM decoder callback: accumulate decoded base64 into DER buffer */
typedef struct { unsigned char *buf; size_t len; size_t cap; } der_buf;

static void der_append(void *ctx, const void *src, size_t len) {
    der_buf *d = (der_buf *)ctx;
    if (d->len + len <= d->cap) {
        memcpy(d->buf + d->len, src, len);
        d->len += len;
    }
}

static bool load_system_cas(void) {
    if (g_tas.count > 0) return true;  /* already loaded */

    /* Find CA bundle */
    const char *ca_path = NULL;
    for (const char **p = ca_paths; *p; p++) {
        if (nc_file_exists(*p)) { ca_path = *p; break; }
    }
    if (!ca_path) {
        nc_log(NC_LOG_ERROR, "No system CA bundle found");
        return false;
    }

    size_t pem_len = 0;
    char *pem = nc_read_file(ca_path, &pem_len);
    if (!pem) return false;

    /* Pre-allocate trust anchor storage */
    g_tas.cap = 256;
    g_tas.items = (br_x509_trust_anchor *)calloc(g_tas.cap, sizeof(br_x509_trust_anchor));
    g_tas.pool_cap = 256 * 1024;
    g_tas.pool = (unsigned char *)malloc(g_tas.pool_cap);
    if (!g_tas.items || !g_tas.pool) { free(pem); return false; }

    /* DER accumulation buffer */
    der_buf der = { .buf = (unsigned char *)malloc(8192), .len = 0, .cap = 8192 };
    if (!der.buf) { free(pem); return false; }

    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    bool in_cert = false;

    size_t off = 0;
    while (off < pem_len) {
        size_t n = br_pem_decoder_push(&pc, (const unsigned char *)pem + off, pem_len - off);
        off += n;

        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ:
            in_cert = (strcmp(br_pem_decoder_name(&pc), "CERTIFICATE") == 0);
            der.len = 0;
            br_pem_decoder_setdest(&pc, der_append, &der);
            break;
        case BR_PEM_END_OBJ:
            if (in_cert && der.len > 0)
                ta_add_from_der(&g_tas, der.buf, der.len);
            in_cert = false;
            break;
        case BR_PEM_ERROR:
            break;  /* skip bad certs, keep going */
        default:
            break;
        }

        if (n == 0) break;
    }

    free(der.buf);
    free(pem);

    nc_log(NC_LOG_INFO, "Loaded %zu trust anchors from %s", g_tas.count, ca_path);
    return g_tas.count > 0;
}

/* ── BearSSL I/O callbacks for br_sslio ──────────────────────── */

static int bear_sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n;
    do { n = read(fd, buf, len); } while (n < 0 && errno == EINTR);
    return (n <= 0) ? -1 : (int)n;
}

static int bear_sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n;
    do { n = write(fd, buf, len); } while (n < 0 && errno == EINTR);
    return (n <= 0) ? -1 : (int)n;
}

/* ── TLS functions ───────────────────────────────────────────── */

/*
 * Minimal TLS client init — ECDHE+AES-GCM only.
 * Replaces br_ssl_client_init_full to avoid pulling in 3DES, CBC, CCM,
 * ChaCha20-Poly1305, RSA key exchange, MD5, SHA-224, TLS 1.0 PRF, etc.
 * This lets LTO + gc-sections strip ~40-50 KB of unused crypto code.
 */
static void ssl_client_init_minimal(
    br_ssl_client_context *cc, br_x509_minimal_context *xc,
    const br_x509_trust_anchor *tas, size_t ta_count)
{
    /* 4 suites instead of 45 — covers all modern API endpoints */
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    };

    /* All hashes (needed for cert chain validation — CAs use various) */
    static const br_hash_class *hashes[] = {
        &br_md5_vtable,
        &br_sha1_vtable,
        &br_sha224_vtable,
        &br_sha256_vtable,
        &br_sha384_vtable,
        &br_sha512_vtable,
    };

    br_ssl_client_zero(cc);
    br_ssl_engine_set_versions(&cc->eng, BR_TLS12, BR_TLS12);
    br_x509_minimal_init(xc, &br_sha256_vtable, tas, ta_count);

    br_ssl_engine_set_suites(&cc->eng, suites,
        sizeof(suites) / sizeof(suites[0]));

    /* Asymmetric: set on engine first, then pass to X.509 */
    br_ssl_engine_set_default_rsavrfy(&cc->eng);
    br_ssl_engine_set_default_ecdsa(&cc->eng);
    br_x509_minimal_set_rsa(xc, br_ssl_engine_get_rsavrfy(&cc->eng));
    br_x509_minimal_set_ecdsa(xc,
        br_ssl_engine_get_ec(&cc->eng),
        br_ssl_engine_get_ecdsa(&cc->eng));
    /* No rsapub — ECDHE suites don't use RSA key exchange */

    /* Hashes on both engine and X.509 */
    for (int id = br_md5_ID; id <= br_sha512_ID; id++) {
        br_ssl_engine_set_hash(&cc->eng, id, hashes[id - 1]);
        br_x509_minimal_set_hash(xc, id, hashes[id - 1]);
    }

    br_ssl_engine_set_x509(&cc->eng, &xc->vtable);

    /* TLS 1.2 PRFs (skip TLS 1.0 PRF — TLS12-only) */
    br_ssl_engine_set_prf_sha256(&cc->eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(&cc->eng, &br_tls12_sha384_prf);

    /* AES-GCM only: explicit ct64 (fast on 64-bit ARM/x86-64) */
    br_ssl_engine_set_gcm(&cc->eng,
        &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_aes_ctr(&cc->eng, &br_aes_ct64_ctr_vtable);
    br_ssl_engine_set_ghash(&cc->eng, &br_ghash_ctmul64);
}

static bool tls_connect(tls_conn *c, const char *host) {
    if (!c->is_tls) return true;

    if (!load_system_cas()) return false;

    ssl_client_init_minimal(&c->sc, &c->xc, g_tas.items, g_tas.count);
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof(c->iobuf), 0);
    br_ssl_client_reset(&c->sc, host, 0);
    br_sslio_init(&c->ioc, &c->sc.eng,
                  bear_sock_read, &c->fd, bear_sock_write, &c->fd);

    return true;
}

static ssize_t tls_read(tls_conn *c, void *buf, size_t len) {
    if (!c->is_tls) return read(c->fd, buf, len);
    int n = br_sslio_read(&c->ioc, buf, len);
    return (n < 0) ? -1 : (ssize_t)n;
}

static ssize_t tls_write(tls_conn *c, const void *buf, size_t len) {
    if (!c->is_tls) return write(c->fd, buf, len);
    int n = br_sslio_write_all(&c->ioc, buf, len);
    return (n < 0) ? -1 : (ssize_t)len;
}

static bool tls_flush(tls_conn *c) {
    if (!c->is_tls) return true;
    return br_sslio_flush(&c->ioc) == 0;
}

static void tls_close(tls_conn *c) {
    if (c->is_tls) br_sslio_close(&c->ioc);
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
}

#endif

/* ── Write all bytes ──────────────────────────────────────────── */

static bool tls_write_all(tls_conn *c, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = tls_write(c, buf + sent, len - sent);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool add_cap(size_t *total, size_t add) {
    if (*total > SIZE_MAX - add) return false;
    *total += add;
    return true;
}

static bool compute_request_cap(const char *method,
                                const char *path,
                                const char *host,
                                bool include_content_length,
                                const char **headers,
                                int header_count,
                                size_t *out_cap) {
    size_t cap = 0;
    if (!add_cap(&cap, strlen(method))) return false;
    if (!add_cap(&cap, 1)) return false; /* space */
    if (!add_cap(&cap, strlen(path))) return false;
    if (!add_cap(&cap, strlen(" HTTP/1.1\r\nHost: "))) return false;
    if (!add_cap(&cap, strlen(host))) return false;
    if (!add_cap(&cap, strlen("\r\n"))) return false;
    if (include_content_length) {
        if (!add_cap(&cap, strlen("Content-Length: "))) return false;
        if (!add_cap(&cap, 20)) return false; /* max decimal digits for 64-bit size_t */
        if (!add_cap(&cap, strlen("\r\n"))) return false;
    }
    if (!add_cap(&cap, strlen("Connection: close\r\n"))) return false;
    for (int i = 0; i < header_count; i++) {
        if (!add_cap(&cap, strlen(headers[i]))) return false;
        if (!add_cap(&cap, 2)) return false; /* CRLF */
    }
    if (!add_cap(&cap, 2)) return false; /* final CRLF */
    if (!add_cap(&cap, 1)) return false; /* NUL terminator */
    *out_cap = cap;
    return true;
}

/* ── Read until connection closes, appending to response ──────── */

static void resp_init(nc_http_response *resp) {
    memset(resp, 0, sizeof(*resp));
    resp->body_cap = 16384;
    resp->body = (char *)malloc(resp->body_cap);
    if (resp->body) {
        resp->body[0] = '\0';
    } else {
        resp->body_cap = 0;
    }
}

static void resp_append(nc_http_response *resp, const char *data, size_t len) {
    while (resp->body_len + len + 1 > resp->body_cap) {
        size_t new_cap = resp->body_cap ? resp->body_cap * 2 : 16384;
        char *nb = (char *)realloc(resp->body, new_cap);
        if (!nb) return;
        resp->body = nb;
        resp->body_cap = new_cap;
    }
    memcpy(resp->body + resp->body_len, data, len);
    resp->body_len += len;
    resp->body[resp->body_len] = '\0';
}

void nc_http_response_free(nc_http_response *resp) {
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}

/* ── Parse HTTP response (status + headers + body) ────────────── */

static bool read_response(tls_conn *c, nc_http_response *resp) {
    /* Read into a raw buffer, then split header/body */
    char *raw = NULL;
    size_t raw_len = 0, raw_cap = 16384;
    raw = (char *)malloc(raw_cap);
    if (!raw) return false;

    /* Read until we have the full headers */
    char *header_end = NULL;
    while (!header_end) {
        if (raw_len + 4096 > raw_cap) {
            raw_cap *= 2;
            char *nb = (char *)realloc(raw, raw_cap);
            if (!nb) { free(raw); return false; }
            raw = nb;
        }
        ssize_t n = tls_read(c, raw + raw_len, 4096);
        if (n <= 0) break;
        raw_len += (size_t)n;
        raw[raw_len] = '\0';
        header_end = strstr(raw, "\r\n\r\n");
    }

    if (!header_end) {
        /* Might have all data already if connection closed */
        if (raw_len > 0) {
            header_end = strstr(raw, "\r\n\r\n");
        }
        if (!header_end) {
            free(raw);
            return false;
        }
    }

    /* Parse status line */
    int status = 0;
    sscanf(raw, "HTTP/%*d.%*d %d", &status);
    resp->status = status;

    /* Find Content-Length or chunked */
    size_t content_length = 0;
    bool chunked = false;
    bool has_content_length = false;
    {
        /* Scan headers (between first \r\n and \r\n\r\n) */
        const char *h = strstr(raw, "\r\n");
        if (h) h += 2;
        while (h && h < header_end) {
            if (strncasecmp(h, "Content-Length:", 15) == 0) {
                content_length = (size_t)atol(h + 15);
                has_content_length = true;
            }
            if (strncasecmp(h, "Transfer-Encoding:", 18) == 0) {
                /* Scope search to this header line only (not into body) */
                const char *eol = strstr(h, "\r\n");
                size_t hlen = eol ? (size_t)(eol - h) : (size_t)(header_end - h);
                for (const char *p = h + 18; p + 7 <= h + hlen; p++) {
                    if (strncasecmp(p, "chunked", 7) == 0) { chunked = true; break; }
                }
            }
            const char *next = strstr(h, "\r\n");
            if (!next) break;
            h = next + 2;
        }
    }

    /* Body starts after \r\n\r\n */
    const char *body_start = header_end + 4;
    size_t body_so_far = raw_len - (size_t)(body_start - raw);

    if (chunked) {
        /* Decode chunked transfer encoding */
        /* Collect all remaining data first */
        while (1) {
            if (raw_len + 4096 > raw_cap) {
                raw_cap *= 2;
                char *nb = (char *)realloc(raw, raw_cap);
                if (!nb) break;
                raw = nb;
                /* Recalculate body_start after realloc */
                header_end = strstr(raw, "\r\n\r\n");
                body_start = header_end + 4;
                body_so_far = raw_len - (size_t)(body_start - raw);
            }
            ssize_t n = tls_read(c, raw + raw_len, 4096);
            if (n <= 0) break;
            raw_len += (size_t)n;
            raw[raw_len] = '\0';

            /* Check for final chunk (0\r\n\r\n) */
            {
                size_t search_start = (raw_len > (size_t)n + 10)
                    ? raw_len - (size_t)n - 10 : 0;
                if (strstr(raw + search_start, "\r\n0\r\n\r\n"))
                    break;
            }
        }

        /* Re-locate body_start after potential reallocs */
        header_end = strstr(raw, "\r\n\r\n");
        body_start = header_end + 4;
        body_so_far = raw_len - (size_t)(body_start - raw);

        /* Decode chunks */
        const char *p = body_start;
        const char *end = raw + raw_len;
        while (p < end) {
            /* Read chunk size (hex) */
            long chunk_size = strtol(p, NULL, 16);
            if (chunk_size <= 0) break;

            /* Skip to chunk data (after \r\n) */
            const char *data = strstr(p, "\r\n");
            if (!data) break;
            data += 2;

            if (data + chunk_size <= end)
                resp_append(resp, data, (size_t)chunk_size);

            p = data + chunk_size;
            if (p + 2 <= end && p[0] == '\r' && p[1] == '\n')
                p += 2;
        }
    } else if (has_content_length) {
        /* Read remaining body by Content-Length */
        resp_append(resp, body_start, body_so_far);

        while (resp->body_len < content_length) {
            char buf[4096];
            size_t want = content_length - resp->body_len;
            if (want > sizeof(buf)) want = sizeof(buf);
            ssize_t n = tls_read(c, buf, want);
            if (n <= 0) break;
            resp_append(resp, buf, (size_t)n);
        }
    } else {
        /* Read until connection close */
        resp_append(resp, body_start, body_so_far);
        while (1) {
            char buf[4096];
            ssize_t n = tls_read(c, buf, sizeof(buf));
            if (n <= 0) break;
            resp_append(resp, buf, (size_t)n);
        }
    }

    free(raw);
    return true;
}

/* ── Public API ───────────────────────────────────────────────── */

bool nc_http_post(const char *url, const char *body, size_t body_len,
                  const char **headers, int header_count,
                  nc_http_response *resp) {
    resp_init(resp);

    parsed_url pu;
    if (!parse_url(url, &pu)) {
        nc_log(NC_LOG_ERROR, "Invalid URL: %s", url);
        return false;
    }

    /* TCP connect */
    int fd = tcp_connect(pu.host, pu.port);
    if (fd < 0) return false;

    /* TLS setup */
    tls_conn conn;
    memset(&conn, 0, sizeof(conn));
    conn.fd = fd;
    conn.is_tls = pu.is_https;

    if (!tls_connect(&conn, pu.host)) {
        close(fd);
        return false;
    }

    /* Build HTTP request */
    size_t req_cap = 0;
    if (!compute_request_cap("POST", pu.path, pu.host, true, headers, header_count, &req_cap)) {
        tls_close(&conn);
        return false;
    }
    char *req_header = (char *)malloc(req_cap);
    if (!req_header) {
        tls_close(&conn);
        return false;
    }
    int wrote = snprintf(req_header, req_cap,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n",
        pu.path, pu.host, body_len);
    if (wrote < 0 || (size_t)wrote >= req_cap) {
        free(req_header);
        tls_close(&conn);
        return false;
    }
    size_t off = (size_t)wrote;

    for (int i = 0; i < header_count; i++) {
        wrote = snprintf(req_header + off, req_cap - off,
            "%s\r\n", headers[i]);
        if (wrote < 0 || (size_t)wrote >= req_cap - off) {
            free(req_header);
            tls_close(&conn);
            return false;
        }
        off += (size_t)wrote;
    }
    wrote = snprintf(req_header + off, req_cap - off, "\r\n");
    if (wrote < 0 || (size_t)wrote >= req_cap - off) {
        free(req_header);
        tls_close(&conn);
        return false;
    }
    off += (size_t)wrote;
    size_t req_len = off;

    /* Send request */
    if (!tls_write_all(&conn, req_header, req_len)) {
        nc_log(NC_LOG_ERROR, "Failed to send HTTP headers");
        free(req_header);
        tls_close(&conn);
        return false;
    }
    free(req_header);

    if (body && body_len > 0) {
        if (!tls_write_all(&conn, body, body_len)) {
            nc_log(NC_LOG_ERROR, "Failed to send HTTP body");
            tls_close(&conn);
            return false;
        }
    }

    /* Flush TLS buffers (BearSSL buffers writes; must flush before reading) */
    if (!tls_flush(&conn)) {
        nc_log(NC_LOG_ERROR, "Failed to flush TLS write buffer");
        tls_close(&conn);
        return false;
    }

    /* Read response */
    bool ok = read_response(&conn, resp);

    tls_close(&conn);

    if (!ok) {
        nc_log(NC_LOG_ERROR, "Failed to read HTTP response");
        return false;
    }

    return true;
}

bool nc_http_get(const char *url, const char **headers, int header_count,
                 nc_http_response *resp) {
    resp_init(resp);

    parsed_url pu;
    if (!parse_url(url, &pu)) {
        nc_log(NC_LOG_ERROR, "Invalid URL: %s", url);
        return false;
    }

    int fd = tcp_connect(pu.host, pu.port);
    if (fd < 0) return false;

    tls_conn conn;
    memset(&conn, 0, sizeof(conn));
    conn.fd = fd;
    conn.is_tls = pu.is_https;

    if (!tls_connect(&conn, pu.host)) {
        close(fd);
        return false;
    }

    size_t req_cap = 0;
    if (!compute_request_cap("GET", pu.path, pu.host, false, headers, header_count, &req_cap)) {
        tls_close(&conn);
        return false;
    }
    char *req_header = (char *)malloc(req_cap);
    if (!req_header) {
        tls_close(&conn);
        return false;
    }
    int wrote = snprintf(req_header, req_cap,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n",
        pu.path, pu.host);
    if (wrote < 0 || (size_t)wrote >= req_cap) {
        free(req_header);
        tls_close(&conn);
        return false;
    }
    size_t off = (size_t)wrote;

    for (int i = 0; i < header_count; i++) {
        wrote = snprintf(req_header + off, req_cap - off,
            "%s\r\n", headers[i]);
        if (wrote < 0 || (size_t)wrote >= req_cap - off) {
            free(req_header);
            tls_close(&conn);
            return false;
        }
        off += (size_t)wrote;
    }
    wrote = snprintf(req_header + off, req_cap - off, "\r\n");
    if (wrote < 0 || (size_t)wrote >= req_cap - off) {
        free(req_header);
        tls_close(&conn);
        return false;
    }
    off += (size_t)wrote;

    size_t req_len = off;
    if (!tls_write_all(&conn, req_header, req_len)) {
        free(req_header);
        tls_close(&conn);
        return false;
    }
    free(req_header);

    if (!tls_flush(&conn)) {
        tls_close(&conn);
        return false;
    }

    bool ok = read_response(&conn, resp);
    tls_close(&conn);
    return ok;
}

/* ── Tests ────────────────────────────────────────────────────── */

#ifdef NC_TEST
void nc_test_http(void) {
    parsed_url pu;

    /* HTTPS URL */
    NC_ASSERT(parse_url("https://api.openai.com/v1/chat/completions", &pu), "parse https url");
    NC_ASSERT(pu.is_https == true, "https detected");
    NC_ASSERT(strcmp(pu.host, "api.openai.com") == 0, "https host parsed");
    NC_ASSERT(strcmp(pu.port, "443") == 0, "https default port");
    NC_ASSERT(strcmp(pu.path, "/v1/chat/completions") == 0, "https path parsed");

    /* HTTP URL */
    NC_ASSERT(parse_url("http://localhost:8080/health", &pu), "parse http url with port");
    NC_ASSERT(pu.is_https == false, "http detected");
    NC_ASSERT(strcmp(pu.host, "localhost") == 0, "http host parsed");
    NC_ASSERT(strcmp(pu.port, "8080") == 0, "http custom port parsed");
    NC_ASSERT(strcmp(pu.path, "/health") == 0, "http path parsed");

    /* URL with no path */
    NC_ASSERT(parse_url("https://example.com", &pu), "parse url no path");
    NC_ASSERT(strcmp(pu.host, "example.com") == 0, "no-path host");
    NC_ASSERT(strcmp(pu.path, "/") == 0, "no-path defaults to /");

    /* HTTP no port */
    NC_ASSERT(parse_url("http://example.com/foo/bar", &pu), "parse http no port");
    NC_ASSERT(strcmp(pu.port, "80") == 0, "http default port 80");

    /* Invalid URL (no scheme) */
    NC_ASSERT(!parse_url("ftp://bad.com", &pu), "reject ftp scheme");
    NC_ASSERT(!parse_url("just-a-string", &pu), "reject no scheme");

    /* Response buffer management */
    nc_http_response resp;
    resp_init(&resp);
    NC_ASSERT(resp.body != NULL, "resp_init allocates body");
    NC_ASSERT(resp.body_len == 0, "resp_init body_len is 0");

    resp_append(&resp, "hello", 5);
    NC_ASSERT(resp.body_len == 5, "resp_append length");
    NC_ASSERT(strcmp(resp.body, "hello") == 0, "resp_append content");

    /* Append enough to trigger realloc */
    char big[8192];
    memset(big, 'A', sizeof(big));
    resp_append(&resp, big, sizeof(big));
    NC_ASSERT(resp.body_len == 5 + sizeof(big), "resp_append after growth");
    NC_ASSERT(resp.body[0] == 'h', "preserved original data after realloc");

    nc_http_response_free(&resp);
    NC_ASSERT(resp.body == NULL, "resp_free nulls body");
}
#endif
