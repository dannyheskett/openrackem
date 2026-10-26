// Visual-verification harness: fabricates deterministic game states and
// screenshots every screen both as a desktop window (the layout in a centred
// column) and as a phone (full screen), so layout work is checked against
// real pixels instead of guessed at. Not part of `make test` — run
// `make shots` (needs a display).
//
// Built with -DPLATFORM_WEB so render_set_portrait() can flip between desktop
// and touch at runtime; the emscripten main loop never runs because this file
// provides its own main(). The window is resized to a phone aspect for the
// phone set.

#include "game.h"
#include "render.h"
#include "online_ui.h"
#include <raylib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void skip_deal(Game* g) {
    for (int i = 0; i < 4096 && g->phase == PHASE_DEAL; i++) game_update(g);
}

static Game* make_game(bool partners) {
    Rules r = rules_default();
    r.player_count = 4;
    r.partners = partners;
    r.human_seat = 0;
    r.seed = 20260825;
    rules_normalize(&r);
    Game* g = game_create(&r);
    skip_deal(g);
    return g;
}

static void shoot(const char* mode, const char* scene, const Game* g, const TableUi* ui) {
    for (int i = 0; i < 3; i++) render_frame(g, ui);
    char path[128];
    snprintf(path, sizeof path, "build/shots/%s-%s.png", mode, scene);
    TakeScreenshot(path);
}

static void snap(const char* mode, const char* scene) {
    char path[128];
    snprintf(path, sizeof path, "build/shots/%s-%s.png", mode, scene);
    TakeScreenshot(path);
}

static void set_names(char names[][NG_NAME], const char* const* src, int n) {
    for (int i = 0; i < n; i++) snprintf(names[i], NG_NAME, "%s", src[i]);
}

// The online screens, from fabricated session data (no network).
static void shoot_online(const char* mode) {
    static NetGame ng;
    memset(&ng, 0, sizeof ng);
    ng.state = NG_IDLE;
    ng.have_rooms = true;
    ng.rooms_total = 37;
    static const char* const r0[] = {"Sam", "Lee", "Kim"};
    static const char* const r1[] = {"Jo"};
    static const char* const r2[] = {"Ana", "Raj"};
    static const char* const r3[] = {"Priyanka", "Bartholomew"};
    const char* const* rn[] = {r0, r1, r2, r3};
    int counts[] = {3, 1, 2, 2};
    for (int i = 0; i < 4; i++) {
        ng.rooms[i].id = 100 + i;
        ng.rooms[i].n = counts[i];
        set_names(ng.rooms[i].names, rn[i], counts[i]);
        ng.rooms[i].target = i == 1 ? 250 : 500;
        ng.rooms[i].bonus = i == 1;
        ng.rooms[i].partners = i == 2;
        ng.rooms[i].ready[0] = 1;
        if (i == 0) ng.rooms[i].ready[1] = 1;
    }
    ng.room_count = 4;
    ng.peek.known = ng.peek.ok = true;
    snprintf(ng.peek.code, sizeof ng.peek.code, "K7QM");
    static const char* const pk[] = {"Dan", "Alex"};
    set_names(ng.peek.names, pk, 2);
    ng.peek.n = 2;
    ng.peek.ready[0] = 1;

    OnlineView v = { .ng = &ng, .player = "DAN", .saved_code = "K7QM",
                     .has_return = false, .sel = 0 };
    for (int i = 0; i < 3; i++) online_render_list(&v);
    snap(mode, "online-list");
    v.has_return = true;
    v.sel = 5;
    v.notice = "THAT ROOM IS FULL";
    for (int i = 0; i < 3; i++) online_render_list(&v);
    snap(mode, "online-list-return");

    // A public waiting room: three in, one ready.
    ng.state = NG_LOBBY;
    NgLobby* L = &ng.lobby;
    memset(L, 0, sizeof *L);
    L->target = 500;
    L->me = 0;
    static const char* const lm[] = {"Dan", "Alex", "Priya"};
    set_names(L->names, lm, 3);
    for (int i = 0; i < 3; i++) { L->used[i] = 1; L->on[i] = 1; }
    L->ready[1] = 1;
    L->count_ms = -1;
    snprintf(L->block, sizeof L->block, "ready");
    L->swap_from = -1;
    OnlineView lv = { .ng = &ng, .player = "DAN", .saved_code = "", .sel = 0 };
    for (int i = 0; i < 3; i++) online_render_lobby(&lv);
    snap(mode, "online-lobby-public");

    // A private room counting down, owner, Wait policy, one player reconnecting.
    L->priv = true;
    snprintf(L->code, sizeof L->code, "K7QM");
    L->away_policy = 1;
    L->bonus = true;
    for (int i = 0; i < 3; i++) L->ready[i] = 1;
    L->on[2] = 0;
    L->block[0] = '\0';
    L->count_ms = 1400;
    lv.is_owner = true;
    for (int i = 0; i < 3; i++) online_render_lobby(&lv);
    snap(mode, "online-lobby-private");

    // Partners: full teams, someone asks us to swap.
    L->partners = true;
    L->count_ms = -1;
    snprintf(L->block, sizeof L->block, "ready");
    static const char* const pm[] = {"Dan", "Alex", "Priya", "Sam"};
    set_names(L->names, pm, 4);
    for (int i = 0; i < 4; i++) { L->used[i] = 1; L->on[i] = 1; L->ready[i] = 0; L->team[i] = i & 1; }
    for (int i = 0; i < 3; i++) online_render_lobby(&lv);
    snap(mode, "online-lobby-partners");
    L->swap_from = 1;
    for (int i = 0; i < 3; i++) online_render_lobby(&lv);
    snap(mode, "online-lobby-swap");
}

// The table with the online overlay: an away seat, the turn clock, and the
// absent-player banner.
static void shoot_online_table(const char* mode) {
    Game* g = make_game(false);
    while (g->anim.frames > 0) game_update(g);
    static const char labels[4][16] = {"Dan", "Alex", "Priya", "Sam"};
    render_set_seat_labels(labels, 4);
    OnlineHud hud = { .on = true, .deadline_ms = 41000, .wait_seat = -1 };
    hud.away[2] = 1;
    snprintf(hud.back, sizeof hud.back, "SAM");
    render_set_online_hud(&hud);
    TableUi ui = { .cursor = 4, .standings = false };
    g->turn = 0;
    g->phase = PHASE_DRAW;
    shoot(mode, "online-table", g, &ui);
    hud.back[0] = '\0';
    hud.wait_seat = 2;
    hud.wait_ms = 23000;
    hud.wait_timer = true;
    render_set_online_hud(&hud);
    g->turn = 2;
    shoot(mode, "online-wait", g, &ui);
    hud.wait_seat = -1;
    hud.deadline_ms = 12000;
    render_set_online_hud(&hud);
    g->phase = PHASE_ROUND_OVER;
    g->round_winner = 1;
    shoot(mode, "online-round-over", g, &ui);
    render_set_online_hud(NULL);
    render_clear_seat_labels();
    game_destroy(g);
}

// Every screen once, in whichever renderer is currently selected.
static void shoot_all(const char* mode) {
    TableUi ui = { .cursor = 4, .standings = false };

    // Mid-round table: a few turns in, human holding a stock card, cursor up.
    // Every apply arms a card-flight tween; drain it (the harness never runs
    // the 60 Hz loop) so still shots don't capture a flyer frozen at t=0.
    Game* g = make_game(false);
    game_apply(g, (Action){ACTION_DRAW_DISCARD, 0});
    game_apply(g, (Action){ACTION_PLACE, 6});
    game_apply(g, (Action){ACTION_DRAW_STOCK, 0});
    game_apply(g, (Action){ACTION_DISCARD, 0});
    game_apply(g, (Action){ACTION_DRAW_STOCK, 0});
    game_apply(g, (Action){ACTION_PLACE, 2});
    while (g->anim.frames > 0) game_update(g);
    shoot(mode, "table-draw", g, &ui);

    g->turn = 0;
    g->phase = PHASE_PLACE;
    g->held_card = 33;
    g->held_from_discard = false;
    shoot(mode, "table-place", g, &ui);

    // Mid-flight tween: an exchange just happened; freeze the displaced card
    // halfway between the slot and the discard pile.
    game_apply(g, (Action){ACTION_PLACE, 4});
    for (int i = 0; i < 5; i++) game_update(g);   // half of SLIDE_FRAMES
    shoot(mode, "table-flight", g, &ui);
    while (g->anim.frames > 0) game_update(g);   // let the tween land

    // Round over: fabricated results (state is public POD, no engine needed).
    uint8_t winner_rack[RACK_SLOTS] = {2, 10, 11, 24, 32, 39, 41, 47, 51, 59};
    memcpy(g->players[2].rack.slots, winner_rack, RACK_SLOTS);
    g->phase = PHASE_ROUND_OVER;
    g->round_winner = 2;
    g->round_no = 3;
    g->round_points[0] = 30; g->round_points[1] = 15;
    g->round_points[2] = 75; g->round_points[3] = 45;
    g->players[0].score = 120; g->players[1].score = 95;
    g->players[2].score = 210; g->players[3].score = 160;
    ui.standings = false;
    shoot(mode, "round-over", g, &ui);
    ui.standings = true;
    shoot(mode, "standings", g, &ui);

    // Match over.
    g->phase = PHASE_MATCH_OVER;
    g->match_winner = 2;
    g->players[2].score = 510;
    ui.standings = false;
    shoot(mode, "match-over", g, &ui);

    // Menus (labels fabricated; the real ones come from main.c).
    static const char* items[] = {"Resume Game", "New Game", "Options",
                                  "Sound: Off", "Exit"};
    render_menu("OPENRACKEM", items, 5, 1, 4);
    render_menu("OPENRACKEM", items, 5, 1, 4);
    char path[128];
    snprintf(path, sizeof path, "build/shots/%s-menu.png", mode);
    TakeScreenshot(path);

    shoot_online(mode);
    shoot_online_table(mode);
}

int main(void) {
    mkdir("build", 0755);
    mkdir("build/shots", 0755);
    render_init();

    render_set_portrait(false);
    shoot_all("desktop");

    // Phone aspect for the phone set; a few frames to let the resize land.
    SetWindowSize(420, 840);
    render_set_portrait(true);
    Game* warm = make_game(false);
    TableUi ui = {0};
    for (int i = 0; i < 5; i++) render_frame(warm, &ui);
    shoot_all("portrait");

    render_cleanup();
    printf("shots written to build/shots/\n");
    return 0;
}
