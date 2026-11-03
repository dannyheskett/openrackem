// Multi-player online test: starts the real orserverd daemon on a loopback
// port and drives real netgame clients — each an independent session playing
// with the actual AI over its own redacted view — through every room flow:
//
//   1. Rooms of 2/3/4 players: browse the list, join, ready, play a match to
//      the end, and land back in the same waiting room with the result. Every
//      client agrees on the winner and sees every other client's name.
//   2. The same room, three matches back to back, with a player leaving and
//      another arriving between games.
//   3. A Partners room: teams, a switch that trades two players, a match.
//   4. A player steps away mid-match, the others hand the seat to the AI,
//      and the player comes back.
//   5. A player is away when the match ends: they land in the waiting room
//      with everyone else.
//   6. The daemon restarts mid-match (snapshot on SIGTERM, reload at boot) and
//      every client rejoins its seat by key and finishes the match; then it
//      restarts again while the room waits between games.
//
// Not part of `make test` (forks a process, binds a port). Run with
// `make net-multi`; it gates the server deploy. Non-zero exit = failure.
#define _POSIX_C_SOURCE 200809L

#include "netgame.h"
#include "ai.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_BOTS 4

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

typedef struct {
    NetGame  ng;
    uint64_t rng;
    char     name[8];
    bool     autoplay;   // plays its turns and confirms round-overs
} Bot;

static Bot  s_bots[MAX_BOTS];
static int  s_port;
static char s_portstr[16];
static char s_datadir[64];
static pid_t s_srv = -1;

static void start_daemon(void) {
    s_srv = fork();
    if (s_srv == 0) {
        setenv("PORT", s_portstr, 1);
        setenv("OR_DATA_DIR", s_datadir, 1);
        execl("build/orserverd", "orserverd", (char*)NULL);
        _exit(127);
    }
    CHECK(s_srv > 0);
    sleep_ms(300);   // let it bind/listen
}

static void stop_daemon(void) {
    if (s_srv > 0) {
        kill(s_srv, SIGTERM);
        waitpid(s_srv, NULL, 0);
        s_srv = -1;
    }
}

static void bot_start(int i, const char* rejoin_key) {
    Bot* b = &s_bots[i];
    if (!rejoin_key) {
        b->rng = 0x9E3779B97F4A7C15ULL * (uint64_t)(i + 1) + 7;
        snprintf(b->name, sizeof b->name, "P%d", i);
        b->autoplay = true;
    }
    netgame_start(&b->ng, "127.0.0.1", s_port, false, b->name, rejoin_key);
}

// One pump of every bot: network, then whatever an autoplaying bot does.
static void pump(int n) {
    for (int i = 0; i < n; i++) {
        Bot* b = &s_bots[i];
        netgame_update(&b->ng);
        if (!b->autoplay) continue;
        NetGame* ng = &b->ng;
        if (ng->state != NG_PLAYING || !ng->have_game) continue;
        if (netgame_my_turn(ng) && !ng->pending) {
            GameView v = game_view_for(&ng->game, ng->my_seat);
            netgame_action(ng, ai_choose(&v, &b->rng));
        } else if (ng->game.phase == PHASE_ROUND_OVER) {
            netgame_confirm(ng);
        }
    }
    sleep_ms(2);
}

// Pump until `cond` holds (or give up after `steps` pumps).
#define WAIT(n, cond, steps) do { \
    for (int _s = 0; _s < (steps) && !(cond); _s++) pump(n); } while (0)

static bool all_state(int n, NgState st) {
    for (int i = 0; i < n; i++) if (s_bots[i].ng.state != st) return false;
    return true;
}

static bool any_error(int n) {
    for (int i = 0; i < n; i++) {
        if (s_bots[i].ng.state == NG_ERROR) {
            printf("  client %d error: %s\n", i, s_bots[i].ng.err);
            return true;
        }
    }
    return false;
}

static bool all_in_lobby_of(int n, long id) {
    for (int i = 0; i < n; i++) {
        if (s_bots[i].ng.state != NG_LOBBY || !s_bots[i].ng.have_lobby) return false;
        if (s_bots[i].ng.lobby.id != id) return false;
    }
    return true;
}

static int lobby_count(const NetGame* ng) {
    int k = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) k += ng->lobby.used[i];
    return k;
}

// Bot `i` finds room `id` in the list and joins it.
static void join_via_list(int n, int i, long id) {
    netgame_list(&s_bots[i].ng);
    bool seen = false;
    for (int s = 0; s < 3000 && !seen; s++) {
        pump(n);
        for (int k = 0; k < s_bots[i].ng.room_count; k++) seen |= s_bots[i].ng.rooms[k].id == id;
    }
    CHECK(seen);
    netgame_join_room(&s_bots[i].ng, id);
    WAIT(n, s_bots[i].ng.state == NG_LOBBY, 3000);
    CHECK(s_bots[i].ng.state == NG_LOBBY);
}

// Everyone readies; the deal follows the countdown.
static void ready_and_start(int n) {
    for (int i = 0; i < n; i++) netgame_ready(&s_bots[i].ng, true);
    WAIT(n, all_state(n, NG_PLAYING), 4000);
    CHECK(all_state(n, NG_PLAYING));
}

// Play until every present bot is back in the waiting room with the result.
static void play_to_lobby(int n) {
    bool back = false;
    for (int s = 0; s < 150000 && !back && !any_error(n); s++) {
        pump(n);
        back = true;
        for (int i = 0; i < n; i++) {
            if (s_bots[i].ng.state != NG_LOBBY || !s_bots[i].ng.have_result) back = false;
        }
    }
    CHECK(back);
    int mw = s_bots[0].ng.game.match_winner;
    for (int i = 0; i < n; i++) {
        CHECK(s_bots[i].ng.game.phase == PHASE_MATCH_OVER);
        CHECK(s_bots[i].ng.game.match_winner == mw);   // all agree on the winner
        CHECK(!s_bots[i].ng.have_game);                // and are in the waiting room
    }
    CHECK(mw != NO_WINNER);
    // The winner's total is strictly the top total on every client (a tie
    // never ends a match), and it reached the target. Under Partners the
    // winner is a pair and the totals are pair sums.
    for (int i = 0; i < n; i++) {
        const Game* g = &s_bots[i].ng.game;
        int np = g->rules.player_count;
        if (g->rules.partners) {
            int win = g->players[mw].score + g->players[mw + 2].score;
            int lose = g->players[1 - mw].score + g->players[3 - mw].score;
            CHECK(win > lose && win >= g->rules.target_score);
        } else {
            for (int p = 0; p < np; p++) {
                if (p != mw) CHECK(g->players[mw].score > g->players[p].score);
            }
            CHECK(g->players[mw].score >= g->rules.target_score);
        }
    }
    printf("  match over, winner seat %d; everyone back in the waiting room\n", mw);
}

static void close_all(int n) {
    for (int i = 0; i < n; i++) netgame_close(&s_bots[i].ng);
}

// --- 1. Rooms of 2/3/4 ------------------------------------------------------------
static void scenario_room(int n) {
    printf("== room, %d players ==\n", n);
    for (int i = 0; i < n; i++) bot_start(i, NULL);
    WAIT(n, all_state(n, NG_IDLE), 3000);
    CHECK(all_state(n, NG_IDLE));

    Rules r = rules_default();
    r.target_score = 50;   // one player going out can decide it
    netgame_create(&s_bots[0].ng, &r);
    WAIT(n, s_bots[0].ng.state == NG_LOBBY && s_bots[0].ng.have_lobby, 3000);
    long id = s_bots[0].ng.room_id;
    CHECK(s_bots[0].ng.lobby.target == 50);

    for (int i = 1; i < n; i++) join_via_list(n, i, id);
    WAIT(n, all_in_lobby_of(n, id) && lobby_count(&s_bots[0].ng) == n, 3000);
    CHECK(lobby_count(&s_bots[0].ng) == n);
    for (int i = 0; i < n; i++) {
        CHECK(strcmp(s_bots[0].ng.lobby.names[i], s_bots[i].name) == 0);
    }
    printf("  all %d in the waiting room; names verified\n", n);

    ready_and_start(n);
    WAIT(n, s_bots[n - 1].ng.have_game, 2000);
    CHECK(s_bots[0].ng.game.rules.player_count == n);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i != j) CHECK(strcmp(s_bots[i].ng.handles[s_bots[j].ng.my_seat], s_bots[j].name) == 0);
        }
    }
    play_to_lobby(n);
    CHECK(s_bots[0].ng.lobby.games == 1);
    close_all(n);
}

// --- 2. The same room, three matches, members coming and going -------------------
static void scenario_rematch(void) {
    printf("== one room, three matches ==\n");
    int n = 3;
    for (int i = 0; i < n; i++) bot_start(i, NULL);
    WAIT(n, all_state(n, NG_IDLE), 3000);
    Rules r = rules_default();
    r.target_score = 50;
    netgame_create(&s_bots[0].ng, &r);
    WAIT(n, s_bots[0].ng.have_lobby, 3000);
    long id = s_bots[0].ng.room_id;
    join_via_list(n, 1, id);
    WAIT(n, lobby_count(&s_bots[0].ng) == 2, 3000);
    char key1[NG_KEY];
    snprintf(key1, sizeof key1, "%s", s_bots[1].ng.key);

    // Game one: two players.
    ready_and_start(2);
    play_to_lobby(2);
    CHECK(all_in_lobby_of(2, id));
    CHECK(s_bots[1].ng.lobby.games == 1);
    CHECK(strcmp(s_bots[1].ng.key, key1) == 0);        // the seat key carries over

    // Game two: a third player finds the room (it is listed again) and joins.
    join_via_list(n, 2, id);
    WAIT(n, lobby_count(&s_bots[0].ng) == 3, 3000);
    ready_and_start(3);
    CHECK(s_bots[2].ng.game.rules.player_count == 3);
    for (int i = 0; i < 3; i++) CHECK(s_bots[i].ng.game.players[s_bots[i].ng.my_seat].score == 0);
    play_to_lobby(3);
    CHECK(s_bots[2].ng.lobby.games == 2);
    printf("  three games in, nobody rejoined, keys intact\n");

    // Game three: P1 leaves between games; the other two play on.
    netgame_leave(&s_bots[1].ng);
    WAIT(n, lobby_count(&s_bots[0].ng) == 2 && s_bots[1].ng.state == NG_IDLE, 3000);
    CHECK(lobby_count(&s_bots[0].ng) == 2);
    Bot* pair[2] = {&s_bots[0], &s_bots[2]};
    for (int i = 0; i < 2; i++) netgame_ready(&pair[i]->ng, true);
    bool playing = false;
    for (int s = 0; s < 4000 && !playing; s++) {
        pump(n);
        playing = pair[0]->ng.state == NG_PLAYING && pair[1]->ng.state == NG_PLAYING;
    }
    CHECK(playing);
    bool back = false;
    for (int s = 0; s < 150000 && !back && !any_error(n); s++) {
        pump(n);
        back = pair[0]->ng.have_result && pair[1]->ng.have_result;
    }
    CHECK(back);
    CHECK(pair[0]->ng.lobby.games == 3);
    close_all(n);
}

// --- 3. Partners ------------------------------------------------------------------
static void scenario_partners(void) {
    printf("== partners room with a team switch ==\n");
    int n = 4;
    for (int i = 0; i < n; i++) bot_start(i, NULL);
    WAIT(n, all_state(n, NG_IDLE), 3000);
    Rules r = rules_default();
    r.target_score = 50;
    r.partners = true;
    netgame_create(&s_bots[0].ng, &r);
    WAIT(n, s_bots[0].ng.have_lobby, 3000);
    long id = s_bots[0].ng.room_id;
    for (int i = 1; i < n; i++) join_via_list(n, i, id);
    WAIT(n, lobby_count(&s_bots[0].ng) == 4, 3000);
    NgLobby* L = &s_bots[0].ng.lobby;
    CHECK(L->team[0] == 0 && L->team[1] == 1 && L->team[2] == 0 && L->team[3] == 1);

    // P0 (team A) switches: both teams are full, so P0 trades with team B's
    // most recent member, P3.
    netgame_team(&s_bots[0].ng);
    WAIT(n, s_bots[0].ng.lobby.team[0] == 1, 2000);
    CHECK(s_bots[0].ng.lobby.team[0] == 1 && s_bots[0].ng.lobby.team[3] == 0);
    CHECK(s_bots[0].ng.lobby.team[1] == 1 && s_bots[0].ng.lobby.team[2] == 0);
    printf("  switch traded P0 and P3: teams now P2+P3 vs P0+P1\n");

    ready_and_start(n);
    CHECK(s_bots[0].ng.game.rules.partners);
    // Partners sit opposite: P0 and P1 two seats apart.
    int d = (s_bots[0].ng.my_seat - s_bots[1].ng.my_seat + 4) % 4;
    CHECK(d == 2);
    play_to_lobby(n);
    close_all(n);
}

// --- 4. Stepping away and coming back ----------------------------------------------
static void scenario_step_away(void) {
    printf("== step away, Let AI play, come back ==\n");
    int n = 2;
    for (int i = 0; i < n; i++) bot_start(i, NULL);
    WAIT(n, all_state(n, NG_IDLE), 3000);
    Rules r = rules_default();
    r.target_score = 500;   // long enough to step away and back mid-match
    netgame_create(&s_bots[0].ng, &r);
    WAIT(n, s_bots[0].ng.have_lobby, 3000);
    join_via_list(n, 1, s_bots[0].ng.room_id);
    WAIT(n, all_in_lobby_of(n, s_bots[0].ng.room_id) && lobby_count(&s_bots[0].ng) == 2, 3000);
    ready_and_start(n);

    // P1 steps away and stops playing.
    s_bots[1].autoplay = false;
    netgame_leave(&s_bots[1].ng);
    int away_seat = s_bots[1].ng.my_seat;
    WAIT(n, s_bots[0].ng.ctl[away_seat] == 1, 2000);
    CHECK(s_bots[0].ng.ctl[away_seat] == 1);
    // The table waits at P1's turn; P0 hands the seat to the AI.
    WAIT(n, s_bots[0].ng.wait_seat == away_seat, 20000);
    CHECK(s_bots[0].ng.wait_seat == away_seat && s_bots[0].ng.wait_ms > 0);
    int round = s_bots[0].ng.game.round_no;
    netgame_letai(&s_bots[0].ng, away_seat);
    WAIT(n, s_bots[0].ng.wait_seat == -1 && s_bots[0].ng.game.turn != away_seat, 4000);
    CHECK(s_bots[0].ng.wait_seat == -1);
    printf("  AI took the absent seat (round %d)\n", round);

    // P1 returns: the others hear about it and control comes back.
    netgame_here(&s_bots[1].ng);
    s_bots[1].autoplay = true;
    WAIT(n, s_bots[0].ng.back_frames > 0, 2000);
    CHECK(strcmp(s_bots[0].ng.back_name, "P1") == 0);
    WAIT(n, s_bots[0].ng.ctl[away_seat] == 0, 2000);
    CHECK(s_bots[0].ng.ctl[away_seat] == 0);
    printf("  P1 is back\n");
    close_all(n);
}

// --- 5. Away when the match ends ----------------------------------------------------
static void scenario_away_at_end(void) {
    printf("== away when the match ends ==\n");
    int n = 2;
    for (int i = 0; i < n; i++) bot_start(i, NULL);
    WAIT(n, all_state(n, NG_IDLE), 3000);
    Rules r = rules_default();
    r.target_score = 50;
    netgame_create(&s_bots[0].ng, &r);
    WAIT(n, s_bots[0].ng.have_lobby, 3000);
    long id = s_bots[0].ng.room_id;
    join_via_list(n, 1, id);
    WAIT(n, lobby_count(&s_bots[0].ng) == 2, 3000);
    ready_and_start(n);

    // P1 steps away for good; the AI plays its seat and P0 lets it.
    s_bots[1].autoplay = false;
    netgame_leave(&s_bots[1].ng);
    bool done = false;
    for (int s = 0; s < 150000 && !done && !any_error(n); s++) {
        pump(n);
        if (s_bots[0].ng.wait_seat >= 0) netgame_letai(&s_bots[0].ng, s_bots[0].ng.wait_seat);
        done = s_bots[0].ng.have_result && s_bots[0].ng.state == NG_LOBBY;
    }
    CHECK(done);
    // P1 is in the same waiting room, with the result, not ready.
    WAIT(n, s_bots[1].ng.state == NG_LOBBY && s_bots[1].ng.have_lobby, 3000);
    CHECK(all_in_lobby_of(n, id));
    CHECK(s_bots[1].ng.have_result);
    CHECK(!s_bots[1].ng.lobby.ready[s_bots[1].ng.lobby.me]);
    CHECK(lobby_count(&s_bots[0].ng) == 2);
    printf("  P1 landed in the waiting room with the result\n");
    close_all(n);
}

// --- 6. Daemon restarts -------------------------------------------------------------
static void scenario_restart(void) {
    printf("== daemon restart mid-match, then between games ==\n");
    int n = 3;
    for (int i = 0; i < n; i++) bot_start(i, NULL);
    WAIT(n, all_state(n, NG_IDLE), 3000);
    Rules r = rules_default();
    r.target_score = 50;
    netgame_create(&s_bots[0].ng, &r);
    WAIT(n, s_bots[0].ng.have_lobby, 3000);
    long id = s_bots[0].ng.room_id;
    for (int i = 1; i < n; i++) join_via_list(n, i, id);
    WAIT(n, lobby_count(&s_bots[0].ng) == n, 3000);
    ready_and_start(n);
    for (int s = 0; s < 200; s++) pump(n);   // a few moves in
    int stock_before = s_bots[0].ng.game.stock_count;

    stop_daemon();       // SIGTERM: the snapshot is written
    for (int s = 0; s < 50; s++) pump(n);    // clients notice the drop
    for (int i = 0; i < n; i++) CHECK(s_bots[i].ng.state == NG_DISCONNECTED);
    start_daemon();      // reloads the snapshot
    WAIT(n, all_state(n, NG_PLAYING) && s_bots[0].ng.have_game, 6000);
    CHECK(all_state(n, NG_PLAYING));
    CHECK(s_bots[0].ng.game.stock_count <= stock_before + 60);
    printf("  all %d rejoined after the restart\n", n);
    play_to_lobby(n);

    // Between games: the occupied waiting room survives a restart too.
    stop_daemon();
    for (int s = 0; s < 50; s++) pump(n);
    for (int i = 0; i < n; i++) CHECK(s_bots[i].ng.state == NG_DISCONNECTED);
    start_daemon();
    WAIT(n, all_in_lobby_of(n, id) && lobby_count(&s_bots[0].ng) == n, 6000);
    CHECK(all_in_lobby_of(n, id));
    CHECK(s_bots[0].ng.lobby.games == 1);
    printf("  the waiting room came back with everyone in it\n");
    ready_and_start(n);
    play_to_lobby(n);
    close_all(n);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    s_port = 20000 + (int)(getpid() % 4000);
    snprintf(s_portstr, sizeof s_portstr, "%d", s_port);
    snprintf(s_datadir, sizeof s_datadir, "/tmp/net_multi_%d", (int)getpid());
    mkdir(s_datadir, 0700);
    start_daemon();

    for (int n = 2; n <= 4; n++) scenario_room(n);
    scenario_rematch();
    scenario_partners();
    scenario_step_away();
    scenario_away_at_end();
    scenario_restart();

    stop_daemon();
    char path[96];
    snprintf(path, sizeof path, "%s/rooms.dat", s_datadir);
    unlink(path);
    rmdir(s_datadir);

    if (failures == 0) {
        printf("OK: rooms 2/3/4, three-game room, partners, step-away, away at end, restarts\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
