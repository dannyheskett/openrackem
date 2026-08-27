// End-to-end online test (plan N2): starts the real orserverd daemon on a
// loopback port, connects two real netgame clients through the actual POSIX
// WebSocket transport, and plays a full match to completion. Exercises the
// whole stack — TCP, the RFC 6455 framing on both sides, the wire protocol,
// redaction, and state reconstruction — that the hermetic unit tests stub out.
//
// Not part of `make test` (it forks a process and binds a port). Run with
// `make net-e2e`, which builds the daemon first. Non-zero exit = failure.
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

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// Pump a client one "frame". The daemon paces AI/turns on a 250 ms tick, so a
// few ms of real sleep per pump keeps this from busy-spinning while staying
// far faster than real play.
static unsigned pump(NetGame* ng) {
    unsigned ev = netgame_update(ng);
    sleep_ms(2);
    return ev;
}

// Drive one client toward completing its own turns; opponents' turns and the
// deal are the server's job. Returns when the match is over or a step budget
// is exhausted.
int main(void) {
    signal(SIGPIPE, SIG_IGN);

    // Pick a port unlikely to collide; hand it to the child via PORT.
    int port = 19000 + (int)(getpid() % 4000);
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

    // Two clients: A creates a 2-player room, B joins by its code.
    NetGame a, b;
    Rules r = rules_default();
    r.player_count = 2;
    r.target_score = 50;    // shortest match: one player going out decides it
    netgame_start(&a, "127.0.0.1", port, false, NG_JOIN_CREATE, NULL, &r, "Alice");

    // Pump A until it has a room code.
    for (int i = 0; i < 2000 && a.code[0] == '\0'; i++) pump(&a);
    CHECK(a.code[0] != '\0');
    printf("  room code: %s\n", a.code);

    netgame_start(&b, "127.0.0.1", port, false, NG_JOIN_CODE, a.code, NULL, "Bob");

    // Play. On its own turn each client runs the real AI over its redacted
    // view (its own rack and the public table are all ai_choose needs), so
    // rounds actually resolve — someone goes out — instead of grinding to the
    // stalemate cap. The server stays authoritative; a client only ever acts
    // on its own turn, and only when no action is already in flight.
    uint64_t rng_a = 111, rng_b = 222;
    bool over = false;
    bool names_checked = false;
    int last_round = 0;
    for (int step = 0; step < 40000 && !over; step++) {
        pump(&a);
        pump(&b);
        // Each client should see the other's chosen name as that seat's handle.
        if (a.have_game && b.have_game && !names_checked) {
            names_checked = true;
            printf("  handles: A sees seat %d='%s'; B sees seat %d='%s'\n",
                   b.my_seat, a.handles[b.my_seat], a.my_seat, b.handles[a.my_seat]);
            CHECK(strcmp(a.handles[b.my_seat], "Bob") == 0);
            CHECK(strcmp(b.handles[a.my_seat], "Alice") == 0);
        }
        if (a.have_game && a.game.round_no != last_round) {
            last_round = a.game.round_no;
            printf("  round %d  scores %d/%d\n", last_round,
                   a.game.players[0].score, a.game.players[1].score);
        }

        int at_match_over = 0;
        for (int who = 0; who < 2; who++) {
            NetGame* ng = who ? &b : &a;
            uint64_t* rng = who ? &rng_b : &rng_a;
            if (!ng->have_game) continue;

            if (netgame_my_turn(ng) && !ng->pending) {
                GameView v = game_view_for(&ng->game, ng->my_seat);
                netgame_action(ng, ai_choose(&v, rng));
            } else if (ng->state == NG_PLAYING && ng->game.phase == PHASE_ROUND_OVER) {
                netgame_confirm(ng);
            }
            if (ng->state == NG_PLAYING && ng->game.phase == PHASE_MATCH_OVER) at_match_over++;
            if (ng->state == NG_ERROR) { printf("  client %d error: %s\n", who, ng->err); CHECK(0); over = true; }
        }
        // Finish only when BOTH clients have seen the match-over state, so the
        // final round's confirmations have gone through on both.
        if (at_match_over == 2) over = true;
    }

    CHECK(over);
    CHECK(a.state == NG_PLAYING && a.game.phase == PHASE_MATCH_OVER);
    CHECK(b.game.phase == PHASE_MATCH_OVER);
    // Both clients agree on who won (authoritative broadcast).
    CHECK(a.game.match_winner == b.game.match_winner);
    CHECK(a.game.match_winner != NO_WINNER);
    printf("  match winner: seat %d (scores %d/%d)\n", a.game.match_winner,
           a.game.players[0].score, a.game.players[1].score);

    netgame_close(&a);
    netgame_close(&b);
    kill(srv, SIGTERM);
    waitpid(srv, NULL, 0);

    if (failures == 0) { printf("OK: full online match played over loopback\n"); return 0; }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
