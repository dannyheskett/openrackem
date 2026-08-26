// M0 scaffold stub: the heuristic evaluator and difficulty tiers land in M2.
#include "ai.h"

Action ai_choose(const GameView* v, uint64_t* rng) {
    (void)rng;
    if (v->held_card) {
        return (Action){ACTION_PLACE, 0};
    }
    return (Action){ACTION_DRAW_STOCK, 0};
}
