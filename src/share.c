// Sharing a room invite (see share.h), one branch per platform.
#include "share.h"
#include "platform.h"
#include "render.h"

#include <stdbool.h>
#include <stddef.h>

#if defined(PLATFORM_IOS)
// ios_main.mm: presents a UIActivityViewController over the game view.
void plat_ios_share(const char* text);
ShareResult share_text(const char* text) {
    plat_ios_share(text);
    return SHARE_SHEET;
}

#elif defined(PLATFORM_ANDROID)
#include <jni.h>
// The Activity reference softkbd.c registers at startup.
JavaVM* or_android_vm(void);
jobject or_android_activity(void);

ShareResult share_text(const char* text) {
    JavaVM* vm = or_android_vm();
    jobject act = or_android_activity();
    if (!vm || !act) return SHARE_NONE;
    JNIEnv* env = NULL;
    bool attached = false;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return SHARE_NONE;
        attached = true;
    }
    ShareResult res = SHARE_NONE;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, cls, "shareText", "(Ljava/lang/String;)V");
    jstring js = (*env)->NewStringUTF(env, text);
    if (m && js) {
        (*env)->CallVoidMethod(env, act, m, js);
        res = SHARE_SHEET;
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); res = SHARE_NONE; }
    if (js) (*env)->DeleteLocalRef(env, js);
    (*env)->DeleteLocalRef(env, cls);
    if (attached) (*vm)->DetachCurrentThread(vm);
    return res;
}

#elif defined(PLATFORM_WEB) && defined(__EMSCRIPTEN__)
#include <emscripten.h>
// Phones get the system share sheet (navigator.share); desktop browsers, or a
// phone without it, get the clipboard. Returns 1 = sheet, 2 = copied, 0 = no.
EM_JS(int, js_share, (const char* text, int touch), {
    var t = UTF8ToString(text);
    if (touch && navigator.share) {
        navigator.share({ text: t }).catch(function () {});
        return 1;
    }
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(t).catch(function () {});
        return 2;
    }
    return 0;
});
ShareResult share_text(const char* text) {
    int r = js_share(text, render_use_portrait() ? 1 : 0);
    return r == 1 ? SHARE_SHEET : r == 2 ? SHARE_COPIED : SHARE_NONE;
}

#else
// Desktop (and the native screenshot harness): raylib's clipboard.
#include <raylib.h>
ShareResult share_text(const char* text) {
    SetClipboardText(text);
    return SHARE_COPIED;
}
#endif
