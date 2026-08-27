// Windows WebSocket client backend for net.h — winsock2 sockets with SChannel
// (SSPI) for wss:// TLS. The RFC 6455 framing is shared with the POSIX backend
// via net_ws.c; only the byte transport and TLS differ here. Nonblocking
// throughout: net_poll drives the async connect, the SChannel handshake, the
// WebSocket upgrade, and encrypted framed I/O without ever blocking the render
// loop — same contract as net_posix.c.
//
// TLS uses the system trust store and validates the server certificate and
// hostname (pszTargetName drives SChannel's automatic validation, the SChannel
// equivalent of OpenSSL's SSL_set1_host); a bad cert fails to NET_ERROR. Legacy
// SCHANNEL_CRED pinned to TLS 1.2 for the broadest reach and to sidestep TLS 1.3
// close-notify token handling, matching the Fly edge.
//
// Not compiled off Windows — other platforms select a different net_*.c.
#include "net.h"

#if defined(_WIN32)

#include "net_ws.h"

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Libraries are linked explicitly in the Makefile (-lws2_32 -lsecur32 -lcrypt32);
// no MSVC #pragma comment(lib) needed for the mingw toolchain.

#define WIN_IN_CAP   16384
#define WIN_OUT_CAP  16384
#define WIN_ENC_CAP  32768
#define WIN_MSGQ     32
#define WIN_MSG_CAP  2048

// Transport shim sentinels (distinct from a byte count >= 0).
#define T_AGAIN (-2)
#define T_ERR   (-1)

// Refcounted WSAStartup — the loopback/live tests open two connections in one
// process, so cleanup must wait for the last close. Single-threaded (the game
// pumps net_poll from one thread), so a plain counter is enough.
static int g_wsa_refs = 0;
static void wsa_ref(void) {
    if (g_wsa_refs++ == 0) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
}
static void wsa_unref(void) {
    if (--g_wsa_refs == 0) WSACleanup();
}

struct NetConn {
    SOCKET   sock;
    NetStatus status;
    bool     connected;     // async TCP connect confirmed
    bool     handshaking;   // transport up, waiting for the WS 101 response
    bool     req_sent_done;
    bool     tls;
    bool     tls_done;      // SChannel handshake complete

    char     host[256];
    char     req[256];      // the WS upgrade request
    size_t   req_sent;

    // Plaintext WebSocket byte streams (framing operates on these).
    uint8_t  in[WIN_IN_CAP];
    size_t   in_len;
    uint8_t  out[WIN_OUT_CAP];
    size_t   out_len;

    // Ciphertext staging for TLS (encin: socket->decrypt, encout: encrypt->socket).
    uint8_t  encin[WIN_ENC_CAP];
    size_t   encin_len;
    uint8_t  encout[WIN_ENC_CAP];
    size_t   encout_len, encout_sent;

    // Handshake token pending transmission (ClientHello / subsequent flights).
    uint8_t  hsout[WIN_ENC_CAP];
    size_t   hsout_len, hsout_sent;
    bool     hs_finish;     // final token queued; connection opens once it drains

    char     msgq[WIN_MSGQ][WIN_MSG_CAP];
    int      msgq_len[WIN_MSGQ];
    int      q_head, q_tail;

    uint64_t rng;

    CredHandle cred;   bool have_cred;
    CtxtHandle ctxt;   bool have_ctxt;
    SecPkgContext_StreamSizes sizes;
};

// --- Raw socket I/O ---------------------------------------------------------
static int sock_send(SOCKET s, const void* buf, size_t len) {
    int n = send(s, (const char*)buf, (int)len, 0);
    if (n >= 0) return n;
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK || e == WSAEINTR) return T_AGAIN;
    return T_ERR;
}
static int sock_recv(SOCKET s, void* buf, size_t len) {
    int n = recv(s, (char*)buf, (int)len, 0);
    if (n > 0) return n;
    if (n == 0) return 0;                 // clean close
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK || e == WSAEINTR) return T_AGAIN;
    return T_ERR;
}

// Confirm an async connect via select(); sets c->connected or NET_ERROR.
static void confirm_connect(NetConn* c) {
    fd_set wf, ef;
    FD_ZERO(&wf); FD_ZERO(&ef);
    FD_SET(c->sock, &wf); FD_SET(c->sock, &ef);
    struct timeval tv = {0, 0};
    if (select(0, NULL, &wf, &ef, &tv) == SOCKET_ERROR) { c->status = NET_ERROR; return; }
    if (FD_ISSET(c->sock, &ef)) { c->status = NET_ERROR; return; }
    if (FD_ISSET(c->sock, &wf)) {
        int err = 0; int sl = sizeof err;
        if (getsockopt(c->sock, SOL_SOCKET, SO_ERROR, (char*)&err, &sl) != 0 || err != 0) {
            c->status = NET_ERROR; return;
        }
        c->connected = true;
    }
}

// --- SChannel TLS -----------------------------------------------------------
static const DWORD ISC_FLAGS =
    ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
    ISC_REQ_EXTENDED_ERROR  | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

// Generate the initial ClientHello token into hsout. Returns false on failure.
static bool tls_begin(NetConn* c) {
    SCHANNEL_CRED sc; memset(&sc, 0, sizeof sc);
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    TimeStamp ts;
    SECURITY_STATUS ss = AcquireCredentialsHandleA(
        NULL, (SEC_CHAR*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc,
        NULL, NULL, &c->cred, &ts);
    if (ss != SEC_E_OK) return false;
    c->have_cred = true;

    SecBuffer outbuf = {0, SECBUFFER_TOKEN, NULL};
    SecBufferDesc outd = {SECBUFFER_VERSION, 1, &outbuf};
    DWORD attr = 0;
    ss = InitializeSecurityContextA(
        &c->cred, NULL, c->host, ISC_FLAGS, 0, SECURITY_NATIVE_DREP,
        NULL, 0, &c->ctxt, &outd, &attr, &ts);
    if (ss != SEC_I_CONTINUE_NEEDED) return false;
    c->have_ctxt = true;
    if (outbuf.cbBuffer && outbuf.pvBuffer) {
        if (outbuf.cbBuffer <= WIN_ENC_CAP) {
            memcpy(c->hsout, outbuf.pvBuffer, outbuf.cbBuffer);
            c->hsout_len = outbuf.cbBuffer;
            c->hsout_sent = 0;
        }
        FreeContextBuffer(outbuf.pvBuffer);
    }
    return true;
}

// Flush the pending handshake token; returns 1 fully sent, 0 would-block, -1 err.
static int tls_flush_hsout(NetConn* c) {
    while (c->hsout_sent < c->hsout_len) {
        int n = sock_send(c->sock, c->hsout + c->hsout_sent, c->hsout_len - c->hsout_sent);
        if (n > 0) c->hsout_sent += (size_t)n;
        else if (n == T_AGAIN) return 0;
        else return -1;
    }
    c->hsout_len = c->hsout_sent = 0;
    return 1;
}

// Drive the SChannel handshake. Reads server tokens, calls ISC, queues reply
// tokens, and completes once ISC returns SEC_E_OK and the final token drains.
static void tls_handshake(NetConn* c) {
    for (;;) {
        int f = tls_flush_hsout(c);
        if (f == 0) return;                       // token still draining
        if (f < 0) { c->status = NET_ERROR; return; }
        if (c->hs_finish) {                       // final token sent -> open
            if (QueryContextAttributes(&c->ctxt, SECPKG_ATTR_STREAM_SIZES, &c->sizes) != SEC_E_OK) {
                c->status = NET_ERROR; return;
            }
            c->tls_done = true;
            return;
        }

        // Need more server bytes before the next ISC.
        int n = sock_recv(c->sock, c->encin + c->encin_len, WIN_ENC_CAP - c->encin_len);
        if (n == 0) { c->status = NET_ERROR; return; }   // closed mid-handshake
        if (n == T_ERR) { c->status = NET_ERROR; return; }
        if (n == T_AGAIN) {
            if (c->encin_len == 0) return;               // nothing to feed ISC yet
            // else fall through and let ISC judge completeness
        } else {
            c->encin_len += (size_t)n;
        }

        SecBuffer inbuf[2];
        inbuf[0].BufferType = SECBUFFER_TOKEN; inbuf[0].pvBuffer = c->encin; inbuf[0].cbBuffer = (DWORD)c->encin_len;
        inbuf[1].BufferType = SECBUFFER_EMPTY; inbuf[1].pvBuffer = NULL;     inbuf[1].cbBuffer = 0;
        SecBufferDesc ind = {SECBUFFER_VERSION, 2, inbuf};

        SecBuffer outbuf = {0, SECBUFFER_TOKEN, NULL};
        SecBufferDesc outd = {SECBUFFER_VERSION, 1, &outbuf};
        DWORD attr = 0; TimeStamp ts;
        SECURITY_STATUS ss = InitializeSecurityContextA(
            &c->cred, &c->ctxt, c->host, ISC_FLAGS, 0, SECURITY_NATIVE_DREP,
            &ind, 0, &c->ctxt, &outd, &attr, &ts);

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            // Need a longer record; keep encin, read more next iteration. If the
            // last read would-blocked, yield so we don't spin.
            if (n == T_AGAIN) return;
            continue;
        }

        // Queue any reply token SChannel produced.
        if (outbuf.cbBuffer && outbuf.pvBuffer) {
            if (outbuf.cbBuffer <= WIN_ENC_CAP) {
                memcpy(c->hsout, outbuf.pvBuffer, outbuf.cbBuffer);
                c->hsout_len = outbuf.cbBuffer; c->hsout_sent = 0;
            }
            FreeContextBuffer(outbuf.pvBuffer);
        }

        // Consume the processed input; SECBUFFER_EXTRA marks trailing bytes that
        // belong to the next record.
        if (inbuf[1].BufferType == SECBUFFER_EXTRA && inbuf[1].cbBuffer > 0) {
            size_t extra = inbuf[1].cbBuffer;
            memmove(c->encin, c->encin + (c->encin_len - extra), extra);
            c->encin_len = extra;
        } else {
            c->encin_len = 0;
        }

        if (ss == SEC_E_OK) {
            c->hs_finish = true;         // may have a final token; flush then open
            continue;
        }
        if (ss == SEC_I_CONTINUE_NEEDED) continue;
        c->status = NET_ERROR; return;   // cert/hostname failure lands here too
    }
}

// Encrypt up to one chunk of plaintext from c->out into c->encout (which must be
// empty). Returns false on a fatal SChannel error.
static bool tls_encrypt_chunk(NetConn* c) {
    size_t max = c->sizes.cbMaximumMessage;
    if (max > 4096) max = 4096;                 // keep records small; frames are tiny
    size_t n = c->out_len < max ? c->out_len : max;
    if (n == 0) return true;

    uint8_t* hdr = c->encout;
    uint8_t* data = hdr + c->sizes.cbHeader;
    uint8_t* trl = data + n;
    memcpy(data, c->out, n);

    SecBuffer b[4];
    b[0].BufferType = SECBUFFER_STREAM_HEADER;  b[0].pvBuffer = hdr;  b[0].cbBuffer = c->sizes.cbHeader;
    b[1].BufferType = SECBUFFER_DATA;           b[1].pvBuffer = data; b[1].cbBuffer = (DWORD)n;
    b[2].BufferType = SECBUFFER_STREAM_TRAILER; b[2].pvBuffer = trl;  b[2].cbBuffer = c->sizes.cbTrailer;
    b[3].BufferType = SECBUFFER_EMPTY;          b[3].pvBuffer = NULL; b[3].cbBuffer = 0;
    SecBufferDesc d = {SECBUFFER_VERSION, 4, b};

    if (EncryptMessage(&c->ctxt, 0, &d, 0) != SEC_E_OK) return false;
    c->encout_len = b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer;
    c->encout_sent = 0;
    memmove(c->out, c->out + n, c->out_len - n);
    c->out_len -= n;
    return true;
}

// --- Outbound / inbound pumps ----------------------------------------------
static void flush_out(NetConn* c) {
    if (!c->tls) {
        while (c->out_len > 0) {
            int n = sock_send(c->sock, c->out, c->out_len);
            if (n > 0) { memmove(c->out, c->out + n, c->out_len - (size_t)n); c->out_len -= (size_t)n; }
            else if (n == T_AGAIN) return;
            else { c->status = NET_ERROR; return; }
        }
        return;
    }
    for (;;) {
        while (c->encout_sent < c->encout_len) {           // drain ciphertext
            int n = sock_send(c->sock, c->encout + c->encout_sent, c->encout_len - c->encout_sent);
            if (n > 0) c->encout_sent += (size_t)n;
            else if (n == T_AGAIN) return;
            else { c->status = NET_ERROR; return; }
        }
        c->encout_len = c->encout_sent = 0;
        if (c->out_len == 0) return;
        if (!tls_encrypt_chunk(c)) { c->status = NET_ERROR; return; }
    }
}

static void push_msg(NetConn* c, const uint8_t* data, size_t len) {
    int next = (c->q_tail + 1) % WIN_MSGQ;
    if (next == c->q_head) return;
    if (len >= WIN_MSG_CAP) len = WIN_MSG_CAP - 1;
    memcpy(c->msgq[c->q_tail], data, len);
    c->msgq[c->q_tail][len] = '\0';
    c->msgq_len[c->q_tail] = (int)len;
    c->q_tail = next;
}

static void enq(NetConn* c, int opcode, const uint8_t* data, size_t len) {
    if (nw_encode_frame(c->out, &c->out_len, WIN_OUT_CAP, opcode, data, len, &c->rng) != 0)
        c->status = NET_ERROR;
}

// Parse the 101 upgrade then decode WS frames sitting in c->in (shared logic
// with net_posix's inbound path).
static void process_in(NetConn* c) {
    if (c->handshaking) {
        long consumed = nw_parse_handshake(c->in, c->in_len);
        if (consumed == 0) {
            if (c->in_len == WIN_IN_CAP) c->status = NET_ERROR;
            return;
        }
        if (consumed < 0) { c->status = NET_ERROR; return; }
        memmove(c->in, c->in + consumed, c->in_len - (size_t)consumed);
        c->in_len -= (size_t)consumed;
        c->handshaking = false;
        c->status = NET_OPEN;
    }
    while (c->status == NET_OPEN) {
        int op; uint8_t* pl; size_t pn;
        int used = nw_decode_frame(c->in, c->in_len, WIN_IN_CAP, &op, &pl, &pn);
        if (used == 0) break;
        if (used < 0) { c->status = NET_ERROR; return; }
        if (op == 0x1)      push_msg(c, pl, pn);
        else if (op == 0x9) enq(c, 0xA, pl, pn);       // ping -> pong
        else if (op == 0x8) c->status = NET_CLOSED;
        memmove(c->in, c->in + used, c->in_len - (size_t)used);
        c->in_len -= (size_t)used;
    }
}

// Read socket bytes; for TLS decrypt into c->in, else copy straight in.
static void pump_read(NetConn* c) {
    if (!c->tls) {
        while (c->in_len < WIN_IN_CAP) {
            int n = sock_recv(c->sock, c->in + c->in_len, WIN_IN_CAP - c->in_len);
            if (n == 0) { c->status = NET_CLOSED; return; }
            if (n == T_AGAIN) break;
            if (n < 0) { c->status = NET_ERROR; return; }
            c->in_len += (size_t)n;
        }
        process_in(c);
        return;
    }

    // Fill encin from the socket.
    while (c->encin_len < WIN_ENC_CAP) {
        int n = sock_recv(c->sock, c->encin + c->encin_len, WIN_ENC_CAP - c->encin_len);
        if (n == 0) { c->status = NET_CLOSED; break; }
        if (n == T_AGAIN) break;
        if (n < 0) { c->status = NET_ERROR; return; }
        c->encin_len += (size_t)n;
    }
    // Decrypt whatever complete records we have.
    while (c->encin_len > 0 && c->in_len < WIN_IN_CAP) {
        SecBuffer b[4];
        b[0].BufferType = SECBUFFER_DATA;  b[0].pvBuffer = c->encin; b[0].cbBuffer = (DWORD)c->encin_len;
        b[1].BufferType = SECBUFFER_EMPTY; b[1].pvBuffer = NULL;     b[1].cbBuffer = 0;
        b[2].BufferType = SECBUFFER_EMPTY; b[2].pvBuffer = NULL;     b[2].cbBuffer = 0;
        b[3].BufferType = SECBUFFER_EMPTY; b[3].pvBuffer = NULL;     b[3].cbBuffer = 0;
        SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
        SECURITY_STATUS ss = DecryptMessage(&c->ctxt, &d, 0, NULL);

        if (ss == SEC_E_INCOMPLETE_MESSAGE) break;      // need more ciphertext
        if (ss == SEC_I_CONTEXT_EXPIRED) { c->status = NET_CLOSED; break; }  // TLS close
        if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE) { c->status = NET_ERROR; return; }

        // Append decrypted plaintext, then compact any leftover ciphertext.
        size_t extra_len = 0; uint8_t* extra_ptr = NULL;
        for (int i = 0; i < 4; i++) {
            if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer) {
                size_t take = b[i].cbBuffer;
                if (c->in_len + take > WIN_IN_CAP) take = WIN_IN_CAP - c->in_len;
                memcpy(c->in + c->in_len, b[i].pvBuffer, take);
                c->in_len += take;
            } else if (b[i].BufferType == SECBUFFER_EXTRA && b[i].cbBuffer) {
                extra_len = b[i].cbBuffer; extra_ptr = (uint8_t*)b[i].pvBuffer;
            }
        }
        if (extra_len) { memmove(c->encin, extra_ptr, extra_len); c->encin_len = extra_len; }
        else c->encin_len = 0;

        if (ss == SEC_I_RENEGOTIATE) { c->status = NET_ERROR; return; }  // server-initiated; unsupported
    }
    process_in(c);
}

// --- net.h API --------------------------------------------------------------
NetConn* net_connect(const char* host, int port, const char* path, bool tls) {
    wsa_ref();
    NetConn* c = (NetConn*)calloc(1, sizeof *c);
    if (!c) { wsa_unref(); return NULL; }
    c->sock = INVALID_SOCKET;
    c->status = NET_CONNECTING;
    c->handshaking = true;
    c->tls = tls;
    LARGE_INTEGER li; QueryPerformanceCounter(&li);
    c->rng = (uint64_t)li.QuadPart * 2654435761u + (uintptr_t)c;
    strncpy(c->host, host ? host : "", sizeof c->host - 1);

    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) { c->status = NET_ERROR; return c; }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); c->status = NET_ERROR; return c; }
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    BOOL one = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one);
    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s); c->status = NET_ERROR; return c;
    }
    c->sock = s;

    if (tls && !tls_begin(c)) { c->status = NET_ERROR; return c; }

    if (nw_build_handshake(c->req, sizeof c->req, host, path, &c->rng) < 0)
        c->status = NET_ERROR;
    return c;
}

void net_poll(NetConn* c) {
    if (!c || c->sock == INVALID_SOCKET) return;
    if (c->status == NET_ERROR || c->status == NET_CLOSED) return;

    if (!c->connected) {
        confirm_connect(c);
        if (c->status == NET_ERROR) return;
        if (!c->connected) return;                 // still connecting
    }

    if (c->tls && !c->tls_done) {
        tls_handshake(c);
        if (c->status == NET_ERROR || !c->tls_done) return;
    }

    // Transport is up: queue the WS upgrade request (once) onto the outbound
    // byte stream, which flush_out then TLS-encrypts (or sends raw). The request
    // is <256 bytes, so it always fits the out buffer in one shot.
    if (!c->req_sent_done) {
        size_t total = strlen(c->req);
        if (c->out_len + total <= WIN_OUT_CAP) {
            memcpy(c->out + c->out_len, c->req, total);
            c->out_len += total;
            c->req_sent_done = true;
        } else {
            c->status = NET_ERROR;
            return;
        }
    }

    flush_out(c);
    pump_read(c);
    flush_out(c);
}

NetStatus net_status(const NetConn* c) { return c ? c->status : NET_ERROR; }

bool net_send(NetConn* c, const char* text, size_t len) {
    if (!c || c->status != NET_OPEN) return false;
    size_t before = c->out_len;
    enq(c, 0x1, (const uint8_t*)text, len);
    if (c->status == NET_ERROR) return false;
    if (c->out_len == before) return false;
    flush_out(c);
    return true;
}

int net_recv(NetConn* c, char* buf, size_t cap) {
    if (!c || c->q_head == c->q_tail) return -1;
    int len = c->msgq_len[c->q_head];
    size_t n = ((size_t)len < cap - 1) ? (size_t)len : cap - 1;
    memcpy(buf, c->msgq[c->q_head], n);
    buf[n] = '\0';
    c->q_head = (c->q_head + 1) % WIN_MSGQ;
    return (int)n;
}

void net_close(NetConn* c) {
    if (!c) return;
    if (c->sock != INVALID_SOCKET) {
        if (c->status == NET_OPEN) { enq(c, 0x8, NULL, 0); flush_out(c); }  // best-effort
        closesocket(c->sock);
    }
    if (c->have_ctxt) DeleteSecurityContext(&c->ctxt);
    if (c->have_cred) FreeCredentialsHandle(&c->cred);
    free(c);
    wsa_unref();
}

bool net_available(void) { return true; }

#endif // _WIN32
