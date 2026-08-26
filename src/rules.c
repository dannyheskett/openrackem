#include "rules.h"

Rules rules_default(void) {
    Rules r = {
        .player_count         = 4,
        .human_seat           = 0,
        .target_score         = 500,
        .bonus_scoring        = false,
        .partners             = false,
        .reshuffle_on_recycle = false,
        .stalemate_cutoff     = true,
        .ai_difficulty        = 1,
        .seed                 = 0,
    };
    return r;
}

void rules_normalize(Rules* r) {
    if (r->player_count < 2) r->player_count = 2;
    if (r->player_count > 4) r->player_count = 4;

    // -1 (full AI) is valid; anything else clamps to a real seat.
    if (r->human_seat < -1) r->human_seat = -1;
    if (r->human_seat >= r->player_count) r->human_seat = r->player_count - 1;

    // Keep the target sane: a round awards at most 475 (bonus 6+ run), so 50 is
    // the lowest target that still takes a full round to reach.
    if (r->target_score < 50) r->target_score = 50;
    if (r->target_score > 10000) r->target_score = 10000;

    // Partners is defined only for exactly 4 players, seated alternately.
    if (r->player_count != 4) r->partners = false;

    if (r->ai_difficulty < 0) r->ai_difficulty = 0;
    if (r->ai_difficulty > 2) r->ai_difficulty = 2;
}

int rules_deck_size(const Rules* r) {
    switch (r->player_count) {
    case 2:  return 40;
    case 3:  return 50;
    default: return 60;
    }
}

bool rules_require_run(const Rules* r) {
    return r->player_count == 2;
}
