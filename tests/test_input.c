// Unit tests for the touch-gesture recognizer in input.c. Like test_game.c,
// the code under test runs in isolation — no raylib, no window — by compiling
// with -DPLATFORM_IOS, the raylib-free configuration input.c already supports:
// or_types.h supplies the types and declares the touch/clock queries, and this
// file provides scripted fakes of them. The recognizer under test is the same
// C compiled into every touch platform (Android / web / iOS); only the poll
// surface behind it differs.
//
// Frames are driven at exactly 60 Hz: the recognizer's thresholds are in
// seconds (tap / pause durations) and screen fractions (the tap movement
// tolerance), so the fake clock advances 1/60 s per polled frame.
//
// Built and run by `make test`. A non-zero exit means a failure.

#include "input.c"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// --- Scripted fake of the poll surface --------------------------------------
// input.c reads fingers, swipe gestures, layout, and the clock through these.
// Tests set the state, then call one of the frame helpers to advance 1/60 s
// and poll.

static double  fake_now;       // GetTime()
static int     fake_np;        // GetTouchPointCount()
static Vector2 fake_pts[8];    // GetTouchPosition(i)
static int     fake_gesture;   // GetGestureDetected() (swipes; 0 = none)
static bool    fake_portrait;  // render_use_portrait()

double  GetTime(void)              { return fake_now; }
int     GetTouchPointCount(void)   { return fake_np; }
Vector2 GetTouchPosition(int i)    { return fake_pts[i]; }
int     GetGestureDetected(void)   { return fake_gesture; }
int     GetScreenWidth(void)       { return 450; }
int     GetScreenHeight(void)      { return 800; }
bool    render_use_portrait(void)  { return fake_portrait; }

// The on-screen keyboard's queue (softkbd.h): tests script the events it
// delivers; input.c drains them in order.
static int fake_kbd[32];
static int fake_kbd_len, fake_kbd_pos;
int softkbd_poll(void) {
    return (fake_kbd_pos < fake_kbd_len) ? fake_kbd[fake_kbd_pos++] : SOFTKBD_NONE;
}
static void kbd_script(const int* evs, int n) {
    for (int i = 0; i < n; i++) fake_kbd[i] = evs[i];
    fake_kbd_len = n;
    fake_kbd_pos = 0;
}

#define DT (1.0 / 60.0)
// 450x800 screen => tap movement tolerance 450/25 = 18 px.
#define TOL 18

// Fresh recognizer + fake surface for each test.
static void reset(void) {
    memset(&s_touch, 0, sizeof s_touch);
    fake_now      = 100.0;
    fake_np       = 0;
    fake_gesture  = 0;
    fake_portrait = true;
    fake_kbd_len  = fake_kbd_pos = 0;
    s_kbd_pending = SOFTKBD_NONE;
}

// Advance one 60 Hz frame and poll with the fingers currently set.
static Input frame(void) {
    fake_now += DT;
    return input_poll();
}

// One frame with a single finger at (x, y).
static Input frame_touch(float x, float y) {
    fake_np = 1;
    fake_pts[0] = (Vector2){x, y};
    return frame();
}

// One frame with two fingers down.
static Input frame_touch2(float x0, float y0, float x1, float y1) {
    fake_np = 2;
    fake_pts[0] = (Vector2){x0, y0};
    fake_pts[1] = (Vector2){x1, y1};
    return frame();
}

// One frame with every finger lifted (the release the recognizer decides on).
static Input frame_release(void) {
    fake_np = 0;
    return frame();
}

// --- Tap fires on release, with its coordinates ------------------------------
static void test_tap_on_release(void) {
    reset();

    // While the finger is down nothing fires — a tap must not trigger on
    // touch-DOWN or every swipe would begin with a spurious tap.
    Input in = frame_touch(200, 400);
    CHECK(!in.touch_tap && !in.any_pressed);
    in = frame_touch(200, 400);
    CHECK(!in.touch_tap);

    // Release after ~50 ms: a tap, with the coordinates for hit-testing.
    in = frame_release();
    CHECK(in.touch_tap);
    CHECK(in.any_pressed);
    CHECK(in.tap_x == 200 && in.tap_y == 400);
    CHECK(!in.escape_pressed);

    // The release is an edge: the next empty frame is silent.
    in = frame_release();
    CHECK(!in.touch_tap && !in.any_pressed);
}

// --- Long press is not a tap -------------------------------------------------
static void test_long_press_is_not_a_tap(void) {
    reset();

    frame_touch(200, 400);
    for (int i = 0; i < 20; i++) frame_touch(200, 400); // 21/60 s > 0.30 s cap
    Input in = frame_release();
    CHECK(!in.touch_tap && !in.any_pressed);
}

// --- Movement beyond the tolerance is not a tap ------------------------------
static void test_drag_is_not_a_tap(void) {
    reset();

    frame_touch(200, 400);
    frame_touch(200 + TOL + 4, 400);   // travelled past the tap tolerance
    frame_touch(200 + TOL + 4, 400);
    Input in = frame_release();
    CHECK(!in.touch_tap && !in.any_pressed);

    // Jitter inside the tolerance still taps.
    reset();
    frame_touch(200, 400);
    frame_touch(200 + TOL - 4, 400 + 5);
    Input in2 = frame_release();
    CHECK(in2.touch_tap);
}

// --- Two-finger tap = back/pause ---------------------------------------------
static void test_two_finger_tap_escapes(void) {
    reset();

    frame_touch(150, 300);
    frame_touch2(150, 300, 260, 320);
    Input in = frame_release();
    CHECK(in.escape_pressed);
    CHECK(in.any_pressed);
    CHECK(!in.touch_tap);   // a pause gesture is not also a tap

    // Long two-finger contact is ignored entirely.
    reset();
    for (int i = 0; i < 40; i++) frame_touch2(150, 300, 260, 320); // > 0.5 s
    in = frame_release();
    CHECK(!in.escape_pressed && !in.touch_tap);
}

// --- A finger landing/lifting mid-sequence doesn't fake a drag ---------------
static void test_second_finger_jump_not_a_drag(void) {
    reset();

    // pts[0] jumps when the second finger lands; the recognizer must not read
    // that as movement (it stops tracking once max_np >= 2).
    frame_touch(150, 300);
    frame_touch2(260, 320, 150, 300);  // pts[0] jumped to the other finger
    Input in = frame_release();
    CHECK(in.escape_pressed);          // still decided as a two-finger tap
}

// --- Swipes drive the menu cursor --------------------------------------------
static void test_swipes_move_menu(void) {
    reset();

    fake_gesture = GESTURE_SWIPE_UP;
    Input in = frame();
    CHECK(in.menu_up && !in.menu_down);

    fake_gesture = GESTURE_SWIPE_DOWN;
    in = frame();
    CHECK(in.menu_down);

    fake_gesture = 0;
    in = frame();
    CHECK(!in.menu_up && !in.menu_down);
}

// --- Landscape (desktop-browser) mode ignores touch --------------------------
static void test_landscape_ignores_touch(void) {
    reset();
    fake_portrait = false;

    frame_touch(200, 400);
    Input in = frame_release();
    CHECK(!in.touch_tap && !in.any_pressed && !in.escape_pressed);
}

// --- On-screen keyboard events reach the text fields, in order ---------------
static void test_softkbd_chars_and_done(void) {
    reset();
    kbd_script((const int[]){'D', 'a', 'n'}, 3);
    Input in = frame();
    CHECK(in.text_len == 3 && strcmp(in.text_input, "Dan") == 0);
    CHECK(!in.backspace_pressed && !in.confirm_pressed);

    // Done confirms (it saves the name / joins the room).
    kbd_script((const int[]){SOFTKBD_DONE}, 1);
    in = frame();
    CHECK(in.confirm_pressed && in.select_pressed && in.text_len == 0);
}

static void test_softkbd_keeps_order(void) {
    reset();
    // "AB", backspace, "C": the backspace must not apply before A and B land,
    // nor a second backspace collapse into the first.
    kbd_script((const int[]){'A', 'B', SOFTKBD_BACKSPACE, SOFTKBD_BACKSPACE, 'C'}, 5);
    Input in = frame();
    CHECK(strcmp(in.text_input, "AB") == 0 && !in.backspace_pressed);
    in = frame();
    CHECK(in.text_len == 0 && in.backspace_pressed);
    in = frame();
    CHECK(in.text_len == 0 && in.backspace_pressed);
    in = frame();
    CHECK(strcmp(in.text_input, "C") == 0 && !in.backspace_pressed);
    in = frame();
    CHECK(in.text_len == 0 && !in.backspace_pressed);
}

static void test_softkbd_overflow_and_non_ascii(void) {
    reset();
    // More than a frame's worth of characters spills into the next frame, and
    // a non-ASCII character arrives as a placeholder that is no letter.
    kbd_script((const int[]){'1', '2', '3', '4', '5', '6', '7', '8', 0x141}, 9);
    Input in = frame();
    CHECK(in.text_len == 7 && strcmp(in.text_input, "1234567") == 0);
    in = frame();
    CHECK(strcmp(in.text_input, "8?") == 0);
}

int main(void) {
    printf("test_input: touch recognizer — tap on release, long press, drag,\n");
    printf("            two-finger tap, finger-jump, swipes, landscape gating,\n");
    printf("            on-screen keyboard events\n");
    test_tap_on_release();
    test_long_press_is_not_a_tap();
    test_drag_is_not_a_tap();
    test_two_finger_tap_escapes();
    test_second_finger_jump_not_a_drag();
    test_swipes_move_menu();
    test_landscape_ignores_touch();
    test_softkbd_chars_and_done();
    test_softkbd_keeps_order();
    test_softkbd_overflow_and_non_ascii();
    if (failures == 0) {
        printf("OK: all checks passed\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
