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
#include <stdint.h>
#include <stdio.h>

#if !defined(PLATFORM_WEB) && !defined(PLATFORM_ANDROID) && \
    !defined(PLATFORM_IOS) && !defined(_WIN32)

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

static uint64_t rng_next(uint64_t* s) {
    uint64_t x = *s ? *s : 0x2545F4914F6CDD1DULL;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}

static void b64(const uint8_t* in, size_t len, char* out) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? T[v & 63] : '=';
    }
    out[o] = '\0';
}

// --- Transport shim: raw socket, or OpenSSL when tls ------------------------
static int t_write(NetConn* c, const void* buf, size_t len) {
#ifdef OR_TLS
    if (c->tls) {
        int n = SSL_write(c->ssl, buf, (int)len);
        if (n > 0) return n;
        int e = SSL_get_error(c->ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return T_AGAIN;
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
    return -1;
#else
    (void)c; return -1;
#endif
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
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        c->status = NET_ERROR;
        return c;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); c->status = NET_ERROR; return c; }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) { close(fd); c->status = NET_ERROR; return c; }
    c->fd = fd;

#ifdef OR_TLS
    if (tls) {
        c->ctx = SSL_CTX_new(TLS_client_method());
        if (!c->ctx) { c->status = NET_ERROR; return c; }
        SSL_CTX_set_default_verify_paths(c->ctx);           // system CA store
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

    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(rng_next(&c->rng) >> 24);
    char key64[25];
    b64(key, 16, key64);
    // Host header without the port (matches the Fly edge's routing; the daemon
    // ignores Host entirely).
    snprintf(c->req, sizeof c->req,
             "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
             "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             path && path[0] ? path : "/", host, key64);
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

static void enqueue_frame(NetConn* c, int opcode, const uint8_t* data, size_t len) {
    // Client frames are masked. Header is 2 or 4 bytes + 4 mask bytes.
    size_t hdr = (len < 126) ? 2 : 4;
    size_t need = hdr + 4 + len;
    if (c->out_len + need > NET_OUT_CAP) { c->status = NET_ERROR; return; }
    uint8_t* p = c->out + c->out_len;
    p[0] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (len < 126) {
        p[1] = (uint8_t)(0x80 | len);
    } else {
        p[1] = (uint8_t)(0x80 | 126);
        p[2] = (uint8_t)(len >> 8);
        p[3] = (uint8_t)len;
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rng_next(&c->rng) >> 17);
    memcpy(p + hdr, mask, 4);
    for (size_t i = 0; i < len; i++) p[hdr + 4 + i] = data[i] ^ mask[i & 3];
    c->out_len += need;
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

// Decode server->client frames (unmasked). Returns bytes consumed, 0 if the
// buffer holds a partial frame, -1 on protocol error.
static int decode_frame(uint8_t* buf, size_t len, int* op, uint8_t** pl, size_t* pn) {
    if (len < 2) return 0;
    int fin = buf[0] & 0x80, rsv = buf[0] & 0x70, o = buf[0] & 0x0F;
    int masked = buf[1] & 0x80;
    size_t l = buf[1] & 0x7F;
    if (rsv || masked || !fin || o == 0) return -1;   // server must not mask/fragment
    size_t hdr = 2;
    if (l == 127) return -1;
    if (l == 126) {
        if (len < 4) return 0;
        l = (size_t)buf[2] << 8 | buf[3];
        hdr = 4;
    }
    if (hdr + l > NET_IN_CAP) return -1;
    if (len < hdr + l) return 0;
    *op = o; *pl = buf + hdr; *pn = l;
    return (int)(hdr + l);
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
        if (n == 0) { c->status = NET_CLOSED; return; }
        if (n == T_AGAIN) break;
        if (n < 0) { c->status = NET_ERROR; return; }
        c->in_len += (size_t)n;
    }

    if (c->handshaking) {
        uint8_t* end = NULL;
        for (size_t i = 0; i + 3 < c->in_len; i++) {
            if (memcmp(c->in + i, "\r\n\r\n", 4) == 0) { end = c->in + i + 4; break; }
        }
        if (!end) {
            // A full buffer with no header terminator can't make progress —
            // treat it as a bad handshake rather than stalling forever.
            if (c->in_len == NET_IN_CAP) c->status = NET_ERROR;
            return;
        }
        if (c->in_len < 12 || memcmp(c->in, "HTTP/1.1 101", 12) != 0) {
            c->status = NET_ERROR;
            return;
        }
        size_t consumed = (size_t)(end - c->in);
        memmove(c->in, end, c->in_len - consumed);
        c->in_len -= consumed;
        c->handshaking = false;
        c->status = NET_OPEN;
    }

    while (c->status == NET_OPEN) {
        int op; uint8_t* pl; size_t pn;
        int used = decode_frame(c->in, c->in_len, &op, &pl, &pn);
        if (used == 0) break;
        if (used < 0) { c->status = NET_ERROR; return; }
        if (op == 0x1) {                 // text
            push_msg(c, pl, pn);
        } else if (op == 0x9) {          // ping -> pong
            enqueue_frame(c, 0xA, pl, pn);
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
            if (err != 0) { c->status = NET_ERROR; return; }
        }
        int h = tls_step(c);
        if (h < 0) { c->status = NET_ERROR; return; }
        if (h == 0) return;   // still negotiating
        c->tls_done = true;
    }

    if (c->handshaking && c->req_sent < strlen(c->req)) {
        // Confirm the async TCP connect for the plain path (TLS did it above).
        if (!c->tls && c->req_sent == 0) {
            int err = 0; socklen_t sl = sizeof err;
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &sl) != 0 || err != 0) {
                if (err != 0) { c->status = NET_ERROR; return; }
            }
        }
        size_t total = strlen(c->req);
        while (c->req_sent < total) {
            int n = t_write(c, c->req + c->req_sent, total - c->req_sent);
            if (n > 0) c->req_sent += (size_t)n;
            else if (n == T_AGAIN) break;
            else { c->status = NET_ERROR; break; }
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
    enqueue_frame(c, 0x1, (const uint8_t*)text, len);
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
            enqueue_frame(c, 0x8, NULL, 0);   // best-effort close frame
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
