// Persisted settings — see prefs.h: the player name, the seat key of a live
// online match for Return to Game, and the last New Room settings. Storage is
// chosen per platform:
// desktop config dir, mobile app-private dir, web localStorage.
#include "prefs.h"
#include "platform.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_CAP 16   // >= the handle buffer; keeps names short for compact HUD
#define KEY_CAP  33

static char s_name[NAME_CAP] = {0};
static char s_seat[KEY_CAP]  = {0};

// The last New Room settings: seats, target, bonus, partners. Stored under
// the key "newroom" — not "room", which files from older builds may still
// carry with another meaning (a saved private room).
static int  s_room[4];
static bool s_room_set = false;

const char* prefs_name(void)     { return s_name; }
const char* prefs_seat_key(void) { return s_seat; }

void prefs_set_name(const char* n) {
    size_t j = 0;
    if (n) {
        for (size_t i = 0; n[i] && j < NAME_CAP - 1; i++) {
            unsigned char c = (unsigned char)n[i];
            // Printable ASCII, less the three characters that would end a
            // quoted string: the web build writes the name into one.
            if (c >= 32 && c < 127 && c != '\'' && c != '"' && c != '\\') s_name[j++] = (char)c;
        }
    }
    s_name[j] = '\0';
}

// Keys are alphanumeric on the wire; anything else (a hand-edited file) is
// dropped rather than carried into a message.
static void set_alnum(char* out, size_t cap, const char* in) {
    size_t j = 0;
    if (in) for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out[j++] = c;
    }
    out[j] = '\0';
}

void prefs_set_seat_key(const char* key) { set_alnum(s_seat, sizeof s_seat, key); }

bool prefs_room(int* seats, int* target, bool* bonus, bool* partners) {
    if (!s_room_set) return false;
    *seats = s_room[0];
    *target = s_room[1];
    *bonus = s_room[2] != 0;
    *partners = s_room[3] != 0;
    return true;
}

void prefs_set_room(int seats, int target, bool bonus, bool partners) {
    s_room[0] = seats;
    s_room[1] = target;
    s_room[2] = bonus ? 1 : 0;
    s_room[3] = partners ? 1 : 0;
    s_room_set = true;
}

// "seats,target,bonus,partners" as stored. Anything that doesn't read as four
// small numbers is ignored (the screen then opens with its defaults).
static void set_room_str(const char* v) {
    int a[4] = {0, 0, 0, 0}, n = 0;
    const char* p = v ? v : "";
    while (n < 4) {
        if (*p < '0' || *p > '9') return;
        int val = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 5) return;
            val = val * 10 + (*p++ - '0');
        }
        a[n++] = val;
        if (n < 4) {
            if (*p != ',') return;
            p++;
        }
    }
    if (*p != '\0') return;
    prefs_set_room(a[0], a[1], a[2] != 0, a[3] != 0);
}

static void room_str(char* out, size_t cap) {
    snprintf(out, cap, "%d,%d,%d,%d", s_room[0], s_room[1], s_room[2], s_room[3]);
}

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
void prefs_load(void) {
    web_get("name", prefs_set_name);
    web_get("seat", prefs_set_seat_key);
    web_get("newroom", set_room_str);
}

static void web_set(const char* key, const char* val) {
    // Every value is sanitized (printable name; alphanumeric key; digits and
    // commas for the room) and the name filter drops quotes and backslashes,
    // so interpolation is safe.
    char js[160];
    snprintf(js, sizeof js, "localStorage.setItem('openrackem_%s','%s')", key, val);
    emscripten_run_script(js);
}

void prefs_save(void) {
    web_set("name", s_name);
    web_set("seat", s_seat);
    if (s_room_set) {
        char room[48];
        room_str(room, sizeof room);
        web_set("newroom", room);
    }
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
    // room= / owner= lines from older builds (private rooms) are ignored.
    if (kl == 4 && strncmp(line, "name", 4) == 0) prefs_set_name(v);
    else if (kl == 4 && strncmp(line, "seat", 4) == 0) set_alnum(s_seat, sizeof s_seat, v);
    else if (kl == 7 && strncmp(line, "newroom", 7) == 0) set_room_str(v);
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
    fprintf(f, "name=%s\nseat=%s\n", s_name, s_seat);
    if (s_room_set) {
        char room[48];
        room_str(room, sizeof room);
        fprintf(f, "newroom=%s\n", room);
    }
    fclose(f);
}

#endif // PLATFORM_WEB
