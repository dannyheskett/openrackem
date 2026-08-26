#ifndef OPENRACKEM_NET_H
#define OPENRACKEM_NET_H

#include <stdbool.h>
#include <stddef.h>

// Transport-agnostic WebSocket client seam. The online session (netgame.c)
// speaks only these calls; the backend behind them is chosen per platform:
//   net_posix.c — a from-scratch WS client over a POSIX socket (desktop
//                 native + the loopback tests). Vendors nothing, mirroring
//                 the server's ws.c.
//   net_web.c   — emscripten's WebSocket API (the WASM build).
//   net_stub.c  — a no-op (platforms without an online client yet); the menu
//                 hides "Play Online" when NetStatus never leaves NET_OFF.
// A backend delivers whole text messages (never partial frames) and never
// blocks the caller.

typedef enum {
    NET_IDLE = 0,    // no connection attempt yet
    NET_CONNECTING,  // TCP + WebSocket handshake in flight
    NET_OPEN,        // ready to send/receive application messages
    NET_CLOSED,      // closed cleanly or by the peer
    NET_ERROR,       // connect failed / transport error
} NetStatus;

typedef struct NetConn NetConn;

// Begin connecting to host:port at `path`. `tls` selects wss:// (TLS via the
// system OpenSSL, verifying the server certificate and hostname) vs plain
// ws://. Non-blocking: returns a handle immediately (status NET_CONNECTING) or
// NULL if a connection can't be started at all. A tls request on a build
// without TLS support fails to NET_ERROR.
NetConn* net_connect(const char* host, int port, const char* path, bool tls);

// Pump the transport (handshake progress, reads, writes). Call once per frame.
void net_poll(NetConn* c);

NetStatus net_status(const NetConn* c);

// Queue one text message for sending. Returns false if the send buffer is
// full or the connection is not open (the caller then treats it as a drop).
bool net_send(NetConn* c, const char* text, size_t len);

// Pop the next received text message into buf (NUL-terminated, truncated to
// cap-1). Returns the length, or -1 when the inbound queue is empty.
int net_recv(NetConn* c, char* buf, size_t cap);

void net_close(NetConn* c);

// True on builds that actually ship an online client (menu gating).
bool net_available(void);

#endif // OPENRACKEM_NET_H
