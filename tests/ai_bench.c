// AI difficulty benchmark (docs/PLAN.md §15): plays batches of seeded
// head-to-head matches between tiers and reports win rates, so tuning changes
// to the evaluator are measured instead of guessed. Not part of `make test` —
// run it with `make ai-bench`.
//
// Usage: ai_bench [matches-per-pairing]   (default 500)

#include "rules.c"
#include "game.c"
#include "ai.c"

#include <stdio.h>
#include <stdlib.h>

static void skip_deal(Game* g) {
    for (int i = 0; i < 4096 && g->phase == PHASE_DEAL; i++) game_update(g);
}

// One full AI-only match, each seat at its own difficulty. Returns the winner
// seat, or -1 on a step-cap blowout (shouldn't happen; counted separately).
static int play_match(int players, uint64_t seed, const int diffs[MAX_PLAYERS]) {
    Rules r = rules_default();
    r.player_count = players;
    r.human_seat = 0;
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
        if (!game_apply(g, a)) return -1;
    }
    return -1;
}

static const char* NAME[3] = {"easy", "normal", "hard"};

int main(int argc, char** argv) {
    int matches = (argc > 1) ? atoi(argv[1]) : 500;
    if (matches < 2) matches = 2;

    printf("ai-bench: %d two-player matches per pairing (seat-alternated)\n\n", matches);
    printf("%-16s %8s %8s %8s\n", "pairing", "A wins", "B wins", "A rate");
    for (int da = 0; da < 3; da++) {
        for (int db = da; db < 3; db++) {
            int a_wins = 0, b_wins = 0, errors = 0;
            for (int m = 0; m < matches; m++) {
                int a_seat = m % 2; // alternate seating to cancel first-mover edge
                int diffs[MAX_PLAYERS] = {0};
                diffs[a_seat] = da;
                diffs[1 - a_seat] = db;
                int w = play_match(2, 1000003ULL * (uint64_t)(m + 1) + (uint64_t)(da * 3 + db), diffs);
                if (w < 0) { errors++; continue; }
                if (w == a_seat) a_wins++; else b_wins++;
            }
            char label[32];
            snprintf(label, sizeof label, "%s vs %s", NAME[da], NAME[db]);
            printf("%-16s %8d %8d %7.1f%%", label, a_wins, b_wins,
                   100.0 * a_wins / (a_wins + b_wins));
            if (errors) printf("   (%d errors!)", errors);
            printf("\n");
        }
    }

    // One four-player mixed table: hard, normal, easy, normal.
    printf("\n%d four-player mixed matches (hard/normal/easy/normal):\n", matches);
    int wins[MAX_PLAYERS] = {0};
    int mixed[MAX_PLAYERS] = {2, 1, 0, 1};
    for (int m = 0; m < matches; m++) {
        int w = play_match(4, 777001ULL * (uint64_t)(m + 1), mixed);
        if (w >= 0 && w < 4) wins[w]++;
    }
    for (int s = 0; s < 4; s++) {
        printf("  seat %d (%s): %d wins\n", s, NAME[mixed[s]], wins[s]);
    }
    return 0;
}
