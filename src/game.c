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

// --- RNG -------------------------------------------------------------------
// xorshift64*: tiny, seedable, and good enough to shuffle 60 cards. The state
// lives in Game.rng and is the engine's only randomness source.
static uint64_t rng_next(uint64_t* s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}

// Uniform-ish value in [0, n). The modulo bias over 2^64 is immeasurable for
// n <= 60.
static uint32_t rng_below(uint64_t* s, uint32_t n) {
    return (uint32_t)(rng_next(s) % n);
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

static void shuffle(Game* g, uint8_t* cards, int count) {
    for (int i = count - 1; i > 0; i--) {
        uint32_t j = rng_below(&g->rng, (uint32_t)(i + 1));
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
        shuffle(g, g->stock, g->stock_count);
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
    shuffle(g, g->stock, deck);

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
// decides the match: first player — or pair, under Partners — at or past the
// target wins; ties break toward the round winner's side, then the lower seat.
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
    for (int s = 0; s < sides; s++) {
        if (totals[s] < g->rules.target_score) continue;
        if (best < 0 || totals[s] > totals[best]) {
            best = s;
        } else if (totals[s] == totals[best]) {
            // Tie at or past the target: the side that went out this round
            // prevails (it earned the closing 75); otherwise the lower seat.
            int winner_side = (winner == NO_WINNER) ? -1
                             : (g->rules.partners ? winner % 2 : winner);
            if (s == winner_side) best = s;
        }
    }
    if (best >= 0) {
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
// cap is the wedge-proof backstop documented at the top of the file.
static void advance_turn(Game* g) {
    int seat = g->turn;
    if (g->turns_taken[seat] < 255) g->turns_taken[seat]++;
    if (g->rules.stalemate_cutoff && g->turns_taken[seat] >= TURN_CAP) {
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
    Game* g = &game;
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
    // The caller seeds from the clock when it wants variety; the engine only
    // guards the one value xorshift can't take.
    g->rng = g->rules.seed ? g->rules.seed : 0x9E3779B97F4A7C15ULL;

    for (int i = 0; i < g->rules.player_count; i++) {
        g->players[i].is_ai = (i != g->rules.human_seat);
    }
    g->match_winner = NO_WINNER;
    g->round_no = 1;

    // First dealer: drawn from the seeded RNG (the digital stand-in for
    // cutting low card); the deal rotates left each round.
    g->dealer = (uint8_t)rng_below(&g->rng, (uint32_t)g->rules.player_count);
    deal_round(g);
    return g;
}

void game_destroy(Game* game) {
    (void)game; // static instance; nothing to free
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
    if (g->round_no < 255) g->round_no++;   // saturate: a 10000-point match can
    deal_round(g);                          // legitimately pass 255 rounds
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
