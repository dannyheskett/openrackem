// Unit tests for the AI (docs/PLAN.md §8 / §11): ai_choose returns a legal
// action from every reachable phase across thousands of seeded positions; a
// full AI-only match always terminates; Normal beats Easy over a seeded
// sample. The AI never reads a hidden rack — enforced structurally, since
// GameView has no field for one; these tests drive it exclusively through
// game_view_for.
//
// Built and run by `make test`. A non-zero exit means a failure.

#include "rules.c"
#include "game.c"
#include "ai.c"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void skip_deal(Game* g) {
    for (int i = 0; i < 4096 && g->phase == PHASE_DEAL; i++) game_update(g);
    CHECK(g->phase == PHASE_DRAW);
}

// Play one full match with the AI in every seat, each seat at its own
// difficulty (the per-seat override goes through the view copy, exactly how a
// mixed-strength table would work). Every chosen action is checked for
// legality before it is applied. Returns the match winner's seat, or -1 if
// the match failed to terminate within the step cap.
static int play_match(int players, uint64_t seed, const int diffs[MAX_PLAYERS],
                      long* positions) {
    Rules r = rules_default();
    r.player_count = players;
    r.human_seat = 0;          // seat flags don't matter: we drive every seat
    r.seed = seed;
    rules_normalize(&r);
    Game* g = game_create(&r);
    skip_deal(g);

    for (long step = 0; step < 200000; step++) {
        if (g->phase == PHASE_MATCH_OVER) return g->match_winner;
        if (g->phase == PHASE_ROUND_OVER) {
            game_next_round(g);
            if (g->phase == PHASE_DEAL) skip_deal(g);
            continue;
        }
        GameView v = game_view_for(g, g->turn);
        v.rules.ai_difficulty = diffs[g->turn];
        Action a = ai_choose(&v, &g->rng);
        if (!game_action_legal(g, a)) {
            printf("  illegal AI action: type=%d slot=%d phase=%d (seed %llu)\n",
                   a.type, a.slot, g->phase, (unsigned long long)seed);
            failures++;
            return -1;
        }
        CHECK(game_apply(g, a));
        if (positions) (*positions)++;
    }
    return -1;
}

// --- Legality + termination across seeds, difficulties, player counts --------
static void test_legal_and_terminates(void) {
    long positions = 0;
    for (int diff = 0; diff <= 2; diff++) {
        for (int players = 2; players <= 4; players++) {
            for (uint64_t seed = 1; seed <= 15; seed++) {
                int diffs[MAX_PLAYERS] = {diff, diff, diff, diff};
                int winner = play_match(players, seed * 7919 + diff, diffs, &positions);
                CHECK(winner >= 0 && winner < players);
            }
        }
    }
    // "Thousands of seeded positions": every one was legality-checked above.
    CHECK(positions > 10000);
    printf("  (evaluated %ld positions)\n", positions);
}

// --- Difficulty ordering: Normal beats Easy ---------------------------------
static void test_normal_beats_easy(void) {
    const int MATCHES = 80;
    int normal_wins = 0;
    for (int m = 0; m < MATCHES; m++) {
        // Alternate which seat is Normal so first-player advantage cancels.
        int normal_seat = m % 2;
        int diffs[MAX_PLAYERS] = {0, 0, 0, 0};
        diffs[normal_seat] = 1;
        int winner = play_match(2, 100000 + (uint64_t)m * 131, diffs, NULL);
        CHECK(winner >= 0);
        if (winner == normal_seat) normal_wins++;
    }
    printf("  (normal won %d/%d vs easy)\n", normal_wins, MATCHES);
    CHECK(normal_wins * 100 >= 60 * MATCHES);
}

// --- Decision determinism ----------------------------------------------------
// The same view and the same rng state must yield the same action: the AI has
// no hidden state of its own, which is what lets a server replay it.
static void test_decision_determinism(void) {
    Rules r = rules_default();
    r.player_count = 4;
    r.seed = 424242;
    rules_normalize(&r);
    Game* g = game_create(&r);
    skip_deal(g);

    for (int i = 0; i < 50; i++) {
        if (g->phase == PHASE_ROUND_OVER) { game_next_round(g); skip_deal(g); continue; }
        if (g->phase == PHASE_MATCH_OVER) break;
        GameView v = game_view_for(g, g->turn);
        uint64_t rng1 = g->rng, rng2 = g->rng;
        Action a1 = ai_choose(&v, &rng1);
        Action a2 = ai_choose(&v, &rng2);
        CHECK(a1.type == a2.type && a1.slot == a2.slot);
        CHECK(rng1 == rng2);
        CHECK(game_apply(g, ai_choose(&v, &g->rng)));
    }
}

// --- The evaluator prefers the winning move ---------------------------------
static void test_takes_the_win(void) {
    // Nine ascending cards and a hole at the top: the AI must place the
    // winning card there, at every difficulty except Easy's fumble roll (so
    // Easy is exercised with an rng state whose next roll is not the fumble).
    GameView v;
    memset(&v, 0, sizeof v);
    v.rules = rules_default();
    v.rules.player_count = 4;
    rules_normalize(&v.rules);
    v.player_count = 4;
    uint8_t rack[RACK_SLOTS] = {2, 5, 9, 14, 20, 26, 31, 38, 44, 1};
    memcpy(v.own_rack.slots, rack, RACK_SLOTS);
    v.held_card = 50;
    v.held_from_discard = 0;

    for (int diff = 0; diff <= 2; diff++) {
        v.rules.ai_difficulty = diff;
        uint64_t rng = 3;   // for Easy: first roll of this state is not a fumble
        Action a = ai_choose(&v, &rng);
        CHECK(a.type == ACTION_PLACE && a.slot == 9);
    }

    // And on the draw side: the winning card on the pile is always taken.
    v.held_card = 0;
    v.top_discard = 50;
    for (int diff = 0; diff <= 2; diff++) {
        v.rules.ai_difficulty = diff;
        uint64_t rng = 3;
        Action a = ai_choose(&v, &rng);
        CHECK(a.type == ACTION_DRAW_DISCARD);
    }

    // At 2 players a useless top-discard is declined for the stock. The card
    // must be genuinely useless there: not adjacent in value to anything in
    // the rack (adjacency would build toward the mandatory 3-run, and taking
    // it would be right).
    v.rules.player_count = 2;
    rules_normalize(&v.rules);
    v.player_count = 2;
    v.rules.ai_difficulty = 1;
    uint8_t rack2p[RACK_SLOTS] = {2, 5, 9, 14, 20, 26, 31, 34, 36, 40};
    memcpy(v.own_rack.slots, rack2p, RACK_SLOTS);
    v.top_discard = 17;  // fits where 20 sits but improves nothing, extends no run
    uint64_t rng = 3;
    Action a = ai_choose(&v, &rng);
    CHECK(a.type == ACTION_DRAW_STOCK);
}

// --- A discard-drawn card is always exchanged --------------------------------
static void test_forced_exchange(void) {
    // Even a terrible forced card gets placed (never ACTION_DISCARD).
    GameView v;
    memset(&v, 0, sizeof v);
    v.rules = rules_default();
    rules_normalize(&v.rules);
    v.player_count = 4;
    uint8_t rack[RACK_SLOTS] = {2, 5, 9, 14, 20, 26, 31, 38, 44, 50};
    memcpy(v.own_rack.slots, rack, RACK_SLOTS);
    v.held_card = 1;
    v.held_from_discard = 1;

    for (int diff = 0; diff <= 2; diff++) {
        v.rules.ai_difficulty = diff;
        for (uint64_t seed = 1; seed <= 8; seed++) {
            uint64_t rng = seed;
            Action a = ai_choose(&v, &rng);
            CHECK(a.type == ACTION_PLACE);
            CHECK(a.slot < RACK_SLOTS);
        }
    }
}

int main(void) {
    printf("test_ai: legality across seeded matches, termination, difficulty\n");
    printf("         ordering, decision determinism, win-taking, forced exchange\n");
    test_legal_and_terminates();
    test_normal_beats_easy();
    test_decision_determinism();
    test_takes_the_win();
    test_forced_exchange();
    if (failures == 0) {
        printf("OK: all checks passed\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
