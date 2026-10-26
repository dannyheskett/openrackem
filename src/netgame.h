#ifndef OPENRACKEM_NETGAME_H
#define OPENRACKEM_NETGAME_H

#include "game.h"
#include "net.h"
#include "input.h"

// The online session: owns a NetConn, speaks the daemon's protocol (v2:
// public and private rooms), and turns each authoritative message into plain
// data the screens draw — the room list, the waiting room, and a redacted
// `Game` the existing renderers draw unchanged. Local input produces the same
// `Action` values as offline play; instead of game_apply they are sent to the
// server, which answers with the next state. Pure aside from the socket — the
// JSON -> data mapping is unit-tested headlessly, and full matches are
// exercised over loopback against the real daemon.
//
// One connection lives for the whole visit to Play Online: browse, enter a
// waiting room, play, return to the waiting room, leave, browse again.

typedef enum {
    NG_CONNECTING = 0, // TCP + WebSocket handshake + hello
    NG_IDLE,           // greeted, not in a room: room list / code entry
    NG_LOBBY,          // in a room's waiting room
    NG_PLAYING,        // a match is live (game valid)
    NG_DISCONNECTED,   // link dropped; auto-rejoin in flight
    NG_ERROR,          // fatal (see err)
} NgState;

#define NG_NAME   16
#define NG_KEY    33
#define NG_CODE   8
#define NG_LIST_MAX 20

// One public room in the list.
typedef struct {
    long    id;
    int     n;                          // players in it
    char    names[MAX_PLAYERS][NG_NAME];
    uint8_t ready[MAX_PLAYERS];
    int     target;
    bool    bonus, partners;
} NgRoomRow;

// The waiting room we're in.
typedef struct {
    long    id;
    char    code[NG_CODE];              // private rooms ("" for public)
    bool    priv;
    int     target;
    bool    bonus, partners;
    int     away_policy;                // 0 AI after 30 s, 1 wait for them
    int     me;                         // our member index
    char    names[MAX_PLAYERS][NG_NAME];
    uint8_t used[MAX_PLAYERS], ready[MAX_PLAYERS], team[MAX_PLAYERS], on[MAX_PLAYERS];
    int     count_ms;                   // start countdown (-1 = none)
    char    block[12];                  // why it can't start ("" = starting)
    bool    can_force;                  // private: "Start without them" offered
    int     swap_from;                  // Partners: member asking to swap (-1)
} NgLobby;

// Live status of a private code (the saved room row).
typedef struct {
    char    code[NG_CODE];
    bool    known;                      // an answer has arrived for `code`
    bool    ok;                         // false = no such room (expired)
    bool    playing;
    int     target, n;
    char    names[MAX_PLAYERS][NG_NAME];
    uint8_t ready[MAX_PLAYERS];
} NgPeek;

typedef struct {
    NetConn* conn;
    NgState  state;
    char     err[32];      // short reason when state == NG_ERROR

    // --- In a room ---
    bool     in_room;
    long     room_id;
    char     code[NG_CODE];             // the room's private code ("" public)
    char     key[NG_KEY];               // member key: rejoin by it
    char     owner[NG_KEY];             // owner key when we created the room
    NgLobby  lobby;
    bool     have_lobby;

    // --- Playing ---
    Game     game;         // reconstructed from the latest `state` (redacted)
    bool     have_game;    // a state has arrived at least once
    bool     result_pending;  // match over: keep the result up until dismissed
    int      my_seat;
    int      players;
    char     handles[MAX_PLAYERS][NG_NAME];  // seat display names
    uint8_t  ctl[MAX_PLAYERS];  // per seat: 0 a human plays it, 1 away (AI or waiting)
    int      deadline_ms;  // turn/reveal countdown from the last state
    int      wait_seat;    // seat the table is waiting for (-1 none)
    int      wait_ms;      // ...remaining (when wait_timer)
    bool     wait_timer;   // false = waiting indefinitely (Wait policy)

    // --- Browsing ---
    NgRoomRow rooms[NG_LIST_MAX];
    int      room_count;
    int      rooms_total;
    bool     have_rooms;
    bool     listing;
    int      list_target;
    NgPeek   peek;

    // --- Notices (shown briefly by the screens) ---
    char     notice[16];   // last transient error / removal reason
    int      notice_frames;
    char     back_name[NG_NAME];  // "X is back"
    int      back_frames;

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

// Browsing (NG_IDLE).
void netgame_list(NetGame* ng, int target);      // subscribe (0 = any target)
void netgame_shuffle(NetGame* ng);
void netgame_unlist(NetGame* ng);
void netgame_peek(NetGame* ng, const char* code);
void netgame_create(NetGame* ng, bool priv, const Rules* rules, bool wait_policy);
void netgame_join_room(NetGame* ng, long id);
void netgame_join_code(NetGame* ng, const char* code);
void netgame_best(NetGame* ng, int target);
void netgame_rejoin(NetGame* ng, const char* key);   // Return to Game

// Waiting room (NG_LOBBY).
void netgame_ready(NetGame* ng, bool on);
void netgame_team(NetGame* ng, int side);
void netgame_swap(NetGame* ng, int member);
void netgame_swap_answer(NetGame* ng, int from, bool accept);
void netgame_force(NetGame* ng);
void netgame_settings(NetGame* ng, int target, bool bonus, bool partners,
                      bool priv, bool wait_policy);   // owner only

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
void netgame_dismiss_result(NetGame* ng);   // match-over screen tapped

// True when the local seat may act right now (its turn, draw/place phase).
bool netgame_my_turn(const NetGame* ng);

// Close the socket. Mid-match the seat is kept (stepped away first).
void netgame_close(NetGame* ng);

// Parsers, exposed for the unit tests. Each returns true on success.
bool netgame_parse_state(NetGame* ng, const char* json, size_t len);
bool netgame_parse_lobby(NetGame* ng, const char* json);
bool netgame_parse_rooms(NetGame* ng, const char* json);

#endif // OPENRACKEM_NETGAME_H
