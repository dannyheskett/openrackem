#ifndef OPENRACKEM_RENDER_H
#define OPENRACKEM_RENDER_H

#include "game.h"
#include "platform.h"
#include "ob_types.h"
#include <stdbool.h>

// The landscape renderer draws to a fixed 640x480 off-screen canvas that
// present() integer-scales and letterboxes into the window. These are the
// landscape canvas dimensions only — the portrait renderer sizes itself from the
// live screen (GetScreenWidth/Height) and does not use BASE_WIDTH/BASE_HEIGHT.
#define BASE_WIDTH  640
#define BASE_HEIGHT 480

// Presentation state owned by the frame loop, read by the renderers: the
// keyboard rack-slot cursor and which round-over page is showing.
typedef struct {
    int  cursor;      // rack-slot cursor 0..9 (keyboard placement), -1 = hidden
    bool standings;   // in PHASE_ROUND_OVER: false = round scoring, true = standings
} TableUi;

void render_init(void);
void render_cleanup(void);

// Gameplay scene: the table, or the scoring/standings/match-over screens,
// selected by the game's phase.
void render_frame(const Game* game, const TableUi* ui);
// Gameplay scene with a "paused" overlay on top.
void render_pause(const Game* game, const TableUi* ui);
// Floating menu: title plus a list of items, one highlighted. gap_before, if
// >= 0, inserts a blank line before that item index.
void render_menu(const char* title, const char* const* items, int count,
                 int selected, int gap_before);

bool render_window_should_close(void);
void render_toggle_fullscreen(void);
// True while the app window holds input focus. Used to auto-pause when the app
// is sent to the background (Android suspend/resume).
bool render_window_focused(void);

// Return the menu item index at screen point `p`, or -1 if none. Uses the item
// rectangles captured by the last render_menu() call (touch menus).
int render_menu_hit_test(Vector2 p);

// Table zones for touch hit-testing, captured by the last render_frame():
// 0..9 are rack slots; HIT_STOCK / HIT_DISCARD are the piles.
enum { HIT_NONE = -1, HIT_STOCK = 10, HIT_DISCARD = 11 };
int render_table_hit_test(Vector2 p);

// Active renderer selection. Native builds have exactly one renderer, so
// render_use_portrait() is a compile-time constant there (true on Android/iOS,
// false on desktop). The web build compiles both and picks at runtime:
// render_set_portrait(true) = portrait touch layout, false = desktop landscape.
void render_set_portrait(bool portrait);
bool render_use_portrait(void);

#endif // OPENRACKEM_RENDER_H
