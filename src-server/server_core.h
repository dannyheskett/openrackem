#ifndef OPENRACKEM_SERVER_CORE_H
#define OPENRACKEM_SERVER_CORE_H

#include "game.h"
#include <stddef.h>
#include <stdint.h>

// The game-pool core: rooms, seats, tokens, the matchmaking queue, turn
// clocks, AI takeover, and per-seat redacted state broadcast. Deliberately
// socket-free and clock-free — the transport (orserverd.c) feeds it
// connections, messages, and a millisecond timestamp, and it answers through
// the SrvIo callbacks — so full matches, timeouts, disconnects, and
// matchmaking run as deterministic unit tests in tests/test_server.c.
//
// Scale envelope (a tiny fly.io instance): everything below is a fixed pool,
// sized so the whole core is a few MB. Hot paths are O(1); only the 1 Hz
// tick walks the room table.

#define SRV_PROTO_VERSION 1
#define SRV_MAX_CLIENTS   2048
#define SRV_MAX_ROOMS     1024

// Pacing (ms). Overridable for tests.
#ifndef SRV_TURN_MS
#define SRV_TURN_MS       45000   // human decision clock (draw and place each)
#endif
#ifndef SRV_AI_PACE_MS
#define SRV_AI_PACE_MS    1000    // AI move spacing while humans are watching
#endif
#ifndef SRV_REVEAL_MS
#define SRV_REVEAL_MS     15000   // round-over auto-advance
#endif
#ifndef SRV_QUEUE_PAIR_MS
#define SRV_QUEUE_PAIR_MS 15000   // quick match: launch with 2-3 humans + AI
#endif
#ifndef SRV_QUEUE_SOLO_MS
#define SRV_QUEUE_SOLO_MS 30000   // quick match: launch solo vs 3 AI
#endif
#ifndef SRV_ABANDON_MS
#define SRV_ABANDON_MS    120000  // no human connected this long: close room
#endif

// Transport interface. `send` delivers one text message to a live client;
// `kick` asks the transport to close a client (the transport then reports it
// back through srv_client_gone); `log` emits one ops/audit line (on fly.io
// that is stdout).
typedef struct {
    void (*send)(void* ud, int client, const char* text, size_t len);
    void (*kick)(void* ud, int client);
    void (*log)(void* ud, const char* line);
    void* ud;
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

#endif // OPENRACKEM_SERVER_CORE_H
