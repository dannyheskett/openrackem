// Unit tests for the online client's state reconstruction (netgame.c): a
// server `state` message must rebuild the exact redacted Game the renderers
// draw. Hermetic — the net backend is faked here, so no sockets are opened;
// the socket path is exercised separately by the loopback e2e (make net-e2e).
//
// Built and run by `make test`. A non-zero exit means a failure.

#include "rules.c"
#include "game.c"
#include "ai.c"
#include "netgame.c"

#include <stdio.h>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// Faked net backend: netgame.c references these, but the parse tests never
// call them. (A real backend links in the app build via the Makefile.)
NetConn* net_connect(const char* h, int p, const char* pa, bool tls) { (void)h;(void)p;(void)pa;(void)tls; return 0; }
void      net_poll(NetConn* c) { (void)c; }
NetStatus net_status(const NetConn* c) { (void)c; return NET_ERROR; }
bool      net_send(NetConn* c, const char* t, size_t l) { (void)c;(void)t;(void)l; return false; }
int       net_recv(NetConn* c, char* b, size_t cap) { (void)c;(void)b;(void)cap; return -1; }
void      net_close(NetConn* c) { (void)c; }
bool      net_available(void) { return true; }

// Build the redacted state the daemon would emit for `seat`, so the client
// parse is checked against the true server-side redaction rather than a
// hand-typed string that could drift.
#include "wire.c"

// A compact re-emit mirroring server_core.c's emit_state, from a redacted Game.
static void emit(char* out, size_t cap, const Game* g, int seat,
                 int last_seat, int last_a, int last_slot, int last_card) {
    Game r;
    game_redact_for(&r, g, seat);
    JW w; jw_init(&w, out, cap);
    int n = r.rules.player_count;
    jw_f(&w, "{\"t\":\"state\",\"v\":1,\"seat\":%d,\"players\":%d,\"phase\":%d,"
             "\"turn\":%d,\"dealer\":%d,\"round\":%d,\"target\":%d,\"bonus\":%d,"
             "\"partners\":%d,\"stock\":%d,", seat, n, r.phase, r.turn, r.dealer,
        r.round_no, r.rules.target_score, r.rules.bonus_scoring?1:0,
        r.rules.partners?1:0, r.stock_count);
    jw_f(&w, "\"discard\":[");
    for (int i=0;i<r.discard_count;i++) jw_f(&w,"%s%d",i?",":"",r.discard[i]);
    jw_f(&w, "],\"held\":%d,\"hfd\":%d,\"rack\":[", r.held_card, r.held_from_discard?1:0);
    for (int i=0;i<RACK_SLOTS;i++) jw_f(&w,"%s%d",i?",":"",r.players[seat].rack.slots[i]);
    jw_f(&w, "],");
    if (r.phase==PHASE_ROUND_OVER || r.phase==PHASE_MATCH_OVER) {
        jw_f(&w,"\"racks\":[");
        for (int p=0;p<n;p++){ jw_f(&w,"%s[",p?",":"");
            for(int i=0;i<RACK_SLOTS;i++) jw_f(&w,"%s%d",i?",":"",r.players[p].rack.slots[i]);
            jw_f(&w,"]"); }
        jw_f(&w,"],");
    }
    jw_f(&w,"\"scores\":[");
    for(int p=0;p<n;p++) jw_f(&w,"%s%d",p?",":"",r.players[p].score);
    jw_f(&w,"],\"round_points\":[");
    for(int p=0;p<n;p++) jw_f(&w,"%s%d",p?",":"",r.round_points[p]);
    jw_f(&w,"],\"last_taken\":[");
    for(int p=0;p<n;p++) jw_f(&w,"%s%d",p?",":"",r.last_taken[p]);
    jw_f(&w,"],\"ai\":[");
    for(int p=0;p<n;p++) jw_f(&w,"%s%d",p?",":"",0);
    jw_f(&w,"],\"winner\":%d,\"match_winner\":%d,\"recycles\":%d,\"events\":%u,",
        r.round_winner, r.match_winner, r.recycles, r.events);
    if (last_seat>=0)
        jw_f(&w,"\"last\":{\"seat\":%d,\"a\":%d,\"slot\":%d,\"card\":%d},",
             last_seat,last_a,last_slot,last_card);
    jw_f(&w,"\"deadline_ms\":%d}", 30000);
}

static void skip_deal(Game* g){ for(int i=0;i<4096&&g->phase==PHASE_DEAL;i++) game_update(g); }

// A live 4-player game a few moves in.
static void make_game(Game* g) {
    Rules r = rules_default();
    r.player_count = 4; r.human_seat = 0; r.seed = 20260826;
    rules_normalize(&r);
    game_init(g, &r);
    skip_deal(g);
    game_apply(g, (Action){ACTION_DRAW_DISCARD, 0});
    game_apply(g, (Action){ACTION_PLACE, 5});
}

static void test_parse_midround(void) {
    Game g; make_game(&g);
    int actor = g.turn;                 // seat about to draw
    int other = (actor + 1) % 4;

    // The acting seat draws from stock: private card held.
    uint8_t stock_top = g.stock[0];
    game_apply(&g, (Action){ACTION_DRAW_STOCK, 0});

    char buf[2048];
    NetGame ng;

    // From the holder's own view: sees the held card, own rack, redacted foes.
    emit(buf, sizeof buf, &g, actor, actor, ACTION_DRAW_STOCK, 0, 0);
    memset(&ng, 0, sizeof ng);
    CHECK(netgame_parse_state(&ng, buf, strlen(buf)));
    CHECK(ng.my_seat == actor);
    CHECK(ng.players == 4);
    CHECK(ng.game.phase == PHASE_PLACE);
    CHECK(ng.game.turn == actor);
    CHECK(ng.game.held_card == stock_top);
    CHECK(ng.game.rules.human_seat == actor);   // renderer shows "YOU" = us
    // Own rack reconstructed exactly.
    CHECK(memcmp(ng.game.players[actor].rack.slots,
                 g.players[actor].rack.slots, RACK_SLOTS) == 0);
    // Opponent racks are hidden (all zero on the wire).
    for (int i = 0; i < RACK_SLOTS; i++) CHECK(ng.game.players[other].rack.slots[i] == 0);
    // The stock draw arms a face-down flight animation.
    CHECK(ng.game.anim.kind == ANIM_DRAW_STOCK);
    CHECK(ng.game.anim.card == 0);
    CHECK(ng.game.anim.frames > 0);
    CHECK(ng.game.stock_count == g.stock_count);
    CHECK(ng.game.discard_count == g.discard_count);
    CHECK(memcmp(ng.game.discard, g.discard, g.discard_count) == 0);

    // From another seat: the stock-drawn held card is hidden, its slot count
    // still shows a card is held (phase PLACE).
    emit(buf, sizeof buf, &g, other, actor, ACTION_DRAW_STOCK, 0, 0);
    memset(&ng, 0, sizeof ng);
    CHECK(netgame_parse_state(&ng, buf, strlen(buf)));
    CHECK(ng.my_seat == other);
    CHECK(ng.game.held_card == 0);
    CHECK(ng.game.phase == PHASE_PLACE);
    for (int i = 0; i < RACK_SLOTS; i++) CHECK(ng.game.players[actor].rack.slots[i] == 0);
}

static void test_parse_reveal(void) {
    // Fabricate a round-over with known racks and scores.
    Game g; make_game(&g);
    g.phase = PHASE_ROUND_OVER;
    g.round_winner = 2;
    g.round_no = 3;
    for (int p = 0; p < 4; p++) {
        for (int i = 0; i < RACK_SLOTS; i++) g.players[p].rack.slots[i] = (uint8_t)(p * 10 + i + 1);
        g.players[p].score = (uint16_t)(100 + p * 25);
        g.round_points[p] = (uint16_t)(p == 2 ? 75 : 20);
    }

    char buf[2048];
    NetGame ng;
    emit(buf, sizeof buf, &g, 1, -1, 0, 0, 0);
    memset(&ng, 0, sizeof ng);
    CHECK(netgame_parse_state(&ng, buf, strlen(buf)));
    CHECK(ng.game.phase == PHASE_ROUND_OVER);
    CHECK(ng.game.round_winner == 2);
    CHECK(ng.game.round_no == 3);
    // Every rack is public at the reveal.
    for (int p = 0; p < 4; p++) {
        for (int i = 0; i < RACK_SLOTS; i++) {
            CHECK(ng.game.players[p].rack.slots[i] == p * 10 + i + 1);
        }
        CHECK(ng.game.players[p].score == 100 + p * 25);
        CHECK(ng.game.round_points[p] == (p == 2 ? 75 : 20));
    }
}

static void test_parse_discard_public(void) {
    Game g; make_game(&g);
    int actor = g.turn;
    int other = (actor + 1) % 4;
    uint8_t top = g.discard[g.discard_count - 1];
    game_apply(&g, (Action){ACTION_DRAW_DISCARD, 0});

    char buf[2048];
    NetGame ng;
    // A discard draw is public: even another seat sees the held card and the
    // face-up flight animation carrying it.
    emit(buf, sizeof buf, &g, other, actor, ACTION_DRAW_DISCARD, 0, top);
    memset(&ng, 0, sizeof ng);
    CHECK(netgame_parse_state(&ng, buf, strlen(buf)));
    CHECK(ng.game.held_card == top);
    CHECK(ng.game.anim.kind == ANIM_DRAW_DISCARD);
    CHECK(ng.game.anim.card == top);
}

static void test_parse_rejects_junk(void) {
    NetGame ng;
    memset(&ng, 0, sizeof ng);
    // Wrong version, missing players, bad seat: all rejected, no crash.
    CHECK(!netgame_parse_state(&ng, "{\"t\":\"state\",\"v\":2}", 18));
    CHECK(!netgame_parse_state(&ng, "{\"t\":\"state\",\"v\":1,\"players\":9,\"seat\":0}", 40));
    CHECK(!netgame_parse_state(&ng, "{\"t\":\"state\",\"v\":1,\"players\":4,\"seat\":9}", 40));
    // Hostile seat indices must be rejected, not written into the game (a
    // turn/dealer/winner past MAX_PLAYERS would be an OOB write in seat_name).
    CHECK(!netgame_parse_state(&ng,
        "{\"t\":\"state\",\"v\":1,\"players\":4,\"seat\":0,\"phase\":1,\"turn\":200}", 62));
    CHECK(!netgame_parse_state(&ng,
        "{\"t\":\"state\",\"v\":1,\"players\":4,\"seat\":0,\"phase\":1,\"turn\":0,\"dealer\":99}", 71));
    CHECK(!netgame_parse_state(&ng,
        "{\"t\":\"state\",\"v\":1,\"players\":4,\"seat\":0,\"phase\":3,\"turn\":0,\"winner\":200}", 72));
    CHECK(!netgame_parse_state(&ng,
        "{\"t\":\"state\",\"v\":1,\"players\":4,\"seat\":0,\"phase\":9,\"turn\":0}", 58));
    // ...but NO_WINNER (255) for the winners is legal.
    CHECK(netgame_parse_state(&ng,
        "{\"t\":\"state\",\"v\":1,\"players\":4,\"seat\":0,\"phase\":1,\"turn\":0,\"winner\":255,"
        "\"match_winner\":255}", 90));

    // A reveal 'racks' with a short subarray must not leak stack: the missing
    // slots read back as 0, not garbage.
    memset(&ng, 0xAB, sizeof ng);   // poison, so any uncopied slot shows
    const char* shortrack =
        "{\"t\":\"state\",\"v\":1,\"players\":2,\"seat\":0,\"phase\":3,\"turn\":0,"
        "\"racks\":[[5,6,7],[1,2,3,4,5,6,7,8,9,10]]}";
    CHECK(netgame_parse_state(&ng, shortrack, strlen(shortrack)));
    CHECK(ng.game.players[0].rack.slots[0] == 5);
    for (int i = 3; i < RACK_SLOTS; i++) CHECK(ng.game.players[0].rack.slots[i] == 0);

    // Deterministic garbage never crashes the parser.
    uint32_t lcg = 999;
    char b[128];
    for (int it = 0; it < 3000; it++) {
        lcg = lcg*1664525u+1013904223u; int len = (int)(lcg % 120u);
        for (int i = 0; i < len; i++) { lcg = lcg*1664525u+1013904223u; b[i] = (char)(lcg>>16); }
        b[len] = '\0';
        netgame_parse_state(&ng, b, (size_t)len);
    }
}

int main(void) {
    printf("test_netgame: state reconstruction — mid-round redaction, reveal,\n");
    printf("              public discard draw, junk rejection\n");
    test_parse_midround();
    test_parse_reveal();
    test_parse_discard_public();
    test_parse_rejects_junk();
    if (failures == 0) { printf("OK: all checks passed\n"); return 0; }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
