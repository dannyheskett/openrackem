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
//
// Persistence: with $OR_DATA_DIR set (the Fly volume, /data), the core's
// snapshot — private rooms and every live match — is loaded at boot and
// rewritten (tmp + rename) at most every SNAP_EVERY_MS while anything changed,
// and once more on SIGTERM/SIGINT, so a deploy or restart doesn't end games.
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
#define OUT_CAP  32768    // a room list plus a burst of states; overflow = close
#define TICK_MS  250      // core tick cadence (well under every core deadline)
#define SNAP_EVERY_MS 5000  // snapshot spacing while the core is dirty

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
static volatile sig_atomic_t s_stop = 0;
static uint8_t* s_snap;           // snapshot buffer, sized once at startup
static size_t   s_snap_cap;
static char     s_snap_path[512]; // "" = persistence off
static char     s_snap_tmp[520];

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
    char body[256];
    srv_status_json(s_core, body, sizeof body, now);
    char resp[448];
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

// --- Persistence ----------------------------------------------------------------
static void on_signal(int sig) { (void)sig; s_stop = 1; }

static bool write_all(int fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void snapshot_save(int64_t now) {
    if (!s_snap_path[0]) return;
    size_t len = srv_snapshot(s_core, s_snap, s_snap_cap, now);
    if (len == 0) { printf("snapshot: buffer too small\n"); return; }
    int fd = open(s_snap_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("snapshot open"); return; }
    bool ok = write_all(fd, s_snap, len) && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(s_snap_tmp, s_snap_path) != 0) {
        perror("snapshot write");
        unlink(s_snap_tmp);
    }
}

static void snapshot_load(int64_t now) {
    if (!s_snap_path[0]) return;
    int fd = open(s_snap_path, O_RDONLY);
    if (fd < 0) { printf("snapshot: none at %s\n", s_snap_path); return; }
    size_t len = 0;
    for (;;) {
        if (len == s_snap_cap) { len = 0; break; }   // larger than any valid image
        ssize_t r = read(fd, s_snap + len, s_snap_cap - len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        len += (size_t)r;
    }
    close(fd);
    if (!srv_restore(s_core, s_snap, len, now)) printf("snapshot: discarded\n");
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

    const char* dir = getenv("OR_DATA_DIR");
    if (dir && dir[0]) {
        snprintf(s_snap_path, sizeof s_snap_path, "%s/rooms.dat", dir);
        snprintf(s_snap_tmp, sizeof s_snap_tmp, "%s.tmp", s_snap_path);
        s_snap_cap = srv_snapshot_cap();
        s_snap = malloc(s_snap_cap);
        if (!s_snap) { perror("snapshot buffer"); return 1; }
        snapshot_load(now_ms());
    }
    struct sigaction sa_stop = {0};
    sa_stop.sa_handler = on_signal;
    sigaction(SIGTERM, &sa_stop, NULL);
    sigaction(SIGINT, &sa_stop, NULL);

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
    int64_t last_snap = last_tick;
    struct epoll_event evs[128];
    for (;;) {
        int n = epoll_wait(s_epfd, evs, 128, TICK_MS);
        if (n < 0) n = 0;   // EINTR (a signal): fall through to the stop check
        int64_t now = now_ms();
        if (s_stop) {
            snapshot_save(now);
            printf("orserverd: stopping (snapshot %s)\n", s_snap_path[0] ? "saved" : "off");
            return 0;
        }

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

        if (now - last_snap >= SNAP_EVERY_MS) {
            last_snap = now;
            if (srv_dirty(s_core)) snapshot_save(now);
        }
    }
}
