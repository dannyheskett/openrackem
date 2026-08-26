// Apple (macOS + iOS) net.h backend over Network.framework. NWConnection with a
// WebSocket protocol option does RFC 6455 framing AND TLS natively, so this
// writes neither — wss:// to the Fly edge validates against the system trust
// store (SNI + hostname verification) with zero crypto code, which also retires
// the OpenSSL dependency the macOS universal binary could not link. One file
// serves both OS targets (identical API).
//
// Compiled as Objective-C++ WITHOUT ARC (the nw_* / dispatch objects are C
// objects stored in a malloc'd struct, so they are retained/released by hand —
// simpler and safer than fighting ARC over C-struct fields). All I/O arrives on
// a private serial dispatch queue via blocks; the async->sync bridge to
// net_recv is a fixed ring of complete text messages guarded by a mutex, so the
// game's synchronous net_poll/net_recv never block.
//
// Selected on macOS via -DOR_NET_APPLE and on iOS via PLATFORM_IOS (see the
// Makefile); every other platform picks a different net_*.c.
#include "net.h"

#if defined(PLATFORM_IOS) || defined(OR_NET_APPLE)

#import <Network/Network.h>
#import <dispatch/dispatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define AP_MSGQ    32
#define AP_MSG_CAP 2048

struct NetConn {
    nw_connection_t conn;          // retained; NULL after close
    dispatch_queue_t queue;        // serial: all callbacks run here
    pthread_mutex_t lock;          // guards status + the ring
    NetStatus status;
    char msgq[AP_MSGQ][AP_MSG_CAP];
    int  msgq_len[AP_MSGQ];
    int  q_head, q_tail;           // net_recv drains head, receive cb fills tail
};

static void set_status(NetConn* c, NetStatus s) {
    pthread_mutex_lock(&c->lock);
    // Never overwrite a terminal ERROR/CLOSED with a lower state.
    if (c->status != NET_ERROR && c->status != NET_CLOSED) c->status = s;
    pthread_mutex_unlock(&c->lock);
}

static void enqueue(NetConn* c, const void* data, size_t len) {
    pthread_mutex_lock(&c->lock);
    int next = (c->q_tail + 1) % AP_MSGQ;
    if (next != c->q_head) {                 // ring not full
        if (len >= AP_MSG_CAP) len = AP_MSG_CAP - 1;
        memcpy(c->msgq[c->q_tail], data, len);
        c->msgq[c->q_tail][len] = '\0';
        c->msgq_len[c->q_tail] = (int)len;
        c->q_tail = next;
    }
    pthread_mutex_unlock(&c->lock);
}

// Arm one receive; re-arms itself from inside the completion (the #1 pitfall is
// forgetting to re-issue, which silently stalls all inbound traffic).
static void arm_receive(NetConn* c) {
    nw_connection_receive_message(c->conn,
        ^(dispatch_data_t content, nw_content_context_t context,
          bool is_complete, nw_error_t error) {
        (void)is_complete;
        if (content) {
            // Only enqueue text frames; inspect the WebSocket opcode metadata.
            bool is_text = true;
            if (context) {
                nw_protocol_metadata_t md = nw_content_context_copy_protocol_metadata(
                    context, nw_protocol_copy_ws_definition());
                if (md) {
                    nw_ws_opcode_t op = nw_ws_metadata_get_opcode(md);
                    is_text = (op == nw_ws_opcode_text);
                    if (op == nw_ws_opcode_close) set_status(c, NET_CLOSED);
                    nw_release(md);
                }
            }
            if (is_text) {
                const void* bytes = NULL; size_t size = 0;
                dispatch_data_t map = dispatch_data_create_map(content, &bytes, &size);
                if (size > 0) enqueue(c, bytes, size);
                dispatch_release(map);
            }
        }
        if (error) { set_status(c, NET_ERROR); return; }
        // A final context with is_complete + no content signals the stream end.
        if (context && nw_content_context_get_is_final(context) && !content) {
            set_status(c, NET_CLOSED);
            return;
        }
        pthread_mutex_lock(&c->lock);
        NetStatus st = c->status;
        pthread_mutex_unlock(&c->lock);
        if (st == NET_OPEN || st == NET_CONNECTING) arm_receive(c);   // re-arm
    });
}

NetConn* net_connect(const char* host, int port, const char* path, bool tls) {
    (void)path;   // the server ignores the WS path; Network.framework uses "/"
    NetConn* c = (NetConn*)calloc(1, sizeof *c);
    if (!c) return NULL;
    pthread_mutex_init(&c->lock, NULL);
    c->status = NET_CONNECTING;
    c->queue = dispatch_queue_create("openrackem.net", DISPATCH_QUEUE_SERIAL);

    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    nw_endpoint_t endpoint = nw_endpoint_create_host(host, portstr);

    // TLS (or disabled) + TCP defaults; WebSocket v13 prepended to the stack.
    nw_parameters_t params = nw_parameters_create_secure_tcp(
        tls ? NW_PARAMETERS_DEFAULT_CONFIGURATION : NW_PARAMETERS_DISABLE_PROTOCOL,
        NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_protocol_options_t ws = nw_ws_create_options(nw_ws_version_13);
    nw_ws_options_set_auto_reply_ping(ws, true);
    nw_protocol_stack_t stack = nw_parameters_copy_default_protocol_stack(params);
    nw_protocol_stack_prepend_application_protocol(stack, ws);
    nw_release(stack);
    nw_release(ws);

    c->conn = nw_connection_create(endpoint, params);
    nw_release(endpoint);
    nw_release(params);
    if (!c->conn) {
        dispatch_release(c->queue);
        pthread_mutex_destroy(&c->lock);
        free(c);
        return NULL;
    }

    nw_connection_set_queue(c->conn, c->queue);
    nw_connection_set_state_changed_handler(c->conn,
        ^(nw_connection_state_t state, nw_error_t error) {
        (void)error;
        switch (state) {
        case nw_connection_state_ready:
            set_status(c, NET_OPEN);
            arm_receive(c);
            break;
        case nw_connection_state_failed:
            set_status(c, NET_ERROR);
            break;
        case nw_connection_state_cancelled:
            set_status(c, NET_CLOSED);
            break;
        default:   // invalid / waiting / preparing
            break;
        }
    });
    nw_connection_start(c->conn);
    return c;
}

void net_poll(NetConn* c) { (void)c; }   // status/messages are callback-driven

NetStatus net_status(const NetConn* c) {
    if (!c) return NET_ERROR;
    pthread_mutex_lock((pthread_mutex_t*)&c->lock);
    NetStatus s = c->status;
    pthread_mutex_unlock((pthread_mutex_t*)&c->lock);
    return s;
}

bool net_send(NetConn* c, const char* text, size_t len) {
    if (!c || net_status(c) != NET_OPEN) return false;
    nw_protocol_metadata_t meta = nw_ws_create_metadata(nw_ws_opcode_text);
    nw_content_context_t ctx = nw_content_context_create("send");
    nw_content_context_set_metadata_for_protocol(ctx, meta);
    dispatch_data_t data = dispatch_data_create(text, len, c->queue,
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);   // copies the bytes
    nw_connection_send(c->conn, data, ctx, true /*is_complete*/,
        ^(nw_error_t error) { if (error) set_status(c, NET_ERROR); });
    dispatch_release(data);
    nw_release(ctx);
    nw_release(meta);
    return true;
}

int net_recv(NetConn* c, char* buf, size_t cap) {
    if (!c) return -1;
    pthread_mutex_lock(&c->lock);
    int ret = -1;
    if (c->q_head != c->q_tail) {
        int len = c->msgq_len[c->q_head];
        size_t n = ((size_t)len < cap - 1) ? (size_t)len : cap - 1;
        memcpy(buf, c->msgq[c->q_head], n);
        buf[n] = '\0';
        c->q_head = (c->q_head + 1) % AP_MSGQ;
        ret = (int)n;
    }
    pthread_mutex_unlock(&c->lock);
    return ret;
}

void net_close(NetConn* c) {
    if (!c) return;
    if (c->conn) {
        nw_connection_cancel(c->conn);
        // Barrier: drain any in-flight callback on the serial queue so nothing
        // touches c after we free it.
        dispatch_sync(c->queue, ^{});
        nw_release(c->conn);
        c->conn = NULL;
    }
    if (c->queue) dispatch_release(c->queue);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

bool net_available(void) { return true; }

#endif // PLATFORM_IOS || OR_NET_APPLE
