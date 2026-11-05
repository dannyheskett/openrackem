#ifndef OPENRACKEM_NETGAME_H
#define OPENRACKEM_NETGAME_H

#include "game.h"
#include "net.h"
#include "input.h"

// The online session: owns a NetConn, speaks the daemon's protocol (v3: one
// kind of room, listed while it waits), and turns each authoritative message
// into plain data the screens draw — the room list, the waiting room, and a
// redacted `Game` the existing renderers draw unchanged. Local input produces
// the same `Action` values as offline play; instead of game_apply they are
// sent to the server, which answers with the next state. Pure aside from the
// socket — the JSON -> data mapping is unit-tested headlessly, and full
// matches are exercised over loopback against the real daemon.
//
// One connection lives for the whole visit to Play Online: browse, enter a
// waiting room, play, return to the waiting room, leave, browse again.

typedef enum {
    NG_CONNECTING = 0, // TCP + WebSocket handshake + hello
    NG_IDLE,           // greeted, not in a room: the room list
    NG_LOBBY,          // in a room's waiting room
    NG_PLAYING,        // a match is live (game valid)
    NG_DISCONNECTED,   // link dropped; auto-rejoin in flight
    NG_ERROR,          // fatal (see err)
} NgState;

#define NG_NAME     16
#define NG_KEY      33
#define NG_LIST_MAX 20
#define NG_PROTO    3

// What a new room is made with: the New Room screen's settings.
typedef struct {
    int  seats;                         // the room's capacity, 2..4
    int  target;                        // Play To
    bool bonus, partners;
} NgRoomSetup;

// One room in the list.
typedef struct {
    long    id;
    int     n;                          // players in it
    int     seats;                      // its capacity (4 when the server doesn't say)
    char    names[MAX_PLAYERS][NG_NAME];
    uint8_t ready[MAX_PLAYERS];
    int     target;
    bool    bonus, partners;
} NgRoomRow;

// The waiting room we're in.
typedef struct {
    long    id;
    int     seats;                      // its capacity (4 when the server doesn't say)
    int     target;
    bool    bonus, partners;
    int     games;                      // matches this room has finished
    int     me;                         // our member index
    char    names[MAX_PLAYERS][NG_NAME];
    uint8_t used[MAX_PLAYERS], ready[MAX_PLAYERS], team[MAX_PLAYERS], on[MAX_PLAYERS];
    int     count_ms;                   // start countdown (-1 = none)
    char    block[12];                  // why it can't start ("" = starting)
    bool    can_force;                  // "Start without them" offered to us
} NgLobby;

typedef struct {
    NetConn* conn;
    NgState  state;
    char     err[32];      // short reason when state == NG_ERROR

    // --- In a room ---
    bool     in_room;
    long     room_id;
    char     key[NG_KEY];               // member key: rejoin by it
    NgLobby  lobby;
    bool     have_lobby;

    // --- Playing ---
    Game     game;         // reconstructed from the latest `state` (redacted)
    bool     have_game;    // a state has arrived at least once
    bool     have_result;  // back in the waiting room: `game` holds the
                           // finished match (phase MATCH_OVER) for the result line
    int      my_seat;
    int      players;
    char     handles[MAX_PLAYERS][NG_NAME];  // seat display names
    uint8_t  ctl[MAX_PLAYERS];  // per seat: 0 a human plays it, 1 away (AI or waiting)
    int      deadline_ms;  // turn/reveal countdown from the last state
    int      wait_seat;    // seat the table is waiting for (-1 none)
    int      wait_ms;      // ...remaining

    // --- Browsing ---
    NgRoomRow rooms[NG_LIST_MAX];
    int      room_count;
    int      rooms_total;
    bool     have_rooms;
    bool     listing;

    // --- Notices (shown briefly by the screens) ---
    char     notice[16];   // last transient error / removal reason
    int      notice_frames;
    char     back_name[NG_NAME];  // "X is back"
    int      back_frames;

    bool     joining;           // a create/join/rejoin is in flight: a second
                                // tap is ignored until the server answers
    bool     confirmed_local;   // we've sent `next` for the current round-over
    bool     pending;           // an action is in flight: don't re-send until
                                // the authoritative state (or a rejection) lands
    int      reconnect_delay;   // frames until the next auto-rejoin attempt
    int      ping_frames;       // frames until the next keep-alive ping

    // Session parameters, held per-instance so more than one session can run
    // in one process (the loopback tests; the app only ever has one).
    char     name[NG_NAME];     // our player name (sanitized alnum)
    char     stale_key[NG_KEY]; // a seat held elsewhere: surrendered on join
    char     host[128];
    int      port;
    bool     tls;
    bool     greeted;           // we've sent hello this connect
    bool     rejoining;         // reconnect in flight: send `rejoin` after hi
} NetGame;

// Open a session and say hello with `name` (sanitized to alphanumerics).
// With `rejoin_key` set, the session rejoins that seat as soon as it is
// greeted (Return to Game); otherwise it lands in NG_IDLE.
void netgame_start(NetGame* ng, const char* host, int port, bool tls,
                   const char* name, const char* rejoin_key);

// One frame: pump the socket, apply arrived messages, advance the local
// animation clock, keep the link alive, auto-rejoin after a drop. Returns
// the events word from a state that just arrived (for sound), or 0.
unsigned netgame_update(NetGame* ng);

// Run the countdowns the screens show (turn clock, absent-player wait,
// waiting-room start) down by `ms` of real time; the next server message
// re-syncs them. Kept separate so netgame needs no clock of its own.
void netgame_elapse(NetGame* ng, int ms);

// A room's settings: the defaults (four seats, play to 500, no variants), and
// any values brought into what the server accepts — seats 2..4, a valid
// target, and Partners off below four seats.
NgRoomSetup netgame_room_default(void);
void netgame_room_normalize(NgRoomSetup* rs);

// Browsing (NG_IDLE).
void netgame_list(NetGame* ng);       // subscribe to the room list
void netgame_unlist(NetGame* ng);
void netgame_create(NetGame* ng, const NgRoomSetup* setup);   // NULL = the defaults
void netgame_join_room(NetGame* ng, long id);
void netgame_rejoin(NetGame* ng, const char* key);   // Return to Game

// Waiting room (NG_LOBBY).
void netgame_ready(NetGame* ng, bool on);
void netgame_team(NetGame* ng);       // Partners: switch (or trade) teams
void netgame_force(NetGame* ng);      // Start without them

// Leave: in a waiting room, leave the room (back to NG_IDLE); in a match,
// step away (the seat is kept and the leave policy applies).
void netgame_leave(NetGame* ng);
void netgame_here(NetGame* ng);       // back from stepping away
void netgame_forfeit(NetGame* ng);    // give the seat up for good
void netgame_letai(NetGame* ng, int seat);

// Playing: the local player's action for this turn (draw/place/discard) or
// the round-over confirmation. No-ops unless it is our turn / right phase.
void netgame_action(NetGame* ng, Action a);
void netgame_confirm(NetGame* ng);   // `next` at round-over

// True when the local seat may act right now (its turn, draw/place phase).
bool netgame_my_turn(const NetGame* ng);

// Close the socket. Mid-match the seat is kept (stepped away first).
void netgame_close(NetGame* ng);

// Parsers, exposed for the unit tests. Each returns true on success.
bool netgame_parse_state(NetGame* ng, const char* json, size_t len);
bool netgame_parse_lobby(NetGame* ng, const char* json);
bool netgame_parse_rooms(NetGame* ng, const char* json);

#endif // OPENRACKEM_NETGAME_H
