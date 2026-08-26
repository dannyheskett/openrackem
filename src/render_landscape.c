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
#include <stdio.h>

RenderTexture2D canvas;              // declared extern in render_internal.h
static int current_scale = 1;

#ifndef PLATFORM_WEB
// Desktop native: crisp integer scaling (1x, 2x, 3x, ...).
static int calculate_scale(void) {
    int scale_w = GetScreenWidth() / BASE_WIDTH;
    int scale_h = GetScreenHeight() / BASE_HEIGHT;
    int scale = (scale_w < scale_h) ? scale_w : scale_h;
    return (scale < 1) ? 1 : scale;
}
#endif

// Blit the fixed-resolution canvas to the window, centered and scaled.
static void present(void) {
    // Record the clean canvas (the recording indicator below is drawn only to
    // the window, so it never appears in the captured video).
    recorder_capture(&canvas);

#ifdef PLATFORM_WEB
    // Web: scale continuously to fill the viewport. Browser chrome usually leaves
    // us just under an integer step, so snapping down (like the desktop app)
    // would render tiny; nearest-neighbor keeps it crisp at fractional scale.
    float sw = (float)GetScreenWidth()  / BASE_WIDTH;
    float sh = (float)GetScreenHeight() / BASE_HEIGHT;
    float scale = (sw < sh) ? sw : sh;
    if (scale < 1.0f) scale = 1.0f;
#else
    float scale = (float)calculate_scale();
#endif
    current_scale = (int)scale;

    float scaled_width  = BASE_WIDTH  * scale;
    float scaled_height = BASE_HEIGHT * scale;
    float offset_x = (GetScreenWidth()  - scaled_width)  / 2.0f;
    float offset_y = (GetScreenHeight() - scaled_height) / 2.0f;

    gfx_begin_frame();
    gfx_clear(BLACK);
    DrawTexturePro(canvas.texture,
        (Rectangle){0, 0, BASE_WIDTH, -BASE_HEIGHT},
        (Rectangle){offset_x, offset_y, scaled_width, scaled_height},
        (Vector2){0, 0}, 0, WHITE);

    // On-screen recording indicator (window-only, not part of the video).
    if (recorder_active()) {
        int s = current_scale;
        DrawCircle(offset_x + 16 * s, offset_y + 14 * s, 5.0f * s, RED);
        gfx_text("REC", offset_x + 24 * s, offset_y + 8 * s, 12 * s, RED);
    }

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

    // Right: stock, discard, the held card, then match state.
    draw_pile_label("STOCK", RIGHT_X, RACK_Y);
    if (game->stock_count > 0) {
        draw_card(RIGHT_X, RACK_Y + 14, CARD_W, CARD_H, 0, false, false);
        snprintf(buf, sizeof buf, "x%d", game->stock_count);
        gfx_text(buf, RIGHT_X + CARD_W + 8, RACK_Y + 14 + (CARD_H - 12) / 2, 12, SLOT_LABEL);
    } else {
        draw_card_outline(RIGHT_X, RACK_Y + 14, CARD_W, CARD_H);
    }

    draw_pile_label("DISCARD", RIGHT_X, RACK_Y + 62);
    if (game->discard_count > 0 && deal_discard_flipped(game)) {
        draw_card(RIGHT_X, RACK_Y + 76, CARD_W, CARD_H,
                  game->discard[game->discard_count - 1], true, false);
    } else {
        draw_card_outline(RIGHT_X, RACK_Y + 76, CARD_W, CARD_H);
    }

    draw_pile_label("HELD", RIGHT_X, RACK_Y + 124);
    if (game->held_card) {
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

// One table scene (into the canvas) with an optional centered overlay.
static void draw_scene_landscape(const Game* game, const TableUi* ui,
                                 const char* overlay_title, const char* overlay_sub,
                                 Color overlay_tc) {
    BeginTextureMode(canvas);
    draw_game_landscape(game, ui);
    if (overlay_title) draw_center_panel_landscape(overlay_title, overlay_sub, overlay_tc);
    EndTextureMode();
    present();
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

    BeginTextureMode(canvas);
    gfx_clear(BLACK);
    draw_menu_panel(m, title, items, count, selected, gap_before, false);
    EndTextureMode();
    present();
}

#endif // OR_LANDSCAPE
