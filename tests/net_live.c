// Live-server smoke test for a net.h backend: two headless clients connect to
// the real deployed daemon, create + join a room (which auto-starts, no queue
// wait), and confirm the full transport works — the wss/TLS handshake, the
// protocol, and bidirectional message flow (both clients receive authoritative
// state). Optionally (OR_LIVE_FULLMATCH=1) plays the whole match with the AI.
//
// Portable across backends: it drives everything through netgame/net.h, so the
// Makefile links whichever net_*.c the platform uses (net_posix+OpenSSL on
// Linux, net_apple+Network.framework on macOS). This is how the macOS CI job
// functionally verifies net_apple.mm against openrackem-server.fly.dev.
//
// Env: H=host (default openrackem-server.fly.dev), P=port (443), TLS=1.
// Exit 0 = both clients reached a live game (and finished, if full-match).
#define _POSIX_C_SOURCE 200809L

#include "netgame.h"
#include "ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void ms(int m) { Sleep((DWORD)m); }
#else
#include <time.h>
static void ms(int m) {
    struct timespec t = { m / 1000, (long)(m % 1000) * 1000000L };
    nanosleep(&t, NULL);
}
#endif

int main(void) {
    const char* host = getenv("H") ? getenv("H") : "openrackem-server.fly.dev";
    int  port = getenv("P")   ? atoi(getenv("P")) : 443;
    bool tls  = getenv("TLS") ? atoi(getenv("TLS")) != 0 : true;
    bool full = getenv("OR_LIVE_FULLMATCH") != NULL;

    printf("net_live: 2 clients -> %s:%d (tls=%d)%s\n", host, port, tls,
           full ? " full match" : "");

    NetGame a, b;
    Rules r = rules_default();
    r.player_count = 2;
    r.target_score = 50;
    netgame_start(&a, host, port, tls, NG_JOIN_CREATE, NULL, &r);

    // A creates the room; wait (bounded) for its code.
    for (int i = 0; i < 5000 && a.code[0] == '\0'; i++) {
        netgame_update(&a); ms(3);
        if (a.state == NG_ERROR) { printf("FAIL: A error: %s\n", a.err); return 1; }
    }
    if (a.code[0] == '\0') { printf("FAIL: no room code (connect failed)\n"); return 1; }
    printf("  created room %s\n", a.code);

    netgame_start(&b, host, port, tls, NG_JOIN_CODE, a.code, NULL);

    // Both clients must reach a live game (state received => full round trip).
    int64_t deadline_steps = 20000;   // ~60s of pumping
    uint64_t rng_a = 1, rng_b = 2;
    bool a_live = false, b_live = false, over = false;
    for (int64_t step = 0; step < deadline_steps && !over; step++) {
        netgame_update(&a);
        netgame_update(&b);
        ms(3);
        if (a.state == NG_ERROR) { printf("FAIL: A error: %s\n", a.err); return 1; }
        if (b.state == NG_ERROR) { printf("FAIL: B error: %s\n", b.err); return 1; }
        if (a.have_game) a_live = true;
        if (b.have_game) b_live = true;

        if (!full) {
            if (a_live && b_live) break;   // transport proven
            continue;
        }
        // Full match: play both seats with the AI until match over.
        int done = 0;
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
            if (ng->state == NG_PLAYING && ng->game.phase == PHASE_MATCH_OVER) done++;
        }
        if (done == 2) over = true;
    }

    if (!a_live || !b_live) {
        printf("FAIL: clients did not reach a live game (a=%d b=%d)\n", a_live, b_live);
        return 1;
    }
    printf("  both clients live: seats %d/%d, phase %d\n",
           a.my_seat, b.my_seat, a.have_game ? a.game.phase : -1);
    if (full && over) printf("  full match finished: winner seat %d\n", a.game.match_winner);

    netgame_close(&a);
    netgame_close(&b);
    printf("OK: live wss round-trip verified\n");
    return 0;
}
