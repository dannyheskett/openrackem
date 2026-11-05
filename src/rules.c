#include "rules.h"

const int RULES_TARGET_CHOICES[RULES_TARGET_CHOICES_N] = {250, 500, 750, 1000};

bool rules_target_valid(int target) {
    return target >= RULES_TARGET_MIN && target <= RULES_TARGET_MAX;
}

bool rules_room_valid(int seats, int target, bool partners) {
    if (seats < RULES_SEATS_MIN || seats > RULES_SEATS_MAX) return false;
    if (!rules_target_valid(target)) return false;
    return !partners || seats == RULES_SEATS_MAX;
}

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
    if (r->target_score < RULES_TARGET_MIN) r->target_score = RULES_TARGET_MIN;
    if (r->target_score > RULES_TARGET_MAX) r->target_score = RULES_TARGET_MAX;

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
