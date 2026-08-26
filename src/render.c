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

// A card at (x, y, w, h). Face up: pale face with the value centered at a size
// that fits two digits. Face down: the back colour with an inset border, the
// classic printed-back look. `highlight` outlines in the accent colour.
void draw_card(int x, int y, int w, int h, int value, bool face_up, bool highlight) {
    gfx_rect(x, y, w, h, face_up ? CARD_FACE : CARD_BACK);
    gfx_rect_lines(x, y, w, h, highlight ? ACCENT : CARD_EDGE);
    if (highlight) gfx_rect_lines(x - 1, y - 1, w + 2, h + 2, ACCENT);
    if (face_up) {
        char txt[4];
        snprintf(txt, sizeof txt, "%d", value);
        int fs = h * 7 / 10;
        if (fs < 8) fs = 8;
        while (fs > 8 && gfx_measure_text(txt, fs) > w - 6) fs -= 2;
        gfx_text(txt, x + (w - gfx_measure_text(txt, fs)) / 2, y + (h - fs) / 2,
                 fs, CARD_TEXT);
    } else {
        // Inset border pattern on the back.
        int inset = (w / 8 < 2) ? 2 : w / 8;
        if (w > 2 * inset + 2 && h > 2 * inset + 2) {
            gfx_rect_lines(x + inset, y + inset, w - 2 * inset, h - 2 * inset,
                           (Color){200, 160, 120, 255});
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

const char* seat_name(const Game* g, int seat) {
    static char buf[MAX_PLAYERS][8];
    if (seat == g->rules.human_seat) return "YOU";
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
    gfx_text(title, m.cx - gfx_measure_text(title, m.title_size) / 2, m.title_y, m.title_size, WHITE);

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
    SetTargetFPS(60);

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
