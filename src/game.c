// M0 scaffold stub: the full rules engine lands in M1. Everything compiles and
// links on every platform; game_create produces an inert match so the frame
// loop, renderers, and tests have a real Game to hold.
#include "game.h"
#include <string.h>

Game* game_create(const Rules* rules) {
    static Game game;
    memset(&game, 0, sizeof game);
    game.rules = *rules;
    rules_normalize(&game.rules);
    game.rng = game.rules.seed ? game.rules.seed : 0x9E3779B97F4A7C15ULL;
    game.phase = PHASE_DEAL;
    game.round_winner = NO_WINNER;
    game.match_winner = NO_WINNER;
    game.round_no = 1;
    for (int i = 0; i < game.rules.player_count; i++) {
        game.players[i].is_ai = (i != game.rules.human_seat);
    }
    return &game;
}

void game_destroy(Game* game) {
    (void)game; // static instance; nothing to free
}

bool game_action_legal(const Game* g, Action a) {
    (void)g; (void)a;
    return false; // no rules yet (M1)
}

bool game_apply(Game* g, Action a) {
    (void)g; (void)a;
    return false; // no rules yet (M1)
}

void game_update(Game* g) {
    g->events = 0;
}

void game_next_round(Game* g) {
    (void)g;
}

bool game_is_over(const Game* g) {
    return g->phase == PHASE_MATCH_OVER;
}

int score_sequence(const Rack* r) { (void)r; return 0; }
int score_longest_run(const Rack* r) { (void)r; return 0; }
int score_bonus(int run_len) { (void)run_len; return 0; }
bool rack_is_out(const Rack* r) { (void)r; return false; }

GameView game_view_for(const Game* g, int seat) {
    GameView v;
    memset(&v, 0, sizeof v);
    v.rules = g->rules;
    v.seat = (uint8_t)seat;
    v.player_count = (uint8_t)g->rules.player_count;
    return v;
}
