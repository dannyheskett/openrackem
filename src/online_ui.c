// The online screens (see online_ui.h): the room list and the waiting room,
// drawn in the same portrait column as the table so they read as one app on
// a phone and in the desktop column. Every size derives from the view.
#include "online_ui.h"
#include "render_internal.h"
#include "gfx.h"

#include <stdio.h>
#include <string.h>

static const Color PANEL      = { 24,  64,  44, 255 };   // a row on the felt
static const Color PANEL_SEL  = { 36,  88,  60, 255 };
static const Color READY_DOT  = { 92, 214, 128, 255 };   // ready: filled green
static const Color DIM        = { 110, 120, 130, 255 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void text_centered(const char* s, int cx, int y, int fs, Color c) {
    gfx_text(s, cx - gfx_measure_text(s, fs) / 2, y, fs, c);
}

// Shrink `fs` until `s` fits `width`.
static int fit(const char* s, int fs, int width) {
    while (fs > 8 && gfx_measure_text(s, fs) > width) fs--;
    return fs;
}

// --- Tap targets ------------------------------------------------------------------
#define OL_MAX_HITS 48
static struct { Rectangle r; int id; } s_hits[OL_MAX_HITS];
static int s_hit_n;

static void hits_reset(void) {
    if (hit_capture_active()) s_hit_n = 0;
}

static void hit_add(int id, int x, int y, int w, int h) {
    if (!hit_capture_active() || s_hit_n >= OL_MAX_HITS) return;
    s_hits[s_hit_n].r = hit_window_rect((Rectangle){ (float)x, (float)y, (float)w, (float)h });
    s_hits[s_hit_n].id = id;
    s_hit_n++;
}

int online_hit_test(Vector2 p) {
    for (int i = s_hit_n - 1; i >= 0; i--) {
        if (CheckCollisionPointRec(p, s_hits[i].r)) return s_hits[i].id;
    }
    return OL_HIT_NONE;
}

// --- Shared bits ------------------------------------------------------------------
// A ready dot: filled green when ready, an empty ring when not.
static void draw_dot(int cx, int cy, int r, bool ready, bool dim) {
    if (ready) {
        gfx_circle((float)cx, (float)cy, (float)r, dim ? DIM : READY_DOT);
    } else {
        Color c = dim ? DIM : WHITE;
        gfx_circle_lines((float)cx, (float)cy, (float)r, c);
        if (r > 6) gfx_circle_lines((float)cx, (float)cy, (float)(r - 1), c);
    }
}

// "o Dan  o Alex  o Kim" on one line, shrunk to fit `width`.
static void draw_names(int x, int y, int fs, int width, char names[][NG_NAME],
                       const uint8_t* ready, int n, Color ink) {
    for (;;) {
        int total = 0;
        for (int i = 0; i < n; i++) total += fs + gfx_measure_text(names[i], fs) + fs;
        if (total <= width || fs <= 8) break;
        fs--;
    }
    int r = fs * 3 / 10;
    for (int i = 0; i < n; i++) {
        draw_dot(x + r, y + fs / 2, r, ready[i], false);
        x += fs;
        gfx_text(names[i], x, y, fs, ink);
        x += gfx_measure_text(names[i], fs) + fs;
    }
}

static void draw_button(int id, int x, int y, int w, int h, const char* label,
                        bool primary, bool focused) {
    if (primary) rect_rounded_px(x, y, w, h, h / 3, ACCENT);
    else rect_rounded_px(x, y, w, h, h / 3, PANEL);
    if (focused) gfx_rect_rounded_lines(x - 2, y - 2, w + 4, h + 4, 0.3f, primary ? WHITE : ACCENT);
    int fs = fit(label, clampi(h * 2 / 5, 9, 64), w - h / 3);
    text_centered(label, x + w / 2, y + (h - fs) / 2, fs, primary ? TURN_INK : WHITE);
    hit_add(id, x, y, w, h);
}

// A room's capacity for drawing: what the session holds, or four when it holds
// nothing (a view built by hand, or a server from before the setting).
static int seats_of(int seats) {
    return (seats >= RULES_SEATS_MIN && seats <= RULES_SEATS_MAX) ? seats : MAX_PLAYERS;
}

static void rules_line(char* out, size_t n, int target, bool bonus, bool partners) {
    snprintf(out, n, "PLAY TO %d%s%s", target, bonus ? " - BONUS" : "",
             partners ? " - PARTNERS" : "");
}

// --- Room list ----------------------------------------------------------------------
int online_list_rows(const OnlineView* v, OlRow rows[OL_MAX_ROWS]) {
    int n = 0;
    if (v->has_return) rows[n++] = (OlRow){ OLR_RETURN, -1 };
    rows[n++] = (OlRow){ OLR_NEW, -1 };
    for (int i = 0; i < v->ng->room_count && n < OL_MAX_ROWS; i++) {
        rows[n++] = (OlRow){ OLR_ROOM, i };
    }
    return n;
}

int online_list_pinned(const OlRow* rows, int n) {
    int k = 0;
    while (k < n && rows[k].kind != OLR_ROOM) k++;
    return k;
}

static void draw_row(const OnlineView* v, const OlRow* row, int idx, int x, int y,
                     int rw, int rh, bool sel) {
    const NetGame* ng = v->ng;
    bool hot = row->kind == OLR_RETURN;
    rect_rounded_px(x, y, rw, rh, rh / 5, hot ? ACCENT : sel ? PANEL_SEL : PANEL);
    if (sel) gfx_rect_rounded_lines(x - 2, y - 2, rw + 4, rh + 4, 0.25f, hot ? WHITE : ACCENT);
    hit_add(OL_HIT_ROW + idx, x, y, rw, rh);

    int pad = rh / 4;
    int tfs = rh * 3 / 10, sfs = rh * 9 / 40;
    int ty = y + rh / 2 - tfs + tfs / 6, sy = ty + tfs + rh / 12;
    int tw = rw - 2 * pad;
    Color ink = hot ? TURN_INK : WHITE, sub = hot ? TURN_INK : SLOT_LABEL;
    char t[64], s[96];

    switch (row->kind) {
    case OLR_RETURN:
        snprintf(t, sizeof t, "RETURN TO GAME");
        snprintf(s, sizeof s, "YOUR SEAT IS WAITING FOR YOU");
        break;
    case OLR_NEW:
        snprintf(t, sizeof t, "NEW ROOM");
        snprintf(s, sizeof s, "SET THE RULES AND WAIT FOR PLAYERS");
        break;
    case OLR_ROOM: {
        const NgRoomRow* rr = &ng->rooms[row->room];
        char nm[MAX_PLAYERS][NG_NAME];
        int k = rr->n < MAX_PLAYERS ? rr->n : MAX_PLAYERS;
        for (int i = 0; i < k; i++) memcpy(nm[i], rr->names[i], NG_NAME);
        draw_names(x + pad, ty, tfs, tw, nm, rr->ready, k, WHITE);
        char rl[48];
        rules_line(rl, sizeof rl, rr->target, rr->bonus, rr->partners);
        snprintf(s, sizeof s, "%d/%d - %s", rr->n, seats_of(rr->seats), rl);
        gfx_text(s, x + pad, sy, fit(s, sfs, tw), SLOT_LABEL);
        return;
    }
    }
    gfx_text(t, x + pad, ty, fit(t, tfs, tw), ink);
    gfx_text(s, x + pad, sy, fit(s, sfs, tw), sub);
}

static void draw_list_cb(void* p) {
    OnlineView* v = (OnlineView*)p;
    const NetGame* ng = v->ng;
    int w = portrait_view_w(), h = portrait_view_h();
    int m = portrait_margin(), tb = portrait_top_bar_h();
    gfx_clear(TABLE_BG);
    portrait_title_bar();
    hits_reset();

    int head_fs = clampi(h / 22, 14, 90);
    int small_fs = clampi(h / 48, 9, 40);
    int y = tb + m;
    text_centered("PLAY ONLINE", w / 2, y, fit("PLAY ONLINE", head_fs, w - 2 * m), WHITE);
    y += head_fs + m / 3;
    char who[40];
    snprintf(who, sizeof who, "PLAYING AS %s", v->player);
    text_centered(who, w / 2, y, fit(who, small_fs, w - 2 * m), SLOT_LABEL);
    y += small_fs + m;

    // Back, at the bottom.
    int ctl_h = clampi(h / 20, 22, 110);
    int back_h = ctl_h, back_y = h - m - back_h;
    draw_button(OL_HIT_BACK, w / 2 - (w - 2 * m) / 4, back_y, (w - 2 * m) / 2, back_h,
                "BACK", false, false);

    OlRow rows[OL_MAX_ROWS];
    int n = online_list_rows(v, rows);
    int pinned = online_list_pinned(rows, n);
    int row_h = clampi(h / 12, 34, 200), gap = m / 3;
    int rw = w - 2 * m;
    for (int i = 0; i < pinned; i++) {
        draw_row(v, &rows[i], i, m, y, rw, row_h, v->sel == i);
        y += row_h + gap;
    }

    char hdr[64];
    int pub = n - pinned;
    if (!ng->have_rooms) snprintf(hdr, sizeof hdr, "LOOKING FOR ROOMS...");
    else if (pub == 0) snprintf(hdr, sizeof hdr, "NO ROOMS WAITING - MAKE ONE");
    else if (ng->rooms_total > pub) snprintf(hdr, sizeof hdr, "ROOMS - %d OF %d", pub, ng->rooms_total);
    else snprintf(hdr, sizeof hdr, "ROOMS - TAP ONE TO JOIN");
    y += m / 3;
    gfx_text(hdr, m, y, fit(hdr, small_fs, rw), SLOT_LABEL);
    y += small_fs + m / 2;

    // The rooms scroll beneath the pinned rows, keeping the selection in view.
    int area_bottom = back_y - m / 2 - small_fs - m / 3;
    int visible = (area_bottom - y) / (row_h + gap);
    if (visible < 1) visible = 1;
    if (v->sel >= pinned) {
        int idx = v->sel - pinned;
        if (idx < v->scroll) v->scroll = idx;
        if (idx >= v->scroll + visible) v->scroll = idx - visible + 1;
    }
    int max_scroll = pub - visible > 0 ? pub - visible : 0;
    if (v->scroll > max_scroll) v->scroll = max_scroll;
    if (v->scroll < 0) v->scroll = 0;
    for (int k = 0; k < visible && v->scroll + k < pub; k++) {
        int i = pinned + v->scroll + k;
        draw_row(v, &rows[i], i, m, y, rw, row_h, v->sel == i);
        y += row_h + gap;
    }

    int foot_y = back_y - m / 3 - small_fs;
    if (v->notice && v->notice[0]) {
        text_centered(v->notice, w / 2, foot_y, fit(v->notice, small_fs, rw), ACCENT);
    } else if (v->scroll + visible < pub) {
        char more[32];
        snprintf(more, sizeof more, "%d MORE BELOW", pub - v->scroll - visible);
        text_centered(more, w / 2, foot_y, small_fs, SLOT_LABEL);
    }
}

void online_render_list(OnlineView* v) {
    portrait_present(draw_list_cb, v);
}

// --- Waiting room ---------------------------------------------------------------------
// Everyone is ready and the start countdown is running.
static bool lobby_counting(const NgLobby* L) {
    return L->block[0] == '\0' && L->count_ms >= 0;
}

int online_lobby_buttons(const OnlineView* v, int ids[4]) {
    const NgLobby* L = &v->ng->lobby;
    int n = 0;
    // The countdown is a commitment: the server takes neither an un-ready nor
    // a team switch while it runs, so neither is offered. Leave always is.
    bool counting = lobby_counting(L);
    if (!counting) ids[n++] = OLB_READY;
    if (L->can_force && !counting) ids[n++] = OLB_FORCE;
    if (L->partners && !counting) ids[n++] = OLB_SWITCH;
    ids[n++] = OLB_LEAVE;
    return n;
}

static const char* button_label(int id, const NgLobby* L) {
    switch (id) {
    case OLB_READY:  return L->ready[L->me] ? "NOT READY" : "READY TO PLAY";
    case OLB_FORCE:  return "START WITHOUT THEM";
    case OLB_SWITCH: return "SWITCH TEAM";
    case OLB_LEAVE:  return "LEAVE";
    }
    return "";
}

void online_result_line(const NetGame* ng, char* out, size_t cap) {
    out[0] = '\0';
    const Game* g = &ng->game;
    if (!ng->have_result || g->phase != PHASE_MATCH_OVER || g->match_winner == NO_WINNER) return;
    int n = g->rules.player_count;
    if (n < 2 || n > MAX_PLAYERS) return;
    size_t k = 0;
    if (g->rules.partners) {
        int w = g->match_winner & 1;
        k = (size_t)snprintf(out, cap, "%s & %s WON  %d - %d",
                             ng->handles[w], ng->handles[w + 2],
                             g->players[w].score + g->players[w + 2].score,
                             g->players[1 - w].score + g->players[3 - w].score);
    } else {
        int w = g->match_winner;
        if (w >= n) return;
        k = (size_t)snprintf(out, cap, "%s WON ", ng->handles[w]);
        // Scores in finishing order.
        int order[MAX_PLAYERS];
        for (int i = 0; i < n; i++) order[i] = i;
        for (int i = 1; i < n; i++) {
            for (int j = i; j > 0 &&
                 g->players[order[j]].score > g->players[order[j - 1]].score; j--) {
                int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
            }
        }
        for (int i = 0; i < n && k < cap; i++) {
            k += (size_t)snprintf(out + k, cap - k, "%s%d", i ? " - " : " ",
                                  g->players[order[i]].score);
        }
    }
    (void)k;
}

// One member row: dot, name, tags.
static void draw_member(const NgLobby* L, int mi, int x, int y, int rw, int rh) {
    bool used = L->used[mi];
    bool me = used && mi == L->me;
    rect_rounded_px(x, y, rw, rh, rh / 4, me ? PANEL_SEL : PANEL);
    if (me) gfx_rect_rounded_lines(x - 1, y - 1, rw + 2, rh + 2, 0.25f, ACCENT);
    int fs = rh * 2 / 5, pad = rh / 3, r = fs * 2 / 5;
    if (!used) {
        draw_dot(x + pad + r, y + rh / 2, r, false, true);
        gfx_text("OPEN SEAT", x + pad + 2 * r + pad / 2, y + (rh - fs) / 2, fs, DIM);
        return;
    }
    bool on = L->on[mi];
    draw_dot(x + pad + r, y + rh / 2, r, L->ready[mi], !on);
    int nx = x + pad + 2 * r + pad / 2;
    // The name comes first: a tag that would crowd it below 3/4 size goes.
    const char* tag = me ? "(YOU)" : !on ? "AWAY" : "";
    int tfs = fs * 3 / 4;
    int tag_w = tag[0] ? gfx_measure_text(tag, tfs) + pad : 0;
    int nfs = fit(L->names[mi], fs, x + rw - pad - tag_w - nx);
    if (tag[0] && nfs < fs * 3 / 4) {
        tag = "";
        nfs = fit(L->names[mi], fs, x + rw - pad - nx);
    }
    gfx_text(L->names[mi], nx, y + (rh - nfs) / 2, nfs, on ? WHITE : DIM);
    if (tag[0]) {
        gfx_text(tag, x + rw - pad - gfx_measure_text(tag, tfs), y + (rh - tfs) / 2, tfs,
                 me ? ACCENT : SLOT_LABEL);
    }
}

static void status_text(const NgLobby* L, char* out, size_t n) {
    if (lobby_counting(L)) {
        snprintf(out, n, "STARTING IN %d", (L->count_ms + 999) / 1000);
    } else if (strcmp(L->block, "need2") == 0) {
        snprintf(out, n, "WAITING FOR ANOTHER PLAYER");
    } else if (strcmp(L->block, "partners4") == 0) {
        snprintf(out, n, "PARTNERS NEEDS 4 PLAYERS");
    } else if (strcmp(L->block, "teams") == 0) {
        snprintf(out, n, "TEAMS MUST BE 2 AND 2");
    } else if (strcmp(L->block, "ready") == 0) {
        if (!L->ready[L->me]) { snprintf(out, n, "READY WHEN YOU ARE"); return; }
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (L->used[i] && L->on[i] && !L->ready[i]) {
                snprintf(out, n, "WAITING FOR %s", L->names[i]);
                return;
            }
        }
        snprintf(out, n, "WAITING FOR EVERYONE TO BE READY");
    } else {
        out[0] = '\0';
    }
}

static void draw_lobby_cb(void* p) {
    const OnlineView* v = (const OnlineView*)p;
    const NgLobby* L = &v->ng->lobby;
    int w = portrait_view_w(), h = portrait_view_h();
    int m = portrait_margin(), tb = portrait_top_bar_h();
    int rw = w - 2 * m;
    gfx_clear(TABLE_BG);
    portrait_title_bar();
    hits_reset();

    int small_fs = clampi(h / 48, 9, 40);
    int head_fs = clampi(h / 26, 12, 80);
    int y = tb + m;
    char head[32];
    if (L->games > 0) snprintf(head, sizeof head, "ROOM - GAME %d", L->games + 1);
    else snprintf(head, sizeof head, "ROOM");
    text_centered(head, w / 2, y, head_fs, SLOT_LABEL);
    y += head_fs + m / 4;
    char rl[96];
    rules_line(rl, sizeof rl, L->target, L->bonus, L->partners);
    text_centered(rl, w / 2, y, fit(rl, small_fs, rw), SLOT_LABEL);
    y += small_fs + m / 3;

    // The last match's result, until the next deal.
    char res[96];
    online_result_line(v->ng, res, sizeof res);
    if (res[0]) {
        int rfs = clampi(h / 36, 10, 60);
        text_centered(res, w / 2, y, fit(res, rfs, rw), ACCENT);
        y += rfs + m / 3;
    }
    y += m * 2 / 3;

    // Members.
    int row_h = clampi(h / 13, 30, 170), gap = m / 3;
    if (!L->partners) {
        // One row per seat: the members, then the seats still open. (Member
        // slots are not seats — with two seats the pair may sit in any two of
        // the four slots — so the open rows are drawn from a free slot.)
        int seats = seats_of(L->seats), shown = 0, free_i = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!L->used[i]) {
                if (free_i < 0) free_i = i;
                continue;
            }
            draw_member(L, i, m, y, rw, row_h);
            y += row_h + gap;
            shown++;
        }
        for (; shown < seats && free_i >= 0; shown++) {
            draw_member(L, free_i, m, y, rw, row_h);
            y += row_h + gap;
        }
    } else {
        // Two teams side by side, two slots each.
        int cw = (rw - m / 2) / 2;
        for (int t = 0; t < 2; t++) {
            int cx = m + t * (cw + m / 2);
            text_centered(t ? "TEAM B" : "TEAM A", cx + cw / 2, y, small_fs, SLOT_LABEL);
            int ry = y + small_fs + m / 3;
            int shown = 0;
            for (int i = 0; i < MAX_PLAYERS && shown < 2; i++) {
                if (!L->used[i] || L->team[i] != t) continue;
                draw_member(L, i, cx, ry, cw, row_h);
                ry += row_h + gap;
                shown++;
            }
            for (; shown < 2; shown++) {
                // An open slot on this team: draw it from an unused member index.
                int free_i = -1;
                for (int i = 0; i < MAX_PLAYERS; i++) if (!L->used[i]) { free_i = i; break; }
                if (free_i >= 0) draw_member(L, free_i, cx, ry, cw, row_h);
                ry += row_h + gap;
            }
        }
        y += small_fs + m / 3 + 2 * (row_h + gap);
    }

    // Status: why it can't start, or the countdown.
    char st[64];
    status_text(L, st, sizeof st);
    bool counting = lobby_counting(L);
    int st_fs = counting ? clampi(h / 18, 14, 110) : clampi(h / 34, 10, 64);
    y += m / 2;
    text_centered(st, w / 2, y, fit(st, st_fs, rw), counting ? ACCENT : WHITE);

    // Buttons from the bottom up: Leave (and Switch Team) in a row, the
    // primary (Ready / Start without them) full width above them.
    int ids[4];
    int nb = online_lobby_buttons(v, ids);
    int focus = (v->sel >= 0 && v->sel < nb) ? ids[v->sel] : -1;
    int btn_h = clampi(h / 15, 26, 150);
    int big_h = btn_h + btn_h / 4;
    int bottom = h - m;   // the bottom edge of the next row up
    int small[4], ns = 0, big[2], nbig = 0;
    for (int i = 0; i < nb; i++) {
        if (ids[i] == OLB_READY || ids[i] == OLB_FORCE) big[nbig++] = ids[i];
        else small[ns++] = ids[i];
    }
    int half = (rw - m / 2) / 2;
    int y0 = bottom - btn_h;
    if (ns == 2) {
        draw_button(OL_HIT_BUTTON + small[0], m, y0, half, btn_h,
                    button_label(small[0], L), false, focus == small[0]);
        draw_button(OL_HIT_BUTTON + small[1], m + half + m / 2, y0, half, btn_h,
                    button_label(small[1], L), false, focus == small[1]);
    } else if (ns == 1) {
        draw_button(OL_HIT_BUTTON + small[0], m, y0, rw, btn_h,
                    button_label(small[0], L), false, focus == small[0]);
    }
    bottom = y0 - m / 2;
    for (int i = nbig - 1; i >= 0; i--) {
        y0 = bottom - big_h;
        bool primary = big[i] == OLB_FORCE || !L->ready[L->me];
        draw_button(OL_HIT_BUTTON + big[i], m, y0, rw, big_h,
                    button_label(big[i], L), primary, focus == big[i]);
        bottom = y0 - m / 2;
    }

    if (v->notice && v->notice[0]) {
        text_centered(v->notice, w / 2, bottom - small_fs - m / 2,
                      fit(v->notice, small_fs, rw), ACCENT);
    }
}

void online_render_lobby(const OnlineView* v) {
    portrait_present(draw_lobby_cb, (void*)v);
}
