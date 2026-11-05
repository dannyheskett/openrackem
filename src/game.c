// The rules engine: deck, deal, actions, legality, scoring, and round/match
// flow, exactly as docs/PLAN.md §3 specifies (the official rulebook is the
// test oracle). Pure and allocation-free after create — no rendering, no
// raylib, no rand(), no wall clock — so it compiles standalone with libc and a
// match replays byte-for-byte from (seed, Rules, action list).
#include "game.h"
#include "ai.h"
#include <string.h>

// --- Presentation pacing (frames at 60 Hz) ---------------------------------
// These shape how play *looks*, never how it resolves: the deal reveal rate,
// the card-slide tween, how long the AI "thinks", and how long a finished
// round lingers in full-AI games before auto-advancing. Overridable from the
// command line (-DTHINK_DRAW_FRAMES=2 ...) for fast dev/visual-test builds.
#ifndef DEAL_STEP_FRAMES
#define DEAL_STEP_FRAMES   2
#endif
#ifndef SLIDE_FRAMES
#define SLIDE_FRAMES      10
#endif
#ifndef THINK_DRAW_FRAMES
#define THINK_DRAW_FRAMES 42
#endif
#ifndef THINK_PLACE_FRAMES
#define THINK_PLACE_FRAMES 26
#endif
#ifndef REVEAL_FRAMES
#define REVEAL_FRAMES    150
#endif

// Engine safeguard on top of the plan's stalemate assumption: a round that
// somehow runs this many turns per seat without a recycle-triggered cutoff
// (e.g. AIs trading the discard pile back and forth forever) is called as a
// stalemate too, so a match can never wedge. Far beyond any real round, and it
// keeps the uint8 turn counters from wrapping.
#define TURN_CAP 200

// --- Deal randomness --------------------------------------------------------
// Everything a match deals — who deals first, every shuffle — is a function
// of its seed, so a match still replays byte-for-byte from (seed, Rules,
// action list). What matters is which function. A player is shown a good part
// of each shuffle's output (their own rack, the first discard, every card they
// draw), and online the shuffle must not be something that can be run
// backwards from that to the cards they were not shown. A small generator such as xorshift (the engine's first
// shuffle source) gives no such assurance: its state is 64 bits, every output
// is a simple function of it, and the visible cards of one round carry more
// than 64 bits about it.
//
// So the cards come from ChaCha20 (the block function of RFC 8439 §2.3, pinned
// to the RFC's test vector in tests/test_game.c), keyed by the seed. Each use
// gets its own keystream, named by a (purpose, deal number, sub-index) nonce,
// and nothing is carried from one to the next: the only state is the seed and
// the count of deals. Recovering the unseen cards now means searching the
// seed space, which at 64 bits is out of reach for a card game.
//
// Integer-only and libc-only like the rest of the engine. The repo vendors
// nothing, so this is the block function written out; it is used as a
// keystream generator and for nothing else.
static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

#define CHACHA_QR(a, b, c, d)                    \
    do {                                         \
        a += b; d ^= a; d = rotl32(d, 16);       \
        c += d; b ^= c; b = rotl32(b, 12);       \
        a += b; d ^= a; d = rotl32(d, 8);        \
        c += d; b ^= c; b = rotl32(b, 7);        \
    } while (0)

static void chacha20_block(const uint32_t key[8], uint32_t counter,
                           const uint32_t nonce[3], uint32_t out[16]) {
    uint32_t in[16] = {
        0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u,   // "expand 32-byte k"
        key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
        counter, nonce[0], nonce[1], nonce[2],
    };
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int round = 0; round < 10; round++) {   // 20 rounds, as column + diagonal pairs
        CHACHA_QR(x[0], x[4], x[8],  x[12]);
        CHACHA_QR(x[1], x[5], x[9],  x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);
        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[8],  x[13]);
        CHACHA_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

// What a keystream is for. Part of the nonce, so no two uses share one.
enum {
    DEAL_DEALER    = 1,   // who deals the first round
    DEAL_SHUFFLE   = 2,   // a round's deck
    DEAL_RESHUFFLE = 3,   // a turned-over stockpile (reshuffle_on_recycle)
    DEAL_AI        = 4,   // the starting state of the AI's own dice (Game.rng)
};

typedef struct {
    uint32_t key[8];
    uint32_t nonce[3];
    uint32_t counter;     // next block
    uint32_t block[16];
    int      used;        // words of `block` consumed (16 = none left)
} DealStream;

// The seed a match actually runs on: 0 means "unseeded" to the callers, and
// stands for one fixed value here so such a game is still reproducible.
static uint64_t match_seed(const Game* g) {
    return g->rules.seed ? g->rules.seed : 0x9E3779B97F4A7C15ULL;
}

static void deal_stream(DealStream* ds, uint64_t seed, uint32_t purpose,
                        uint32_t deal_no, uint32_t sub) {
    ds->key[0] = (uint32_t)seed;
    ds->key[1] = (uint32_t)(seed >> 32);
    ds->key[2] = 0x6e65706fu;   // "openrackem deal stream 1" fills the rest of
    ds->key[3] = 0x6b636172u;   // the key: a fixed label, so this keystream is
    ds->key[4] = 0x64206d65u;   // this engine's and no other use of the cipher
    ds->key[5] = 0x206c6165u;   // with the same 64 bits could collide with it
    ds->key[6] = 0x65727473u;
    ds->key[7] = 0x31206d61u;
    ds->nonce[0] = purpose;
    ds->nonce[1] = deal_no;
    ds->nonce[2] = sub;
    ds->counter = 0;
    ds->used = 16;
}

static uint64_t deal_next(DealStream* ds) {
    if (ds->used > 14) {
        chacha20_block(ds->key, ds->counter++, ds->nonce, ds->block);
        ds->used = 0;
    }
    uint64_t v = (uint64_t)ds->block[ds->used] | ((uint64_t)ds->block[ds->used + 1] << 32);
    ds->used += 2;
    return v;
}

// A uniform value in [0, n): draws above the largest multiple of n are thrown
// away rather than folded in, so no card position is favored even slightly.
// (The loop all but never repeats, and is deterministic when it does.)
static uint32_t deal_below(DealStream* ds, uint32_t n) {
    uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
    uint64_t v;
    do { v = deal_next(ds); } while (v >= limit);
    return (uint32_t)(v % n);
}

// --- Scoring (pure) --------------------------------------------------------
bool rack_is_out(const Rack* r) {
    for (int i = 1; i < RACK_SLOTS; i++) {
        if (r->slots[i] <= r->slots[i - 1]) return false;
    }
    return true;
}

int score_sequence(const Rack* r) {
    // 5 per card in ascending sequence starting at slot #5, stopping at the
    // first break. Cards past the break score nothing even if they happen to
    // be ordered; a #10 lower than the #5 scores the minimum 5.
    int n = 1;
    while (n < RACK_SLOTS && r->slots[n] > r->slots[n - 1]) n++;
    return 5 * n;
}

int score_longest_run(const Rack* r) {
    // Longest chain of consecutive numbers in adjacent slots (14-15-16 style).
    int best = 1, cur = 1;
    for (int i = 1; i < RACK_SLOTS; i++) {
        cur = (r->slots[i] == r->slots[i - 1] + 1) ? cur + 1 : 1;
        if (cur > best) best = cur;
    }
    return best;
}

int score_bonus(int run_len) {
    if (run_len >= 6) return 400;
    if (run_len == 5) return 200;
    if (run_len == 4) return 100;
    if (run_len == 3) return 50;
    return 0;
}

// --- Pile helpers ----------------------------------------------------------
static uint8_t pop_stock(Game* g) {
    uint8_t card = g->stock[0];
    g->stock_count--;
    memmove(g->stock, g->stock + 1, g->stock_count);
    return card;
}

static void push_discard(Game* g, uint8_t card) {
    g->discard[g->discard_count++] = card;
}

static void shuffle(DealStream* ds, uint8_t* cards, int count) {
    for (int i = count - 1; i > 0; i--) {
        uint32_t j = deal_below(ds, (uint32_t)(i + 1));
        uint8_t t = cards[i];
        cards[i] = cards[j];
        cards[j] = t;
    }
}

// Turn the discard pile over to form a new stockpile (official rule: no
// shuffle). The face-up top stays as the discard; the block beneath it flips,
// so the oldest discard ends up on top of the new stock — with stock[0] as the
// top, that is simply the pile in insertion order. A config flag reshuffles
// instead.
static void recycle_stock(Game* g) {
    uint8_t top = g->discard[g->discard_count - 1];
    g->stock_count = g->discard_count - 1;
    memcpy(g->stock, g->discard, g->stock_count);
    g->discard[0] = top;
    g->discard_count = 1;
    g->recycles++;
    if (g->rules.reshuffle_on_recycle) {
        // This round's deal number (deal_round has already counted it) and
        // which turn-over this is: a keystream of its own every time.
        DealStream ds;
        deal_stream(&ds, match_seed(g), DEAL_RESHUFFLE, g->deals, g->recycles);
        shuffle(&ds, g->stock, g->stock_count);
    }
    g->events |= EV_RECYCLE;
}

// --- Round / match flow ----------------------------------------------------
// Shuffle the deck subset for this player count and deal ten cards each, one
// at a time starting left of the dealer: the first card a player receives goes
// to slot #50 (index 9), the next to #45, down to #5. The remainder is the
// stockpile and its top card turns face up to start the discard pile.
static void deal_round(Game* g) {
    int n = g->rules.player_count;
    int deck = rules_deck_size(&g->rules);

    for (int i = 0; i < deck; i++) g->stock[i] = (uint8_t)(i + 1);
    g->stock_count = (uint8_t)deck;
    DealStream ds;
    deal_stream(&ds, match_seed(g), DEAL_SHUFFLE, g->deals, 0);
    g->deals++;
    shuffle(&ds, g->stock, deck);

    for (int slot = RACK_SLOTS - 1; slot >= 0; slot--) {
        for (int k = 0; k < n; k++) {
            int seat = (g->dealer + 1 + k) % n;
            g->players[seat].rack.slots[slot] = pop_stock(g);
        }
    }
    g->discard_count = 0;
    push_discard(g, pop_stock(g));

    g->turn = (uint8_t)((g->dealer + 1) % n);
    g->phase = PHASE_DEAL;
    g->held_card = 0;
    g->held_from_discard = false;
    g->recycles = 0;
    g->round_winner = NO_WINNER;
    memset(g->turns_taken, 0, sizeof g->turns_taken);
    memset(g->last_taken, 0, sizeof g->last_taken);

    memset(&g->anim, 0, sizeof g->anim);
    g->anim.frames = DEAL_STEP_FRAMES;
}

// Score the finished round and bank it. `winner` went out (NO_WINNER for a
// stalemate: everyone keeps base sequence points, nobody collects 75). Also
// decides the match: the player — or pair, under Partners — at or past the
// target with the highest total wins. A dead tie for the top decides nothing:
// the match plays another round until someone is clearly ahead, so no screen
// ever tells one of two equal totals "you win" and the other "you lose".
static void end_round(Game* g, int winner) {
    int n = g->rules.player_count;
    g->round_winner = (uint8_t)winner;

    for (int i = 0; i < n; i++) {
        int pts;
        if (i == winner) {
            pts = 75; // 5 per card, plus 25 for going out
            if (g->rules.bonus_scoring) {
                int bonus = score_bonus(score_longest_run(&g->players[i].rack));
                if (bonus > 0) {
                    pts += bonus;
                    g->events |= EV_BONUS;
                }
            }
        } else {
            pts = score_sequence(&g->players[i].rack);
        }
        g->round_points[i] = (uint16_t)pts;
        g->players[i].score = (uint16_t)(g->players[i].score + pts);
    }

    // Match decision on effective totals: individual scores, or summed pair
    // scores under Partners (seats 0+2 vs 1+3).
    int totals[MAX_PLAYERS];
    int sides = g->rules.partners ? 2 : n;
    for (int s = 0; s < sides; s++) {
        totals[s] = g->rules.partners
            ? g->players[s].score + g->players[s + 2].score
            : g->players[s].score;
    }
    int best = -1;
    bool tied = false;
    for (int s = 0; s < sides; s++) {
        if (totals[s] < g->rules.target_score) continue;
        if (best < 0 || totals[s] > totals[best]) {
            best = s;
            tied = false;
        } else if (totals[s] == totals[best]) {
            tied = true;
        }
    }
    if (best >= 0 && !tied) {
        g->match_winner = (uint8_t)best; // under Partners: pair index 0 or 1
        g->events |= EV_ROUND_END;       // the match banner shows after the
    } else {                             // round screens (game_next_round)
        g->events |= EV_ROUND_END;
    }

    g->phase = PHASE_ROUND_OVER;
    g->anim.kind = ANIM_NONE;
    g->anim.frames = 0;
    g->anim.reveal = REVEAL_FRAMES;
}

// A completed turn: count it, pass to the left, and pace the next AI. The turn
// cap is the wedge-proof backstop documented at the top of the file — and it
// is deliberately NOT gated on stalemate_cutoff: with the cutoff off, two
// deterministic AIs trading an unshuffled pile provably cycle forever (the
// whole state repeats exactly), and an infinite round is worse than a
// slightly-unofficial one. At 200 turns per seat no real round is affected.
static void advance_turn(Game* g) {
    int seat = g->turn;
    if (g->turns_taken[seat] < 255) g->turns_taken[seat]++;
    if (g->turns_taken[seat] >= TURN_CAP) {
        end_round(g, NO_WINNER);
        return;
    }
    g->turn = (uint8_t)((g->turn + 1) % g->rules.player_count);
    g->phase = PHASE_DRAW;
    g->events |= EV_TURN;
    g->anim.think = THINK_DRAW_FRAMES;
}

// --- Lifecycle -------------------------------------------------------------
// The game is a fixed-size POD and there is only ever one instance, so a single
// static value backs the pointer-returning API instead of a heap allocation
// (which also removes the out-of-memory exit path). game_destroy is a no-op
// kept for API symmetry; the frame loop's NULL check still distinguishes "no
// game yet" from a live game.
Game* game_create(const Rules* rules) {
    static Game game;
    game_init(&game, rules);
    return &game;
}

void game_init(Game* out, const Rules* rules) {
    Game* g = out;
    memset(g, 0, sizeof *g);

    // Field-by-field, not struct assignment: assignment copies the caller's
    // padding bytes into state that must stay byte-identical across replays
    // (memset above zeroed ours; keep it that way).
    g->rules.player_count         = rules->player_count;
    g->rules.human_seat           = rules->human_seat;
    g->rules.target_score         = rules->target_score;
    g->rules.bonus_scoring        = rules->bonus_scoring;
    g->rules.partners             = rules->partners;
    g->rules.reshuffle_on_recycle = rules->reshuffle_on_recycle;
    g->rules.stalemate_cutoff     = rules->stalemate_cutoff;
    g->rules.ai_difficulty        = rules->ai_difficulty;
    g->rules.seed                 = rules->seed;
    rules_normalize(&g->rules);
    // The caller seeds from the clock (or, in the daemon, the system's random
    // source) when it wants variety. The AI's dice start from the deal stream
    // rather than from the seed itself, so nothing the AI is seen to do is a
    // direct function of the seed; 0 is the one state xorshift can't hold.
    DealStream ds;
    deal_stream(&ds, match_seed(g), DEAL_AI, 0, 0);
    g->rng = deal_next(&ds);
    if (g->rng == 0) g->rng = 0x9E3779B97F4A7C15ULL;

    for (int i = 0; i < g->rules.player_count; i++) {
        g->players[i].is_ai = (i != g->rules.human_seat);
    }
    g->match_winner = NO_WINNER;
    g->round_no = 1;

    // First dealer: drawn from the seed (the digital stand-in for cutting low
    // card); the deal rotates left each round.
    deal_stream(&ds, match_seed(g), DEAL_DEALER, 0, 0);
    g->dealer = (uint8_t)deal_below(&ds, (uint32_t)g->rules.player_count);
    deal_round(g);
}

void game_destroy(Game* game) {
    (void)game; // static instance; nothing to free
}

void game_redact_for(Game* out, const Game* g, int seat) {
    memcpy(out, g, sizeof *out);

    // The seed reconstructs every hidden card (the GameView seed-leak
    // lesson), and the AI's dice are derived from it: neither may reach a
    // client in any form. (`deals` stays: it is the round count.)
    out->rng = 0;
    out->rules.seed = 0;

    // The face-down stock: the count is public, the order never is.
    memset(out->stock, 0, sizeof out->stock);

    // Other racks are hidden until they physically turn face up at the
    // round-over reveal (and stay up through the match banner).
    bool reveal = (g->phase == PHASE_ROUND_OVER || g->phase == PHASE_MATCH_OVER);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i != seat && !reveal) {
            memset(out->players[i].rack.slots, 0, RACK_SLOTS);
        }
    }

    // A stock-drawn card is private to its holder; a discard draw happened
    // face up. Mirrors game_view_for: held_card 0 while phase == PHASE_PLACE
    // means "a hidden card is held" (clients render a back).
    if (g->held_card && !(seat == g->turn || g->held_from_discard)) {
        out->held_card = 0;
    }

    // Presentation never crosses the wire.
    memset(&out->anim, 0, sizeof out->anim);
}

bool game_is_over(const Game* g) {
    return g->phase == PHASE_MATCH_OVER;
}

// --- Actions ---------------------------------------------------------------
bool game_action_legal(const Game* g, Action a) {
    switch (g->phase) {
    case PHASE_DRAW:
        if (a.type == ACTION_DRAW_STOCK) {
            // Drawable directly, after a recycle, or — on the recycle that
            // would exceed the cutoff — as the draw that calls the stalemate.
            if (g->stock_count > 0) return true;
            if (g->rules.stalemate_cutoff && g->recycles >= 3) return true;
            return g->discard_count >= 2;
        }
        if (a.type == ACTION_DRAW_DISCARD) {
            return g->discard_count > 0;
        }
        return false;

    case PHASE_PLACE:
        if (a.type == ACTION_PLACE) {
            return a.slot < RACK_SLOTS;
        }
        if (a.type == ACTION_DISCARD) {
            // The rule that makes the illegal move unrepresentable: a card
            // taken from the discard pile must be exchanged into the rack.
            return !g->held_from_discard;
        }
        return false;

    default: // PHASE_DEAL, PHASE_ROUND_OVER, PHASE_MATCH_OVER: nothing is legal
        return false;
    }
}

bool game_apply(Game* g, Action a) {
    if (!game_action_legal(g, a)) return false;

    switch (a.type) {
    case ACTION_DRAW_STOCK:
        if (g->stock_count == 0) {
            if (g->rules.stalemate_cutoff && g->recycles >= 3) {
                // A fourth turn-over would be needed: the round stalls out.
                end_round(g, NO_WINNER);
                return true;
            }
            recycle_stock(g);
        }
        g->held_card = pop_stock(g);
        g->held_from_discard = false;
        g->phase = PHASE_PLACE;
        g->events |= EV_DRAW;
        g->anim.kind = ANIM_DRAW_STOCK;
        g->anim.seat = g->turn;
        g->anim.card = g->held_card;
        g->anim.frames = g->anim.total = SLIDE_FRAMES;
        g->anim.think = THINK_PLACE_FRAMES;
        return true;

    case ACTION_DRAW_DISCARD:
        g->held_card = g->discard[--g->discard_count];
        g->held_from_discard = true;
        g->last_taken[g->turn] = g->held_card; // public: everyone saw it go
        g->phase = PHASE_PLACE;
        g->events |= EV_DRAW;
        g->anim.kind = ANIM_DRAW_DISCARD;
        g->anim.seat = g->turn;
        g->anim.card = g->held_card;
        g->anim.frames = g->anim.total = SLIDE_FRAMES;
        g->anim.think = THINK_PLACE_FRAMES;
        return true;

    case ACTION_PLACE: {
        // The exchange: the new card takes exactly the slot the old card
        // vacated, and the old card goes face up on the discard pile.
        Rack* rack = &g->players[g->turn].rack;
        uint8_t displaced = rack->slots[a.slot];
        rack->slots[a.slot] = g->held_card;
        push_discard(g, displaced);
        g->held_card = 0;
        g->held_from_discard = false;
        g->events |= EV_PLACE;
        g->anim.kind = ANIM_PLACE;
        g->anim.seat = g->turn;
        g->anim.slot = a.slot;
        g->anim.card = displaced;
        g->anim.frames = g->anim.total = SLIDE_FRAMES;

        // Going out is not an action: a rack that reads ascending (and, at 2
        // players, carries the mandatory 3-run) ends the round on the spot.
        if (rack_is_out(rack) &&
            (!rules_require_run(&g->rules) || score_longest_run(rack) >= 3)) {
            end_round(g, g->turn);
        } else {
            advance_turn(g);
        }
        return true;
    }

    case ACTION_DISCARD:
        push_discard(g, g->held_card);
        g->held_card = 0;
        g->events |= EV_DISCARD;
        g->anim.kind = ANIM_DISCARD;
        g->anim.seat = g->turn;
        g->anim.card = g->discard[g->discard_count - 1];
        g->anim.frames = g->anim.total = SLIDE_FRAMES;
        advance_turn(g);
        return true;

    default:
        return false; // unreachable: legality already filtered
    }
}

void game_next_round(Game* g) {
    if (g->phase != PHASE_ROUND_OVER) return;
    if (g->match_winner != NO_WINNER) {
        g->phase = PHASE_MATCH_OVER;
        g->events |= EV_MATCH_END;
        return;
    }
    g->dealer = (uint8_t)((g->dealer + 1) % g->rules.player_count);
    if (g->round_no < 255) g->round_no++;   // saturate: a long run of stalemates
    deal_round(g);                          // and ties can pass 255 rounds
}

// --- Per-step presentation + AI pacing -------------------------------------
void game_update(Game* g) {
    g->events = 0;
    Anim* a = &g->anim;

    switch (g->phase) {
    case PHASE_DEAL: {
        // One card lands every DEAL_STEP_FRAMES; the final step flips the
        // first discard, then play begins. Racks and piles were fully dealt at
        // round start — this reveal is presentation only.
        int total = g->rules.player_count * RACK_SLOTS + 1;
        if (a->frames > 0) a->frames--;
        if (a->frames == 0) {
            if ((int)a->deal_step < total) {
                a->deal_step++;
                a->frames = DEAL_STEP_FRAMES;
            }
            if ((int)a->deal_step >= total) {
                g->phase = PHASE_DRAW;
                g->events |= EV_TURN;
                a->frames = 0;
                a->think = THINK_DRAW_FRAMES;
            }
        }
        break;
    }

    case PHASE_DRAW:
    case PHASE_PLACE: {
        // Finish any slide first so moves stay legible, then run the think
        // delay, then let the AI act. Humans act through game_apply whenever
        // they like; the pacing only throttles AI seats.
        if (a->frames > 0) {
            a->frames--;
            if (a->frames == 0) a->kind = ANIM_NONE;
            break;
        }
        if (!g->players[g->turn].is_ai) break;
        if (a->think > 0) {
            a->think--;
            break;
        }
        GameView v = game_view_for(g, g->turn);
        Action act = ai_choose(&v, &g->rng);
        if (!game_apply(g, act)) {
            // A misbehaving AI must never wedge the match: fall back to the
            // always-legal move for the phase. test_ai asserts this path is
            // never needed.
            if (g->phase == PHASE_DRAW) {
                if (!game_apply(g, (Action){ACTION_DRAW_STOCK, 0}))
                    game_apply(g, (Action){ACTION_DRAW_DISCARD, 0});
            } else {
                game_apply(g, (Action){ACTION_PLACE, 0});
            }
        }
        break;
    }

    case PHASE_ROUND_OVER:
        if (a->frames > 0) a->frames--;
        // Full-AI games advance themselves after a reveal pause; with a human
        // seated, the frame loop advances on confirmation instead.
        if (g->rules.human_seat < 0) {
            if (a->reveal > 0) a->reveal--;
            else game_next_round(g);
        }
        break;

    default: // PHASE_MATCH_OVER: nothing moves
        break;
    }
}

// --- Redaction -------------------------------------------------------------
GameView game_view_for(const Game* g, int seat) {
    GameView v;
    memset(&v, 0, sizeof v);

    v.rules = g->rules;
    // The seed IS the hidden state: replaying the seeded shuffle from it
    // reconstructs every rack and the stock order. Redaction by construction
    // means the view must not carry it.
    v.rules.seed = 0;
    v.own_rack = g->players[seat].rack;
    v.top_discard = g->discard_count ? g->discard[g->discard_count - 1] : 0;
    v.stock_count = g->stock_count;
    v.seat = (uint8_t)seat;
    v.player_count = (uint8_t)g->rules.player_count;

    // The held card is private while it came face down off the stock: only the
    // holder's own view carries it. A discard draw happened face up, so that
    // one is public.
    if (g->held_card) {
        if (seat == g->turn || g->held_from_discard) v.held_card = g->held_card;
        v.held_from_discard = g->held_from_discard;
    }

    // The discard pile landed face up card by card, so its full order is
    // public knowledge (a perfect-memory player tracks it; so may the AI).
    memcpy(v.discard_history, g->discard, g->discard_count);
    v.discard_history_count = g->discard_count;

    for (int i = 0; i < g->rules.player_count; i++) {
        v.opponent_out_risk[i] = g->turns_taken[i];
        v.last_taken[i] = g->last_taken[i];
    }
    return v;
}
