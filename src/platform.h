#ifndef OPENRACKEM_PLATFORM_H
#define OPENRACKEM_PLATFORM_H

// OR_TOUCH selects the touch-first mobile frontend: the adaptive portrait
// layout, on-screen control buttons, and tap-driven menus. It is enabled on
// Android and on the WebAssembly build (which targets mobile browsers but also
// accepts keyboard for desktop browsers). Desktop native builds leave it unset
// and use the fixed-canvas landscape layout with keyboard input.
//
// raylib defines PLATFORM_ANDROID / PLATFORM_WEB for its own sources; our build
// passes the matching -D for the game translation units.
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_WEB) || defined(PLATFORM_IOS)
#define OR_TOUCH 1
#endif

// The two renderers, selected by availability:
//   OR_PORTRAIT  — the touch-first portrait renderer (adaptive layout + on-screen
//                  buttons). Available on Android and web.
//   OR_LANDSCAPE — the desktop renderer (fixed 640x480 canvas, integer letterbox
//                  scaling, 3-column layout). Available on desktop native and web.
// Native desktop compiles only landscape; Android only portrait; the web build
// compiles BOTH and chooses at runtime (desktop browser -> landscape, phone ->
// portrait), so a laptop browser gets the same look as the native desktop app.
#ifdef OR_TOUCH
#define OR_PORTRAIT 1
#endif
#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_IOS)
#define OR_LANDSCAPE 1
#endif

// True only on the build that has both renderers and must pick at runtime (web).
#if defined(OR_PORTRAIT) && defined(OR_LANDSCAPE)
#define OR_RUNTIME_RENDERER 1
#endif

#endif // OPENRACKEM_PLATFORM_H
