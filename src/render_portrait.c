// Portrait (touch) renderer: adaptive layout drawn straight to the screen at the
// device's real resolution — a thin OPENRACKEM title bar, opponents band, the
// ten rack slots, and the stock/discard band at the bottom where the thumb is.
// Compiles to an empty object off OR_PORTRAIT.
#include "render_internal.h"
#include "safe_area.h"
#include <stddef.h> // NULL

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

// M0 scaffold: the adaptive table layout (opponents band, rack, piles) lands
// in M4.
static void draw_game_portrait(const Game* game, const TableUi* ui) {
    (void)game; (void)ui;
    gfx_clear(TABLE_BG);
    draw_title_bar();

    int w = GetScreenWidth(), h = GetScreenHeight();
    int fs = h / 38;
    const char* msg = "Table renderer lands in M4";
    gfx_text(msg, (w - gfx_measure_text(msg, fs)) / 2, h / 2 - fs / 2, fs, SLOT_LABEL);
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

#endif // OR_PORTRAIT
