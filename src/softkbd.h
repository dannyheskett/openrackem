#ifndef OPENRACKEM_SOFTKBD_H
#define OPENRACKEM_SOFTKBD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" { // ios_main.mm pushes events from Objective-C++
#endif

// The platform's on-screen keyboard, for the text-entry screens (player name,
// room code) on touch devices: Android (an input-connection view in
// OpenrackemActivity), iOS (UIKeyInput on the Metal view) and mobile browsers
// (a hidden <input> in web/shell.html). Typed characters, backspace and the
// keyboard's Done key come back through softkbd_poll() and are merged into the
// frame's Input by input.c, so the screens see them exactly like desktop typing.
//
// Desktop builds compile the same API as no-ops: they have a real keyboard.
// Only where neither keyboard is detected does the picker fall back to its
// tap-to-cycle letter spinners.

// True where this build can raise a native keyboard: Android once the
// Activity has registered its keyboard view, iOS, and on the web the touch
// layout when the page provides the hidden input.
bool softkbd_available(void);

// True where the player has a way to type: the native keyboard above, or a
// physical one. Desktop builds and the desktop browser layout assume a
// physical keyboard; elsewhere one counts once a hardware key has been pressed.
// The text-entry picker shows its letter spinners only when this is false.
bool softkbd_keyboard_detected(void);

// input.c: a key arrived from a physical keyboard this frame.
void softkbd_note_hw_key(void);

// Raise or lower the keyboard. Idempotent: called every frame with the wanted
// state, it only reaches the platform when that state changes.
void softkbd_set_visible(bool visible);

// Raise the keyboard again even if it is already wanted: the player may have
// dismissed it with the system's own control (Android Back, the iPad hide key),
// so a tap on the field asks for it back.
void softkbd_reshow(void);

// Events the keyboard produced since the last poll, drained in order.
enum { SOFTKBD_NONE = 0, SOFTKBD_BACKSPACE = -1, SOFTKBD_DONE = -2 };
// Returns a character code point, SOFTKBD_BACKSPACE, SOFTKBD_DONE, or
// SOFTKBD_NONE once the queue is empty.
int softkbd_poll(void);

// Platform side: queue one event (a code point, or a SOFTKBD_* value). Safe to
// call from the Android UI thread while the game thread polls.
void softkbd_push(int ev);

#ifdef __cplusplus
}
#endif

#endif // OPENRACKEM_SOFTKBD_H
