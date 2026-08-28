// Store-screenshot generator: four portrait scenes at 540x960, upscaled to
// the Play sizes by scripts/gen_store_assets.py's sibling PIL step:
//
//   gcc -std=c99 -Isrc -Ithird_party/minih264 -Ithird_party/minimp4 -O2 \
//       -DPLATFORM_WEB -Ithird_party/raylib-install/include \
//       scripts/gen_store_shots.c $(ls src/*.c | grep -v main.c) \
//       -o build/store_shots -Lthird_party/raylib-install/lib \
//       -Wl,-Bstatic -lraylib -Wl,-Bdynamic -lm -lpthread -ldl -lrt -lX11
//   ./build/store_shots   # then upscale build/store-shots/*.png x2 and x4
//
// -DPLATFORM_WEB compiles both renderers into one native binary (the same
// trick tests/vis_shots.c uses); this file supplies main().
#include "game.h"
#include "render.h"
#include <raylib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
// Optional argv: WIDTH HEIGHT OUTDIR. Defaults reproduce the Play set exactly
// (540x960 -> build/store-shots). The App Store wants 1290x2796 native:
//   xvfb-run -s "-screen 0 1400x2900x24" ./build/store_shots 1290 2796 \
//       ios/app-store-assets/screenshots/iphone-6.9
static int SHOT_W = 540, SHOT_H = 960;
static const char* SHOT_DIR = "build/store-shots";
static void skip_deal(Game* g){ for(int i=0;i<4096&&g->phase==PHASE_DEAL;i++) game_update(g);}
static void drain(Game* g){ while(g->anim.frames>0) game_update(g);}
static void snap(const char* name, const Game* g, const TableUi* ui){
    for(int i=0;i<3;i++) render_frame(g,ui);
    char p[256]; snprintf(p,sizeof p,"%s/%s.png",SHOT_DIR,name);
    TakeScreenshot(p);
}
int main(int argc, char** argv){
    if(argc >= 3){ SHOT_W = atoi(argv[1]); SHOT_H = atoi(argv[2]); }
    if(argc >= 4){ SHOT_DIR = argv[3]; }
    mkdir("build",0755); mkdir(SHOT_DIR,0755);
    render_init();
    SetWindowSize(SHOT_W, SHOT_H);
    render_set_portrait(true);
    Rules r = rules_default(); r.player_count=4; r.human_seat=0; r.seed=20260825;
    rules_normalize(&r);
    Game* g = game_create(&r); skip_deal(g);
    TableUi ui = { .cursor = 4, .standings = false };
    for(int i=0;i<8;i++) render_frame(g,&ui);   // let the resize settle

    // 02: mid-round table, your turn to draw.
    game_apply(g,(Action){ACTION_DRAW_DISCARD,0}); drain(g);
    game_apply(g,(Action){ACTION_PLACE,6}); drain(g);
    game_apply(g,(Action){ACTION_DRAW_STOCK,0}); drain(g);
    game_apply(g,(Action){ACTION_DISCARD,0}); drain(g);
    game_apply(g,(Action){ACTION_DRAW_STOCK,0}); drain(g);
    game_apply(g,(Action){ACTION_PLACE,2}); drain(g);
    snap("02-table", g, &ui);

    // 03: holding a card, choosing a slot.
    g->turn = 0; g->phase = PHASE_PLACE; g->held_card = 33; g->held_from_discard = false;
    snap("03-place", g, &ui);

    // 04: the round-scoring reveal.
    uint8_t win[RACK_SLOTS] = {2,10,11,24,32,39,41,47,51,59};
    memcpy(g->players[2].rack.slots, win, RACK_SLOTS);
    g->phase = PHASE_ROUND_OVER; g->round_winner = 2; g->round_no = 3;
    g->round_points[0]=30; g->round_points[1]=15; g->round_points[2]=75; g->round_points[3]=45;
    g->players[0].score=120; g->players[1].score=95; g->players[2].score=210; g->players[3].score=160;
    snap("04-roundover", g, &ui);

    // 01: the menu.
    static const char* items[] = {"New Game","Options","Sound: Off","Exit"};
    for(int i=0;i<3;i++) render_menu("OPENRACKEM", items, 4, 0, 3);
    char mp[256]; snprintf(mp,sizeof mp,"%s/01-menu.png",SHOT_DIR); TakeScreenshot(mp);
    render_cleanup();
    printf("store shots written\n");
    return 0;
}
