#ifndef OPENRACKEM_GAME_H
#define OPENRACKEM_GAME_H

#include "rules.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Rules and state for a rack-sorting card game. Pure and allocation-free after
// create: no rendering, no raylib, no rand(). All randomness comes from the
// seeded xorshift64* state in Game.rng, so a match is fully reproducible from
// (seed, Rules, action list). The struct is a flat POD with no pointers; a
// snapshot is a memcpy (see GAME_SERIAL_SIZE).

#define RACK_SLOTS   10
#define MAX_PLAYERS   4
#define MAX_CARDS    60
#define NO_WINNER  0xFF

typedef struct {
    uint8_t slots[RACK_SLOTS];   // card values, index 0 = slot #5 ... 9 = slot #50
} Rack;

typedef struct {
    Rack rack;
    uint16_t score;              // running match score
    bool is_ai;
} Player;

typedef enum {
    PHASE_DEAL = 0,   // animated deal, no input accepted
    PHASE_DRAW,       // current player must draw (stock or discard)
    PHASE_PLACE,      // holding a drawn card: choose a slot, or discard it
    PHASE_ROUND_OVER, // scoring screen
    PHASE_MATCH_OVER,
} Phase;

// Transient events produced during a step, used to drive sound and UI cues.
// Cleared at the top of every game_update; game_apply ORs into them, so the
// frame loop accumulates across the steps it runs (as openblocks does).
enum {
    EV_DRAW      = 1 << 0, // a card was drawn (stock or discard)
    EV_PLACE     = 1 << 1, // an exchange: held card into a slot, old card out
    EV_DISCARD   = 1 << 2, // a stock-drawn card thrown away outright
    EV_TURN      = 1 << 3, // the next seat's turn began
    EV_RECYCLE   = 1 << 4, // the discard pile was turned over into a new stock
    EV_ROUND_END = 1 << 5, // round finished; round_winner / round_points valid
    EV_BONUS     = 1 << 6, // bonus variant points were awarded to the winner
    EV_MATCH_END = 1 << 7, // match decided; match_winner valid
};

// --- Presentation ----------------------------------------------------------
// Card-movement tweens, the AI think delay, and the round-end reveal timer.
// Advanced by game_update at 60 Hz. Never affects a rules outcome, and is
// excluded from the serialized form (it is the final field of Game).
typedef enum {
    ANIM_NONE = 0,
    ANIM_DRAW_STOCK,    // stock -> held card
    ANIM_DRAW_DISCARD,  // discard top -> held card
    ANIM_PLACE,         // held -> rack slot, displaced card -> discard
    ANIM_DISCARD,       // held -> discard
} AnimKind;

typedef struct {
    uint8_t  kind;      // AnimKind of the tween in flight (ANIM_NONE when idle)
    uint8_t  seat;      // seat that acted, for positioning the tween
    uint8_t  slot;      // rack slot involved (ANIM_PLACE)
    uint8_t  card;      // card value in flight (own seat only; opponents render backs)
    uint8_t  frames;    // frames remaining on the tween
    uint8_t  total;     // tween length, so renderers can interpolate frames/total
    uint16_t deal_step; // cards dealt so far in the deal animation
    uint16_t think;     // AI think-delay countdown, in frames
    uint16_t reveal;    // ROUND_OVER auto-advance countdown (full-AI games only)
} Anim;

// --- State -----------------------------------------------------------------
typedef struct {
    Rules   rules;                // normalized at create; never read from globals
    uint64_t rng;                 // xorshift64* state; the ONLY randomness source
    Player  players[MAX_PLAYERS];
    uint8_t stock[MAX_CARDS];     // stock[0] is the top
    uint8_t stock_count;
    uint8_t discard[MAX_CARDS];   // discard[discard_count-1] is the face-up top
    uint8_t discard_count;
    uint8_t dealer;
    uint8_t turn;                 // seat to act
    uint8_t phase;
    uint8_t held_card;            // valid in PHASE_PLACE, 0 = none
    bool    held_from_discard;    // if true, it MUST be placed (cannot be discarded)
    uint8_t recycles;             // stockpile turn-overs this round
    uint8_t round_winner;         // seat that went out; NO_WINNER = none (stalemate)
    uint8_t match_winner;         // seat on the winning side; NO_WINNER while live
    uint8_t round_no;             // 1-based, for the standings screen
    uint8_t turns_taken[MAX_PLAYERS];    // per-round turn counts (public info)
    uint8_t last_taken[MAX_PLAYERS];     // last card each seat took off the discard
                                         // pile this round, 0 = none (public info)
    uint16_t round_points[MAX_PLAYERS];  // what each seat scored last round
    unsigned events;              // EV_* flags for this step, cleared each step
    /* presentation-only, excluded from the serialized form */
    Anim    anim;
} Game;

// The serializable prefix: everything up to the presentation sub-struct. A
// memcpy of this many bytes is a complete snapshot; a test enforces that a
// round-trip reproduces play exactly.
#define GAME_SERIAL_SIZE offsetof(Game, anim)

// --- Actions ---------------------------------------------------------------
// A turn is a sequence of validated actions. This is the interface the v2
// server will speak: game_apply never trusts its input and never partially
// mutates on rejection.
typedef enum {
    ACTION_DRAW_STOCK,
    ACTION_DRAW_DISCARD,
    ACTION_PLACE,     // slot 0..9; the displaced card is discarded
    ACTION_DISCARD,   // throw the held card away (illegal if held_from_discard)
} ActionType;

typedef struct { uint8_t type; uint8_t slot; } Action;

Game* game_create(const Rules* rules);
void  game_destroy(Game* game);

bool game_action_legal(const Game* g, Action a);
bool game_apply(Game* g, Action a);          // false = rejected, state untouched
void game_update(Game* g);                   // one 60 Hz step: animation + AI pacing

// From PHASE_ROUND_OVER: deal the next round, or enter PHASE_MATCH_OVER when
// the match was decided. Called by the frame loop on player confirmation (or by
// the reveal timer in full-AI games).
void game_next_round(Game* g);

bool game_is_over(const Game* g);            // true once the match is decided

// --- Scoring (pure, separately testable) -----------------------------------
int  score_sequence(const Rack* r);          // 5 per ascending card from slot #5
int  score_longest_run(const Rack* r);       // length of longest consecutive run
int  score_bonus(int run_len);               // 0 / 50 / 100 / 200 / 400
bool rack_is_out(const Rack* r);             // strictly ascending, all ten

// --- Redacted view ---------------------------------------------------------
// Everything a seat is allowed to know: its own rack, the public table, and
// public per-opponent facts. Hidden information (other racks, the face-down
// stock order) is absent by construction — the same function that feeds the
// local AI will feed a remote client in v2.
typedef struct {
    Rules   rules;
    Rack    own_rack;
    uint8_t top_discard;              // 0 only if the pile is empty (never in play)
    uint8_t stock_count;
    uint8_t seat, player_count;
    uint8_t held_card;                // 0 = none; nonzero = deciding a placement
    uint8_t held_from_discard;
    uint8_t discard_history[MAX_CARDS];  // current pile, bottom to top: every card
    uint8_t discard_history_count;       // landed face up in full view (public)
    uint8_t opponent_out_risk[MAX_PLAYERS]; // turns each seat has taken (public)
    uint8_t last_taken[MAX_PLAYERS];        // last card each seat took off the
                                            // pile, 0 = none (public)
} GameView;

GameView game_view_for(const Game* g, int seat);   // hides all other racks

#endif // OPENRACKEM_GAME_H
