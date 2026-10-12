// The on-screen keyboard bridge (see softkbd.h): one small event queue the
// platform fills and input.c drains, plus the per-platform show/hide call.
#include "softkbd.h"

#include "platform.h"
#include "render.h" // render_use_portrait

// --- Event queue ------------------------------------------------------------
// A ring of keyboard events. Android pushes from its UI thread while the game
// thread polls, so there it is guarded by a mutex; everywhere else both sides
// run on one thread.
#define Q_CAP 64
static int s_q[Q_CAP];
static int s_head = 0, s_len = 0;

#if defined(PLATFORM_ANDROID)
#include <pthread.h>
static pthread_mutex_t s_q_lock = PTHREAD_MUTEX_INITIALIZER;
#define Q_LOCK()   pthread_mutex_lock(&s_q_lock)
#define Q_UNLOCK() pthread_mutex_unlock(&s_q_lock)
#else
#define Q_LOCK()   ((void)0)
#define Q_UNLOCK() ((void)0)
#endif

void softkbd_push(int ev) {
    if (ev == SOFTKBD_NONE) return;
    Q_LOCK();
    if (s_len < Q_CAP) { // a full queue drops: nobody types 64 keys in a frame
        s_q[(s_head + s_len) % Q_CAP] = ev;
        s_len++;
    }
    Q_UNLOCK();
}

static int q_pop(void) {
    int ev = SOFTKBD_NONE;
    Q_LOCK();
    if (s_len > 0) {
        ev = s_q[s_head];
        s_head = (s_head + 1) % Q_CAP;
        s_len--;
    }
    Q_UNLOCK();
    return ev;
}

static void q_clear(void) {
    Q_LOCK();
    s_head = s_len = 0;
    Q_UNLOCK();
}

// --- Platform show / hide ---------------------------------------------------
#if defined(PLATFORM_ANDROID)
#include <jni.h>

// OpenrackemActivity registers itself once from onCreate; the game thread
// later calls back into it to raise or lower the keyboard. The game thread is
// not a Java thread, so each call attaches it for the duration if needed.
static JavaVM* s_vm;
static jobject s_activity; // global ref

JNIEXPORT void JNICALL
Java_com_danheskett_openrackem_OpenrackemActivity_nativeKbdRegister(JNIEnv* env, jobject thiz) {
    if (s_activity) (*env)->DeleteGlobalRef(env, s_activity);
    (*env)->GetJavaVM(env, &s_vm);
    s_activity = (*env)->NewGlobalRef(env, thiz);
}

JNIEXPORT void JNICALL
Java_com_danheskett_openrackem_OpenrackemActivity_nativeKbdEvent(JNIEnv* env, jobject thiz,
                                                                 jint ev) {
    (void)env;
    (void)thiz;
    softkbd_push((int)ev);
}

static void platform_show(bool visible) {
    if (!s_vm || !s_activity) return;
    JNIEnv* env = NULL;
    bool attached = false;
    if ((*s_vm)->GetEnv(s_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*s_vm)->AttachCurrentThread(s_vm, &env, NULL) != JNI_OK) return;
        attached = true;
    }
    jclass cls = (*env)->GetObjectClass(env, s_activity);
    jmethodID m = (*env)->GetMethodID(env, cls, "setKeyboardVisible", "(Z)V");
    if (m) (*env)->CallVoidMethod(env, s_activity, m, visible ? JNI_TRUE : JNI_FALSE);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    if (attached) (*s_vm)->DetachCurrentThread(s_vm);
}

#elif defined(PLATFORM_IOS)
// ios_main.mm: makes the Metal view first responder (or resigns it).
void plat_ios_set_keyboard(bool visible);
static void platform_show(bool visible) { plat_ios_set_keyboard(visible); }

#elif defined(PLATFORM_WEB) && defined(__EMSCRIPTEN__)
// (The screenshot harnesses compile -DPLATFORM_WEB natively; they get the
// no-op branch.)
#include <emscripten.h>
// web/shell.html defines window.orKbd (the hidden <input> and its queue).
EM_JS(void, js_kbd_set, (int on), {
    if (window.orKbd) window.orKbd.set(!!on);
});
EM_JS(int, js_kbd_next, (), {
    return window.orKbd ? window.orKbd.next() : 0;
});
static void platform_show(bool visible) { js_kbd_set(visible ? 1 : 0); }

#else
static void platform_show(bool visible) { (void)visible; }
#endif

// --- Public API -------------------------------------------------------------
bool softkbd_available(void) {
#if defined(PLATFORM_ANDROID) || defined(PLATFORM_IOS)
    return true;
#elif defined(PLATFORM_WEB) && defined(__EMSCRIPTEN__)
    return render_use_portrait(); // the touch layout; desktop browsers type directly
#else
    return false;
#endif
}

static bool s_visible = false;

void softkbd_set_visible(bool visible) {
    if (visible && !softkbd_available()) visible = false;
    if (visible == s_visible) return;
    s_visible = visible;
    q_clear(); // keys typed for the previous screen don't leak into the next
    platform_show(visible);
}

void softkbd_reshow(void) {
    if (s_visible) platform_show(true);
}

int softkbd_poll(void) {
#if defined(PLATFORM_WEB) && defined(__EMSCRIPTEN__)
    for (int ev; (ev = js_kbd_next()) != 0;) softkbd_push(ev);
#endif
    return q_pop();
}
