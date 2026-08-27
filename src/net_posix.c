// POSIX WebSocket client backend for net.h — desktop native (Linux/macOS) and
// the loopback tests. Hand-written client half of RFC 6455 (client frames are
// masked, server frames are not), so the repo still vendors no network
// library. Nonblocking throughout: net_poll drives the TCP connect, an
// optional TLS handshake, the WebSocket handshake, and framed I/O without ever
// blocking the render loop.
//
// wss:// support (OR_TLS) is a thin byte-transport shim over OpenSSL — the
// framing code is unchanged and just reads/writes through t_read/t_write,
// which pick SSL_read/SSL_write or the raw socket. Built with OR_TLS on the
// Linux/macOS desktop targets (linking -lssl -lcrypto, as the game already
// links system libraries); without it, a tls request fails cleanly and only
// ws:// works (the Windows/mingw path, until a SChannel backend lands).
//
// Not compiled on Windows (winsock) or the web/mobile builds — those select a
// different net_*.c in the Makefile.
//
// -std=c99 hides getaddrinfo/clock_gettime/TCP_NODELAY behind feature macros;
// request the modern POSIX surface before any header is pulled in.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "net.h"
#include "net_ws.h"
#include <stdint.h>
#include <stdio.h>

// Android (Bionic) has the full POSIX socket surface this backend uses, so it
// reuses net_posix unchanged for the transport; only the TLS trust anchor
// differs (no OpenSSL-visible system CA store — see the PLATFORM_ANDROID branch
// in net_connect). Web/iOS/Windows/macOS select a different net_*.c.
#if !defined(PLATFORM_WEB) && \
    !defined(PLATFORM_IOS) && !defined(_WIN32) && !defined(OR_NET_APPLE)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef OR_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#ifdef PLATFORM_ANDROID
// Android ships no CA bundle OpenSSL can find (set_default_verify_paths finds
// nothing, and /system/etc/security/cacerts is incomplete on Android 14+ where
// the roots moved into a Conscrypt APEX). The Mozilla bundle is compiled in as a
// byte array (scripts/gen_cacert.py) and loaded from memory into the store.
#include <openssl/x509.h>
#include <openssl/pem.h>
#include "cacert_pem.h"
#endif
#endif

#define NET_IN_CAP   16384
#define NET_OUT_CAP  16384
#define NET_MSGQ     32
#define NET_MSG_CAP  2048

// Transport shim return sentinels (distinct from a byte count >= 0).
#define T_AGAIN (-2)   // would block; retry on the next poll
#define T_ERR   (-1)   // fatal transport error

struct NetConn {
    int      fd;
    NetStatus status;
    bool     handshaking;   // TCP/TLS up, waiting for the 101 response
    bool     tls;           // wss:// — bytes go through OpenSSL
    bool     tls_done;      // TLS handshake complete
    char     req[256];      // the upgrade request, sent as the socket drains
    size_t   req_sent;

    uint8_t  in[NET_IN_CAP];
    size_t   in_len;
    uint8_t  out[NET_OUT_CAP];
    size_t   out_len;

    char     msgq[NET_MSGQ][NET_MSG_CAP];
    int      msgq_len[NET_MSGQ];
    int      q_head, q_tail;

    uint64_t rng;
#ifdef OR_TLS
    SSL_CTX* ctx;
    SSL*     ssl;
#endif
};

// Append a masked client frame; a full out buffer is a fatal transport error
// (the caller can no longer stay in sync), matching the pre-extraction behavior.
static void enq(NetConn* c, int opcode, const uint8_t* data, size_t len) {
    if (nw_encode_frame(c->out, &c->out_len, NET_OUT_CAP, opcode, data, len,
                        &c->rng) != 0)
        c->status = NET_ERROR;
}

// --- Transport shim: raw socket, or OpenSSL when tls ------------------------
static int t_write(NetConn* c, const void* buf, size_t len) {
#ifdef OR_TLS
    if (c->tls) {
        int n = SSL_write(c->ssl, buf, (int)len);
        if (n > 0) return n;
        int e = SSL_get_error(c->ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return T_AGAIN;
        if (getenv("OR_NET_DIAG"))
            fprintf(stderr, "[net_posix] fail@ssl_write sslerr=%d errno=%d\n", e, errno);
        return T_ERR;
    }
#endif
    ssize_t n = write(c->fd, buf, len);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return T_AGAIN;
    return T_ERR;
}

// Returns bytes read (>0), 0 on clean close, T_AGAIN, or T_ERR.
static int t_read(NetConn* c, void* buf, size_t len) {
#ifdef OR_TLS
    if (c->tls) {
        int n = SSL_read(c->ssl, buf, (int)len);
        if (n > 0) return n;
        int e = SSL_get_error(c->ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return T_AGAIN;
        if (e == SSL_ERROR_ZERO_RETURN) return 0;   // clean TLS close_notify
        if (getenv("OR_NET_DIAG")) {
            char eb[160]; ERR_error_string_n(ERR_peek_last_error(), eb, sizeof eb);
            fprintf(stderr, "[net_posix] fail@ssl_read sslerr=%d errno=%d hs=%d status=%d reason=%s\n",
                    e, errno, c->handshaking, (int)c->status, eb);
        }
        return T_ERR;
    }
#endif
    ssize_t n = read(c->fd, buf, len);
    if (n > 0) return (int)n;
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return T_AGAIN;
    return T_ERR;
}

// Drive the TLS handshake. Returns 1 done, 0 in progress, -1 error.
static int tls_step(NetConn* c) {
#ifdef OR_TLS
    int r = SSL_connect(c->ssl);
    if (r == 1) return 1;
    int e = SSL_get_error(c->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
    if (getenv("OR_NET_DIAG"))
        fprintf(stderr, "[net_posix] fail@ssl_connect sslerr=%d errno=%d err=%lu\n",
                e, errno, (unsigned long)ERR_peek_last_error());
    return -1;
#else
    (void)c; return -1;
#endif
}

// Startup failure tracing. Set OR_NET_DIAG=1 to print exactly which step failed
// (and errno) to stderr; a no-op otherwise. Used to root-cause connect failures.
static void diag(const char* where, int e) {
    if (getenv("OR_NET_DIAG"))
        fprintf(stderr, "[net_posix] fail@%s errno=%d (%s)\n",
                where, e, e ? strerror(e) : "-");
}

NetConn* net_connect(const char* host, int port, const char* path, bool tls) {
    NetConn* c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = -1;
    c->status = NET_CONNECTING;
    c->handshaking = true;
    c->tls = tls;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->rng = (uint64_t)ts.tv_nsec * 2654435761u + (uint64_t)ts.tv_sec + (uintptr_t)c;

#ifndef OR_TLS
    if (tls) { c->status = NET_ERROR; return c; }   // TLS not built in
#endif

    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int grc = getaddrinfo(host, portstr, &hints, &res);
    if (grc != 0 || !res) {
        if (getenv("OR_NET_DIAG"))
            fprintf(stderr, "[net_posix] fail@getaddrinfo rc=%d (%s) host=%s\n",
                    grc, gai_strerror(grc), host ? host : "(null)");
        c->status = NET_ERROR;
        return c;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { diag("socket", errno); freeaddrinfo(res); c->status = NET_ERROR; return c; }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) { diag("connect", errno); close(fd); c->status = NET_ERROR; return c; }
    c->fd = fd;

#ifdef OR_TLS
    if (tls) {
        c->ctx = SSL_CTX_new(TLS_client_method());
        if (!c->ctx) { c->status = NET_ERROR; return c; }
#ifdef PLATFORM_ANDROID
        {   // Load the embedded Mozilla roots into the verify store.
            X509_STORE* store = SSL_CTX_get_cert_store(c->ctx);
            BIO* bio = BIO_new_mem_buf(cacert_pem, (int)cacert_pem_len);
            X509* x;
            while (bio && (x = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
                X509_STORE_add_cert(store, x);
                X509_free(x);
            }
            if (bio) BIO_free(bio);
        }
#else
        SSL_CTX_set_default_verify_paths(c->ctx);           // system CA store
#endif
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
        c->ssl = SSL_new(c->ctx);
        if (!c->ssl) { c->status = NET_ERROR; return c; }
        SSL_set_fd(c->ssl, fd);
        SSL_set_tlsext_host_name(c->ssl, host);             // SNI
        SSL_set1_host(c->ssl, host);                        // verify the hostname
        SSL_set_connect_state(c->ssl);
    }
#endif

    if (nw_build_handshake(c->req, sizeof c->req, host, path, &c->rng) < 0)
        c->status = NET_ERROR;
    return c;
}

static void push_msg(NetConn* c, const uint8_t* data, size_t len) {
    int next = (c->q_tail + 1) % NET_MSGQ;
    if (next == c->q_head) return;   // queue full: drop oldest-unread policy is
    if (len >= NET_MSG_CAP) len = NET_MSG_CAP - 1;   // caller-visible via recv
    memcpy(c->msgq[c->q_tail], data, len);
    c->msgq[c->q_tail][len] = '\0';
    c->msgq_len[c->q_tail] = (int)len;
    c->q_tail = next;
}

static void try_write(NetConn* c) {
    while (c->out_len > 0) {
        int n = t_write(c, c->out, c->out_len);
        if (n > 0) {
            memmove(c->out, c->out + n, c->out_len - (size_t)n);
            c->out_len -= (size_t)n;
        } else if (n == T_AGAIN) {
            return;
        } else {
            c->status = NET_ERROR;
            return;
        }
    }
}

static void try_read(NetConn* c) {
    for (;;) {
        // Buffer full: stop reading and fall through to the decoder, which
        // consumes the complete frames sitting in it and frees space for the
        // next poll. Only a single frame larger than the buffer is fatal, and
        // decode_frame reports that as a protocol error below — erroring here
        // would instead drop a buffer full of perfectly valid frames.
        if (c->in_len == NET_IN_CAP) break;
        int n = t_read(c, c->in + c->in_len, NET_IN_CAP - c->in_len);
        if (n == 0) { diag("read_eof", 0); c->status = NET_CLOSED; return; }
        if (n == T_AGAIN) break;
        if (n < 0) { c->status = NET_ERROR; return; }   // t_read already traced
        c->in_len += (size_t)n;
    }

    if (c->handshaking) {
        long consumed = nw_parse_handshake(c->in, c->in_len);
        if (consumed == 0) {
            // A full buffer with no header terminator can't make progress —
            // treat it as a bad handshake rather than stalling forever.
            if (c->in_len == NET_IN_CAP) { diag("handshake_nohdr", 0); c->status = NET_ERROR; }
            return;
        }
        if (consumed < 0) { diag("handshake_not101", 0); c->status = NET_ERROR; return; }
        memmove(c->in, c->in + consumed, c->in_len - (size_t)consumed);
        c->in_len -= (size_t)consumed;
        c->handshaking = false;
        c->status = NET_OPEN;
    }

    while (c->status == NET_OPEN) {
        int op; uint8_t* pl; size_t pn;
        int used = nw_decode_frame(c->in, c->in_len, NET_IN_CAP, &op, &pl, &pn);
        if (used == 0) break;
        if (used < 0) { c->status = NET_ERROR; return; }
        if (op == 0x1) {                 // text
            push_msg(c, pl, pn);
        } else if (op == 0x9) {          // ping -> pong
            enq(c, 0xA, pl, pn);
        } else if (op == 0x8) {          // close
            c->status = NET_CLOSED;
        }
        memmove(c->in, c->in + used, c->in_len - (size_t)used);
        c->in_len -= (size_t)used;
    }
}

void net_poll(NetConn* c) {
    if (!c || c->fd < 0) return;
    if (c->status == NET_ERROR || c->status == NET_CLOSED) return;

    // TLS handshake first (before any WebSocket bytes). Confirm the TCP
    // connect result once so a refused/unreachable host fails fast.
    if (c->tls && !c->tls_done) {
        int err = 0; socklen_t sl = sizeof err;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &sl) != 0 || err != 0) {
            if (err != 0) { diag("so_error_tls", err); c->status = NET_ERROR; return; }
        }
        int h = tls_step(c);
        if (h < 0) { c->status = NET_ERROR; return; }   // tls_step already traced
        if (h == 0) return;   // still negotiating
        c->tls_done = true;
    }

    if (c->handshaking && c->req_sent < strlen(c->req)) {
        // Confirm the async TCP connect for the plain path (TLS did it above).
        if (!c->tls && c->req_sent == 0) {
            int err = 0; socklen_t sl = sizeof err;
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &sl) != 0 || err != 0) {
                if (err != 0) { diag("so_error_plain", err); c->status = NET_ERROR; return; }
            }
        }
        size_t total = strlen(c->req);
        while (c->req_sent < total) {
            int n = t_write(c, c->req + c->req_sent, total - c->req_sent);
            if (n > 0) c->req_sent += (size_t)n;
            else if (n == T_AGAIN) break;
            else { diag("req_write", errno); c->status = NET_ERROR; break; }
        }
    }

    try_write(c);
    try_read(c);
    try_write(c);
}

NetStatus net_status(const NetConn* c) { return c ? c->status : NET_ERROR; }

bool net_send(NetConn* c, const char* text, size_t len) {
    if (!c || c->status != NET_OPEN) return false;
    size_t before = c->out_len;
    enq(c, 0x1, (const uint8_t*)text, len);
    if (c->status == NET_ERROR) return false;
    if (c->out_len == before) return false;   // frame didn't fit
    try_write(c);
    return true;
}

int net_recv(NetConn* c, char* buf, size_t cap) {
    if (!c || c->q_head == c->q_tail) return -1;
    int len = c->msgq_len[c->q_head];
    size_t n = ((size_t)len < cap - 1) ? (size_t)len : cap - 1;
    memcpy(buf, c->msgq[c->q_head], n);
    buf[n] = '\0';
    c->q_head = (c->q_head + 1) % NET_MSGQ;
    return (int)n;
}

void net_close(NetConn* c) {
    if (!c) return;
    if (c->fd >= 0) {
        if (c->status == NET_OPEN) {
            enq(c, 0x8, NULL, 0);   // best-effort close frame
            try_write(c);
        }
#ifdef OR_TLS
        if (c->ssl) SSL_shutdown(c->ssl);
#endif
        close(c->fd);
    }
#ifdef OR_TLS
    if (c->ssl) SSL_free(c->ssl);
    if (c->ctx) SSL_CTX_free(c->ctx);
#endif
    free(c);
}

bool net_available(void) { return true; }

#endif // POSIX platforms
