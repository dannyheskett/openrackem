// Common renderer TU: shared state and draw helpers, window/loop lifecycle, and
// the public entry points that dispatch to the active renderer. The two
// renderers live in render_portrait.c and render_landscape.c.
#include "render_internal.h"
#include <stdio.h>
#if !defined(PLATFORM_IOS)
#include <raylib.h>  // window/timing (InitWindow, …); absent on iOS
#endif

// render_use_portrait() reports the active renderer. Native builds have exactly
// one (compile-time constant); the web build has both and picks at runtime.
#if defined(OR_RUNTIME_RENDERER)
static bool s_portrait_mode = false;
void render_set_portrait(bool portrait) { s_portrait_mode = portrait; }
bool render_use_portrait(void) { return s_portrait_mode; }
#elif defined(OR_PORTRAIT)
bool render_use_portrait(void) { return true; }   // Android / iOS: portrait only
#else
bool render_use_portrait(void) { return false; }  // desktop native: landscape only
#endif

// Table palette, named for clarity (shared with both renderers). A dark felt
// green table with pale card faces and a deep red card back.
const Color TABLE_BG   = { 16,  48,  32, 255};
const Color CARD_FACE  = {236, 232, 220, 255};
const Color CARD_BACK  = {140,  36,  40, 255};
const Color CARD_TEXT  = { 30,  30,  40, 255};
const Color CARD_EDGE  = { 90,  90,  90, 255};
const Color SLOT_LABEL = {150, 156, 170, 255};
const Color ACCENT     = {253, 249,   0, 255};   // raylib YELLOW

static Color lighten(Color c, int d) {
    int r = c.r + d, g = c.g + d, b = c.b + d;
    return (Color){ r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, c.a };
}
static Color with_alpha(Color c, unsigned char a) { c.a = a; return c; }

// Corner radius scaled to the card height, clamped so tiny cards (opponent
// backs, mini-racks) still round cleanly.
static int card_radius(int h) {
    int r = h / 6;
    if (r < 3) r = 3;
    if (r > 8) r = 8;
    return r;
}

// A card at (x, y, w, h): rounded body with a soft drop shadow and, face up, a
// subtle top sheen and the value centered at a size that fits two digits. Face
// down shows the printed-back frame. `highlight` adds an accent glow + border
// (the selection / current-turn affordance). Palette is unchanged.
void draw_card(int x, int y, int w, int h, int value, bool face_up, bool highlight) {
    int r = card_radius(h);

    // Soft drop shadow (two stacked translucent rounded rects, offset down-right).
    gfx_rect_rounded(x + 2, y + 3, w, h, r, with_alpha((Color){0, 0, 0, 255}, 45));
    gfx_rect_rounded(x + 1, y + 2, w, h, r, with_alpha((Color){0, 0, 0, 255}, 30));

    // Selection glow, spreading beyond the card edges.
    if (highlight) {
        gfx_rect_rounded(x - 3, y - 3, w + 6, h + 6, r + 3, with_alpha(ACCENT, 55));
        gfx_rect_rounded(x - 2, y - 2, w + 4, h + 4, r + 2, with_alpha(ACCENT, 95));
    }

    // Border ring, then the inset body (thicker accent border when highlighted).
    int bt = highlight ? 2 : 1;
    int ir = r - bt < 1 ? 1 : r - bt;
    gfx_rect_rounded(x, y, w, h, r, highlight ? ACCENT : CARD_EDGE);

    if (face_up) {
        gfx_rect_rounded(x + bt, y + bt, w - 2 * bt, h - 2 * bt, ir, CARD_FACE);
        // Top sheen: a gentle vertical gradient over the interior, inset by the
        // radius so its square corners stay within the rounded face.
        if (w > 2 * r + 2 && h > 8) {
            gfx_rect_gradient_v(x + r, y + bt + 1, w - 2 * r, (h - 2 * bt) * 3 / 5,
                                lighten(CARD_FACE, 16), CARD_FACE);
        }
        char txt[4];
        snprintf(txt, sizeof txt, "%d", value);
        int fs = h * 7 / 10;
        if (fs < 8) fs = 8;
        while (fs > 8 && gfx_measure_text(txt, fs) > w - 6) fs -= 2;
        gfx_text(txt, x + (w - gfx_measure_text(txt, fs)) / 2, y + (h - fs) / 2,
                 fs, CARD_TEXT);
    } else {
        gfx_rect_rounded(x + bt, y + bt, w - 2 * bt, h - 2 * bt, ir, CARD_BACK);
        // Printed-back frame: a tan ring inset from the edge.
        int inset = (w / 7 < 3) ? 3 : w / 7;
        int fr = ir - 1 < 1 ? 1 : ir - 1;
        if (w > 2 * inset + 4 && h > 2 * inset + 4) {
            gfx_rect_rounded(x + inset, y + inset, w - 2 * inset, h - 2 * inset, fr,
                             (Color){200, 160, 120, 255});
            gfx_rect_rounded(x + inset + 2, y + inset + 2, w - 2 * inset - 4,
                             h - 2 * inset - 4, fr > 2 ? fr - 2 : 1, CARD_BACK);
        }
    }
}

// An empty pile / vacated slot: just a dim outline.
void draw_card_outline(int x, int y, int w, int h) {
    gfx_rect_lines(x, y, w, h, (Color){70, 90, 80, 255});
}

void draw_mini_rack(int x, int y, int cw, int ch, const Rack* rack, bool face_up) {
    for (int i = 0; i < RACK_SLOTS; i++) {
        draw_card(x + i * (cw + 1), y, cw, ch, rack->slots[i], face_up, false);
    }
}

// Optional per-seat display labels (online handles). Set while an online game is
// on screen so opponents show their chosen names (or the server's directional
// default) instead of "CPU N"; the local seat still shows "YOU". Cleared for
// offline play.
static char s_seat_labels[MAX_PLAYERS][16];
static bool s_seat_labels_on = false;

void render_set_seat_labels(const char labels[][16], int count) {
    if (count > MAX_PLAYERS) count = MAX_PLAYERS;
    for (int i = 0; i < count; i++)
        snprintf(s_seat_labels[i], sizeof s_seat_labels[i], "%s", labels[i]);
    for (int i = count < 0 ? 0 : count; i < MAX_PLAYERS; i++) s_seat_labels[i][0] = '\0';
    s_seat_labels_on = true;
}
void render_clear_seat_labels(void) { s_seat_labels_on = false; }

const char* seat_name(const Game* g, int seat) {
    static char buf[MAX_PLAYERS][8];
    // Defensive clamp: buf is indexed by seat, so an out-of-range value (from
    // a corrupt state) must never write past it. The online client already
    // rejects such states, but this is the last line before the memory write.
    if (seat < 0 || seat >= MAX_PLAYERS) return "?";
    if (seat == g->rules.human_seat) return "YOU";
    if (s_seat_labels_on && s_seat_labels[seat][0]) return s_seat_labels[seat];
    // Number the CPUs 1..k in seat order, skipping the human seat.
    int n = 0;
    for (int s = 0; s <= seat; s++) {
        if (s != g->rules.human_seat) n++;
    }
    snprintf(buf[seat], sizeof buf[seat], "CPU %d", n);
    return buf[seat];
}

int deal_cards_for_seat(const Game* g, int seat) {
    int n = g->rules.player_count;
    int step = g->anim.deal_step;
    if (g->phase != PHASE_DEAL) return RACK_SLOTS;
    if (step > n * RACK_SLOTS) step = n * RACK_SLOTS;
    // Cards go out one at a time starting left of the dealer; this seat has
    // received one from each completed round of the table, plus one more if
    // the partial round has already passed it.
    int pos = (seat - g->dealer - 1 + n) % n;
    int count = step / n + ((step % n) > pos ? 1 : 0);
    return (count > RACK_SLOTS) ? RACK_SLOTS : count;
}

bool deal_discard_flipped(const Game* g) {
    if (g->phase != PHASE_DEAL) return true;
    return g->anim.deal_step >= g->rules.player_count * RACK_SLOTS + 1;
}

bool anim_in_flight(const Game* g) {
    return g->anim.kind != ANIM_NONE && g->anim.frames > 0 && g->anim.total > 0;
}

float anim_progress(const Game* g) {
    float t = 1.0f - (float)g->anim.frames / (float)g->anim.total;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);   // smoothstep: ease out of and into rest
}

void draw_flying_card(Rectangle from, Rectangle to, float t, int value, bool face_up) {
    int x = (int)(from.x + (to.x - from.x) * t);
    int y = (int)(from.y + (to.y - from.y) * t);
    int w = (int)(from.width  + (to.width  - from.width)  * t);
    int h = (int)(from.height + (to.height - from.height) * t);
    draw_card(x, y, w, h, value, face_up, false);
}

// A floating, centered panel with a title and an optional subtitle, drawn over a
// dimmed background. Shared by the pause overlay and end-of-round banners.
void draw_center_panel_at(int w, int h, int panel_w, int panel_h, int ts,
                          int ss, int title_dy, int sub_dy, const char* title,
                          const char* subtitle, Color title_color) {
    int cx = w / 2;
    int px = cx - panel_w / 2;
    int py = (h - panel_h) / 2;

    gfx_rect(0, 0, w, h, (Color){0, 0, 0, 170}); // dim
    gfx_rect(px, py, panel_w, panel_h, (Color){15, 15, 25, 255});
    gfx_rect_lines(px, py, panel_w, panel_h, LIGHTGRAY);

    gfx_text(title, cx - gfx_measure_text(title, ts) / 2, py + title_dy, ts, title_color);
    if (subtitle) {
        gfx_text(subtitle, cx - gfx_measure_text(subtitle, ss) / 2, py + sub_dy, ss, GRAY);
    }
}

// --- Touch hit zones -------------------------------------------------------
// Table rectangles captured by the last render_frame (rack slots + piles) and
// menu item rectangles captured by the last render_menu. Written by the active
// renderer, read by the hit-test entry points below.
#define TABLE_ZONES 12   // 0..9 slots, HIT_STOCK, HIT_DISCARD
static Rectangle s_table_rects[TABLE_ZONES];
static bool      s_table_valid[TABLE_ZONES];

void table_hits_reset(void) {
    for (int i = 0; i < TABLE_ZONES; i++) s_table_valid[i] = false;
}

void table_hit_set(int id, Rectangle r) {
    if (id < 0 || id >= TABLE_ZONES) return;
    s_table_rects[id] = r;
    s_table_valid[id] = true;
}

int render_table_hit_test(Vector2 p) {
    for (int i = 0; i < TABLE_ZONES; i++) {
        if (s_table_valid[i] && CheckCollisionPointRec(p, s_table_rects[i])) return i;
    }
    return HIT_NONE;
}

static Rectangle s_menu_item_rects[8];
static int s_menu_item_count = 0;

// Draw the menu panel, centred title, and item list with selection markers.
// `capture` records each row's rectangle for touch hit-testing (portrait);
// landscape passes false (keyboard-only).
void draw_menu_panel(MenuLayout m, const char* title, const char* const* items,
                     int count, int selected, int gap_before, bool capture) {
    gfx_rect(m.px, m.py, m.panel_w, m.panel_h, (Color){15, 15, 25, 255});
    gfx_rect_lines(m.px, m.py, m.panel_w, m.panel_h, LIGHTGRAY);
    // Shrink a long title to fit inside the panel (e.g. "WAITING FOR PLAYERS"),
    // then re-center it in the original title band.
    int ts = m.title_size;
    int maxw = m.panel_w - 24;
    while (ts > 10 && gfx_measure_text(title, ts) > maxw) ts -= 2;
    gfx_text(title, m.cx - gfx_measure_text(title, ts) / 2,
             m.title_y + (m.title_size - ts) / 2, ts, WHITE);

    s_menu_item_count = capture ? ((count < 8) ? count : 8) : 0;
    int y = m.items_y;
    for (int i = 0; i < count; i++) {
        if (gap_before == i) y += m.line_h;
        const char* label = items[i];
        int lw = gfx_measure_text(label, m.item_fs);
        Color col = (i == selected) ? YELLOW : GRAY;
        if (i == selected) {
            gfx_text(">", m.cx - lw / 2 - m.item_fs * 3 / 2, y, m.item_fs, YELLOW);
            gfx_text("<", m.cx + lw / 2 + m.item_fs / 2, y, m.item_fs, YELLOW);
        }
        gfx_text(label, m.cx - lw / 2, y, m.item_fs, col);
        if (capture && i < 8) {
            s_menu_item_rects[i] = (Rectangle){ (float)m.px, (float)(y - (m.line_h - m.item_fs) / 2),
                                                (float)m.panel_w, (float)m.line_h };
        }
        y += m.line_h;
    }
}

// --- Lifecycle -------------------------------------------------------------
void render_init(void) {
#if defined(PLATFORM_IOS)
    // iOS: UIKit owns the window/surface and drives the loop (CADisplayLink); the
    // Metal layer is attached separately by the app shell. Nothing to do here.
#else
#if defined(PLATFORM_ANDROID)
    // Request immersive fullscreen so the app draws under the status bar / camera
    // cutout (paired with windowLayoutInDisplayCutoutMode=shortEdges in the theme)
    // — otherwise the surface is letterboxed below the status bar.
    SetConfigFlags(FLAG_FULLSCREEN_MODE);
#elif defined(PLATFORM_WEB)
    // Let the GL canvas follow the browser viewport (the HTML shell sizes it);
    // GetScreenWidth/Height then track it so the layout re-fits on resize/rotate.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
#else
    // Desktop native: a freely resizable window (minimum 640x480, enforced
    // below) whose 640x480 layout scales up to fill it at native resolution;
    // MSAA keeps the scaled vector edges clean.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
#endif
#if defined(PLATFORM_ANDROID)
    // Request 0x0: raylib's Android backend then renders at the device's native
    // resolution. Any fixed size here gets aspect-letterboxed into the display
    // (with GetScreenWidth/Height reporting the request, not the device), which
    // would shrink the whole game into a 640x480 box in the middle of the screen.
    InitWindow(0, 0, "openrackem");
#else
    // Fixed 640x480 window (not resizable); Alt+Enter toggles fullscreen.
    InitWindow(BASE_WIDTH, BASE_HEIGHT, "openrackem");
#endif
    SetExitKey(KEY_NULL); // Escape is handled by the game, not the window
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_WEB)
    SetWindowMinSize(BASE_WIDTH, BASE_HEIGHT); // never render below 640x480
#endif
    SetTargetFPS(60);
    gfx_font_init();      // load the bundled UI font now that the GL context exists

#ifdef OR_LANDSCAPE
    canvas = LoadRenderTexture(BASE_WIDTH, BASE_HEIGHT);
#endif
#endif // PLATFORM_IOS
}

void render_toggle_fullscreen(void) {
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_IOS)
    // Android / iOS apps are always fullscreen; nothing to toggle.
    (void)0;
}
#else
    // Borderless fullscreen at the monitor's resolution; present() integer-
    // scales and centers the fixed canvas inside it.
    if (!IsWindowFullscreen()) {
        int mon = GetCurrentMonitor();
        SetWindowSize(GetMonitorWidth(mon), GetMonitorHeight(mon));
        ToggleFullscreen();
    } else {
        ToggleFullscreen();
        SetWindowSize(BASE_WIDTH, BASE_HEIGHT);
    }
}
#endif // PLATFORM_ANDROID / PLATFORM_IOS

void render_cleanup(void) {
#ifdef OR_LANDSCAPE
    UnloadRenderTexture(canvas);
#endif
#if !defined(PLATFORM_IOS)
    CloseWindow();
#endif
}

// --- Public entry points ---------------------------------------------------
// Dispatch to the active renderer: compile-time on native builds that have only
// one, runtime on web, which has both.
#if defined(OR_RUNTIME_RENDERER)
  #define OR_DISPATCH(fn, ...) do { if (s_portrait_mode) fn##_portrait(__VA_ARGS__); else fn##_landscape(__VA_ARGS__); } while (0)
#elif defined(OR_PORTRAIT)
  #define OR_DISPATCH(fn, ...) fn##_portrait(__VA_ARGS__)
#else
  #define OR_DISPATCH(fn, ...) fn##_landscape(__VA_ARGS__)
#endif

void render_frame(const Game* game, const TableUi* ui) { OR_DISPATCH(render_frame, game, ui); }
void render_pause(const Game* game, const TableUi* ui) { OR_DISPATCH(render_pause, game, ui); }
void render_menu(const char* title, const char* const* items, int count,
                 int selected, int gap_before) {
    OR_DISPATCH(render_menu, title, items, count, selected, gap_before);
}

int render_menu_hit_test(Vector2 p) {
    for (int i = 0; i < s_menu_item_count; i++) {
        if (CheckCollisionPointRec(p, s_menu_item_rects[i])) return i;
    }
    return -1;
}

bool render_window_should_close(void) {
    return WindowShouldClose();
}

bool render_window_focused(void) {
    return IsWindowFocused();
}
