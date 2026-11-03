// Site demo generator: plays one scripted four-player game for the camera and
// pipes every rendered frame to ffmpeg, so the clip on danheskett.com is
// re-recordable from code (make demo). 1280x720 — the desktop window with the
// table in its centred column — 60 fps, 60 s, a fade through black at the
// loop point, no audio.
//
// "You" (seat 0) are played by the Hard AI with a human-ish pause before each
// decision; the three CPUs are the engine's own Easy players. The seed is
// chosen (gen_demo find) so that you go out in round 1 within the clip, then
// the scoring screen, the standings, and the start of round 2 fill the rest.
//
// Built with -DPLATFORM_WEB like the screenshot harnesses (both renderers in
// one native binary, render_set_portrait(false) = the desktop column). The
// desktop recorder is not used: it captures at phone size.
//
//   ./build/gen_demo find            # print candidate seeds
//   ./build/gen_demo <outdir> [seed] # write <outdir>/openrackem-demo.mp4 + poster.png
#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "ai.h"
#include "render.h"
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEMO_W      1280
#define DEMO_H      720
#define DEMO_FRAMES 3600          // 60 s at 60 fps
#define YOU_THINK_DRAW  48        // frames you pause before drawing
#define YOU_THINK_PLACE 36        // ...and before placing
#define SCORE_FRAMES   270        // the round scoring screen
#define STAND_FRAMES   240        // the standings page
#define DEFAULT_SEED   20261316ULL

// One 60 Hz step of the demo's game: the engine paces the CPUs, this paces you.
typedef struct {
    Game*    g;
    TableUi  ui;
    uint64_t rng;
    int      think;      // frames left before your next decision
    int      hold;       // frames left on a round screen
    int      round_won;  // frame at which you went out in round 1 (-1 = not yet)
} Demo;

static void demo_step(Demo* d, int frame) {
    Game* g = d->g;
    if (g->phase == PHASE_ROUND_OVER) {
        if (d->round_won < 0 && g->round_winner == 0 && g->round_no == 1) d->round_won = frame;
        if (d->hold == 0) d->hold = SCORE_FRAMES + STAND_FRAMES;
        d->hold--;
        d->ui.standings = d->hold < STAND_FRAMES;
        if (d->hold == 0) { game_next_round(g); d->ui.standings = false; }
        game_update(g);
        return;
    }
    game_update(g);
    if (g->phase == PHASE_MATCH_OVER) return;
    bool yours = (g->turn == 0) && (g->phase == PHASE_DRAW || g->phase == PHASE_PLACE);
    if (!yours || g->anim.frames > 0) return;
    if (d->think < 0) d->think = (g->phase == PHASE_DRAW) ? YOU_THINK_DRAW : YOU_THINK_PLACE;
    if (d->think-- > 0) return;
    GameView v = game_view_for(g, 0);
    v.rules.ai_difficulty = 2;      // you play well; the CPUs are on Easy
    Action a = ai_choose(&v, &d->rng);
    if (a.type == ACTION_PLACE) d->ui.cursor = a.slot;
    if (!game_apply(g, a)) {
        a = (Action){ACTION_DRAW_STOCK, 0};
        if (g->phase == PHASE_PLACE) a = (Action){ACTION_PLACE, 0};
        game_apply(g, a);
    }
    d->think = -1;
}

static Game* demo_game(uint64_t seed) {
    Rules r = rules_default();
    r.player_count = 4;
    r.human_seat = 0;
    r.ai_difficulty = 0;
    r.seed = seed;
    rules_normalize(&r);
    return game_create(&r);
}

static void demo_init(Demo* d, uint64_t seed) {
    memset(d, 0, sizeof *d);
    d->g = demo_game(seed);
    d->ui = (TableUi){ .cursor = 4, .standings = false };
    d->rng = seed * 0x9E3779B97F4A7C15ULL + 1;
    d->think = -1;
    d->round_won = -1;
}

// Headless: run the clip's frames and report when (and whether) you went out
// in round 1. Candidates land the reveal between 30 s and 45 s in.
static int find_seeds(void) {
    int shown = 0;
    for (uint64_t seed = DEFAULT_SEED; seed < DEFAULT_SEED + 400 && shown < 12; seed++) {
        Demo d;
        demo_init(&d, seed);
        int end = -1, winner = -1;
        for (int f = 0; f < 2 * DEMO_FRAMES; f++) {
            demo_step(&d, f);
            if (d.g->phase == PHASE_ROUND_OVER) { end = f; winner = d.g->round_winner; break; }
        }
        if (seed < DEFAULT_SEED + 20) {
            printf("seed %llu: round 1 ends at %.1f s, winner %d\n",
                   (unsigned long long)seed, end / 60.0, winner);
        }
        if (winner == 0 && end >= 1500 && end <= 2700) {
            printf("CANDIDATE seed %llu: you go out at %.1f s\n", (unsigned long long)seed, end / 60.0);
            shown++;
        }
    }
    if (!shown) printf("no seed in range; widen the window\n");
    return 0;
}

static int record(const char* outdir, uint64_t seed) {
    mkdir(outdir, 0755);
    char mp4[512], poster[512], cmd[1200];
    snprintf(mp4, sizeof mp4, "%s/openrackem-demo.mp4", outdir);
    snprintf(poster, sizeof poster, "%s/poster.png", outdir);
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -y -f rawvideo -pix_fmt rgba -s %dx%d -r 60 -i - "
             "-vf \"fade=t=in:st=0:d=0.5,fade=t=out:st=59.5:d=0.5\" "
             "-c:v libx264 -preset slow -crf 24 -pix_fmt yuv420p -an -movflags +faststart \"%s\"",
             DEMO_W, DEMO_H, mp4);
    FILE* ff = popen(cmd, "w");
    if (!ff) { perror("ffmpeg"); return 1; }

    render_init();
    SetWindowSize(DEMO_W, DEMO_H);
    render_set_portrait(false);
    Demo d;
    demo_init(&d, seed);
    for (int i = 0; i < 8; i++) render_frame(d.g, &d.ui);   // let the resize settle

    bool poster_done = false;
    for (int f = 0; f < DEMO_FRAMES; f++) {
        demo_step(&d, f);
        render_frame(d.g, &d.ui);
        Image img = LoadImageFromScreen();
        if (img.width != DEMO_W || img.height != DEMO_H) {
            fprintf(stderr, "screen is %dx%d, not %dx%d\n", img.width, img.height, DEMO_W, DEMO_H);
            UnloadImage(img);
            pclose(ff);
            return 1;
        }
        // The poster is the first frame with the deal finished and your rack up.
        if (!poster_done && d.g->phase == PHASE_DRAW && d.g->anim.frames == 0) {
            ExportImage(img, poster);
            poster_done = true;
        }
        fwrite(img.data, 1, (size_t)DEMO_W * DEMO_H * 4, ff);
        UnloadImage(img);
    }
    render_cleanup();
    int rc = pclose(ff);
    printf("demo: %s (seed %llu, you went out at %.1f s)\n", mp4,
           (unsigned long long)seed, d.round_won >= 0 ? d.round_won / 60.0 : -1.0);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "find") == 0) return find_seeds();
    const char* outdir = argc >= 2 ? argv[1] : "build/demo";
    uint64_t seed = argc >= 3 ? strtoull(argv[2], NULL, 10) : DEFAULT_SEED;
    return record(outdir, seed);
}
