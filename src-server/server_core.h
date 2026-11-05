#ifndef OPENRACKEM_SERVER_CORE_H
#define OPENRACKEM_SERVER_CORE_H

#include "game.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The game-pool core: rooms (one kind — listed while waiting, joinable while
// not full, gone when empty), waiting rooms with ready-up, the live room list,
// member keys, turn clocks, the leave policy (wait, then AI until the player
// returns), per-seat redacted state broadcast, and the snapshot that lets
// occupied rooms and live matches survive a restart. Deliberately socket-free
// and clock-free — the transport (orserverd.c) feeds it connections, messages,
// and a millisecond timestamp, and it answers through the SrvIo callbacks — so
// every flow runs as a deterministic unit test in tests/test_server.c.
//
// Scale envelope (a tiny fly.io instance): everything below is a fixed pool.
// Hot paths are O(1) or a walk of one room; the tick walks the room table.

#define SRV_PROTO_VERSION 3
#define SRV_MAX_CLIENTS   2048
#define SRV_MAX_ROOMS     4096
#define SRV_LIST_SHOW     20      // rooms shown in the list

// Pacing (ms). Overridable for tests.
#ifndef SRV_TURN_MS
#define SRV_TURN_MS       45000   // human decision clock (draw and place each)
#endif
#ifndef SRV_AI_PACE_MS
#define SRV_AI_PACE_MS    1000    // AI move spacing while humans are watching
#endif
#ifndef SRV_REVEAL_MS
#define SRV_REVEAL_MS     30000   // round-over auto-advance backstop
#endif
#ifndef SRV_AWAY_WAIT_MS
#define SRV_AWAY_WAIT_MS  30000   // an absent player's turn: wait, then AI
#endif
#ifndef SRV_COUNTDOWN_MS
#define SRV_COUNTDOWN_MS  2000    // everyone ready -> "Starting..." -> deal
#endif
#ifndef SRV_IDLE_READY_MS
#define SRV_IDLE_READY_MS 60000   // others ready this long: Start without them
#endif
#ifndef SRV_LOBBY_GRACE_MS
#define SRV_LOBBY_GRACE_MS 120000 // a dropped waiting-room member keeps the seat
#endif
#ifndef SRV_ABANDON_MS
#define SRV_ABANDON_MS    300000  // match with nobody present: closed
#endif
#ifndef SRV_SILENCE_MS
#define SRV_SILENCE_MS    25000   // no message (clients ping every 10 s): drop
#endif
#ifndef SRV_LIST_PUSH_MS
#define SRV_LIST_PUSH_MS  2000    // room-list update spacing per viewer
#endif
#ifndef SRV_LOBBY_UNREADY_MS
#define SRV_LOBBY_UNREADY_MS 600000 // waiting room: not ready this long, moved out
#endif

// Connections one address may hold at once (a household or a classroom fits;
// one machine can no longer take every slot).
#ifndef SRV_IP_MAX_CONNS
#define SRV_IP_MAX_CONNS  32
#endif

// Transport interface. `send` delivers one text message to a live client;
// `kick` asks the transport to close a client (the transport then reports it
// back through srv_client_gone); `log` emits one ops/audit line (on fly.io
// that is stdout). `entropy` fills a buffer from the operating system's random
// source: member keys and match seeds are drawn from it, because either one
// reconstructs hidden state (a key is a seat; a seed is every card) and so
// must not be derivable from anything a client is ever sent. The daemon always
// provides it. Left NULL, the core falls back to its seeded generator, which
// is predictable and exists only so the unit tests are deterministic.
typedef struct {
    void (*send)(void* ud, int client, const char* text, size_t len);
    void (*kick)(void* ud, int client);
    void (*log)(void* ud, const char* line);
    void* ud;
    void (*entropy)(void* ud, uint8_t* out, size_t n);
} SrvIo;

typedef struct Srv Srv;

// The one instance (static storage inside; the daemon is single-threaded).
Srv* srv_create(const SrvIo* io, uint64_t rng_seed);

void srv_client_connected(Srv* s, int client, const char* ip);
void srv_client_msg(Srv* s, int client, const char* text, size_t len, int64_t now_ms);
void srv_client_gone(Srv* s, int client, int64_t now_ms);
void srv_tick(Srv* s, int64_t now_ms);

// The /status health payload.
void srv_status_json(Srv* s, char* out, size_t cap, int64_t now_ms);

// --- Persistence ------------------------------------------------------------
// Every occupied room and every live match, as a versioned byte image. The
// core does no I/O: the daemon writes this buffer to its volume and hands it
// back at boot. srv_dirty() reports whether anything worth saving changed
// since the last snapshot.
size_t srv_snapshot_cap(void);   // bytes a full snapshot can need
size_t srv_snapshot(Srv* s, uint8_t* buf, size_t cap, int64_t now_ms);
bool   srv_restore(Srv* s, const uint8_t* buf, size_t len, int64_t now_ms);
bool   srv_dirty(const Srv* s);

#endif // OPENRACKEM_SERVER_CORE_H
