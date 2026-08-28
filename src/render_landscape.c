// Landscape (desktop) renderer: draws the 3-column table layout into a fixed
// 640x480 offscreen canvas that present() letterboxes into the window —
// opponents on the left, the player's labeled rack centered, piles and match
// state on the right. Compiles to an empty object off OR_LANDSCAPE (i.e. on
// Android / iOS).
#include "render_internal.h"

#ifdef OR_LANDSCAPE

#include "recorder.h"
#include <raylib.h>
#include <stddef.h> // NULL
#include <string.h>  // strlen
#include <stdio.h>

RenderTexture2D canvas;              // declared extern in render_internal.h; the
                                     // fixed 640x480 canvas is now used only by
                                     // the recorder (fixed-size video).

// Continuous scale that fits the 640x480 layout in the window (never below 1x,
// so 640x480 is the minimum). Fractional is fine — the layout is vector (Nunito
// text, rounded cards), so a Camera2D re-rasterizes it crisply at any scale.
static float fit_scale(void) {
    float sw = (float)GetScreenWidth()  / BASE_WIDTH;
    float sh = (float)GetScreenHeight() / BASE_HEIGHT;
    float s = (sw < sh) ? sw : sh;
    return (s < 1.0f) ? 1.0f : s;
}

// Render `draw(ctx)` — which issues 640x480-logical gfx calls — to the window,
// scaled + centered at the window's native resolution via a Camera2D (so text
// and shapes stay crisp when enlarged, instead of upscaling a 640x480 texture).
// When recording, the same scene is also drawn into the fixed 640x480 canvas so
// captured video stays a constant size regardless of window size.
static void present_scaled(void (*draw)(void*), void* ctx) {
    if (recorder_active()) {
        BeginTextureMode(canvas);
        gfx_clear(BLACK);
        draw(ctx);
        EndTextureMode();
        recorder_capture(&canvas);   // clean 640x480 frame (no REC indicator)
    }

    float scale = fit_scale();
    float ox = (GetScreenWidth()  - BASE_WIDTH  * scale) / 2.0f;
    float oy = (GetScreenHeight() - BASE_HEIGHT * scale) / 2.0f;

    gfx_begin_frame();
    gfx_clear(BLACK);               // letterbox bars stay black
    Camera2D cam = { .offset = (Vector2){ox, oy}, .target = (Vector2){0, 0},
                     .rotation = 0.0f, .zoom = scale };
    BeginMode2D(cam);
    draw(ctx);
    if (recorder_active()) {        // window-only indicator, in 640x480 space
        DrawCircle(16, 14, 5.0f, RED);
        gfx_text("REC", 24, 8, 12, RED);
    }
    EndMode2D();
    gfx_end_frame();
}

// --- Layout constants (hand-tuned for 640x480) ------------------------------
#define TITLE_H    24
#define CARD_W     60
#define CARD_H     38
#define ROW_STEP   42                       // CARD_H + 4
#define RACK_X     ((BASE_WIDTH - CARD_W) / 2)
#define RACK_Y     44                       // centers 10 rows in 24..480
#define LEFT_X     16
#define RIGHT_X    460

// The seat whose rack fills the center column: the human, or seat 0 as the
// spectator's view in a full-AI (autoplay) game.
static int focus_seat(const Game* g) {
    return (g->rules.human_seat >= 0) ? g->rules.human_seat : 0;
}

// --- Card-flight geometry ---------------------------------------------------
static Rectangle stock_rect(void)   { return (Rectangle){RIGHT_X, RACK_Y + 14,  CARD_W, CARD_H}; }
static Rectangle discard_rect(void) { return (Rectangle){RIGHT_X, RACK_Y + 76,  CARD_W, CARD_H}; }
static Rectangle held_rect(void)    { return (Rectangle){RIGHT_X, RACK_Y + 138, CARD_W, CARD_H}; }

static Rectangle slot_rect(int s) {
    return (Rectangle){RACK_X, (float)(RACK_Y + (RACK_SLOTS - 1 - s) * ROW_STEP),
                       CARD_W, CARD_H};
}

// A small card footprint at an opponent's left-column block, used as the
// flight endpoint for their exchanges. Deliberately the block, not a slot:
// which slot an opponent filled is information the table doesn't show.
static Rectangle opp_card_rect(const Game* g, int seat) {
    int focus = focus_seat(g);
    int bi = 0;
    for (int s = 0; s < seat; s++) {
        if (s != focus) bi++;
    }
    int oy = RACK_Y + 4 + bi * 64;
    return (Rectangle){LEFT_X + 62, (float)(oy + 14), 26, 20};
}

// The one visibly moving card, drawn over the finished table. Draws travel to
// the held spot; an exchange sends the displaced card to the discard pile
// (from the exact slot for the focus seat, from the block for opponents).
static void draw_flight(const Game* game) {
    if (!anim_in_flight(game)) return;
    float t = anim_progress(game);
    int focus = focus_seat(game);
    switch (game->anim.kind) {
    case ANIM_DRAW_STOCK:
        draw_flying_card(stock_rect(), held_rect(), t, 0, false);
        break;
    case ANIM_DRAW_DISCARD:
        draw_flying_card(discard_rect(), held_rect(), t, game->anim.card, true);
        break;
    case ANIM_PLACE: {
        Rectangle from = (game->anim.seat == focus)
                       ? slot_rect(game->anim.slot)
                       : opp_card_rect(game, game->anim.seat);
        draw_flying_card(from, discard_rect(), t, game->anim.card, true);
        break;
    }
    case ANIM_DISCARD:
        draw_flying_card(held_rect(), discard_rect(), t, game->anim.card, true);
        break;
    }
}

static void text_centered(const char* s, int cx, int y, int fs, Color c) {
    gfx_text(s, cx - gfx_measure_text(s, fs) / 2, y, fs, c);
}

static void draw_title_bar(void) {
    gfx_rect(0, 0, BASE_WIDTH, TITLE_H, DARKGRAY);
    text_centered("OPENRACKEM", BASE_WIDTH / 2, 4, 16, WHITE);
}

// A pile position: label above a card footprint.
static void draw_pile_label(const char* label, int x, int y) {
    gfx_text(label, x, y, 12, SLOT_LABEL);
}

// The winner's name + the table's winning call, or the stalemate banner.
static void round_banner(const Game* g, char* out, size_t n) {
    if (g->round_winner == NO_WINNER) {
        snprintf(out, n, "STALEMATE");
    } else if ((int)g->round_winner == g->rules.human_seat) {
        snprintf(out, n, "RACK 'EM!");
    } else {
        snprintf(out, n, "%s: RACK 'EM!", seat_name(g, g->round_winner));
    }
}

// Team label under Partners (seats 0+2 vs 1+3), e.g. "YOU & CPU 2".
static void team_label(const Game* g, int pair, char* out, size_t n) {
    snprintf(out, n, "%s & %s", seat_name(g, pair), seat_name(g, pair + 2));
}

// --- The table --------------------------------------------------------------
static void draw_table_landscape(const Game* game, const TableUi* ui) {
    char buf[64];
    int focus = focus_seat(game);
    int human = game->rules.human_seat;
    bool human_turn = (human >= 0 && game->turn == human &&
                       (game->phase == PHASE_DRAW || game->phase == PHASE_PLACE));

    // Center: the focus rack, slot #50 on top, its label left of each card.
    int dealt = deal_cards_for_seat(game, focus);
    for (int s = RACK_SLOTS - 1; s >= 0; s--) {
        int y = RACK_Y + (RACK_SLOTS - 1 - s) * ROW_STEP;
        snprintf(buf, sizeof buf, "%d", (s + 1) * 5);
        gfx_text(buf, RACK_X - 10 - gfx_measure_text(buf, 12), y + (CARD_H - 12) / 2,
                 12, SLOT_LABEL);
        if (RACK_SLOTS - 1 - s < dealt) {
            bool cur = (human_turn && game->phase == PHASE_PLACE && ui->cursor == s);
            draw_card(RACK_X, y, CARD_W, CARD_H, game->players[focus].rack.slots[s],
                      true, cur);
        } else {
            draw_card_outline(RACK_X, y, CARD_W, CARD_H);
        }
    }

    // Left: the other seats — name, running score, a strip of card backs.
    int oy = RACK_Y + 4;
    for (int s = 0; s < game->rules.player_count; s++) {
        if (s == focus) continue;
        bool their_turn = (game->turn == s &&
                           (game->phase == PHASE_DRAW || game->phase == PHASE_PLACE));
        if (their_turn) {
            gfx_rect_lines(LEFT_X - 4, oy - 4, 156, 52, ACCENT);
        }
        gfx_text(seat_name(game, s), LEFT_X, oy, 12, their_turn ? ACCENT : WHITE);
        snprintf(buf, sizeof buf, "%d", game->players[s].score);
        gfx_text(buf, LEFT_X + 148 - gfx_measure_text(buf, 12), oy, 12, YELLOW);
        int backs = deal_cards_for_seat(game, s);
        for (int i = 0; i < backs; i++) {
            draw_card(LEFT_X + i * 14, oy + 18, 13, 20, 0, false, false);
        }
        oy += 64;
    }

    // Right: stock, discard, the held card, then match state. While a card is
    // in flight its destination draws one step behind (the pile's previous
    // top, an empty held spot), so the landing coincides with the arrival.
    bool draw_flying = anim_in_flight(game);
    bool draw_arrives_held = draw_flying && (game->anim.kind == ANIM_DRAW_STOCK ||
                                             game->anim.kind == ANIM_DRAW_DISCARD);
    bool card_arrives_pile = draw_flying && (game->anim.kind == ANIM_PLACE ||
                                             game->anim.kind == ANIM_DISCARD);

    draw_pile_label("STOCK", RIGHT_X, RACK_Y);
    if (game->stock_count > 0) {
        draw_card(RIGHT_X, RACK_Y + 14, CARD_W, CARD_H, 0, false, false);
        snprintf(buf, sizeof buf, "x%d", game->stock_count);
        gfx_text(buf, RIGHT_X + CARD_W + 8, RACK_Y + 14 + (CARD_H - 12) / 2, 12, SLOT_LABEL);
    } else {
        draw_card_outline(RIGHT_X, RACK_Y + 14, CARD_W, CARD_H);
    }

    draw_pile_label("DISCARD", RIGHT_X, RACK_Y + 62);
    if (card_arrives_pile) {
        if (game->discard_count >= 2) {
            draw_card(RIGHT_X, RACK_Y + 76, CARD_W, CARD_H,
                      game->discard[game->discard_count - 2], true, false);
        } else {
            draw_card_outline(RIGHT_X, RACK_Y + 76, CARD_W, CARD_H);
        }
    } else if (game->discard_count > 0 && deal_discard_flipped(game)) {
        draw_card(RIGHT_X, RACK_Y + 76, CARD_W, CARD_H,
                  game->discard[game->discard_count - 1], true, false);
    } else {
        draw_card_outline(RIGHT_X, RACK_Y + 76, CARD_W, CARD_H);
    }

    draw_pile_label("HELD", RIGHT_X, RACK_Y + 124);
    if (game->held_card && !draw_arrives_held) {
        bool face_up = (game->turn == focus) || game->held_from_discard;
        draw_card(RIGHT_X, RACK_Y + 138, CARD_W, CARD_H, game->held_card, face_up, false);
    } else {
        draw_card_outline(RIGHT_X, RACK_Y + 138, CARD_W, CARD_H);
    }

    // Match state: score(s), round, target, whose turn.
    int sy = RACK_Y + 196;
    if (game->rules.partners) {
        gfx_text("TEAMS", RIGHT_X, sy, 12, SLOT_LABEL);
        for (int p = 0; p < 2; p++) {
            int total = game->players[p].score + game->players[p + 2].score;
            char tl[32];
            team_label(game, p, tl, sizeof tl);
            snprintf(buf, sizeof buf, "%d", total);
            gfx_text(tl, RIGHT_X, sy + 16 + p * 16, 10, WHITE);
            gfx_text(buf, RIGHT_X + 148 - gfx_measure_text(buf, 12), sy + 16 + p * 16, 12, YELLOW);
        }
        sy += 56;
    } else {
        gfx_text("SCORE", RIGHT_X, sy, 12, SLOT_LABEL);
        snprintf(buf, sizeof buf, "%d", game->players[focus].score);
        gfx_text(buf, RIGHT_X, sy + 14, 20, YELLOW);
        sy += 44;
    }
    snprintf(buf, sizeof buf, "ROUND %d   TO %d", game->round_no, game->rules.target_score);
    gfx_text(buf, RIGHT_X, sy, 12, SLOT_LABEL);
    sy += 22;

    if (game->phase == PHASE_DEAL) {
        gfx_text("DEALING...", RIGHT_X, sy, 12, GRAY);
    } else if (human_turn) {
        gfx_text("YOUR TURN", RIGHT_X, sy, 12, ACCENT);
    } else {
        snprintf(buf, sizeof buf, "%s THINKING", seat_name(game, game->turn));
        gfx_text(buf, RIGHT_X, sy, 12, GRAY);
    }

    // 2-player rule reminder: an ascending rack also needs a run of 3 consecutive
    // cards to go out, so a sorted-but-stuck rack makes sense. Shows the local
    // player's current longest run.
    if (rules_require_run(&game->rules) && game->rules.human_seat >= 0 &&
        game->phase != PHASE_DEAL && game->phase != PHASE_ROUND_OVER &&
        game->phase != PHASE_MATCH_OVER) {
        int run = score_longest_run(&game->players[game->rules.human_seat].rack);
        sy += 26;
        gfx_text("2P: NEED A RUN OF 3", RIGHT_X, sy, 10, SLOT_LABEL);
        snprintf(buf, sizeof buf, "YOUR LONGEST: %d", run);
        Color rc = (run >= 3) ? (Color){120, 220, 120, 255} : (Color){235, 175, 90, 255};
        gfx_text(buf, RIGHT_X, sy + 13, 12, rc);
    }

    // Bottom hint line, contextual to what the keyboard can do right now.
    const char* hint = NULL;
    if (human_turn && game->phase == PHASE_DRAW) {
        hint = "S: DRAW STOCK    D: TAKE DISCARD";
    } else if (human_turn && game->phase == PHASE_PLACE) {
        hint = game->held_from_discard
             ? "UP/DOWN: SLOT    ENTER: EXCHANGE"
             : "UP/DOWN: SLOT    ENTER: EXCHANGE    X: THROW AWAY";
    }
    if (hint) text_centered(hint, BASE_WIDTH / 2, BASE_HEIGHT - 14, 10, SLOT_LABEL);

    draw_flight(game);
}

// --- Round scoring / standings / match over ---------------------------------
static void draw_round_scoring(const Game* game) {
    char buf[64];
    round_banner(game, buf, sizeof buf);
    text_centered(buf, BASE_WIDTH / 2, 36,  30,
                  ((int)game->round_winner == game->rules.human_seat &&
                   game->round_winner != NO_WINNER) ? ACCENT : WHITE);
    snprintf(buf, sizeof buf, "ROUND %d", game->round_no);
    text_centered(buf, BASE_WIDTH / 2, 72, 12, SLOT_LABEL);

    // Every rack face up — the reveal that proves the winner really was
    // ascending — with the points it just earned and the new totals.
    int y = 108;
    for (int s = 0; s < game->rules.player_count; s++) {
        bool won = ((int)game->round_winner == s);
        gfx_text(seat_name(game, s), 24, y + 12, 12, won ? ACCENT : WHITE);
        draw_mini_rack(130, y, 30, 42, &game->players[s].rack, true);
        snprintf(buf, sizeof buf, "+%d", game->round_points[s]);
        gfx_text(buf, 470, y + 6, 20, YELLOW);
        snprintf(buf, sizeof buf, "= %d", game->players[s].score);
        gfx_text(buf, 545, y + 12, 12, SLOT_LABEL);
        y += 62;
    }
    // The bonus variant's cut, called out when it fired.
    if (game->round_winner != NO_WINNER && game->round_points[game->round_winner] > 75) {
        snprintf(buf, sizeof buf, "INCLUDES A %d-POINT RUN BONUS",
                 game->round_points[game->round_winner] - 75);
        text_centered(buf, BASE_WIDTH / 2, y + 2, 10, ACCENT);
    }
    text_centered("ENTER: STANDINGS", BASE_WIDTH / 2, BASE_HEIGHT - 18, 10, GRAY);
}

static void draw_standings(const Game* game) {
    char buf[64];
    text_centered("STANDINGS", BASE_WIDTH / 2, 48, 30, WHITE);
    snprintf(buf, sizeof buf, "AFTER ROUND %d   -   FIRST TO %d WINS",
             game->round_no, game->rules.target_score);
    text_centered(buf, BASE_WIDTH / 2, 88, 12, SLOT_LABEL);

    int y = 140;
    if (game->rules.partners) {
        for (int p = 0; p < 2; p++) {
            char tl[32];
            team_label(game, p, tl, sizeof tl);
            int total = game->players[p].score + game->players[p + 2].score;
            gfx_text(tl, 180, y + 4, 16, WHITE);
            snprintf(buf, sizeof buf, "%d", total);
            gfx_text(buf, 460 - gfx_measure_text(buf, 24), y, 24, YELLOW);
            y += 58;
        }
    } else {
        // Seats sorted by total, best first.
        int order[MAX_PLAYERS];
        int n = game->rules.player_count;
        for (int i = 0; i < n; i++) order[i] = i;
        for (int i = 1; i < n; i++) {
            for (int j = i; j > 0 &&
                 game->players[order[j]].score > game->players[order[j - 1]].score; j--) {
                int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
            }
        }
        for (int i = 0; i < n; i++) {
            int s = order[i];
            snprintf(buf, sizeof buf, "%d.", i + 1);
            gfx_text(buf, 180, y + 6, 12, SLOT_LABEL);
            gfx_text(seat_name(game, s), 210, y + 2, 16,
                     (s == game->rules.human_seat) ? ACCENT : WHITE);
            snprintf(buf, sizeof buf, "%d", game->players[s].score);
            gfx_text(buf, 460 - gfx_measure_text(buf, 24), y, 24, YELLOW);
            y += 48;
        }
    }
    text_centered("ENTER: CONTINUE", BASE_WIDTH / 2, BASE_HEIGHT - 18, 10, GRAY);
}

static void draw_match_over(const Game* game) {
    char buf[64];
    text_centered("MATCH OVER", BASE_WIDTH / 2, 70, 36, WHITE);

    bool human_won = false;
    if (game->rules.partners) {
        char tl[32];
        team_label(game, game->match_winner, tl, sizeof tl);
        human_won = (game->rules.human_seat >= 0 &&
                     game->rules.human_seat % 2 == game->match_winner);
        snprintf(buf, sizeof buf, human_won ? "%s WIN THE MATCH!" : "%s WIN THE MATCH", tl);
    } else {
        human_won = ((int)game->match_winner == game->rules.human_seat);
        if (human_won) snprintf(buf, sizeof buf, "YOU WIN THE MATCH!");
        else snprintf(buf, sizeof buf, "%s WINS THE MATCH",
                      seat_name(game, game->match_winner));
    }
    text_centered(buf, BASE_WIDTH / 2, 130, 20, human_won ? ACCENT : WHITE);

    int y = 190;
    if (game->rules.partners) {
        for (int p = 0; p < 2; p++) {
            char tl[32];
            team_label(game, p, tl, sizeof tl);
            int total = game->players[p].score + game->players[p + 2].score;
            gfx_text(tl, 200, y + 4, 14, WHITE);
            snprintf(buf, sizeof buf, "%d", total);
            gfx_text(buf, 440 - gfx_measure_text(buf, 20), y, 20, YELLOW);
            y += 44;
        }
    } else {
        for (int s = 0; s < game->rules.player_count; s++) {
            gfx_text(seat_name(game, s), 220, y + 4, 14,
                     (s == game->rules.human_seat) ? ACCENT : WHITE);
            snprintf(buf, sizeof buf, "%d", game->players[s].score);
            gfx_text(buf, 420 - gfx_measure_text(buf, 20), y, 20, YELLOW);
            y += 38;
        }
    }
    text_centered("ENTER: MENU", BASE_WIDTH / 2, BASE_HEIGHT - 18, 10, GRAY);
}

// Draw the scene for the game's phase into the currently-active render target.
static void draw_game_landscape(const Game* game, const TableUi* ui) {
    gfx_clear(TABLE_BG);
    draw_title_bar();
    if (game->phase == PHASE_ROUND_OVER) {
        if (ui->standings) draw_standings(game);
        else draw_round_scoring(game);
    } else if (game->phase == PHASE_MATCH_OVER) {
        draw_match_over(game);
    } else {
        draw_table_landscape(game, ui);
    }
}

static void draw_center_panel_landscape(const char* title, const char* subtitle, Color tc) {
    draw_center_panel_at(BASE_WIDTH, BASE_HEIGHT, 340, 120, 30, 14, 28, 76, title, subtitle, tc);
}

// Scene draw callbacks for present_scaled (issue 640x480-logical draws; defined
// here, after the draw helpers they call).
typedef struct {
    const Game* g; const TableUi* ui;
    const char* ot; const char* os; Color otc;
} LandSceneCtx;
static void draw_land_scene_cb(void* p) {
    LandSceneCtx* c = (LandSceneCtx*)p;
    draw_game_landscape(c->g, c->ui);
    if (c->ot) draw_center_panel_landscape(c->ot, c->os, c->otc);
}

typedef struct {
    MenuLayout m; const char* title; const char* const* items;
    int count, selected, gap;
} LandMenuCtx;
static void draw_land_menu_cb(void* p) {
    LandMenuCtx* c = (LandMenuCtx*)p;
    draw_menu_panel(c->m, c->title, c->items, c->count, c->selected, c->gap, false);
}

// One table scene with an optional centered overlay, scaled to the window.
static void draw_scene_landscape(const Game* game, const TableUi* ui,
                                 const char* overlay_title, const char* overlay_sub,
                                 Color overlay_tc) {
    LandSceneCtx c = { game, ui, overlay_title, overlay_sub, overlay_tc };
    present_scaled(draw_land_scene_cb, &c);
}

void render_frame_landscape(const Game* g, const TableUi* ui) {
    draw_scene_landscape(g, ui, NULL, NULL, WHITE);
}
void render_pause_landscape(const Game* g, const TableUi* ui) {
    draw_scene_landscape(g, ui, "GAME PAUSED", "Press any key to resume", YELLOW);
}

void render_menu_landscape(const char* title, const char* const* items, int count,
                           int selected, int gap_before) {
    int cx = BASE_WIDTH / 2;
    int line_h = 30, item_fs = 20, title_size = 44;
    int extra = (gap_before >= 0) ? 1 : 0;
    int panel_w = 320;
    int panel_h = title_size + 40 + (count + extra) * line_h + 60;
    int px = cx - panel_w / 2, py = (BASE_HEIGHT - panel_h) / 2;
    MenuLayout m = { .cx = cx, .px = px, .py = py, .panel_w = panel_w, .panel_h = panel_h,
                     .title_size = title_size, .title_y = py + 28,
                     .items_y = py + 28 + title_size + 28,
                     .line_h = line_h, .item_fs = item_fs };

    LandMenuCtx c = { m, title, items, count, selected, gap_before };
    present_scaled(draw_land_menu_cb, &c);
}

// The picker on the fixed 640x480 canvas. Desktop and desktop-browser players
// type the value outright, so this is the keyboard's read-out: same widget,
// no tap capture.
typedef struct {
    PickerLayout L; const char* title; const char* slots; int cursor;
    const char* alphabet; const char* hint; const char* ok;
} LandPickCtx;
static void draw_land_picker_cb(void* p) {
    LandPickCtx* c = (LandPickCtx*)p;
    draw_picker_panel(c->L, c->title, c->slots, c->cursor, c->alphabet, c->hint,
                      c->ok, false);
}

void render_picker_landscape(const char* title, const char* slots, int cursor,
                             const char* alphabet, const char* hint,
                             const char* ok_label) {
    int n = (int)strlen(slots);
    if (n < 1) n = 1;
    if (n > PICKER_MAX_SLOTS) n = PICKER_MAX_SLOTS;

    int cx = BASE_WIDTH / 2;
    int panel_w = 480, gap = 6;
    int slot_w = (panel_w - 40 - (n - 1) * gap) / n;
    if (slot_w > 48) slot_w = 48;
    int slot_h = slot_w * 3 / 2, slot_fs = slot_h * 3 / 5, band_h = slot_h / 2;
    int title_size = 40, hint_fs = 14, btn_h = 30, btn_fs = 18;
    int panel_h = 20 + title_size + 20 + band_h + slot_h + band_h + 20
                + hint_fs + 20 + btn_h + 20;
    int px = cx - panel_w / 2, py = (BASE_HEIGHT - panel_h) / 2;

    PickerLayout L = {
        .cx = cx, .px = px, .py = py, .panel_w = panel_w, .panel_h = panel_h,
        .title_size = title_size, .title_y = py + 20,
        .slots_y = py + 20 + title_size + 20 + band_h,
        .slot_w = slot_w, .slot_h = slot_h, .slot_gap = gap, .slot_fs = slot_fs,
        .band_h = band_h,
        .hint_y = py + 20 + title_size + 20 + band_h + slot_h + band_h + 20,
        .hint_fs = hint_fs,
        .btn_y = py + panel_h - 20 - btn_h,
        .btn_w = panel_w * 2 / 5, .btn_h = btn_h, .btn_fs = btn_fs,
    };
    LandPickCtx c = { L, title, slots, cursor, alphabet, hint, ok_label };
    present_scaled(draw_land_picker_cb, &c);
}

#endif // OR_LANDSCAPE
