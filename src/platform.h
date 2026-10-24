#ifndef OPENRACKEM_PLATFORM_H
#define OPENRACKEM_PLATFORM_H

// The game's name: window title and recording file prefix.
#define GAME_NAME "openrackem"

// Recording size (recorder.c): the desktop column's 3:5 shape, so the layout
// fills the video. Both are multiples of 16 for the H.264 encoder.
#define REC_W 480
#define REC_H 800

// OR_TOUCH compiles in touch input: taps, swipes and the two-finger back
// gesture. It is enabled on Android, iOS and the WebAssembly build (which
// serves phones as well as desktop browsers). Every platform draws the same
// portrait layout (render_portrait.c); desktops centre it in a column and
// drive it with the mouse and keyboard.
//
// raylib defines PLATFORM_ANDROID / PLATFORM_WEB for its own sources; our build
// passes the matching -D for the game translation units.
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_WEB) || defined(PLATFORM_IOS)
#define OR_TOUCH 1
#endif

#endif // OPENRACKEM_PLATFORM_H
