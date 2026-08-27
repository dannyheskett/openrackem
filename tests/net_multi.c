// Multi-player online test: starts the real orserverd daemon on a loopback port
// and plays a full match with N real netgame clients — each an independent
// session driven by the actual AI over its own redacted view — for N = 2, 3, 4.
// Verifies, for each size, that the room fills and starts, the match runs to a
// real conclusion (someone goes out each round; a match winner emerges), every
// client agrees on the authoritative winner and phase, and each client's chosen
// name propagates to every other client as that seat's handle.
//
// Not part of `make test` (forks a process, binds a port). Run with
// `make net-multi`. Non-zero exit = failure.
#define _POSIX_C_SOURCE 200809L

#include "netgame.h"
#include "ai.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 4

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// Play a full N-player match against the daemon at 127.0.0.1:port.
static void play_match(int port, int n) {
    printf("== %d-player match ==\n", n);
    NetGame  cl[MAX_CLIENTS];
    uint64_t rng[MAX_CLIENTS];
    char     names[MAX_CLIENTS][8];
    for (int i = 0; i < n; i++) {
        rng[i] = 0x9E3779B97F4A7C15ULL * (uint64_t)(i + 1) + 1;
        snprintf(names[i], sizeof names[i], "P%d", i);
    }

    // Client 0 creates an N-seat room; the rest join by its code. target_score 50
    // keeps the match short (one player going out can decide it).
    Rules r = rules_default();
    r.player_count = n;
    r.target_score = 50;
    netgame_start(&cl[0], "127.0.0.1", port, false, NG_JOIN_CREATE, NULL, &r, names[0]);

    for (int i = 0; i < 3000 && cl[0].code[0] == '\0'; i++) { netgame_update(&cl[0]); sleep_ms(2); }
    if (cl[0].code[0] == '\0') { printf("  FAIL: no room code\n"); failures++; return; }
    printf("  room %s\n", cl[0].code);

    for (int i = 1; i < n; i++)
        netgame_start(&cl[i], "127.0.0.1", port, false, NG_JOIN_CODE, cl[0].code, NULL, names[i]);

    int  last_round = 0;
    bool names_checked = false;
    bool over = false;
    for (int step = 0; step < 120000 && !over; step++) {
        for (int i = 0; i < n; i++) netgame_update(&cl[i]);
        sleep_ms(2);

        for (int i = 0; i < n; i++)
            if (cl[i].state == NG_ERROR) {
                printf("  client %d error: %s\n", i, cl[i].err);
                CHECK(0);
                over = true;
            }
        if (over) break;

        // Once everyone is seated and playing, each client should see every other
        // client's chosen name as that seat's handle (server-broadcast).
        bool all_live = true;
        for (int i = 0; i < n; i++) if (!cl[i].have_game) all_live = false;
        if (all_live && !names_checked) {
            names_checked = true;
            for (int i = 0; i < n; i++)
                for (int j = 0; j < n; j++)
                    if (i != j)
                        CHECK(strcmp(cl[i].handles[cl[j].my_seat], names[j]) == 0);
            printf("  all %d seated; handles verified\n", n);
        }

        if (cl[0].have_game && cl[0].game.round_no != last_round) {
            last_round = cl[0].game.round_no;
            printf("  round %d\n", last_round);
        }

        // Each client acts only on its own turn (AI over its redacted view),
        // confirms at round-over, and the server stays authoritative.
        int done = 0;
        for (int i = 0; i < n; i++) {
            NetGame* ng = &cl[i];
            if (!ng->have_game) continue;
            if (netgame_my_turn(ng) && !ng->pending) {
                GameView v = game_view_for(&ng->game, ng->my_seat);
                netgame_action(ng, ai_choose(&v, &rng[i]));
            } else if (ng->state == NG_PLAYING && ng->game.phase == PHASE_ROUND_OVER) {
                netgame_confirm(ng);
            }
            if (ng->state == NG_PLAYING && ng->game.phase == PHASE_MATCH_OVER) done++;
        }
        if (done == n) over = true;   // every client has the final state
    }

    CHECK(over);
    int mw = cl[0].have_game ? cl[0].game.match_winner : NO_WINNER;
    for (int i = 0; i < n; i++) {
        CHECK(cl[i].game.phase == PHASE_MATCH_OVER);
        CHECK(cl[i].game.match_winner == mw);   // all clients agree on the winner
    }
    CHECK(mw >= 0 && mw < n);
    printf("  winner: seat %d\n", mw);

    for (int i = 0; i < n; i++) netgame_close(&cl[i]);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int port = 20000 + (int)(getpid() % 4000);
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);

    pid_t srv = fork();
    if (srv == 0) {
        setenv("PORT", portstr, 1);
        execl("build/orserverd", "orserverd", (char*)NULL);
        _exit(127);
    }
    CHECK(srv > 0);
    sleep_ms(300);   // let it bind/listen

    for (int n = 2; n <= 4; n++) play_match(port, n);

    kill(srv, SIGTERM);
    waitpid(srv, NULL, 0);

    if (failures == 0) {
        printf("OK: 2/3/4-player online matches all completed\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
