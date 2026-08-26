// Visual-verification harness: fabricates deterministic game states and
// screenshots every screen in both renderers, so layout work is checked
// against real pixels instead of guessed at. Not part of `make test` — run
// `make shots` (needs a display).
//
// Built with -DPLATFORM_WEB so both renderers compile (OR_RUNTIME_RENDERER)
// and render_set_portrait() can flip between them at runtime; the emscripten
// main loop never runs because this file provides its own main(). The window
// is resized to a phone aspect for the portrait set.

#include "game.h"
#include "render.h"
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
}

int main(void) {
    mkdir("build", 0755);
    mkdir("build/shots", 0755);
    render_init();

    render_set_portrait(false);
    shoot_all("landscape");

    // Phone aspect for the portrait set; a few frames to let the resize land.
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
