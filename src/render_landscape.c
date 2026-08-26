// Landscape (desktop) renderer: draws the 3-column table layout into a fixed
// 640x480 offscreen canvas that present() letterboxes into the window. Compiles
// to an empty object off OR_LANDSCAPE (i.e. on Android / iOS).
#include "render_internal.h"

#ifdef OR_LANDSCAPE

#include "recorder.h"
#include <raylib.h>
#include <stddef.h> // NULL

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

// Draw the table scene into the currently-active render target (canvas).
// M0 scaffold: the full 3-column table (opponents / rack / piles) lands in M3.
static void draw_game_landscape(const Game* game, const TableUi* ui) {
    (void)game; (void)ui;
    gfx_clear(TABLE_BG);

    // Top border/title
    gfx_rect(0, 0, BASE_WIDTH, 24, DARKGRAY);
    gfx_text("OPENRACKEM", (BASE_WIDTH - gfx_measure_text("OPENRACKEM", 16)) / 2, 4, 16, WHITE);

    const char* msg = "Table renderer lands in M3";
    gfx_text(msg, (BASE_WIDTH - gfx_measure_text(msg, 16)) / 2, BASE_HEIGHT / 2 - 8, 16, SLOT_LABEL);
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
