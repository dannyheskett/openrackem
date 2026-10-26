#ifndef OPENRACKEM_RENDER_H
#define OPENRACKEM_RENDER_H

#include "game.h"
#include "platform.h"
#include "or_types.h"
#include <stdbool.h>

// Presentation state owned by the frame loop, read by the renderers: the
// keyboard rack-slot cursor and which round-over page is showing.
typedef struct {
    int  cursor;      // rack-slot cursor 0..9 (keyboard placement), -1 = hidden
    bool standings;   // in PHASE_ROUND_OVER: false = round scoring, true = standings
} TableUi;

// Window setup and teardown (window.c) plus the recorder's capture canvas.
void render_init(void);
void render_cleanup(void);

// Gameplay scene: the table, or the scoring/standings/match-over screens,
// selected by the game's phase.
void render_frame(const Game* game, const TableUi* ui);
// Gameplay scene with a "paused" overlay on top.
void render_pause(const Game* game, const TableUi* ui);

// Per-seat display names (online handles). Set before rendering an online game
// so opponents show their chosen names; clear for offline play (seat_name then
// falls back to "YOU" / "CPU N").
void render_set_seat_labels(const char labels[][16], int count);
void render_clear_seat_labels(void);

// The online overlay on a live table (NULL / on = false offline): which seats
// are away (an AI standing in, or the table waiting for them), the running
// turn or round-screen clock, the absent-player wait with its Let AI play
// button (HIT_LETAI), and a brief "X is back".
typedef struct {
    bool    on;
    uint8_t away[MAX_PLAYERS];
    int     deadline_ms;   // current turn / round-screen countdown (0 = none)
    int     wait_seat;     // seat the table is waiting for (-1 = none)
    int     wait_ms;       // ...remaining, when wait_timer
    bool    wait_timer;    // false = waiting until they return
    char    back[16];      // name of a player who just came back ("" = none)
} OnlineHud;
void render_set_online_hud(const OnlineHud* hud);
// The family menu (menu.c). gap_before, if >= 0, inserts a blank line before
// that item index. Hit-test its rows with menu_hit_test().
void render_menu(const char* title, const char* const* items, int count,
                 int selected, int gap_before);

// Slot picker: the player-name and room-code entry screens. `slots` is one
// character per slot (NUL-terminated), `cursor` the slot being edited, and
// `alphabet` the ring those slots cycle through — the picker draws the
// neighbouring letters above and below the active slot as a spinner, so the
// two bands are self-explanatory targets rather than an undiscoverable swipe.
// `ok_label` names the confirm button ("SAVE", "JOIN").
#define PICKER_MAX_SLOTS 12
void render_picker(const char* title, const char* slots, int cursor,
                   const char* alphabet, const char* hint, const char* ok_label);

// What a tap landed on: a slot index (0..slots-1), one of the PICK_* controls,
// or PICK_NONE. Uses the rectangles captured by the last render_picker().
enum { PICK_NONE = -1, PICK_NEXT = 100, PICK_PREV = 101, PICK_OK = 102, PICK_CANCEL = 103 };
int render_picker_hit_test(Vector2 p);

// Table zones for touch hit-testing, captured by the last render_frame():
// 0..9 are rack slots; HIT_STOCK / HIT_DISCARD are the piles; HIT_LETAI is the
// online "Let AI play" button on the absent-player banner.
enum { HIT_NONE = -1, HIT_STOCK = 10, HIT_DISCARD = 11, HIT_LETAI = 12 };
int render_table_hit_test(Vector2 p);

// Touch device or desktop. Both draw the same layout; a touch device fills the
// screen with it and takes taps, a desktop centres it in a phone-shaped column
// and takes the mouse and keyboard. A compile-time constant on native builds
// (true on Android/iOS, false on desktop); the web build sets it at startup:
// render_set_portrait(true) = touch, false = desktop browser.
void render_set_portrait(bool portrait);
bool render_use_portrait(void);

#endif // OPENRACKEM_RENDER_H
