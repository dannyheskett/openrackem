// Unit tests for the AI. The real suite (legality from every reachable phase
// across thousands of seeded positions, full-match termination, difficulty
// ordering) lands in M2; this scaffold checks the stub responds in both
// decision shapes.
//
// Built and run by `make test`. A non-zero exit means a failure.

#include "rules.c"
#include "game.c"
#include "ai.c"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_stub_shapes(void) {
    GameView v;
    memset(&v, 0, sizeof v);
    v.rules = rules_default();
    uint64_t rng = 42;

    // No held card: a draw decision.
    v.held_card = 0;
    Action a = ai_choose(&v, &rng);
    CHECK(a.type == ACTION_DRAW_STOCK || a.type == ACTION_DRAW_DISCARD);

    // Holding a card: a placement decision.
    v.held_card = 17;
    a = ai_choose(&v, &rng);
    CHECK(a.type == ACTION_PLACE || a.type == ACTION_DISCARD);
    if (a.type == ACTION_PLACE) CHECK(a.slot < RACK_SLOTS);
}

int main(void) {
    printf("test_ai: stub decision shapes (M0 scaffold; the AI suite lands in M2)\n");
    test_stub_shapes();
    if (failures == 0) {
        printf("OK: all checks passed\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
