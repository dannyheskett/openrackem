// orserverd — the openrackem multiplayer daemon (Linux only).
//
// A thin epoll transport around server_core.c: accepts sockets, speaks the
// ws.c WebSocket subset, answers GET /status for the fly.io health check, and
// feeds everything else to the core with a monotonic millisecond clock.
// Single-threaded, static pools, no allocation after startup — sized so the
// whole process idles in a few tens of MB on a shared-cpu-1x machine.
//
// fly.io terminates TLS at its edge; this listens on plain TCP ($PORT,
// default 8080) and trusts the Fly-Client-IP header for rate limiting.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "server_core.h"
#include "ws.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define IN_CAP   4096     // handshake headers / one batch of client frames
#define OUT_CAP  16384    // ~a dozen queued state broadcasts; overflow = close
#define TICK_MS  250      // core tick cadence (well under every core deadline)

typedef struct {
    int    fd;            // -1 = free slot
    bool   ws;            // handshake complete; the core knows this client
    bool   kill;          // deferred close (swept after each event batch)
    size_t in_len;
    size_t out_len;
    char   ip[46];
    uint8_t in[IN_CAP];
    uint8_t out[OUT_CAP];
} Conn;

static Conn s_conns[SRV_MAX_CLIENTS];
static int  s_epfd = -1;
static Srv* s_core;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void epoll_update(int idx) {
    Conn* c = &s_conns[idx];
    struct epoll_event ev = {0};
    ev.data.u32 = (uint32_t)idx;
    ev.events = EPOLLIN | (c->out_len ? EPOLLOUT : 0);
    epoll_ctl(s_epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

// Hard close. Reports to the core only if the handshake had completed.
static void close_conn(int idx, int64_t now) {
    Conn* c = &s_conns[idx];
    if (c->fd < 0) return;
    epoll_ctl(s_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    bool was_ws = c->ws;
    memset(c, 0, sizeof *c);
    c->fd = -1;
    if (was_ws) srv_client_gone(s_core, idx, now);
}

// Append raw bytes to the out buffer; a client too slow to drain OUT_CAP is
// beyond saving and gets closed (a seated player just reconnects via token).
static void out_append(int idx, const uint8_t* data, size_t len) {
    Conn* c = &s_conns[idx];
    if (c->fd < 0) return;
    if (c->out_len + len > OUT_CAP) {
        c->kill = true;
        return;
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
}

static void flush_conn(int idx) {
    Conn* c = &s_conns[idx];
    if (c->fd < 0 || c->out_len == 0) return;
    ssize_t n = write(c->fd, c->out, c->out_len);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c->kill = true;
        return;
    }
    memmove(c->out, c->out + n, c->out_len - (size_t)n);
    c->out_len -= (size_t)n;
    epoll_update(idx);
}

// --- Core -> transport callbacks --------------------------------------------
static void io_send(void* ud, int client, const char* text, size_t len) {
    (void)ud;
    if (client < 0 || client >= SRV_MAX_CLIENTS) return;
    Conn* c = &s_conns[client];
    if (c->fd < 0 || !c->ws) return;
    uint8_t hdr[4];
    size_t hlen = ws_encode_header(hdr, WS_TEXT, len);
    out_append(client, hdr, hlen);
    out_append(client, (const uint8_t*)text, len);
    flush_conn(client);
}

static void io_kick(void* ud, int client) {
    (void)ud;
    if (client >= 0 && client < SRV_MAX_CLIENTS && s_conns[client].fd >= 0) {
        s_conns[client].kill = true;   // swept after the event batch: no
    }                                  // reentrancy into the core mid-message
}

static void io_log(void* ud, const char* line) {
    (void)ud;
    printf("%s\n", line);
    fflush(stdout);
}

// --- HTTP ---------------------------------------------------------------------
// Case-insensitive single-header lookup within the handshake bytes.
static bool http_header(const char* req, size_t len, const char* name,
                        char* out, size_t cap) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i + nlen + 1 < len; i++) {
        if (i != 0 && req[i - 1] != '\n') continue;
        size_t j = 0;
        while (j < nlen) {
            char a = req[i + j], b = name[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != b) break;
            j++;
        }
        if (j != nlen || req[i + nlen] != ':') continue;
        size_t v = i + nlen + 1;
        while (v < len && (req[v] == ' ' || req[v] == '\t')) v++;
        size_t e = v;
        while (e < len && req[e] != '\r' && req[e] != '\n') e++;
        if (e - v == 0 || e - v >= cap) return false;
        memcpy(out, req + v, e - v);
        out[e - v] = '\0';
        return true;
    }
    return false;
}

static void send_status(int idx, int64_t now) {
    char body[192];
    srv_status_json(s_core, body, sizeof body, now);
    char resp[384];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     strlen(body), body);
    if (n > 0) out_append(idx, (const uint8_t*)resp, (size_t)n);
    flush_conn(idx);
    s_conns[idx].kill = true;
}

// --- Per-connection input ----------------------------------------------------
static void handle_readable(int idx, int64_t now) {
    Conn* c = &s_conns[idx];
    for (;;) {
        if (c->in_len == IN_CAP) { c->kill = true; return; }   // header/frame too big
        ssize_t n = read(c->fd, c->in + c->in_len, IN_CAP - c->in_len);
        if (n == 0) { c->kill = true; return; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            c->kill = true;
            return;
        }
        c->in_len += (size_t)n;
    }

    if (!c->ws) {
        // Accumulate until the blank line ends the HTTP request.
        const char* req = (const char*)c->in;
        const char* end = NULL;
        for (size_t i = 0; i + 3 < c->in_len; i++) {
            if (memcmp(c->in + i, "\r\n\r\n", 4) == 0) { end = req + i + 4; break; }
        }
        if (!end) return;
        size_t req_len = (size_t)(end - req);

        if (c->in_len >= 12 && memcmp(c->in, "GET /status", 11) == 0) {
            send_status(idx, now);
            return;
        }

        char resp[256];
        int rn = ws_handshake(req, req_len, resp, sizeof resp);
        if (rn < 0) { c->kill = true; return; }

        // Prefer the edge proxy's idea of the client address.
        char ip[46];
        if (http_header(req, req_len, "fly-client-ip", ip, sizeof ip)) {
            snprintf(c->ip, sizeof c->ip, "%s", ip);
        }

        out_append(idx, (const uint8_t*)resp, (size_t)rn);
        flush_conn(idx);
        if (c->kill) return;

        // A first frame may already trail the headers in the same segment.
        memmove(c->in, end, c->in_len - req_len);
        c->in_len -= req_len;
        c->ws = true;
        srv_client_connected(s_core, idx, c->ip);
    }

    // Decode as many complete frames as the buffer holds.
    while (c->ws && !c->kill) {
        int opcode;
        uint8_t* payload;
        size_t plen;
        int used = ws_decode(c->in, c->in_len, &opcode, &payload, &plen);
        if (used == 0) break;
        if (used < 0) { c->kill = true; return; }

        if (opcode == WS_TEXT) {
            srv_client_msg(s_core, idx, (const char*)payload, plen, now);
        } else if (opcode == WS_PING) {
            uint8_t hdr[4];
            size_t hlen = ws_encode_header(hdr, WS_PONG, plen);
            out_append(idx, hdr, hlen);
            out_append(idx, payload, plen);
            flush_conn(idx);
        } else if (opcode == WS_CLOSE) {
            uint8_t hdr[4];
            size_t hlen = ws_encode_header(hdr, WS_CLOSE, 0);
            out_append(idx, hdr, hlen);
            flush_conn(idx);
            c->kill = true;
            return;
        } // WS_PONG: ignored

        memmove(c->in, c->in + used, c->in_len - (size_t)used);
        c->in_len -= (size_t)used;
    }
}

// --- main ---------------------------------------------------------------------
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    for (int i = 0; i < SRV_MAX_CLIENTS; i++) s_conns[i].fd = -1;

    uint64_t seed = 0;
    if (getrandom(&seed, sizeof seed, 0) != (ssize_t)sizeof seed || seed == 0) {
        seed = (uint64_t)time(NULL) * 2654435761u + (uint64_t)getpid();
    }
    SrvIo io = {io_send, io_kick, io_log, NULL};
    s_core = srv_create(&io, seed);

    int port = 8080;
    const char* pe = getenv("PORT");
    if (pe && atoi(pe) > 0) port = atoi(pe);

    int lfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int one = 1, zero = 0;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero); // dual-stack
    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr*)&addr, sizeof addr) != 0) { perror("bind"); return 1; }
    if (listen(lfd, 256) != 0) { perror("listen"); return 1; }
    set_nonblock(lfd);

    s_epfd = epoll_create1(0);
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.u32 = UINT32_MAX;   // the listener
    epoll_ctl(s_epfd, EPOLL_CTL_ADD, lfd, &ev);

    printf("orserverd listening on :%d (proto v%d, %d rooms, %d clients)\n",
           port, SRV_PROTO_VERSION, SRV_MAX_ROOMS, SRV_MAX_CLIENTS);
    fflush(stdout);

    int64_t last_tick = now_ms();
    struct epoll_event evs[128];
    for (;;) {
        int n = epoll_wait(s_epfd, evs, 128, TICK_MS);
        int64_t now = now_ms();

        for (int e = 0; e < n; e++) {
            if (evs[e].data.u32 == UINT32_MAX) {
                for (;;) {
                    struct sockaddr_storage sa;
                    socklen_t sl = sizeof sa;
                    int fd = accept(lfd, (struct sockaddr*)&sa, &sl);
                    if (fd < 0) break;
                    int slot = -1;
                    for (int i = 0; i < SRV_MAX_CLIENTS; i++) {
                        if (s_conns[i].fd < 0) { slot = i; break; }
                    }
                    if (slot < 0) { close(fd); continue; }
                    set_nonblock(fd);
                    Conn* c = &s_conns[slot];
                    memset(c, 0, sizeof *c);
                    c->fd = fd;
                    // Peer address as the fallback IP (Fly-Client-IP wins later).
                    if (sa.ss_family == AF_INET6) {
                        inet_ntop(AF_INET6, &((struct sockaddr_in6*)&sa)->sin6_addr,
                                  c->ip, sizeof c->ip);
                    } else {
                        inet_ntop(AF_INET, &((struct sockaddr_in*)&sa)->sin_addr,
                                  c->ip, sizeof c->ip);
                    }
                    struct epoll_event cev = {0};
                    cev.events = EPOLLIN;
                    cev.data.u32 = (uint32_t)slot;
                    epoll_ctl(s_epfd, EPOLL_CTL_ADD, fd, &cev);
                }
                continue;
            }

            int idx = (int)evs[e].data.u32;
            Conn* c = &s_conns[idx];
            if (c->fd < 0) continue;
            if (evs[e].events & (EPOLLHUP | EPOLLERR)) { c->kill = true; continue; }
            if (evs[e].events & EPOLLOUT) flush_conn(idx);
            if (evs[e].events & EPOLLIN) handle_readable(idx, now);
        }

        // Deferred closes: never mid-callback, so the core is not reentered.
        for (int i = 0; i < SRV_MAX_CLIENTS; i++) {
            if (s_conns[i].fd >= 0 && s_conns[i].kill) close_conn(i, now);
        }

        if (now - last_tick >= TICK_MS) {
            last_tick = now;
            srv_tick(s_core, now);
            for (int i = 0; i < SRV_MAX_CLIENTS; i++) {
                if (s_conns[i].fd >= 0 && s_conns[i].kill) close_conn(i, now);
            }
        }
    }
}
