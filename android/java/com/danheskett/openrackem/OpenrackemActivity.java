package com.danheskett.openrackem;

import android.app.NativeActivity;
import android.content.Context;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

/**
 * NativeActivity subclass whose only job is to make the game truly immersive:
 * hide the status and navigation bars and draw edge-to-edge, so there is no
 * reserved black band above the game. The C game code is unchanged and
 * still loaded via the "android.app.lib_name" manifest meta-data.
 *
 * The theme (windowLayoutInDisplayCutoutMode=shortEdges) plus raylib's
 * FLAG_FULLSCREEN_MODE were not enough on Android 11-15 — the system still
 * reserved the status-bar inset. Hiding the system bars from the Activity is
 * the reliable fix. Re-applied on focus gain because the bars can transiently
 * reappear (e.g. after a swipe or returning from background).
 */
public class OpenrackemActivity extends NativeActivity {

    // NativeActivity loads libopenrackem.so by dlopen()ing it in framework
    // native code, which never registers it with this class's ClassLoader. ART's
    // implicit JNI resolution only searches libraries the ClassLoader knows
    // about, so a `native` method declared here would fail to resolve at the
    // first call ("No implementation found for nativeSetSafeInsets") and crash
    // the app. Loading the same .so again from Java registers it for JNI
    // resolution; dlopen is idempotent, so NativeActivity's own load and the
    // single android_main entry are unaffected.
    static {
        System.loadLibrary("openrackem");
    }

    // Implemented in native (src/safe_area.c). Hands the window insets to the
    // layout so the game stays clear of the camera cutout and the gesture bar.
    // All four edges: sideways, the cutout and the bar sit on a side edge.
    private native void nativeSetSafeInsets(int top, int bottom, int left, int right,
                                            int cutoutLeft, int cutoutRight);

    // Implemented in native (src/softkbd.c). Register hands native this
    // Activity so it can call setKeyboardVisible(); Event queues one keyboard
    // event: a character, or one of the KBD_* codes below.
    private native void nativeKbdRegister();
    private native void nativeKbdEvent(int ev);
    private static final int KBD_BACKSPACE = -1; // SOFTKBD_BACKSPACE
    private static final int KBD_DONE      = -2; // SOFTKBD_DONE

    private KbdView kbdView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // NativeActivity asks for adjustResize; the name screen draws at the top
        // instead, so the keyboard must not resize (and re-create) the surface.
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        kbdView = new KbdView(this);
        addContentView(kbdView, new ViewGroup.LayoutParams(1, 1));
        try {
            nativeKbdRegister();
        } catch (Throwable t) {
            // Without it the name screen keeps its on-screen slot picker.
        }
        goImmersive();
        preferHighestRefreshRate();
        // Re-read the cutout whenever the window insets settle (first layout,
        // rotation, foldables) and forward it to native.
        getWindow().getDecorView().setOnApplyWindowInsetsListener((v, insets) -> {
            pushSafeInsets();
            return insets;
        });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            goImmersive();
            pushSafeInsets();
        }
    }

    // Called from the game thread (src/softkbd.c) when a text-entry screen opens
    // or closes, and again when the player taps the field after dismissing the
    // keyboard themselves.
    public void setKeyboardVisible(final boolean visible) {
        runOnUiThread(() -> {
            InputMethodManager imm =
                    (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm == null || kbdView == null) return;
            if (visible) {
                kbdView.requestFocus();
                imm.restartInput(kbdView);
                imm.showSoftInput(kbdView, 0);
            } else {
                imm.hideSoftInputFromWindow(kbdView.getWindowToken(), 0);
                kbdView.clearFocus();
                goImmersive(); // the keyboard can bring the system bars back
            }
        });
    }

    private void kbdEvent(int ev) {
        try {
            nativeKbdEvent(ev);
        } catch (Throwable t) {
            // Never let a JNI hiccup take the game down over a keystroke.
        }
    }

    /**
     * An invisible 1x1 view that is the keyboard's target. NativeActivity's own
     * content view offers the keyboard no input connection, so typing would
     * arrive (if at all) as raw key events with no characters. This one hands
     * the keyboard a connection that forwards what it commits to native.
     *
     * A visible-password field, so keyboards commit each letter directly instead
     * of composing words with suggestions. Composing is still handled for
     * keyboards that do it anyway: each change to the composing text is sent as
     * the backspaces and characters that turn the old text into the new.
     */
    private final class KbdView extends View {
        KbdView(Context ctx) {
            super(ctx);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo out) {
            out.inputType = InputType.TYPE_CLASS_TEXT
                    | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                    | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                    | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS;
            out.imeOptions = EditorInfo.IME_ACTION_DONE
                    | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                    | EditorInfo.IME_FLAG_NO_FULLSCREEN;
            return new KbdConnection(this);
        }
    }

    private final class KbdConnection extends BaseInputConnection {
        private String composing = "";

        KbdConnection(View v) {
            super(v, false);
        }

        // Send the edit that turns `from` into `to`: backspaces down to their
        // common prefix, then the rest of `to`.
        private void sendDiff(String from, String to) {
            int common = 0;
            while (common < from.length() && common < to.length()
                    && from.charAt(common) == to.charAt(common)) {
                common++;
            }
            for (int i = from.length(); i > common; i--) kbdEvent(KBD_BACKSPACE);
            for (int i = common; i < to.length(); i++) {
                char ch = to.charAt(i);
                kbdEvent(ch == '\n' ? KBD_DONE : ch);
            }
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            sendDiff(composing, text.toString());
            composing = "";
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            String next = text.toString();
            sendDiff(composing, next);
            composing = next;
            return true;
        }

        @Override
        public boolean finishComposingText() {
            composing = ""; // what was composed stays typed
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            for (int i = 0; i < beforeLength; i++) kbdEvent(KBD_BACKSPACE);
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent e) {
            if (e.getAction() != KeyEvent.ACTION_DOWN) return true;
            switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_DEL:
                kbdEvent(KBD_BACKSPACE);
                break;
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER:
                kbdEvent(KBD_DONE);
                break;
            default:
                int ch = e.getUnicodeChar();
                if (ch >= 32 && ch < 127) kbdEvent(ch);
                break;
            }
            return true;
        }

        @Override
        public boolean performEditorAction(int actionCode) {
            kbdEvent(KBD_DONE);
            return true;
        }
    }

    private void pushSafeInsets() {
        int top = 0, bottom = 0, left = 0, right = 0, cutLeft = 0, cutRight = 0;
        if (Build.VERSION.SDK_INT >= 28) {
            WindowInsets wi = getWindow().getDecorView().getRootWindowInsets();
            DisplayCutout dc = (wi != null) ? wi.getDisplayCutout() : null;
            if (dc != null) {
                top = dc.getSafeInsetTop();
                bottom = dc.getSafeInsetBottom();
                left = dc.getSafeInsetLeft();
                right = dc.getSafeInsetRight();
                // The top-edge cutout is the bounding rect flush with the top of
                // the screen; take its horizontal extent so native knows where
                // the camera sits.
                for (Rect r : dc.getBoundingRects()) {
                    if (r.top <= 0) {
                        cutLeft = r.left;
                        cutRight = r.right;
                        break;
                    }
                }
            }
        }
        // The cutout layout is cosmetic; never let a JNI/link hiccup crash the
        // game.
        try {
            nativeSetSafeInsets(top, bottom, left, right, cutLeft, cutRight);
        } catch (Throwable t) {
            // Fall back to the plain centered wordmark (native keeps zeros).
        }
    }

    // Ask for the display's fastest mode at its current resolution. Many phones
    // otherwise serve apps 60 Hz even on a 120 Hz panel. The simulation stays a
    // fixed 60 steps/s regardless (src/tick.c); a faster display only makes
    // rendering and input smoother. No-op on 60 Hz panels. (Display.Mode is
    // API 23; minSdk is 24. getDefaultDisplay is deprecated in API 30, but its
    // successor needs an attached-display Context minSdk 24 doesn't have.)
    @SuppressWarnings("deprecation")
    private void preferHighestRefreshRate() {
        try {
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode active = display.getMode();
            Display.Mode best = active;
            for (Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() == active.getPhysicalWidth()
                        && m.getPhysicalHeight() == active.getPhysicalHeight()
                        && m.getRefreshRate() > best.getRefreshRate()) {
                    best = m;
                }
            }
            if (best.getModeId() != active.getModeId()) {
                WindowManager.LayoutParams lp = getWindow().getAttributes();
                lp.preferredDisplayModeId = best.getModeId();
                getWindow().setAttributes(lp);
            }
        } catch (Throwable t) {
            // Refresh-rate selection is best-effort; never crash the game.
        }
    }

    private void goImmersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }
}
