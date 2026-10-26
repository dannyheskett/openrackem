// Persisted settings — see prefs.h: the player name, the saved private room
// (and its owner key, when we created it), and the seat key of a live online
// match for Return to Game. Storage is chosen per platform: desktop config
// dir, mobile app-private dir, web localStorage.
#include "prefs.h"
#include "platform.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_CAP 16   // >= the handle buffer; keeps names short for compact HUD
#define CODE_CAP 8
#define KEY_CAP  33

static char s_name[NAME_CAP] = {0};
static char s_room[CODE_CAP] = {0};
static char s_owner[KEY_CAP] = {0};
static char s_seat[KEY_CAP]  = {0};

const char* prefs_name(void)       { return s_name; }
const char* prefs_room_code(void)  { return s_room; }
const char* prefs_room_owner(void) { return s_owner; }
const char* prefs_seat_key(void)   { return s_seat; }

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

// Codes and keys are alphanumeric on the wire; anything else (a hand-edited
// file) is dropped rather than carried into a message.
static void set_alnum(char* out, size_t cap, const char* in) {
    size_t j = 0;
    if (in) for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out[j++] = c;
    }
    out[j] = '\0';
}

void prefs_set_room(const char* code, const char* owner_key) {
    set_alnum(s_room, sizeof s_room, code);
    set_alnum(s_owner, sizeof s_owner, s_room[0] ? owner_key : NULL);
}

void prefs_set_seat_key(const char* key) { set_alnum(s_seat, sizeof s_seat, key); }

#if defined(PLATFORM_WEB)
// ---------------------------------------------------------------------------
// Web: browser localStorage (survives reloads; no filesystem needed).
// ---------------------------------------------------------------------------
#include <emscripten.h>

static void web_get(const char* key, void (*set)(const char*)) {
    char js[96];
    snprintf(js, sizeof js, "(localStorage.getItem('openrackem_%s')||'')", key);
    const char* v = emscripten_run_script_string(js);
    set(v ? v : "");
}
static void set_room_only(const char* v)  { set_alnum(s_room, sizeof s_room, v); }
static void set_owner_only(const char* v) { set_alnum(s_owner, sizeof s_owner, v); }

void prefs_load(void) {
    web_get("name", prefs_set_name);
    web_get("room", set_room_only);
    web_get("owner", set_owner_only);
    web_get("seat", prefs_set_seat_key);
    // A share link (?room=K7QM) sets the saved room, ready to join.
    const char* q = emscripten_run_script_string(
        "(new URLSearchParams(location.search).get('room')||'')");
    if (q && q[0]) {
        char code[CODE_CAP];
        set_alnum(code, sizeof code, q);
        if (strcmp(code, s_room) != 0) prefs_set_room(code, NULL);
    }
}

static void web_set(const char* key, const char* val) {
    // Every value is sanitized (printable name; alphanumeric codes/keys) and
    // the name filter drops quotes and backslashes, so interpolation is safe.
    char js[160];
    snprintf(js, sizeof js, "localStorage.setItem('openrackem_%s','%s')", key, val);
    emscripten_run_script(js);
}

void prefs_save(void) {
    web_set("name", s_name);
    web_set("room", s_room);
    web_set("owner", s_owner);
    web_set("seat", s_seat);
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

// One "key=value" line into the matching field. A line with no '=' is the
// legacy format (the name alone).
static void apply_line(const char* line) {
    const char* eq = strchr(line, '=');
    if (!eq) { prefs_set_name(line); return; }
    size_t kl = (size_t)(eq - line);
    const char* v = eq + 1;
    if (kl == 4 && strncmp(line, "name", 4) == 0) prefs_set_name(v);
    else if (kl == 4 && strncmp(line, "room", 4) == 0) set_alnum(s_room, sizeof s_room, v);
    else if (kl == 5 && strncmp(line, "owner", 5) == 0) set_alnum(s_owner, sizeof s_owner, v);
    else if (kl == 4 && strncmp(line, "seat", 4) == 0) set_alnum(s_seat, sizeof s_seat, v);
}

void prefs_load(void) {
    char path[600], dir[560];
    if (!prefs_paths(path, sizeof path, dir, sizeof dir)) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char buf[96];
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (buf[0]) apply_line(buf);
    }
    fclose(f);
}

void prefs_save(void) {
    char path[600], dir[560];
    if (!prefs_paths(path, sizeof path, dir, sizeof dir)) return;
    make_dirs(dir); // create the config dir chain if needed
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "name=%s\nroom=%s\nowner=%s\nseat=%s\n", s_name, s_room, s_owner, s_seat);
    fclose(f);
}

#endif // PLATFORM_WEB
