// Unit tests for openrackem's rules and game logic. These exercise the
// simulation in isolation — no raylib, no window — by including the sources
// directly so the file-static helpers are visible.
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

int main(void) {
    printf("test_game: rules normalization (M0 scaffold; the engine suite lands in M1)\n");
    test_rules_normalize();
    if (failures == 0) {
        printf("OK: all checks passed\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
