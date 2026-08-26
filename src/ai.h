#ifndef OPENRACKEM_AI_H
#define OPENRACKEM_AI_H

#include "game.h"

// The AI consumes a redacted GameView and returns one legal Action: a draw
// decision when no card is held, a placement decision when one is. It has no
// access to hidden state — enforced by the view type, not by discipline. `rng`
// is the caller's xorshift64* state (the game's own, so play stays
// reproducible from the seed).
Action ai_choose(const GameView* v, uint64_t* rng);

#endif // OPENRACKEM_AI_H
