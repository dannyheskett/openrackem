// Portrait (touch) renderer: adaptive layout drawn straight to the screen at
// the device's real resolution — the OPENRACKEM title bar pinned top, a
// compact opponents band beneath it, the ten rack slots filling the middle at
// the largest row height that fits, and the stock/discard band fixed at the
// bottom within the safe area, which is where the thumb is. Compiles to an
// empty object off OR_PORTRAIT.
#include "render_internal.h"
#include "safe_area.h"
#include <stddef.h> // NULL
#include <stdio.h>
#include <string.h>  // strlen

#ifdef OR_PORTRAIT

// Thin full-width title bar across the very top, matching the landscape
// renderer's (24px tall with 16px text on the 480px canvas — keep that ratio).
static int title_fs(int h)    { int fs = h / 45; return (fs < 10) ? 10 : fs; }
static int title_bar_h(int h) { int fs = title_fs(h); return fs + fs / 2; }

// Effective top-bar height: the thin title bar, grown to clear the display
// cutout (front camera) when the surface draws under it, so neither the
// wordmark nor the table below ever sits beneath the camera.
static int top_bar_h(int h) {
    int top, cl, cr;
    safe_area_get(&top, &cl, &cr);
    int tb = title_bar_h(h);
    return (top > tb) ? top : tb;
}

// Global margin: the breathing room applied on every edge and between the
// layout bands. Scales with the short screen dimension so it stays
// proportional across resolutions and aspect ratios.
static int outer_margin(void) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    int m = ((w < h) ? w : h) / 28;
    return (m < 6) ? 6 : m;
}

static int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static void text_centered(const char* s, int cx, int y, int fs, Color c) {
    gfx_text(s, cx - gfx_measure_text(s, fs) / 2, y, fs, c);
}

// Full-width title bar at the very top. When a display cutout (front camera)
// sits in the bar, the "OPENRACKEM" wordmark is laid out around it:
//   - cutout absent            -> centered full word (the desktop/web look).
//   - room both sides          -> "OPEN" left of the camera, "RACKEM" right.
//   - lopsided (corner camera) -> whole word on the roomier side, if it fits.
//   - wide notch, nothing fits -> bar only, no wordmark.
static void draw_title_bar(void) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    int fs = title_fs(h), tb_h = top_bar_h(h);
    int ty = (tb_h - fs) / 2;   // wordmark vertically centered in the bar
    gfx_rect(0, 0, w, tb_h, DARKGRAY);

    int top, cl, cr;
    safe_area_get(&top, &cl, &cr);
    int full = gfx_measure_text("OPENRACKEM", fs);

    // No horizontal extent reported. With no top inset either, there is no
    // cutout at all -> original centered wordmark. If there IS a top inset we
    // just couldn't localize, leave the bar bare rather than risk centering the
    // word under the camera.
    if (cr <= cl) {
        if (top <= 0)
            gfx_text("OPENRACKEM", (w - full) / 2, ty, fs, WHITE);
        return;
    }

    int pad        = fs / 2;        // clearance kept between text and the camera
    int left_room  = cl;            // px available left of the cutout
    int right_room = w - cr;        // px available right of the cutout
    int open_w     = gfx_measure_text("OPEN", fs);
    int rackem_w   = gfx_measure_text("RACKEM", fs);

    if (left_room >= open_w + pad && right_room >= rackem_w + pad) {
        // Split the word around the camera.
        gfx_text("OPEN",   cl - pad - open_w, ty, fs, WHITE);
        gfx_text("RACKEM", cr + pad,          ty, fs, WHITE);
    } else if (right_room >= full + pad || left_room >= full + pad) {
        // Corner camera: keep the word whole on whichever side has more room.
        if (right_room >= left_room)
            gfx_text("OPENRACKEM", cr + pad, ty, fs, WHITE);
        else
            gfx_text("OPENRACKEM", cl - pad - full, ty, fs, WHITE);
    }
    // else: wide notch — leave the bar bare.
}

// --- Shared portrait table layout -------------------------------------------
// Everything is derived from the live screen size each frame, so rotation and
// resize (web) re-fit automatically.
typedef struct {
    int m;                       // outer margin
    int opp_y, opp_h, opp_fs;    // opponents band (one compact row per seat)
    int stats_y, stats_fs;       // points / round / run, small
    int status_y, status_fs;     // the instruction line under it, large
    int rack_y, row, card_w, card_h, rack_x, label_fs;
    int pile_y, pile_w, pile_h, pile_label_fs; // bottom band (3 pile spots)
} PortraitLayout;

static void portrait_layout(const Game* game, PortraitLayout* L) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    int m = outer_margin();
    int tb = top_bar_h(h);

    // Type sizes are ratios of the screen height, and the upper bounds are
    // deliberately generous. The portrait renderer draws at the device's real
    // resolution -- a phone panel is ~2800px tall -- so caps tuned for the
    // 480px landscape canvas pinned every label at a couple of physical
    // millimetres and made the table unreadable on a real device. The LOWER
    // bounds are the ones that still do work, on the small web canvas.
    int nopp = game->rules.player_count - 1;
    L->opp_fs = clampi(h / 44, 10, 64);
    // Two or three opponents share the width; shrink so a name, its thinking
    // dots and the score fit one block without colliding.
    if (nopp >= 3)      L->opp_fs = L->opp_fs * 5 / 8;
    else if (nopp == 2) L->opp_fs = L->opp_fs * 4 / 5;
    L->m = m;
    L->opp_y = tb + m;
    L->opp_h = (nopp > 0) ? L->opp_fs * 3 / 2 : 0;

    L->pile_label_fs = clampi(h / 52, 9, 48);
    L->pile_w = (w - 4 * m) / 3;
    L->pile_h = L->pile_w * 5 / 8;
    // Keep the bottom band from swallowing a squat screen: cap at h/6.
    if (L->pile_h > h / 6) {
        L->pile_h = h / 6;
        L->pile_w = L->pile_h * 8 / 5;
    }
    L->pile_y = h - m - L->pile_h;

    // Two lines: the running totals small, the instruction large. Mid-game the
    // line you actually read is the one telling you what a tap will do.
    L->stats_fs  = clampi(h / 62, 8, 40);
    L->status_fs = clampi(h / 46, 9, 56);
    int status_h = L->stats_fs + 6 + L->status_fs + 4;
    L->stats_y  = L->pile_y - L->pile_label_fs - 6 - status_h;
    L->status_y = L->stats_y + L->stats_fs + 6;

    int rack_top = L->opp_y + L->opp_h + m;
    int avail = L->stats_y - m - rack_top;
    L->row = avail / RACK_SLOTS;
    L->card_h = L->row - 2;
    if (L->card_h < 8) L->card_h = 8;
    L->card_w = clampi(L->card_h * 2, L->card_h + 24, w / 2);
    L->label_fs = clampi(L->card_h / 2, 9, 56);
    // Centre the rack as it reads: the slot labels sit outside the cards, so
    // centring the card alone leaves the whole group visibly right of centre.
    int label_w = gfx_measure_text("50", L->label_fs) + 8;
    L->rack_x = (w - L->card_w + label_w) / 2;
    L->rack_y = rack_top + (avail - L->row * RACK_SLOTS) / 2;
}

// Three dots beside an opponent's name, standing in for the card backs that
// used to sit there: dim and static normally, cycling in the accent colour
// while that seat is deciding. This is the whole of what the old ten backs
// actually communicated -- whose turn it is -- since a rack is never revealed
// until the round ends.
static void draw_thinking_dots(int x, int cy, int fs, bool active) {
    int d = fs / 4;
    if (d < 3) d = 3;
    int gap = d / 2 + 1;
    // Wall-clock, not frame count: the animation reads the same however the
    // display link is pacing us.
    int lit = active ? (int)(GetTime() * 3.0) % 3 : -1;
    for (int i = 0; i < 3; i++) {
        Color c = (i == lit) ? ACCENT
                : active     ? (Color){120, 120,  90, 255}
                             : (Color){ 70,  90,  80, 255};
        gfx_rect_rounded(x + i * (d + gap), cy - d / 2, d, d, d / 2, c);
    }
}

// The seat whose rack fills the middle: the human, or seat 0 as the
// spectator's view in a full-AI (autoplay) game.
static int focus_seat(const Game* g) {
    return (g->rules.human_seat >= 0) ? g->rules.human_seat : 0;
}

static void draw_table_portrait(const Game* game, const TableUi* ui) {
    char buf[64];
    int w = GetScreenWidth();
    PortraitLayout L;
    portrait_layout(game, &L);
    int focus = focus_seat(game);
    int human = game->rules.human_seat;
    bool human_turn = (human >= 0 && game->turn == human &&
                       (game->phase == PHASE_DRAW || game->phase == PHASE_PLACE));

    table_hits_reset();

    // Opponents band: one compact row per other seat -- name, thinking dots,
    // score. No card backs. Ten identical backs per opponent revealed nothing
    // (a rack stays hidden until the round-over reveal) while eating the
    // vertical space the rack itself needs to be legible on a phone.
    int nopp = game->rules.player_count - 1;
    if (nopp > 0) {
        int bw = (w - L.m * (nopp + 1)) / nopp;
        int bi = 0;
        for (int s = 0; s < game->rules.player_count; s++) {
            if (s == focus) continue;
            int bx = L.m + bi * (bw + L.m);
            bool their_turn = (game->turn == s &&
                               (game->phase == PHASE_DRAW || game->phase == PHASE_PLACE));
            if (their_turn) {
                gfx_rect_lines(bx - 2, L.opp_y - 2, bw + 4, L.opp_h + 4, ACCENT);
            }
            snprintf(buf, sizeof buf, "%d", game->players[s].score);
            int score_w = gfx_measure_text(buf, L.opp_fs);
            int dots_w  = L.opp_fs;             // three dots plus their gaps
            gfx_text(buf, bx + bw - score_w, L.opp_y + (L.opp_h - L.opp_fs) / 2,
                     L.opp_fs, YELLOW);
            draw_thinking_dots(bx + bw - score_w - L.opp_fs / 3 - dots_w,
                               L.opp_y + L.opp_h / 2, L.opp_fs, their_turn);

            // Names are player-chosen online, so shrink one that would run
            // into the dots rather than letting it overlap them.
            const char* nm = seat_name(game, s);
            int room = bw - score_w - dots_w - L.opp_fs;
            int nfs = L.opp_fs;
            while (nfs > 8 && gfx_measure_text(nm, nfs) > room) nfs -= 1;
            gfx_text(nm, bx, L.opp_y + (L.opp_h - nfs) / 2, nfs,
                     their_turn ? ACCENT : WHITE);
            bi++;
        }
    }

    // The rack: slot #50 on top, labels left of each card, full-width tap rows.
    int dealt = deal_cards_for_seat(game, focus);
    for (int s = RACK_SLOTS - 1; s >= 0; s--) {
        int y = L.rack_y + (RACK_SLOTS - 1 - s) * L.row;
        snprintf(buf, sizeof buf, "%d", (s + 1) * 5);
        gfx_text(buf, L.rack_x - 8 - gfx_measure_text(buf, L.label_fs),
                 y + (L.card_h - L.label_fs) / 2, L.label_fs, SLOT_LABEL);
        if (RACK_SLOTS - 1 - s < dealt) {
            bool cur = (human_turn && game->phase == PHASE_PLACE && ui->cursor == s);
            draw_card(L.rack_x, y, L.card_w, L.card_h,
                      game->players[focus].rack.slots[s], true, cur);
        } else {
            draw_card_outline(L.rack_x, y, L.card_w, L.card_h);
        }
        table_hit_set(s, (Rectangle){0, (float)y, (float)w, (float)L.row});
    }

    // Status line: score, round, whose turn / what a tap does.
    const char* turn_msg;
    if (game->phase == PHASE_DEAL) turn_msg = "DEALING...";
    else if (human_turn) {
        turn_msg = (game->phase == PHASE_DRAW) ? "TAP A PILE TO DRAW"
                 : game->held_from_discard     ? "TAP A SLOT TO PLACE"
                                               : "TAP A SLOT, OR THE DISCARD TO THROW";
    } else {
        snprintf(buf, sizeof buf, "%s THINKING", seat_name(game, game->turn));
        turn_msg = buf;
    }
    // Two lines, because they are read at different moments. The totals sit on
    // top in small type; the instruction gets the large type, since that is the
    // line you look at every single turn. In 2-player games an ascending rack
    // also needs a run of 3 consecutive cards to go out, so the current longest
    // run rides along and a sorted rack that won't finish makes sense.
    char stats[64];
    if (rules_require_run(&game->rules) && game->rules.human_seat >= 0 &&
        game->phase != PHASE_DEAL && game->phase != PHASE_ROUND_OVER &&
        game->phase != PHASE_MATCH_OVER) {
        int run = score_longest_run(&game->players[game->rules.human_seat].rack);
        snprintf(stats, sizeof stats, "%d PTS   R%d   RUN %d/3",
                 game->players[focus].score, game->round_no, run);
    } else {
        snprintf(stats, sizeof stats, "%d PTS   R%d",
                 game->players[focus].score, game->round_no);
    }
    text_centered(stats, w / 2, L.stats_y, L.stats_fs, SLOT_LABEL);

    // The longest instruction ("TAP A SLOT, OR THE DISCARD TO THROW") is what
    // sets the size; shrink it to the margins rather than let it run off-screen.
    int msg_fs = L.status_fs;
    while (msg_fs > 9 && gfx_measure_text(turn_msg, msg_fs) > w - 2 * L.m) msg_fs -= 2;
    text_centered(turn_msg, w / 2, L.status_y + (L.status_fs - msg_fs) / 2, msg_fs,
                  human_turn ? ACCENT : SLOT_LABEL);

    // Bottom band: STOCK / DISCARD / HELD, thumb-sized, inside the margin.
    // While a card is in flight its destination draws one step behind (the
    // pile's previous top, an empty held spot), so landing = arrival.
    bool flying = anim_in_flight(game);
    bool draw_arrives_held = flying && (game->anim.kind == ANIM_DRAW_STOCK ||
                                        game->anim.kind == ANIM_DRAW_DISCARD);
    bool card_arrives_pile = flying && (game->anim.kind == ANIM_PLACE ||
                                        game->anim.kind == ANIM_DISCARD);
    int px[3];
    px[0] = L.m;
    px[1] = L.m * 2 + L.pile_w;
    px[2] = L.m * 3 + L.pile_w * 2;
    int label_y = L.pile_y - L.pile_label_fs - 4;

    gfx_text("STOCK", px[0], label_y, L.pile_label_fs, SLOT_LABEL);
    if (game->stock_count > 0) {
        draw_card(px[0], L.pile_y, L.pile_w, L.pile_h, 0, false, false);
        snprintf(buf, sizeof buf, "x%d", game->stock_count);
        gfx_text(buf, px[0] + 4, L.pile_y + L.pile_h - L.pile_label_fs - 2,
                 L.pile_label_fs, (Color){220, 200, 180, 255});
    } else {
        draw_card_outline(px[0], L.pile_y, L.pile_w, L.pile_h);
    }
    table_hit_set(HIT_STOCK, (Rectangle){(float)(px[0] - L.m / 2), (float)(label_y - L.m / 2),
                                         (float)(L.pile_w + L.m), (float)(L.pile_h + L.pile_label_fs + L.m)});

    gfx_text("DISCARD", px[1], label_y, L.pile_label_fs, SLOT_LABEL);
    if (card_arrives_pile) {
        if (game->discard_count >= 2) {
            draw_card(px[1], L.pile_y, L.pile_w, L.pile_h,
                      game->discard[game->discard_count - 2], true, false);
        } else {
            draw_card_outline(px[1], L.pile_y, L.pile_w, L.pile_h);
        }
    } else if (game->discard_count > 0 && deal_discard_flipped(game)) {
        draw_card(px[1], L.pile_y, L.pile_w, L.pile_h,
                  game->discard[game->discard_count - 1], true, false);
    } else {
        draw_card_outline(px[1], L.pile_y, L.pile_w, L.pile_h);
    }
    table_hit_set(HIT_DISCARD, (Rectangle){(float)(px[1] - L.m / 2), (float)(label_y - L.m / 2),
                                           (float)(L.pile_w + L.m), (float)(L.pile_h + L.pile_label_fs + L.m)});

    gfx_text("HELD", px[2], label_y, L.pile_label_fs, SLOT_LABEL);
    if (game->held_card && !draw_arrives_held) {
        bool face_up = (game->turn == focus) || game->held_from_discard;
        draw_card(px[2], L.pile_y, L.pile_w, L.pile_h, game->held_card, face_up, false);
    } else {
        draw_card_outline(px[2], L.pile_y, L.pile_w, L.pile_h);
    }

    // The one visibly moving card, over the finished table. An opponent's
    // exchange flies from their block, not a slot — which slot they filled is
    // information the table doesn't show.
    if (flying) {
        float t = anim_progress(game);
        Rectangle stock_r   = {(float)px[0], (float)L.pile_y, (float)L.pile_w, (float)L.pile_h};
        Rectangle discard_r = {(float)px[1], (float)L.pile_y, (float)L.pile_w, (float)L.pile_h};
        Rectangle held_r    = {(float)px[2], (float)L.pile_y, (float)L.pile_w, (float)L.pile_h};
        Rectangle from;
        switch (game->anim.kind) {
        case ANIM_DRAW_STOCK:
            draw_flying_card(stock_r, held_r, t, 0, false);
            break;
        case ANIM_DRAW_DISCARD:
            draw_flying_card(discard_r, held_r, t, game->anim.card, true);
            break;
        case ANIM_PLACE:
            if (game->anim.seat == focus) {
                int y = L.rack_y + (RACK_SLOTS - 1 - game->anim.slot) * L.row;
                from = (Rectangle){(float)L.rack_x, (float)y, (float)L.card_w, (float)L.card_h};
            } else {
                int bi = 0;
                for (int s = 0; s < game->anim.seat; s++) {
                    if (s != focus) bi++;
                }
                int bw = (w - L.m * (game->rules.player_count)) / (game->rules.player_count - 1);
                from = (Rectangle){(float)(L.m + bi * (bw + L.m) + bw / 2 - 12),
                                   (float)(L.opp_y + 4), 24.0f, (float)(L.opp_h - 8)};
            }
            draw_flying_card(from, discard_r, t, game->anim.card, true);
            break;
        case ANIM_DISCARD:
            draw_flying_card(held_r, discard_r, t, game->anim.card, true);
            break;
        }
    }
}

// --- Round scoring / standings / match over ---------------------------------
static void round_banner(const Game* g, char* out, size_t n) {
    if (g->round_winner == NO_WINNER) {
        snprintf(out, n, "STALEMATE");
    } else if ((int)g->round_winner == g->rules.human_seat) {
        snprintf(out, n, "RACK 'EM!");
    } else {
        snprintf(out, n, "%s: RACK 'EM!", seat_name(g, g->round_winner));
    }
}

static void team_label(const Game* g, int pair, char* out, size_t n) {
    snprintf(out, n, "%s & %s", seat_name(g, pair), seat_name(g, pair + 2));
}

static void draw_round_scoring_portrait(const Game* game) {
    char buf[64];
    int w = GetScreenWidth(), h = GetScreenHeight();
    int m = outer_margin();
    int tb = top_bar_h(h);
    int title_fs2 = clampi(h / 22, 14, 120);
    int fs = clampi(h / 45, 9, 56);

    round_banner(game, buf, sizeof buf);
    text_centered(buf, w / 2, tb + m * 2, title_fs2,
                  ((int)game->round_winner == game->rules.human_seat &&
                   game->round_winner != NO_WINNER) ? ACCENT : WHITE);
    snprintf(buf, sizeof buf, "ROUND %d", game->round_no);
    text_centered(buf, w / 2, tb + m * 2 + title_fs2 + 6, fs, SLOT_LABEL);

    // One block per seat: name + points line, the revealed rack beneath.
    int n = game->rules.player_count;
    int y = tb + m * 2 + title_fs2 + 6 + fs + m * 2;
    int block_h = (h - y - m * 3 - fs) / n;
    int cw = (w - 2 * m - 9) / 10;
    int ch = clampi(block_h - fs - 8, 12, cw * 2);
    for (int s = 0; s < n; s++) {
        bool won = ((int)game->round_winner == s);
        gfx_text(seat_name(game, s), m, y, fs, won ? ACCENT : WHITE);
        snprintf(buf, sizeof buf, "+%d = %d", game->round_points[s], game->players[s].score);
        gfx_text(buf, w - m - gfx_measure_text(buf, fs), y, fs, YELLOW);
        draw_mini_rack(m, y + fs + 4, cw, ch, &game->players[s].rack, true);
        y += block_h;
    }
    text_centered("TAP TO CONTINUE", w / 2, h - m - fs, fs, GRAY);
}

static void draw_standings_portrait(const Game* game) {
    char buf[64];
    int w = GetScreenWidth(), h = GetScreenHeight();
    int m = outer_margin();
    int tb = top_bar_h(h);
    int title_fs2 = clampi(h / 22, 14, 120);
    int fs = clampi(h / 40, 10, 64);

    text_centered("STANDINGS", w / 2, tb + m * 2, title_fs2, WHITE);
    snprintf(buf, sizeof buf, "AFTER ROUND %d - FIRST TO %d",
             game->round_no, game->rules.target_score);
    text_centered(buf, w / 2, tb + m * 2 + title_fs2 + 6, fs * 3 / 4, SLOT_LABEL);

    int y = tb + m * 2 + title_fs2 + 6 + fs + m * 2;
    if (game->rules.partners) {
        for (int p = 0; p < 2; p++) {
            char tl[32];
            team_label(game, p, tl, sizeof tl);
            int total = game->players[p].score + game->players[p + 2].score;
            gfx_text(tl, m * 2, y, fs, WHITE);
            snprintf(buf, sizeof buf, "%d", total);
            gfx_text(buf, w - m * 2 - gfx_measure_text(buf, fs * 3 / 2), y, fs * 3 / 2, YELLOW);
            y += fs * 3;
        }
    } else {
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
            gfx_text(buf, m * 2, y + fs / 4, fs * 3 / 4, SLOT_LABEL);
            gfx_text(seat_name(game, s), m * 2 + fs * 2, y, fs,
                     (s == game->rules.human_seat) ? ACCENT : WHITE);
            snprintf(buf, sizeof buf, "%d", game->players[s].score);
            gfx_text(buf, w - m * 2 - gfx_measure_text(buf, fs * 3 / 2), y, fs * 3 / 2, YELLOW);
            y += fs * 3;
        }
    }
    int fs2 = clampi(h / 45, 9, 48);
    text_centered("TAP TO CONTINUE", w / 2, h - m - fs2, fs2, GRAY);
}

static void draw_match_over_portrait(const Game* game) {
    char buf[64];
    int w = GetScreenWidth(), h = GetScreenHeight();
    int m = outer_margin();
    int tb = top_bar_h(h);
    int title_fs2 = clampi(h / 20, 16, 132);
    int fs = clampi(h / 40, 10, 64);

    text_centered("MATCH OVER", w / 2, tb + m * 3, title_fs2, WHITE);

    bool human_won;
    if (game->rules.partners) {
        char tl[32];
        team_label(game, game->match_winner, tl, sizeof tl);
        human_won = (game->rules.human_seat >= 0 &&
                     game->rules.human_seat % 2 == game->match_winner);
        snprintf(buf, sizeof buf, human_won ? "%s WIN!" : "%s WIN", tl);
    } else {
        human_won = ((int)game->match_winner == game->rules.human_seat);
        if (human_won) snprintf(buf, sizeof buf, "YOU WIN!");
        else snprintf(buf, sizeof buf, "%s WINS", seat_name(game, game->match_winner));
    }
    text_centered(buf, w / 2, tb + m * 3 + title_fs2 + m, fs * 3 / 2,
                  human_won ? ACCENT : WHITE);

    int y = tb + m * 3 + title_fs2 + m + fs * 2 + m * 2;
    if (game->rules.partners) {
        for (int p = 0; p < 2; p++) {
            char tl[32];
            team_label(game, p, tl, sizeof tl);
            int total = game->players[p].score + game->players[p + 2].score;
            gfx_text(tl, m * 2, y, fs, WHITE);
            snprintf(buf, sizeof buf, "%d", total);
            gfx_text(buf, w - m * 2 - gfx_measure_text(buf, fs * 3 / 2), y, fs * 3 / 2, YELLOW);
            y += fs * 3;
        }
    } else {
        for (int s = 0; s < game->rules.player_count; s++) {
            gfx_text(seat_name(game, s), m * 2, y, fs,
                     (s == game->rules.human_seat) ? ACCENT : WHITE);
            snprintf(buf, sizeof buf, "%d", game->players[s].score);
            gfx_text(buf, w - m * 2 - gfx_measure_text(buf, fs * 3 / 2), y, fs * 3 / 2, YELLOW);
            y += fs * 3;
        }
    }
    int fs2 = clampi(h / 45, 9, 48);
    text_centered("TAP TO CONTINUE", w / 2, h - m - fs2, fs2, GRAY);
}

static void draw_game_portrait(const Game* game, const TableUi* ui) {
    gfx_clear(TABLE_BG);
    draw_title_bar();
    if (game->phase == PHASE_ROUND_OVER) {
        table_hits_reset();
        if (ui->standings) draw_standings_portrait(game);
        else draw_round_scoring_portrait(game);
    } else if (game->phase == PHASE_MATCH_OVER) {
        table_hits_reset();
        draw_match_over_portrait(game);
    } else {
        draw_table_portrait(game, ui);
    }
}

static void draw_center_panel_portrait(const char* title, const char* subtitle, Color tc) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    int base = (w < h) ? w : h;   // keep the dialog compact even in a wide window
    int pw = base * 82 / 100;
    int ph = base * 26 / 100;
    draw_center_panel_at(w, h, pw, ph, ph * 28 / 100, ph * 13 / 100,
                         ph * 24 / 100, ph * 62 / 100, title, subtitle, tc);
}

// One table scene with an optional centered overlay (pause).
static void draw_scene_portrait(const Game* game, const TableUi* ui,
                                const char* overlay_title, const char* overlay_sub,
                                Color overlay_tc) {
    gfx_begin_frame();
    draw_game_portrait(game, ui);
    if (overlay_title) draw_center_panel_portrait(overlay_title, overlay_sub, overlay_tc);
    gfx_end_frame();
}

void render_frame_portrait(const Game* g, const TableUi* ui) {
    draw_scene_portrait(g, ui, NULL, NULL, WHITE);
}
void render_pause_portrait(const Game* g, const TableUi* ui) {
    draw_scene_portrait(g, ui, "GAME PAUSED", "Tap to resume", YELLOW);
}

void render_menu_portrait(const char* title, const char* const* items, int count,
                          int selected, int gap_before) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    int line_h = h / 20, item_fs = h / 28;
    int extra = (gap_before >= 0) ? 1 : 0;
    int base = (w < h) ? w : h;              // keep the panel compact in a wide window
    int panel_w = base * 82 / 100;

    // Shrink the title if it would overrun the panel (wide tablets).
    int title_size = h / 16;
    while (title_size > 12 && gfx_measure_text(title, title_size) > panel_w - line_h) title_size -= 2;

    int panel_h = title_size + line_h + (count + extra) * line_h + line_h * 2;
    int px = w / 2 - panel_w / 2, py = (h - panel_h) / 2;
    MenuLayout m = { .cx = w / 2, .px = px, .py = py, .panel_w = panel_w, .panel_h = panel_h,
                     .title_size = title_size, .title_y = py + line_h,
                     .items_y = py + line_h + title_size + line_h,
                     .line_h = line_h, .item_fs = item_fs };

    gfx_begin_frame();
    gfx_clear(BLACK);
    draw_menu_panel(m, title, items, count, selected, gap_before, true);
    gfx_end_frame();
}

// The slot picker, sized off the live screen like everything else here. Twelve
// name slots have to fit a phone's width, so the slots themselves are modest;
// the spinner bands and the buttons are what get the thumb-sized targets.
void render_picker_portrait(const char* title, const char* slots, int cursor,
                            const char* alphabet, const char* hint,
                            const char* ok_label) {
    int w = GetScreenWidth(), h = GetScreenHeight();
    int base = (w < h) ? w : h;
    int n = (int)strlen(slots);
    if (n < 1) n = 1;
    if (n > PICKER_MAX_SLOTS) n = PICKER_MAX_SLOTS;

    int pad     = base / 24;
    int panel_w = base * 96 / 100;
    int gap     = clampi(base / 100, 2, 20);
    int slot_w  = (panel_w - pad - (n - 1) * gap) / n;
    if (slot_w > base / 6) slot_w = base / 6;   // few slots shouldn't sprawl
    int slot_h  = slot_w * 9 / 5;
    int slot_fs = slot_h * 3 / 5;
    int band_h  = slot_h * 4 / 5;

    int title_size = clampi(h / 20, 14, 120);
    int hint_fs    = clampi(h / 52, 9, 44);
    int btn_h      = clampi(h / 16, 22, 150);
    int btn_fs     = clampi(h / 34, 10, 64);
    int btn_w      = panel_w * 2 / 5;

    int panel_h = pad + title_size + pad + band_h + slot_h + band_h + pad
                + hint_fs + pad + btn_h + pad;
    int px = w / 2 - panel_w / 2;
    int py = (h - panel_h) / 2;
    int tb = top_bar_h(h);
    if (py < tb + pad) py = tb + pad;

    PickerLayout L = {
        .cx = w / 2, .px = px, .py = py, .panel_w = panel_w, .panel_h = panel_h,
        .title_size = title_size, .title_y = py + pad,
        .slots_y = py + pad + title_size + pad + band_h,
        .slot_w = slot_w, .slot_h = slot_h, .slot_gap = gap, .slot_fs = slot_fs,
        .band_h = band_h,
        .hint_y = py + pad + title_size + pad + band_h + slot_h + band_h + pad,
        .hint_fs = hint_fs,
        .btn_y = py + panel_h - pad - btn_h,
        .btn_w = btn_w, .btn_h = btn_h, .btn_fs = btn_fs,
    };

    gfx_begin_frame();
    gfx_clear(BLACK);
    draw_title_bar();
    draw_picker_panel(L, title, slots, cursor, alphabet, hint, ok_label, true);
    gfx_end_frame();
}

#endif // OR_PORTRAIT
