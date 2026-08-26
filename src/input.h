#ifndef OPENRACKEM_INPUT_H
#define OPENRACKEM_INPUT_H

#include <stdbool.h>

// One normalized input snapshot per frame. Keyboard and touch both fill this
// struct, and both ultimately produce the same Action values, so the engine
// cannot tell a key from a thumb.
//
// Overlapping keys are deliberate — Enter sets both confirm_pressed and
// pause_pressed — and the frame loop picks by context (holding a card, a menu,
// a scoring screen). Every field is edge-triggered; there is no held state in
// a turn-based game.
typedef struct {
    // Table actions
    bool draw_stock_pressed;    // S or Left: draw from the stockpile
    bool draw_discard_pressed;  // D or Right: draw the face-up discard
    bool cursor_up;             // move the rack-slot cursor toward slot #50
    bool cursor_down;           // move the rack-slot cursor toward slot #5
    bool confirm_pressed;       // Enter or Space: place at the cursor / advance
    bool throw_pressed;         // X: discard a stock-drawn held card
    bool pause_pressed;         // Enter (the loop uses it when nothing confirms)

    // Menu / overlays
    bool menu_up, menu_down;    // move the menu cursor
    bool menu_left, menu_right; // cycle an Options value
    bool select_pressed;        // Enter or Space (keyboard)
    bool escape_pressed;        // Escape (or two-finger tap on touch)
    bool any_pressed;           // anything at all (used to dismiss overlays)

    // Touch: a tap's coordinates, hit-tested by the renderer against the menu
    // rows and the table zones (stock, discard, rack slots). Valid only when
    // touch_tap is set.
    bool touch_tap;
    float tap_x, tap_y;

    // Window
    bool fullscreen_toggle;     // Alt+Enter
} Input;

Input input_poll(void);

#endif // OPENRACKEM_INPUT_H
