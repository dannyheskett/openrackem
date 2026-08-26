// The AI: one heuristic evaluator over a redacted GameView, with difficulty as
// a deliberate degradation of that single evaluator — not three separate AIs
// (docs/PLAN.md §8). It sees exactly what a seated human sees: its own rack,
// the public table, and public per-opponent facts. Compiles standalone with
// libc; the only randomness is the caller's xorshift64* state.
#include "ai.h"

// --- RNG (same generator the engine uses; the state is the caller's) --------
static uint64_t ai_rng_next(uint64_t* s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}

static uint32_t ai_rng_below(uint64_t* s, uint32_t n) {
    return (uint32_t)(ai_rng_next(s) % n);
}

// --- The evaluator ----------------------------------------------------------
// Each card has an ideal slot for its value: slot = (card - 1) * 10 / deck.
static int ideal_slot(int card, int deck) {
    return (card - 1) * RACK_SLOTS / deck;
}

// Weights, tuned so the terms rank the way the plan orders them: the ascending
// prefix is what actually scores when the round is lost, so it dominates;
// adjacent ascending pairs are progress toward going out; ideal-slot distance
// is a tie-breaking gradient; the run term only matters where runs pay (the
// 2-player go-out requirement, or the bonus variant).
#define W_PREFIX 100
#define W_PAIR    60
#define W_DIST     8
#define W_RUN     40
#define WIN_SCORE 1000000

// A rack that goes out under the active rules trumps everything.
static bool rack_wins(const Rack* r, const Rules* rules) {
    if (!rack_is_out(r)) return false;
    return !rules_require_run(rules) || score_longest_run(r) >= 3;
}

// Note: no win bonus here — the caller credits WIN_SCORE to candidate racks.
// (The current rack can itself read as "out" in the dealt-perfect edge case,
// and folding the bonus into the baseline would cancel it out of every gain,
// leaving the AI unable to see the winning move.)
static int eval_rack(const Rack* r, const Rules* rules) {
    int prefix = 1;
    while (prefix < RACK_SLOTS && r->slots[prefix] > r->slots[prefix - 1]) prefix++;

    if (rules->ai_difficulty == 0) {
        // Easy considers nothing but the scoring prefix.
        return prefix * W_PREFIX;
    }

    int deck = rules_deck_size(rules);
    int pairs = 0, dist = 0;
    for (int i = 1; i < RACK_SLOTS; i++) {
        if (r->slots[i] > r->slots[i - 1]) pairs++;
    }
    for (int i = 0; i < RACK_SLOTS; i++) {
        int d = ideal_slot(r->slots[i], deck) - i;
        dist += (d < 0) ? -d : d;
    }
    int score = prefix * W_PREFIX + pairs * W_PAIR - dist * W_DIST;

    if (rules_require_run(rules) || rules->bonus_scoring) {
        score += score_longest_run(r) * W_RUN;
    }
    return score;
}

// --- Hard-tier public-information reads -------------------------------------
// "Many turns elapsed" means somebody may be close to going out: shrink the
// improvement the AI waits for and take what is on the table.
static bool endgame_urgency(const GameView* v) {
    for (int i = 0; i < v->player_count; i++) {
        if (i == v->seat) continue;
        if (v->opponent_out_risk[i] >= 25) return true;
    }
    return false;
}

// Discarding a card an opponent visibly collected from the pile (or one
// adjacent to it) hands them progress. Returns a penalty for throwing `card`.
// Deliberately tie-break sized: it should steer between near-equal placements,
// never talk the AI out of a genuinely better rack (bench-tuned — a W_PAIR
// sized penalty here made Hard lose to Normal).
static int feed_penalty(const GameView* v, int card) {
    for (int i = 0; i < v->player_count; i++) {
        if (i == v->seat || v->last_taken[i] == 0) continue;
        int d = card - (int)v->last_taken[i];
        if (d < 0) d = -d;
        if (d <= 2) return W_DIST;
    }
    return 0;
}

// Gain from exchanging `card` into `slot`, including what the exchange throws
// to the pile (hard avoids feeding opponents with the displaced card). A
// candidate that goes out under the active rules trumps everything.
static int placement_gain(const GameView* v, int base, int card, int slot) {
    Rack cand = v->own_rack;
    uint8_t displaced = cand.slots[slot];
    cand.slots[slot] = (uint8_t)card;
    if (rack_wins(&cand, &v->rules)) return WIN_SCORE;
    int gain = eval_rack(&cand, &v->rules) - base;
    if (v->rules.ai_difficulty == 2) {
        gain -= feed_penalty(v, displaced);
    }
    return gain;
}

// Best exchange for `card` across all ten slots. Returns the gain; *out_slot
// gets the slot.
static int best_placement(const GameView* v, int base, int card, int* out_slot) {
    int best_gain = 0, best_slot = -1;
    for (int s = 0; s < RACK_SLOTS; s++) {
        int gain = placement_gain(v, base, card, s);
        if (best_slot < 0 || gain > best_gain) {
            best_gain = gain;
            best_slot = s;
        }
    }
    *out_slot = best_slot;
    return best_gain;
}

// --- The decision -----------------------------------------------------------
Action ai_choose(const GameView* v, uint64_t* rng) {
    const Rules* rules = &v->rules;
    int base = eval_rack(&v->own_rack, rules);

    if (v->held_card == 0) {
        // Draw decision: evaluate the face-up card against every slot, then
        // take it or decline for the blind stock. Normal and Hard wait for a
        // real improvement rather than churning the rack; Hard drops that bar
        // when an opponent looks close to going out. Easy grabs any uptick.
        if (v->top_discard != 0) {
            int slot;
            int gain = best_placement(v, base, v->top_discard, &slot);
            int need;
            switch (rules->ai_difficulty) {
            case 0:  need = 1; break;
            case 2:  need = endgame_urgency(v) ? 12 : 25; break;
            default: need = 25; break;
            }
            if (gain >= need) {   // a winning placement is WIN_SCORE: always taken
                return (Action){ACTION_DRAW_DISCARD, 0};
            }
        }
        return (Action){ACTION_DRAW_STOCK, 0};
    }

    // Placement decision. Easy fumbles: a quarter of the time it shoves the
    // card into a random slot instead of thinking.
    if (rules->ai_difficulty == 0 && ai_rng_below(rng, 4) == 0) {
        return (Action){ACTION_PLACE, (uint8_t)ai_rng_below(rng, RACK_SLOTS)};
    }

    int slot;
    int gain = best_placement(v, base, v->held_card, &slot);

    // A stock-drawn card that helps nowhere goes straight to the pile. (A
    // discard-drawn card must be exchanged: the least-bad slot it is.)
    if (!v->held_from_discard && gain <= 0) {
        return (Action){ACTION_DISCARD, 0};
    }
    return (Action){ACTION_PLACE, (uint8_t)slot};
}
