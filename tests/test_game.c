// Unit tests for openrackem's rules and game logic — docs/PLAN.md §3 (the
// official rulebook) is the oracle, §11 lists this suite. The simulation runs
// in isolation — no raylib, no window — by including the sources directly so
// the file-static helpers (rng, deal, recycle, end_round) are visible.
//
// Built and run by `make test`. A non-zero exit means a failure.

#include "rules.c"
#include "game.c"
#include "ai.c"
#include "tick.c"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// --- Helpers ----------------------------------------------------------------

// Drive the presentation deal to completion so the game sits in PHASE_DRAW.
// Bounded: the deal is (players*10 + 1) steps of DEAL_STEP_FRAMES.
static void skip_deal(Game* g) {
    for (int i = 0; i < 4096 && g->phase == PHASE_DEAL; i++) game_update(g);
    CHECK(g->phase == PHASE_DRAW);
}

// A dealt game in PHASE_DRAW. Variants tweak the returned rules first.
static Rules test_rules(int players, uint64_t seed) {
    Rules r = rules_default();
    r.player_count = players;
    r.human_seat = 0;      // seats 1.. are "AI", but tests apply actions
    r.seed = seed;         // directly and never let game_update reach ai_choose
    rules_normalize(&r);
    return r;
}

static Game* fresh(int players, uint64_t seed) {
    Rules r = test_rules(players, seed);
    Game* g = game_create(&r);
    skip_deal(g);
    return g;
}

static Action A(int type, int slot) {
    return (Action){(uint8_t)type, (uint8_t)slot};
}

// Assert an action is rejected AND that the rejection mutated nothing: the
// plan's §14.5 invariant is byte-level (never partially mutates), so the whole
// serialized prefix is compared, not a handful of fields.
static void check_rejected_untouched(Game* g, Action a) {
    uint8_t before[sizeof(Game)];
    memcpy(before, g, GAME_SERIAL_SIZE);
    CHECK(!game_apply(g, a));
    CHECK(memcmp(before, g, GAME_SERIAL_SIZE) == 0);
}

// Overwrite the acting player's rack and hand them a held card, entering
// PHASE_PLACE directly. For scenario tests that need exact cards.
static void force_place(Game* g, const uint8_t slots[RACK_SLOTS],
                        uint8_t held, bool from_discard) {
    memcpy(g->players[g->turn].rack.slots, slots, RACK_SLOTS);
    g->held_card = held;
    g->held_from_discard = from_discard;
    g->phase = PHASE_PLACE;
}

// --- rules_normalize ---------------------------------------------------------
static void test_rules_normalize(void) {
    Rules r = rules_default();
    CHECK(r.player_count == 4);
    CHECK(r.target_score == 500);
    CHECK(!r.bonus_scoring && !r.partners && !r.reshuffle_on_recycle);
    CHECK(r.stalemate_cutoff);
    CHECK(r.ai_difficulty == 1);

    // Deck subsets from player count: 2 -> 1-40, 3 -> 1-50, 4 -> 1-60.
    r = rules_default(); r.player_count = 2; rules_normalize(&r);
    CHECK(rules_deck_size(&r) == 40);
    r.player_count = 3; rules_normalize(&r);
    CHECK(rules_deck_size(&r) == 50);
    r.player_count = 4; rules_normalize(&r);
    CHECK(rules_deck_size(&r) == 60);

    // The 2-player run requirement is mandatory, not an option.
    r = rules_default(); r.player_count = 2; rules_normalize(&r);
    CHECK(rules_require_run(&r));
    r.player_count = 3; rules_normalize(&r);
    CHECK(!rules_require_run(&r));

    // Partners is rejected below 4 players.
    r = rules_default(); r.player_count = 3; r.partners = true; rules_normalize(&r);
    CHECK(!r.partners);
    r = rules_default(); r.player_count = 4; r.partners = true; rules_normalize(&r);
    CHECK(r.partners);

    // Clamping.
    r = rules_default(); r.player_count = 9; rules_normalize(&r);
    CHECK(r.player_count == 4);
    r.player_count = 0; rules_normalize(&r);
    CHECK(r.player_count == 2);
    r = rules_default(); r.human_seat = 7; rules_normalize(&r);
    CHECK(r.human_seat == r.player_count - 1);
    r.human_seat = -3; rules_normalize(&r);
    CHECK(r.human_seat == -1);
    r = rules_default(); r.ai_difficulty = 9; rules_normalize(&r);
    CHECK(r.ai_difficulty == 2);
    r.ai_difficulty = -1; rules_normalize(&r);
    CHECK(r.ai_difficulty == 0);
    r = rules_default(); r.target_score = 1; rules_normalize(&r);
    CHECK(r.target_score == 50);
}

// --- Deal --------------------------------------------------------------------
static void test_deal_invariants(void) {
    for (int players = 2; players <= 4; players++) {
        Game* g = fresh(players, 123 + (uint64_t)players);
        int deck = rules_deck_size(&g->rules);

        // Ten cards each, all in the deck subset, no duplicates anywhere.
        bool seen[MAX_CARDS + 1] = {false};
        for (int s = 0; s < players; s++) {
            for (int i = 0; i < RACK_SLOTS; i++) {
                int c = g->players[s].rack.slots[i];
                CHECK(c >= 1 && c <= deck);
                CHECK(!seen[c]);
                seen[c] = true;
            }
        }
        for (int i = 0; i < g->stock_count; i++) {
            int c = g->stock[i];
            CHECK(c >= 1 && c <= deck);
            CHECK(!seen[c]);
            seen[c] = true;
        }
        // Exactly one card face up, and the count balances the whole deck.
        CHECK(g->discard_count == 1);
        int c = g->discard[0];
        CHECK(c >= 1 && c <= deck && !seen[c]);
        CHECK(g->stock_count + g->discard_count + players * RACK_SLOTS == deck);

        // Play starts left of the dealer.
        CHECK(g->turn == (g->dealer + 1) % players);
        CHECK(g->round_winner == NO_WINNER);
        CHECK(g->match_winner == NO_WINNER);
    }
}

// Deal order: one card at a time starting left of the dealer, each player's
// first card to slot #50 (index 9) down to #5, then the flip. Verified by
// replaying the same rng primitives the engine used.
static void test_deal_order(void) {
    const uint64_t SEED = 7777;
    const int players = 3;
    Game* g = fresh(players, SEED);
    int deck = rules_deck_size(&g->rules);

    uint64_t s = SEED;
    int dealer = (int)rng_below(&s, (uint32_t)players);
    CHECK(dealer == g->dealer);

    uint8_t expect[MAX_CARDS];
    for (int i = 0; i < deck; i++) expect[i] = (uint8_t)(i + 1);
    for (int i = deck - 1; i > 0; i--) {
        uint32_t j = rng_below(&s, (uint32_t)(i + 1));
        uint8_t t = expect[i]; expect[i] = expect[j]; expect[j] = t;
    }

    int idx = 0;
    for (int slot = RACK_SLOTS - 1; slot >= 0; slot--) {
        for (int k = 0; k < players; k++) {
            int seat = (dealer + 1 + k) % players;
            CHECK(g->players[seat].rack.slots[slot] == expect[idx]);
            idx++;
        }
    }
    CHECK(g->discard[0] == expect[idx]);
    idx++;
    for (int i = 0; i < g->stock_count; i++) CHECK(g->stock[i] == expect[idx + i]);
}

// --- Legality ----------------------------------------------------------------
static void test_legality(void) {
    Game* g = fresh(3, 42);

    // PHASE_DRAW: only the two draws are legal (acting out of sequence —
    // placing or discarding before drawing — is rejected).
    CHECK(game_action_legal(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(game_action_legal(g, A(ACTION_DRAW_DISCARD, 0)));
    CHECK(!game_action_legal(g, A(ACTION_PLACE, 0)));
    CHECK(!game_action_legal(g, A(ACTION_DISCARD, 0)));
    check_rejected_untouched(g, A(ACTION_PLACE, 3));
    check_rejected_untouched(g, A(ACTION_DISCARD, 0));

    // A discard-drawn card cannot be discarded, and no second draw can start.
    CHECK(game_apply(g, A(ACTION_DRAW_DISCARD, 0)));
    CHECK(g->phase == PHASE_PLACE);
    CHECK(g->held_from_discard);
    CHECK(!game_action_legal(g, A(ACTION_DISCARD, 0)));
    check_rejected_untouched(g, A(ACTION_DISCARD, 0));
    CHECK(!game_action_legal(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(!game_action_legal(g, A(ACTION_DRAW_DISCARD, 0)));
    check_rejected_untouched(g, A(ACTION_DRAW_STOCK, 0));

    // Placing outside 0..9 is rejected; a garbage action type is rejected.
    CHECK(!game_action_legal(g, A(ACTION_PLACE, 10)));
    CHECK(!game_action_legal(g, A(ACTION_PLACE, 255)));
    CHECK(!game_action_legal(g, A(200, 0)));
    CHECK(game_apply(g, A(ACTION_PLACE, 4)));

    // A stock-drawn card may be discarded outright.
    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(!g->held_from_discard);
    CHECK(game_action_legal(g, A(ACTION_DISCARD, 0)));
    CHECK(game_apply(g, A(ACTION_DISCARD, 0)));
    CHECK(g->events & EV_DISCARD);

    // Every action is rejected during the deal...
    Rules r = test_rules(3, 43);
    g = game_create(&r);
    CHECK(g->phase == PHASE_DEAL);
    for (int t = 0; t < 4; t++) CHECK(!game_action_legal(g, A(t, 0)));
    skip_deal(g);

    // ...in PHASE_ROUND_OVER...
    g->phase = PHASE_ROUND_OVER;
    for (int t = 0; t < 4; t++) {
        CHECK(!game_action_legal(g, A(t, 0)));
        check_rejected_untouched(g, A(t, 0));
    }

    // ...and in PHASE_MATCH_OVER.
    g->phase = PHASE_MATCH_OVER;
    for (int t = 0; t < 4; t++) CHECK(!game_action_legal(g, A(t, 0)));
}

// --- Exchange ----------------------------------------------------------------
static void test_exchange(void) {
    Game* g = fresh(4, 99);
    Rack before = g->players[g->turn].rack;
    uint8_t top = g->discard[g->discard_count - 1];
    uint8_t held_after_draw;
    int pile_before = g->discard_count;

    CHECK(game_apply(g, A(ACTION_DRAW_DISCARD, 0)));
    held_after_draw = g->held_card;
    CHECK(held_after_draw == top);
    CHECK(g->events & EV_DRAW);
    CHECK(g->discard_count == pile_before - 1);

    int actor = g->turn;
    uint8_t displaced = before.slots[6];
    CHECK(game_apply(g, A(ACTION_PLACE, 6)));
    CHECK(g->events & EV_PLACE);
    CHECK(g->events & EV_TURN);

    // The new card landed in exactly the vacated slot; the displaced card is
    // the new face-up top; no other slot moved; the pile count balances.
    CHECK(g->players[actor].rack.slots[6] == top);
    CHECK(g->discard[g->discard_count - 1] == displaced);
    CHECK(g->discard_count == pile_before);
    for (int i = 0; i < RACK_SLOTS; i++) {
        if (i != 6) CHECK(g->players[actor].rack.slots[i] == before.slots[i]);
    }
    CHECK(g->turn == (actor + 1) % g->rules.player_count);
    CHECK(g->phase == PHASE_DRAW);
    CHECK(g->held_card == 0);

    // A stock draw takes the top of the stock.
    uint8_t stock_top = g->stock[0];
    int stock_before = g->stock_count;
    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(g->held_card == stock_top);
    CHECK(g->stock_count == stock_before - 1);
}

// --- Stock exhaustion --------------------------------------------------------
static void test_recycle(void) {
    // Official rule: the pile is turned over, not shuffled — order preserved,
    // face-up top retained. The flip puts the oldest discard on top of the new
    // stock.
    Game* g = fresh(2, 5);
    uint8_t pile[5] = {5, 9, 2, 7, 30};
    memcpy(g->discard, pile, 5);
    g->discard_count = 5;
    g->stock_count = 0;
    g->phase = PHASE_DRAW;

    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(g->events & EV_RECYCLE);
    CHECK(g->events & EV_DRAW);
    CHECK(g->recycles == 1);
    CHECK(g->discard_count == 1);
    CHECK(g->discard[0] == 30);          // the face-up top stayed
    CHECK(g->held_card == 5);            // oldest discard came off the top
    CHECK(g->stock_count == 3);
    CHECK(g->stock[0] == 9 && g->stock[1] == 2 && g->stock[2] == 7);

    // Under the convenience flag the new stock is shuffled instead: same
    // cards, top still retained.
    Rules r = test_rules(2, 6);
    r.reshuffle_on_recycle = true;
    g = game_create(&r);
    skip_deal(g);
    memcpy(g->discard, pile, 5);
    g->discard_count = 5;
    g->stock_count = 0;
    g->phase = PHASE_DRAW;
    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(g->discard_count == 1 && g->discard[0] == 30);
    bool found[41] = {false};
    found[g->held_card] = true;
    for (int i = 0; i < g->stock_count; i++) found[g->stock[i]] = true;
    CHECK(g->stock_count == 3);
    CHECK(found[5] && found[9] && found[2] && found[7]);
}

// --- Going out ---------------------------------------------------------------
static void test_going_out(void) {
    // rack_is_out: strictly ascending detected; equal-adjacent and a single
    // out-of-order card (nine of ten right) rejected.
    Rack r;
    uint8_t up[RACK_SLOTS] = {3, 7, 12, 18, 22, 30, 41, 45, 52, 58};
    memcpy(r.slots, up, RACK_SLOTS);
    CHECK(rack_is_out(&r));
    r.slots[4] = r.slots[3];             // equal adjacent
    CHECK(!rack_is_out(&r));
    memcpy(r.slots, up, RACK_SLOTS);
    r.slots[9] = 1;                      // nine of ten ascending
    CHECK(!rack_is_out(&r));

    // 4 players: completing an ascending rack ends the round immediately —
    // going out is not an action and cannot be declined.
    Game* g = fresh(4, 1234);
    int actor = g->turn;
    uint8_t no_run[RACK_SLOTS] = {2, 4, 6, 8, 10, 12, 14, 16, 18, 1};
    force_place(g, no_run, 20, false);
    CHECK(game_apply(g, A(ACTION_PLACE, 9)));
    CHECK(g->phase == PHASE_ROUND_OVER);
    CHECK(g->round_winner == actor);
    CHECK(g->events & EV_ROUND_END);
    CHECK(g->round_points[actor] == 75);

    // 2 players: the same ascending rack with no 3-run does NOT go out...
    g = fresh(2, 1235);
    actor = g->turn;
    force_place(g, no_run, 20, false);
    CHECK(game_apply(g, A(ACTION_PLACE, 9)));
    CHECK(g->phase == PHASE_DRAW);           // round continues
    CHECK(g->round_winner == NO_WINNER);
    CHECK(g->turn == (actor + 1) % 2);

    // ...and adding a 3-run makes it go out.
    g = fresh(2, 1236);
    actor = g->turn;
    uint8_t with_run[RACK_SLOTS] = {2, 3, 4, 8, 10, 12, 14, 16, 18, 1};
    force_place(g, with_run, 20, false);
    CHECK(game_apply(g, A(ACTION_PLACE, 9)));
    CHECK(g->phase == PHASE_ROUND_OVER);
    CHECK(g->round_winner == actor);
}

// --- Base scoring ------------------------------------------------------------
static void test_base_scoring(void) {
    // The pure sequence scorer: 5 per ascending card from slot #5, stopping at
    // the first break.
    Rack r;
    uint8_t full[RACK_SLOTS] = {1, 5, 9, 14, 20, 26, 31, 38, 44, 50};
    memcpy(r.slots, full, RACK_SLOTS);
    CHECK(score_sequence(&r) == 50);         // fully ordered (a loser caps at 50)

    // The rulebook's Figure 3 shape: six ascending, then a break — 30 points.
    uint8_t fig3[RACK_SLOTS] = {3, 10, 19, 27, 31, 40, 12, 45, 50, 55};
    memcpy(r.slots, fig3, RACK_SLOTS);
    CHECK(score_sequence(&r) == 30);

    // #10 lower than #5: the minimum 5 points.
    uint8_t low[RACK_SLOTS] = {30, 8, 42, 12, 50, 3, 22, 47, 9, 14};
    memcpy(r.slots, low, RACK_SLOTS);
    CHECK(score_sequence(&r) == 5);

    // Cards past the break score nothing even if they happen to be ordered.
    uint8_t past[RACK_SLOTS] = {4, 9, 2, 10, 20, 30, 40, 45, 50, 55};
    memcpy(r.slots, past, RACK_SLOTS);
    CHECK(score_sequence(&r) == 10);

    // Round-level: the winner banks 75, everyone else their sequence points.
    Game* g = fresh(3, 777);
    memcpy(g->players[0].rack.slots, fig3, RACK_SLOTS);   // 30
    memcpy(g->players[1].rack.slots, full, RACK_SLOTS);   // fully ordered loser: 50
    memcpy(g->players[2].rack.slots, low, RACK_SLOTS);    // 5
    end_round(g, 1);
    CHECK(g->round_points[1] == 75);
    CHECK(g->round_points[0] == 30);
    CHECK(g->round_points[2] == 5);
    CHECK(g->players[1].score == 75);
    CHECK(g->players[0].score == 30);
    CHECK(g->players[2].score == 5);

    // Stalemate: everyone scores their base points; nobody collects 75.
    g = fresh(3, 778);
    memcpy(g->players[0].rack.slots, fig3, RACK_SLOTS);
    memcpy(g->players[1].rack.slots, full, RACK_SLOTS);
    memcpy(g->players[2].rack.slots, low, RACK_SLOTS);
    end_round(g, NO_WINNER);
    CHECK(g->round_points[0] == 30);
    CHECK(g->round_points[1] == 50);
    CHECK(g->round_points[2] == 5);
    CHECK(g->round_winner == NO_WINNER);
}

// --- Bonus variant -----------------------------------------------------------
static void test_bonus_scoring(void) {
    // The pure pieces: longest consecutive run, and the bonus table.
    Rack r;
    uint8_t run3[RACK_SLOTS]  = {1, 2, 3, 5, 7, 9, 11, 13, 15, 17};
    uint8_t run4[RACK_SLOTS]  = {1, 2, 3, 4, 7, 9, 11, 13, 15, 17};
    uint8_t run5[RACK_SLOTS]  = {1, 2, 3, 4, 5, 9, 11, 13, 15, 17};
    uint8_t run6[RACK_SLOTS]  = {1, 2, 3, 4, 5, 6, 11, 13, 15, 17};
    uint8_t run10[RACK_SLOTS] = {21, 22, 23, 24, 25, 26, 27, 28, 29, 30};
    uint8_t two_runs[RACK_SLOTS] = {1, 2, 3, 6, 7, 8, 9, 12, 14, 16};   // 3 then 4
    uint8_t twin_runs[RACK_SLOTS] = {1, 2, 3, 5, 6, 7, 9, 11, 13, 15};  // two 3s

    memcpy(r.slots, run3, RACK_SLOTS);  CHECK(score_longest_run(&r) == 3);
    memcpy(r.slots, run4, RACK_SLOTS);  CHECK(score_longest_run(&r) == 4);
    memcpy(r.slots, run5, RACK_SLOTS);  CHECK(score_longest_run(&r) == 5);
    memcpy(r.slots, run6, RACK_SLOTS);  CHECK(score_longest_run(&r) == 6);
    memcpy(r.slots, run10, RACK_SLOTS); CHECK(score_longest_run(&r) == 10);
    memcpy(r.slots, two_runs, RACK_SLOTS);  CHECK(score_longest_run(&r) == 4);
    memcpy(r.slots, twin_runs, RACK_SLOTS); CHECK(score_longest_run(&r) == 3);

    CHECK(score_bonus(2) == 0);
    CHECK(score_bonus(3) == 50);
    CHECK(score_bonus(4) == 100);
    CHECK(score_bonus(5) == 200);
    CHECK(score_bonus(6) == 400);
    CHECK(score_bonus(10) == 400);

    // Round totals per the official table: 125 / 175 / 275 / 475. Only the
    // player going out scores a bonus; two runs pay only the longest; equal
    // runs pay once.
    const struct { const uint8_t* rack; int total; } cases[] = {
        {run3, 125}, {run4, 175}, {run5, 275}, {run6, 475}, {run10, 475},
        {two_runs, 175}, {twin_runs, 125},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Rules rr = test_rules(3, 900 + i);
        rr.bonus_scoring = true;
        Game* g = game_create(&rr);
        skip_deal(g);
        memcpy(g->players[1].rack.slots, cases[i].rack, RACK_SLOTS);
        uint8_t loser_run[RACK_SLOTS] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 2}; // long run, didn't go out
        memcpy(g->players[0].rack.slots, loser_run, RACK_SLOTS);
        end_round(g, 1);
        CHECK(g->round_points[1] == cases[i].total);
        CHECK(g->events & EV_BONUS);
        CHECK(g->round_points[0] == 45);   // losers never score bonus points
    }

    // With the variant off, the winner stays at a flat 75.
    Game* g = fresh(3, 950);
    memcpy(g->players[1].rack.slots, run10, RACK_SLOTS);
    end_round(g, 1);
    CHECK(g->round_points[1] == 75);
    CHECK(!(g->events & EV_BONUS));
}

// --- Match flow --------------------------------------------------------------
static void test_match(void) {
    // Accumulation to 500 and first-past-the-post.
    Game* g = fresh(3, 31);
    g->players[0].score = 400;
    g->players[1].score = 440;
    g->players[2].score = 10;
    uint8_t ordered[RACK_SLOTS] = {1, 5, 9, 14, 20, 26, 31, 38, 44, 50};
    uint8_t weak[RACK_SLOTS] = {30, 8, 42, 12, 50, 3, 22, 47, 9, 14};
    memcpy(g->players[0].rack.slots, weak, RACK_SLOTS);     // +5  -> 405
    memcpy(g->players[1].rack.slots, ordered, RACK_SLOTS);  // +75 -> 515
    memcpy(g->players[2].rack.slots, weak, RACK_SLOTS);
    end_round(g, 1);
    CHECK(g->match_winner == 1);
    CHECK(g->phase == PHASE_ROUND_OVER);   // scoring screen first...
    game_next_round(g);
    CHECK(g->phase == PHASE_MATCH_OVER);   // ...then the match banner
    CHECK(g->events & EV_MATCH_END);
    CHECK(game_is_over(g));

    // Short of the target: the match continues, dealer rotates, fresh deal.
    g = fresh(3, 32);
    int dealer = g->dealer;
    memcpy(g->players[1].rack.slots, ordered, RACK_SLOTS);
    end_round(g, 1);
    CHECK(g->match_winner == NO_WINNER);
    game_next_round(g);
    CHECK(g->phase == PHASE_DEAL);
    CHECK(g->dealer == (dealer + 1) % 3);
    CHECK(g->round_no == 2);
    skip_deal(g);
    CHECK(g->discard_count == 1);

    // Two players cross together: the higher total takes the match.
    g = fresh(3, 33);
    g->players[0].score = 450;             // +50 (fully ordered loser) -> 500
    g->players[1].score = 440;             // +75 -> 515
    memcpy(g->players[0].rack.slots, ordered, RACK_SLOTS);
    memcpy(g->players[1].rack.slots, ordered, RACK_SLOTS);
    memcpy(g->players[2].rack.slots, weak, RACK_SLOTS);
    end_round(g, 1);
    CHECK(g->match_winner == 1);

    // A dead tie at the target goes to the seat that went out.
    g = fresh(3, 34);
    g->players[0].score = 450;             // +50 -> 500
    g->players[1].score = 425;             // +75 -> 500
    memcpy(g->players[0].rack.slots, ordered, RACK_SLOTS);
    memcpy(g->players[1].rack.slots, ordered, RACK_SLOTS);
    memcpy(g->players[2].rack.slots, weak, RACK_SLOTS);
    end_round(g, 1);
    CHECK(g->match_winner == 1);

    // Partners: pair scores sum after each round; the first pair to 500 wins.
    Rules pr = test_rules(4, 35);
    pr.partners = true;
    g = game_create(&pr);
    skip_deal(g);
    g->players[0].score = 300;
    g->players[2].score = 150;             // pair 0: 450 + 75 = 525
    g->players[1].score = 200;
    g->players[3].score = 100;             // pair 1: 300 + change
    memcpy(g->players[0].rack.slots, ordered, RACK_SLOTS);
    memcpy(g->players[1].rack.slots, weak, RACK_SLOTS);
    memcpy(g->players[2].rack.slots, weak, RACK_SLOTS);
    memcpy(g->players[3].rack.slots, weak, RACK_SLOTS);
    end_round(g, 0);
    CHECK(g->match_winner == 0);           // pair index under Partners

    // Stalemate cutoff: the draw that would force a fourth turn-over ends the
    // round instead — base points all around, no 75, match not decided.
    g = fresh(3, 36);
    memcpy(g->players[0].rack.slots, ordered, RACK_SLOTS);
    memcpy(g->players[1].rack.slots, weak, RACK_SLOTS);
    memcpy(g->players[2].rack.slots, weak, RACK_SLOTS);
    g->recycles = 3;
    g->stock_count = 0;
    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(g->phase == PHASE_ROUND_OVER);
    CHECK(g->round_winner == NO_WINNER);
    CHECK(g->round_points[0] == 50);
    CHECK(g->round_points[1] == 5);
    CHECK(g->match_winner == NO_WINNER);

    // With the cutoff disabled the fourth recycle happens and play goes on.
    Rules nr = test_rules(3, 37);
    nr.stalemate_cutoff = false;
    g = game_create(&nr);
    skip_deal(g);
    uint8_t pile[4] = {8, 3, 11, 25};
    memcpy(g->discard, pile, 4);
    g->discard_count = 4;
    g->stock_count = 0;
    g->recycles = 3;
    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(g->phase == PHASE_PLACE);
    CHECK(g->recycles == 4);
    CHECK(g->held_card == 8);

    // Engine backstop: a round that reaches the per-seat turn cap is called as
    // a stalemate (keeps a theoretical loop from wedging the match).
    g = fresh(3, 38);
    g->turns_taken[g->turn] = TURN_CAP - 1;
    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(game_apply(g, A(ACTION_DISCARD, 0)));
    CHECK(g->phase == PHASE_ROUND_OVER);
    CHECK(g->round_winner == NO_WINNER);

    // The cap holds even with the stalemate cutoff off: deterministic AIs
    // trading an unshuffled pile can provably cycle forever, and disabling
    // the official-rules cutoff must not also disable the wedge guard.
    Rules cr = test_rules(2, 39);
    cr.stalemate_cutoff = false;
    g = game_create(&cr);
    skip_deal(g);
    g->turns_taken[g->turn] = TURN_CAP - 1;
    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    CHECK(game_apply(g, A(ACTION_DISCARD, 0)));
    CHECK(g->phase == PHASE_ROUND_OVER);
    CHECK(g->round_winner == NO_WINNER);
}

// --- Determinism -------------------------------------------------------------
// A deterministic pseudo-driver: picks a legal action from a tiny LCG, so two
// runs with the same seed replay the identical action list.
static uint32_t s_lcg;
static uint32_t lcg(void) {
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg >> 16;
}

static void drive_scripted(Game* g, int actions) {
    for (int i = 0; i < actions; i++) {
        if (g->phase == PHASE_ROUND_OVER) {
            game_next_round(g);
            if (g->phase == PHASE_DEAL) skip_deal(g);
            continue;
        }
        if (g->phase == PHASE_MATCH_OVER) return;
        Action a;
        if (g->phase == PHASE_DRAW) {
            a = (lcg() % 3 == 0) ? A(ACTION_DRAW_DISCARD, 0) : A(ACTION_DRAW_STOCK, 0);
            if (!game_action_legal(g, a)) a = A(ACTION_DRAW_DISCARD, 0);
        } else {
            uint32_t r = lcg() % 12;
            if (r >= 10 && game_action_legal(g, A(ACTION_DISCARD, 0))) {
                a = A(ACTION_DISCARD, 0);
            } else {
                a = A(ACTION_PLACE, (int)(lcg() % RACK_SLOTS));
            }
        }
        CHECK(game_apply(g, a));
    }
}

static void test_determinism(void) {
    uint8_t snap1[sizeof(Game)], snap2[sizeof(Game)];

    s_lcg = 2024;
    Game* g = fresh(4, 555);
    drive_scripted(g, 400);
    memcpy(snap1, g, GAME_SERIAL_SIZE);

    s_lcg = 2024;
    g = fresh(4, 555);
    drive_scripted(g, 400);
    memcpy(snap2, g, GAME_SERIAL_SIZE);

    CHECK(memcmp(snap1, snap2, GAME_SERIAL_SIZE) == 0);

    // A different seed diverges (the shuffle actually depends on it).
    s_lcg = 2024;
    g = fresh(4, 556);
    drive_scripted(g, 400);
    memcpy(snap2, g, GAME_SERIAL_SIZE);
    CHECK(memcmp(snap1, snap2, GAME_SERIAL_SIZE) != 0);
}

// --- Serialization -----------------------------------------------------------
// The no-pointers invariant for the v2 server: a memcpy of the serialized
// prefix is a complete snapshot, and a game restored from one continues
// exactly like the original.
static void test_serialization(void) {
    uint8_t buf[sizeof(Game)];

    s_lcg = 77;
    Game* g = fresh(4, 888);
    drive_scripted(g, 25);
    memcpy(buf, g, GAME_SERIAL_SIZE);

    Game restored;
    memset(&restored, 0xAB, sizeof restored);   // scribble: nothing may leak through
    memcpy(&restored, buf, GAME_SERIAL_SIZE);
    memset(&restored.anim, 0, sizeof restored.anim);
    CHECK(memcmp(&restored, buf, GAME_SERIAL_SIZE) == 0);

    // Same scripted continuation on both copies stays byte-identical.
    s_lcg = 4242;
    drive_scripted(g, 60);
    s_lcg = 4242;
    drive_scripted(&restored, 60);
    CHECK(memcmp(g, &restored, GAME_SERIAL_SIZE) == 0);
}

// --- Redacted view -----------------------------------------------------------
static void test_view_redaction(void) {
    Game* g = fresh(4, 4321);

    // A stock-drawn card is visible only in the holder's own view.
    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    int actor = g->turn;
    GameView own = game_view_for(g, actor);
    GameView other = game_view_for(g, (actor + 1) % 4);
    CHECK(own.held_card == g->held_card);
    CHECK(other.held_card == 0);
    CHECK(game_apply(g, A(ACTION_PLACE, 0)));

    // A discard-drawn card was taken face up: public in every view.
    CHECK(game_apply(g, A(ACTION_DRAW_DISCARD, 0)));
    actor = g->turn;
    own = game_view_for(g, actor);
    other = game_view_for(g, (actor + 1) % 4);
    CHECK(own.held_card == g->held_card);
    CHECK(other.held_card == g->held_card);
    CHECK(other.last_taken[actor] == g->held_card);
    CHECK(game_apply(g, A(ACTION_PLACE, 3)));

    // The seed must never reach a view: replaying the seeded shuffle from it
    // reconstructs every hidden rack and the stock order.
    CHECK(own.rules.seed == 0);
    CHECK(other.rules.seed == 0);

    // The view mirrors the public table and own rack, nothing else: the
    // GameView struct has no field for another seat's rack, which is the
    // structural enforcement the plan calls for.
    GameView v = game_view_for(g, 2);
    CHECK(memcmp(v.own_rack.slots, g->players[2].rack.slots, RACK_SLOTS) == 0);
    CHECK(v.top_discard == g->discard[g->discard_count - 1]);
    CHECK(v.stock_count == g->stock_count);
    CHECK(v.discard_history_count == g->discard_count);
    CHECK(memcmp(v.discard_history, g->discard, g->discard_count) == 0);
}

// --- Pooling + server-side snapshot redaction --------------------------------
static void test_init_and_redact(void) {
    // game_init into caller storage is byte-identical to game_create (the v2
    // daemon holds many games; the static instance is only for local play).
    Rules r = test_rules(4, 31337);
    Game pooled;
    game_init(&pooled, &r);
    Game* st = game_create(&r);
    CHECK(memcmp(&pooled, st, GAME_SERIAL_SIZE) == 0);

    Game* g = fresh(4, 31338);
    int actor = g->turn;
    int other = (actor + 1) % 4;

    // Mid-round, stock-drawn card held: the holder sees it, nobody else does.
    CHECK(game_apply(g, A(ACTION_DRAW_STOCK, 0)));
    Game red;
    game_redact_for(&red, g, actor);
    CHECK(red.rng == 0 && red.rules.seed == 0);
    CHECK(red.held_card == g->held_card);
    CHECK(red.stock_count == g->stock_count);
    for (int i = 0; i < red.stock_count; i++) CHECK(red.stock[i] == 0);
    CHECK(memcmp(red.players[actor].rack.slots,
                 g->players[actor].rack.slots, RACK_SLOTS) == 0);
    for (int s = 0; s < RACK_SLOTS; s++) CHECK(red.players[other].rack.slots[s] == 0);
    CHECK(red.anim.frames == 0 && red.anim.kind == ANIM_NONE);

    game_redact_for(&red, g, other);
    CHECK(red.held_card == 0);                       // hidden held card
    CHECK(red.phase == PHASE_PLACE);                 // ...but its existence shows
    for (int s = 0; s < RACK_SLOTS; s++) CHECK(red.players[actor].rack.slots[s] == 0);
    CHECK(memcmp(red.players[other].rack.slots,
                 g->players[other].rack.slots, RACK_SLOTS) == 0);

    // A discard-drawn card was taken face up: public in every seat's snapshot.
    CHECK(game_apply(g, A(ACTION_PLACE, 3)));
    CHECK(game_apply(g, A(ACTION_DRAW_DISCARD, 0)));
    game_redact_for(&red, g, (g->turn + 1) % 4);
    CHECK(red.held_card == g->held_card);

    // The discard pile and public per-seat facts survive redaction.
    CHECK(red.discard_count == g->discard_count);
    CHECK(memcmp(red.discard, g->discard, g->discard_count) == 0);
    CHECK(memcmp(red.turns_taken, g->turns_taken, sizeof g->turns_taken) == 0);

    // At the reveal every rack is public — that's the physical table rule.
    g->phase = PHASE_ROUND_OVER;
    game_redact_for(&red, g, other);
    for (int i = 0; i < 4; i++) {
        CHECK(memcmp(red.players[i].rack.slots,
                     g->players[i].rack.slots, RACK_SLOTS) == 0);
    }
    CHECK(red.rng == 0 && red.rules.seed == 0);      // never, in any phase
}

// --- Fixed-timestep accumulator (tick.c) -------------------------------------
static void test_sim_clock(void) {
    SimClock c = {0};

    // Exactly one step's worth of time -> one step, nothing banked.
    CHECK(sim_clock_advance(&c, SIM_DT) == 1);
    CHECK(c.accum == 0.0);

    // Two steps' worth -> two steps.
    c.accum = 0.0;
    CHECK(sim_clock_advance(&c, 2 * SIM_DT) == 2);

    // Half a step banks with no step; the following half completes one.
    c.accum = 0.0;
    CHECK(sim_clock_advance(&c, SIM_DT / 2) == 0);
    CHECK(sim_clock_advance(&c, SIM_DT / 2) == 1);

    // A 120 Hz display (half-steps) averages to exactly 60 steps over one second.
    c.accum = 0.0;
    int total = 0;
    for (int i = 0; i < 120; i++) total += sim_clock_advance(&c, SIM_DT / 2);
    CHECK(total == 60);

    // Spiral guard: a long stall runs at most SIM_MAX_STEPS and drops the backlog.
    c.accum = 0.0;
    CHECK(sim_clock_advance(&c, 100.0) == SIM_MAX_STEPS);
    CHECK(c.accum == 0.0);

    // A stalled / backward clock contributes nothing.
    c.accum = 0.0;
    CHECK(sim_clock_advance(&c, -1.0) == 0);
    CHECK(c.accum == 0.0);

    c.accum = 1.0;
    sim_clock_reset(&c);
    CHECK(c.accum == 0.0);
}

int main(void) {
    printf("test_game: rules, deal, legality, exchange, recycle, going out,\n");
    printf("           scoring, bonus, match flow, determinism, serialization,\n");
    printf("           view redaction, fixed-timestep accumulator\n");
    test_rules_normalize();
    test_deal_invariants();
    test_deal_order();
    test_legality();
    test_exchange();
    test_recycle();
    test_going_out();
    test_base_scoring();
    test_bonus_scoring();
    test_match();
    test_determinism();
    test_serialization();
    test_view_redaction();
    test_init_and_redact();
    test_sim_clock();
    if (failures == 0) {
        printf("OK: all checks passed\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
