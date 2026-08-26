// Web (Emscripten) net.h backend. The browser owns RFC 6455 framing AND TLS, so
// this writes neither: net_connect opens a WebSocket to wss://host:port/path and
// the browser handshakes with the Fly edge using the OS/browser trust store
// (SNI + hostname verification for free). The API is callback-based but all
// callbacks fire on the main thread, interleaved with — never during —
// net_poll/net_recv, so the async->sync bridge is a plain lock-free ring of
// complete text messages the onmessage handler fills and net_recv drains.
//
// Compiles to an empty TU off PLATFORM_WEB (every other platform selects a
// different net_*.c). Link needs -lwebsocket.js (see the Makefile web target).
#include "net.h"

#ifdef PLATFORM_WEB

#include <emscripten/websocket.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define WEB_MSGQ    32
#define WEB_MSG_CAP 2048

struct NetConn {
    EMSCRIPTEN_WEBSOCKET_T sock;   // small positive handle, <=0 = failed
    NetStatus status;
    char msgq[WEB_MSGQ][WEB_MSG_CAP];
    int  msgq_len[WEB_MSGQ];
    int  q_head, q_tail;           // net_recv drains head, onmessage fills tail
};

static EM_BOOL on_open(int t, const EmscriptenWebSocketOpenEvent* e, void* ud) {
    (void)t; (void)e;
    ((NetConn*)ud)->status = NET_OPEN;
    return EM_TRUE;
}
static EM_BOOL on_error(int t, const EmscriptenWebSocketErrorEvent* e, void* ud) {
    (void)t; (void)e;
    ((NetConn*)ud)->status = NET_ERROR;
    return EM_TRUE;
}
static EM_BOOL on_close(int t, const EmscriptenWebSocketCloseEvent* e, void* ud) {
    (void)t; (void)e;
    NetConn* c = (NetConn*)ud;
    if (c->status != NET_ERROR) c->status = NET_CLOSED;
    return EM_TRUE;
}
static EM_BOOL on_message(int t, const EmscriptenWebSocketMessageEvent* e, void* ud) {
    (void)t;
    NetConn* c = (NetConn*)ud;
    if (!e->isText) return EM_TRUE;               // protocol is text-only
    int next = (c->q_tail + 1) % WEB_MSGQ;
    if (next == c->q_head) return EM_TRUE;         // ring full: drop (idempotent states)
    // For text frames numBytes counts the trailing NUL, so the string length is
    // numBytes-1; copying numBytes would append a stray NUL.
    size_t len = e->numBytes > 0 ? (size_t)e->numBytes - 1 : 0;
    if (len >= WEB_MSG_CAP) len = WEB_MSG_CAP - 1;
    memcpy(c->msgq[c->q_tail], e->data, len);
    c->msgq[c->q_tail][len] = '\0';
    c->msgq_len[c->q_tail] = (int)len;
    c->q_tail = next;
    return EM_TRUE;
}

NetConn* net_connect(const char* host, int port, const char* path, bool tls) {
    if (!emscripten_websocket_is_supported()) return NULL;
    NetConn* c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->status = NET_CONNECTING;

    char url[256];
    snprintf(url, sizeof url, "%s://%s:%d%s", tls ? "wss" : "ws", host, port,
             (path && path[0]) ? path : "/");
    EmscriptenWebSocketCreateAttributes attrs;
    emscripten_websocket_init_create_attributes(&attrs);
    attrs.url = url;
    attrs.createOnMainThread = EM_TRUE;

    c->sock = emscripten_websocket_new(&attrs);
    if (c->sock <= 0) { free(c); return NULL; }

    emscripten_websocket_set_onopen_callback(c->sock, c, on_open);
    emscripten_websocket_set_onerror_callback(c->sock, c, on_error);
    emscripten_websocket_set_onclose_callback(c->sock, c, on_close);
    emscripten_websocket_set_onmessage_callback(c->sock, c, on_message);
    return c;
}

// Status is entirely callback-driven; nothing to pump.
void net_poll(NetConn* c) { (void)c; }

NetStatus net_status(const NetConn* c) { return c ? c->status : NET_ERROR; }

bool net_send(NetConn* c, const char* text, size_t len) {
    (void)len;
    if (!c || c->status != NET_OPEN) return false;
    return emscripten_websocket_send_utf8_text(c->sock, text) == EMSCRIPTEN_RESULT_SUCCESS;
}

int net_recv(NetConn* c, char* buf, size_t cap) {
    if (!c || c->q_head == c->q_tail) return -1;
    int len = c->msgq_len[c->q_head];
    size_t n = ((size_t)len < cap - 1) ? (size_t)len : cap - 1;
    memcpy(buf, c->msgq[c->q_head], n);
    buf[n] = '\0';
    c->q_head = (c->q_head + 1) % WEB_MSGQ;
    return (int)n;
}

void net_close(NetConn* c) {
    if (!c) return;
    if (c->sock > 0) {
        emscripten_websocket_close(c->sock, 1000, NULL);
        emscripten_websocket_delete(c->sock);
    }
    free(c);
}

bool net_available(void) { return emscripten_websocket_is_supported(); }

#endif // PLATFORM_WEB
