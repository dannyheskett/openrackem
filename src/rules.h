#ifndef OPENRACKEM_RULES_H
#define OPENRACKEM_RULES_H

#include <stdint.h>
#include <stdbool.h>

// Everything tunable about a match, set once from the menu and passed into
// game_create. Official rules are the defaults. The logic layer never reads
// configuration from anywhere else.
typedef struct {
    int  player_count;        // 2..4 (default 4)
    int  human_seat;          // 0..player_count-1, or -1 for full AI (autoplay/tests)
    int  target_score;        // default 500
    bool bonus_scoring;       // Bonus variant (default off)
    bool partners;            // Partners variant, 4 players only (default off)
    bool reshuffle_on_recycle;// non-official convenience (default off)
    bool stalemate_cutoff;    // end a round with no winner after 3 recycles (default on)
    int  ai_difficulty;       // 0 easy, 1 normal, 2 hard (default 1)
    uint64_t seed;            // 0 = caller seeds from the clock before game_create
} Rules;

// The match target ("Play To"), defined once for the menu, the room screen,
// the engine and the daemon. The range is what any Rules may hold (the low end
// is one round's worth, which the loopback tests use for short matches); the
// choices are what the screens offer.
#define RULES_TARGET_MIN        50
#define RULES_TARGET_MAX        1000
#define RULES_TARGET_CHOICES_N  4
extern const int RULES_TARGET_CHOICES[RULES_TARGET_CHOICES_N];

bool rules_target_valid(int target);

// An online room's settings, as asked for when it is made: its capacity
// ("Seats") is 2 to 4, its target is a valid one, and Partners needs all four
// seats. The room screen keeps to this and the daemon refuses anything else.
#define RULES_SEATS_MIN 2
#define RULES_SEATS_MAX 4
bool rules_room_valid(int seats, int target, bool partners);

Rules rules_default(void);

// Clamps every field into range and resolves the option interactions: partners
// is forced off below 4 players. This is the single place those derivations
// live; game.c consumes an already-normalized Rules.
void rules_normalize(Rules* r);

// Official derivations from player count. Deck subset: 2 players use cards
// 1-40, 3 players 1-50, 4 players 1-60.
int rules_deck_size(const Rules* r);

// The official two-player rule: at 2 players a rack may not go out without a
// run of at least 3 consecutive numbers. Mandatory, not an option.
bool rules_require_run(const Rules* r);

#endif // OPENRACKEM_RULES_H
