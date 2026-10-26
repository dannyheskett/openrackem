#ifndef OPENRACKEM_SERVER_CORE_H
#define OPENRACKEM_SERVER_CORE_H

#include "game.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The game-pool core: public and private rooms, waiting rooms with ready-up,
// the live room list, member keys, turn clocks, the leave policy (wait, then
// AI until the player returns), per-seat redacted state broadcast, and the
// snapshot that lets private rooms and live matches survive a restart.
// Deliberately socket-free and clock-free — the transport (orserverd.c) feeds
// it connections, messages, and a millisecond timestamp, and it answers
// through the SrvIo callbacks — so every flow runs as a deterministic unit
// test in tests/test_server.c.
//
// Scale envelope (a tiny fly.io instance): everything below is a fixed pool.
// Hot paths are O(1) or a walk of one room; the tick walks the room table.

#define SRV_PROTO_VERSION 2
#define SRV_MAX_CLIENTS   2048
#define SRV_MAX_ROOMS     4096
#define SRV_LIST_SHOW     20      // public rooms shown per viewer
#define SRV_CODE_LEN      4       // private room code length

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
#ifndef SRV_AWAY_WAIT_MS
#define SRV_AWAY_WAIT_MS  30000   // an absent player's turn: wait, then AI
#endif
#ifndef SRV_COUNTDOWN_MS
#define SRV_COUNTDOWN_MS  2000    // everyone ready -> "Starting..." -> deal
#endif
#ifndef SRV_IDLE_READY_MS
#define SRV_IDLE_READY_MS 60000   // others ready this long: idle player handled
#endif
#ifndef SRV_LOBBY_GRACE_MS
#define SRV_LOBBY_GRACE_MS 30000  // a dropped waiting-room member is removed
#endif
#ifndef SRV_ABANDON_MS
#define SRV_ABANDON_MS    300000  // public match with nobody present: closed
#endif
#ifndef SRV_PRIVATE_TTL_MS
#define SRV_PRIVATE_TTL_MS (30LL * 24 * 3600 * 1000)  // private room life
#endif
#ifndef SRV_SILENCE_MS
#define SRV_SILENCE_MS    25000   // no message (clients ping every 10 s): drop
#endif
#ifndef SRV_LIST_PUSH_MS
#define SRV_LIST_PUSH_MS  2000    // room-list update spacing per viewer
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

// --- Persistence ------------------------------------------------------------
// Private rooms and every live match, as a versioned byte image. The core
// does no I/O: the daemon writes this buffer to its volume and hands it back
// at boot. srv_dirty() reports whether anything worth saving changed since
// the last snapshot.
size_t srv_snapshot_cap(void);   // bytes a full snapshot can need
size_t srv_snapshot(Srv* s, uint8_t* buf, size_t cap, int64_t now_ms);
bool   srv_restore(Srv* s, const uint8_t* buf, size_t len, int64_t now_ms);
bool   srv_dirty(const Srv* s);

#endif // OPENRACKEM_SERVER_CORE_H
