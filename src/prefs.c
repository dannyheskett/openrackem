// Persisted settings — see prefs.h. One value today (the player name); the
// storage helpers are structured so more can be added later. Storage is chosen
// per platform: desktop config dir, mobile app-private dir, web localStorage.
#include "prefs.h"
#include "platform.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_CAP 16   // >= the handle buffer; keeps names short for compact HUD

static char s_name[NAME_CAP] = {0};

const char* prefs_name(void) { return s_name; }

void prefs_set_name(const char* n) {
    size_t j = 0;
    if (n) {
        for (size_t i = 0; n[i] && j < NAME_CAP - 1; i++) {
            unsigned char c = (unsigned char)n[i];
            if (c >= 32 && c < 127) s_name[j++] = (char)c;
        }
    }
    s_name[j] = '\0';
}

#if defined(PLATFORM_WEB)
// ---------------------------------------------------------------------------
// Web: browser localStorage (survives reloads; no filesystem needed).
// ---------------------------------------------------------------------------
#include <emscripten.h>

void prefs_load(void) {
    const char* v = emscripten_run_script_string(
        "(localStorage.getItem('openrackem_name')||'')");
    prefs_set_name(v ? v : "");
}

void prefs_save(void) {
    // s_name is sanitized to printable ASCII (no quotes/backslashes survive the
    // name field's alnum filter), so direct interpolation is safe.
    char js[128];
    snprintf(js, sizeof js, "localStorage.setItem('openrackem_name','%s')", s_name);
    emscripten_run_script(js);
}

#else
// ---------------------------------------------------------------------------
// File-based platforms: desktop config dir, Android/iOS app-private storage.
// ---------------------------------------------------------------------------
#if defined(PLATFORM_IOS)
extern const char* plat_ios_prefs_path(void); // the app's Documents dir (ios shell)
#endif
#if defined(_WIN32)
#include <direct.h>
#define OR_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define OR_MKDIR(p) mkdir((p), 0755)
#endif

// Create every component of a directory path (best-effort; already-exists is
// fine). Needed because the config parent (e.g. ~/.config) may not exist yet.
static void make_dirs(const char* path) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/'
#if defined(_WIN32)
            || *p == '\\'
#endif
        ) {
            char sep = *p; *p = '\0';
            OR_MKDIR(tmp);
            *p = sep;
        }
    }
    OR_MKDIR(tmp);
}

// Fill `path` with the prefs file and `dir` with the directory to ensure exists.
// Returns false if no writable location can be determined.
static bool prefs_paths(char* path, int pcap, char* dir, int dcap) {
#if defined(PLATFORM_IOS)
    const char* base = plat_ios_prefs_path();
    if (!base || !base[0]) return false;
    snprintf(dir, dcap, "%s", base);
    snprintf(path, pcap, "%s/openrackem.prefs", base);
    return true;
#elif defined(PLATFORM_ANDROID)
    // The app-private files dir for our fixed package (created on demand below).
    snprintf(dir, dcap, "/data/data/com.danheskett.openrackem/files");
    snprintf(path, pcap, "%s/openrackem.prefs", dir);
    return true;
#elif defined(_WIN32)
    const char* appdata = getenv("APPDATA");
    if (!appdata || !appdata[0]) return false;
    snprintf(dir, dcap, "%s\\openrackem", appdata);
    snprintf(path, pcap, "%s\\prefs", dir);
    return true;
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    if (!home || !home[0]) return false;
    snprintf(dir, dcap, "%s/Library/Application Support/openrackem", home);
    snprintf(path, pcap, "%s/prefs", dir);
    return true;
#else // Linux / XDG
    char base[512];
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(base, sizeof base, "%s", xdg);
    } else {
        const char* home = getenv("HOME");
        if (!home || !home[0]) return false;
        snprintf(base, sizeof base, "%s/.config", home);
    }
    snprintf(dir, dcap, "%s/openrackem", base);
    snprintf(path, pcap, "%s/prefs", dir);
    return true;
#endif
}

void prefs_load(void) {
    char path[600], dir[560];
    if (!prefs_paths(path, sizeof path, dir, sizeof dir)) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char buf[NAME_CAP] = {0};
    if (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        prefs_set_name(buf);
    }
    fclose(f);
}

void prefs_save(void) {
    char path[600], dir[560];
    if (!prefs_paths(path, sizeof path, dir, sizeof dir)) return;
    make_dirs(dir); // create the config dir chain if needed
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "%s\n", s_name);
    fclose(f);
}

#endif // PLATFORM_WEB
