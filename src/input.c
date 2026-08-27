#include "input.h"
#include "render.h"
#include "platform.h"
#if !defined(PLATFORM_IOS)
#include <raylib.h>  // keyboard/mouse; iOS is touch-only (queries come from plat_ios)
#endif

// input_poll() composes up to two sources into one Input:
//   - keyboard: desktop native builds and the web build (PC browsers)
//   - touch:    Android, iOS, and the web build (mobile browsers)
// The web build runs both, so a phone uses taps while a desktop browser uses
// the keyboard — same binary. Android/iOS run only touch; desktop native runs
// only keyboard.

#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_IOS)
// Keyboard source: sets the base field values.
static void poll_keyboard(Input* in) {
    bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);

    // Draws. S/Left = stock, D/Right = discard.
    in->draw_stock_pressed   = IsKeyPressed(KEY_S) || IsKeyPressed(KEY_LEFT);
    in->draw_discard_pressed = IsKeyPressed(KEY_D) || IsKeyPressed(KEY_RIGHT);

    // Rack-slot cursor. Only the arrows: W/S would collide with the draw keys.
    in->cursor_up   = IsKeyPressed(KEY_UP);
    in->cursor_down = IsKeyPressed(KEY_DOWN);

    // Alt+Enter toggles fullscreen; a plain Enter (without Alt) confirms — or
    // pauses, when the current context has nothing to confirm (the frame loop
    // decides which).
    in->fullscreen_toggle = alt && IsKeyPressed(KEY_ENTER);
    bool enter = IsKeyPressed(KEY_ENTER) && !alt;
    in->confirm_pressed = enter || IsKeyPressed(KEY_SPACE);
    in->pause_pressed   = enter;

    in->throw_pressed = IsKeyPressed(KEY_X);

    // Menu navigation. W/S are safe here: the draw keys are table-only.
    in->menu_up    = IsKeyPressed(KEY_UP)    || IsKeyPressed(KEY_W);
    in->menu_down  = IsKeyPressed(KEY_DOWN)  || IsKeyPressed(KEY_S);
    in->menu_left  = IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A);
    in->menu_right = IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D);
    in->select_pressed = in->confirm_pressed;
    in->escape_pressed = IsKeyPressed(KEY_ESCAPE);

    // Text entry: drain this frame's printable characters (name / room-code
    // fields) and note a backspace edge (with key-repeat for held delete). The
    // char queue is separate from the key queue drained below.
    int ci = 0, cp;
    while (ci < (int)sizeof in->text_input - 1 && (cp = GetCharPressed()) != 0) {
        if (cp >= 32 && cp < 127) in->text_input[ci++] = (char)cp;
    }
    in->text_input[ci] = '\0';
    in->text_len = (uint8_t)ci;
    in->backspace_pressed = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);

    // Any key (drains one entry from the per-frame key-press queue).
    in->any_pressed = GetKeyPressed() != 0;
}
#endif // !PLATFORM_ANDROID && !PLATFORM_IOS

#ifdef OR_TOUCH
// Gesture-recognizer state that persists across frames for the current touch
// sequence (first finger down to last finger up), kept in one module-owned
// value so the hidden state is explicit and resettable.
typedef struct {
    Vector2 last_pos;  // last active pointer position (source of tap coords)
    bool    active;    // a touch sequence is in progress
    Vector2 origin;    // where the sequence started (px)
    double  t0;        // sequence start time (s)
    bool    moved;     // travelled beyond the tap tolerance: not a tap
    int     max_np;    // most simultaneous fingers seen during the sequence
} TouchState;

static TouchState s_touch;

// Touch source: taps drive everything in a turn-based card game — tap a pile
// to draw, tap a slot to place, tap a menu row to select. A two-finger tap is
// the pause/back gesture; swipes move the menu cursor. Only ever sets fields
// true, so it composes over the keyboard source on web without clobbering it.
static void poll_touch(Input* in) {
    if (!render_use_portrait()) return; // landscape (desktop-browser) mode: keyboard only

    // Active pointers: touch points, or the mouse while its button is held.
    int n = GetTouchPointCount();
    Vector2 pts[8];
    int np = 0;
    for (int i = 0; i < n && np < 8; i++) pts[np++] = GetTouchPosition(i);
#if !defined(PLATFORM_IOS)
    // Desktop browsers report a mouse, not a touch point; fold it in so the
    // touch controls work with a click. iOS has no mouse.
    if (n == 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) pts[np++] = GetMousePosition();
#endif

    if (np > 0) {
        s_touch.last_pos = pts[0];
    }
#if !defined(PLATFORM_IOS)
    else if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        s_touch.last_pos = GetMousePosition(); // remember where a click ended, for the tap below
    }
#endif

    double now = GetTime();

    // Tap tolerance: movement beyond this is a drag (a swipe or a scroll
    // attempt), not a tap. Scaled to the short screen dimension so it stays
    // proportional across phones and tablets.
    int w = GetScreenWidth(), h = GetScreenHeight();
    float tol = (float)((w < h ? w : h) / 25);
    if (tol < 12.0f) tol = 12.0f;

    if (np > 0) {
        Vector2 p = pts[0];
        if (!s_touch.active) {
            s_touch.active = true;
            s_touch.origin = p;
            s_touch.t0 = now;
            s_touch.moved = false;
            s_touch.max_np = 0;
        }
        if (np > s_touch.max_np) s_touch.max_np = np;
        // pts[0] can jump when a second finger lands or lifts, so only track
        // movement while the sequence is single-finger.
        if (s_touch.max_np < 2) {
            float dx = p.x - s_touch.origin.x, dy = p.y - s_touch.origin.y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > tol || dy > tol) s_touch.moved = true;
        }
    } else if (s_touch.active) {
        // Touch ended: decide the discrete action on RELEASE. (raylib's
        // GESTURE_TAP fires on touch-DOWN, so using it here would act at the
        // start of every swipe.)
        double dur = now - s_touch.t0;
        if (s_touch.max_np >= 2) {
            // Two-finger tap: back to the menu (the game stays resumable).
            // Long multi-finger contact is ignored.
            if (dur < 0.5) {
                in->escape_pressed = true;
                in->any_pressed = true;
            }
        } else if (!s_touch.moved && dur < 0.30) {
            in->touch_tap = true;
            in->tap_x = s_touch.last_pos.x;
            in->tap_y = s_touch.last_pos.y;
            in->any_pressed = true;
        }
        s_touch.active = false;
    }

    // Swipe gestures drive menu navigation (taps are decided on release, above).
    // Left/right move the active slot on the name / room-code pickers.
    int g = GetGestureDetected();
    if (g == GESTURE_SWIPE_UP)    in->menu_up    = true;
    if (g == GESTURE_SWIPE_DOWN)  in->menu_down  = true;
    if (g == GESTURE_SWIPE_LEFT)  in->menu_left  = true;
    if (g == GESTURE_SWIPE_RIGHT) in->menu_right = true;

#if !defined(PLATFORM_IOS)
    // Android hardware/gesture Back button (KEY_BACK); harmless no-op on web.
    // iOS has no key events (and no hardware back button).
    if (IsKeyPressed(KEY_BACK)) {
        in->escape_pressed = true;
        in->any_pressed    = true;
    }
#endif
}
#endif // OR_TOUCH

Input input_poll(void) {
    Input in = {0};
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_IOS)
    poll_keyboard(&in);   // desktop native + web (PC browsers)
#endif
#ifdef OR_TOUCH
    poll_touch(&in);      // Android + iOS + web (mobile browsers)
#endif
    return in;
}
