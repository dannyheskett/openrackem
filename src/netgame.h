#ifndef OPENRACKEM_NETGAME_H
#define OPENRACKEM_NETGAME_H

#include "game.h"
#include "net.h"
#include "input.h"

// The online session: owns a NetConn, speaks the daemon's protocol, and turns
// each authoritative `state` message back into a redacted `Game` the existing
// renderers draw unchanged. Local input produces the same `Action` values as
// offline play; instead of game_apply they are sent to the server, which
// answers with the next state. Pure aside from the socket — the JSON<->state
// mapping is unit-tested headlessly, and a full match is exercised over
// loopback against the real daemon.

typedef enum {
    NG_CONNECTING = 0, // TCP + WebSocket handshake
    NG_LOBBY,          // connected, choosing quick/create/join
    NG_QUEUED,         // in the quick-match queue
    NG_WAITING,        // in a created room, waiting for players
    NG_PLAYING,        // a match is live (game valid)
    NG_DISCONNECTED,   // link dropped; auto-rejoin in flight
    NG_ERROR,          // fatal (server told us to upgrade, etc.)
} NgState;

typedef enum {
    NG_JOIN_QUICK = 0, // quick match
    NG_JOIN_CREATE,    // create a private room
    NG_JOIN_CODE,      // join by code
} NgJoin;

typedef struct {
    NetConn* conn;
    NgState  state;
    char     err[32];      // short reason when state == NG_ERROR

    Game     game;         // reconstructed from the latest `state` (redacted)
    bool     have_game;    // a state has arrived at least once
    int      my_seat;
    char     code[8];      // room code (create/join)
    char     token[33];    // seat token, for reconnect
    char     handles[MAX_PLAYERS][16];  // seat display names (server-assigned or chosen)
    char     name[16];     // our chosen player name, sent on join (sanitized alnum)
    uint8_t  ai[MAX_PLAYERS];   // which seats are AI-controlled right now
    int      players;
    int      queue_pos;
    int      joined;       // seats filled in a waiting room
    int      deadline_ms;  // turn/reveal countdown from the last state

    // The last action anyone took, for the client-side flight animation and
    // sound (the renderers already read game.anim).
    bool     confirmed_local;   // we've sent `next` for the current round-over
    bool     pending;           // an action is in flight: don't re-send until
                                // the authoritative state (or a rejection) lands
    int      reconnect_delay;   // frames until the next auto-rejoin attempt

    // Session parameters, held per-instance so more than one session can run
    // in one process (the loopback e2e; the app only ever has one).
    NgJoin   join;
    Rules    rules;             // create parameters
    char     host[128];
    int      port;
    bool     tls;              // wss:// vs ws://
    bool     rejoining;         // reconnect in flight: send `rejoin`, not intent
    bool     greeted;           // we've sent our post-`hi` intent this connect
} NetGame;

// Open a session. `join` selects the flow; `code` is used for NG_JOIN_CODE.
// `rules` supplies create parameters (players/bonus/partners/target); ignored
// for quick/join. `name` is the chosen player name sent to the server (may be
// NULL/empty; sanitized to alphanumerics internally).
void netgame_start(NetGame* ng, const char* host, int port, bool tls,
                   NgJoin join, const char* code, const Rules* rules,
                   const char* name);

// One frame: pump the socket, apply any arrived state, advance the local
// animation clock. Returns the events word from a state that just arrived
// (for sound), or 0.
unsigned netgame_update(NetGame* ng);

// Send the local player's action for this turn (draw/place/discard) or the
// round-over confirmation. No-ops unless it is our turn / the right phase.
void netgame_action(NetGame* ng, Action a);
void netgame_confirm(NetGame* ng);   // `next` at round-over

// True when the local seat may act right now (its turn, draw/place phase).
bool netgame_my_turn(const NetGame* ng);

void netgame_close(NetGame* ng);

// Parse a server `state` message into `out` (redacted Game) plus the session
// side-channel fields. Returns true on success. Exposed for the unit tests.
bool netgame_parse_state(NetGame* ng, const char* json, size_t len);

#endif // OPENRACKEM_NETGAME_H
