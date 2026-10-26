#include "game.h"
#include "rules.h"
#include "render.h"
#include "input.h"
#include "sound.h"
#include "recorder.h"
#include "app.h"
#include "tick.h"
#include "menu.h"
#include "window.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
#endif

#include "netgame.h"
#include "online_ui.h"
#include "prefs.h"
#include "share.h"
#include "softkbd.h"

typedef enum {
    STATE_MENU,
    STATE_OPTIONS,
    STATE_NAME,         // editing the persisted player name
    STATE_PLAYING,
    STATE_PAUSED,
    STATE_ONLINE,       // Play Online: room list, waiting room, or a live match
} AppState;

// Screens within STATE_ONLINE that the session state alone doesn't decide.
typedef enum {
    OS_LIST = 0,      // the room list (session idle)
    OS_NEW_ROOM,      // New Room: public or private
    OS_CODE,          // entering a private room code
    OS_SETTINGS,      // the owner's room settings (in the waiting room)
    OS_GAME_MENU,     // mid-match menu: resume / step away / forfeit
    OS_FORFEIT,       // forfeit confirmation
} OnlineScreen;

// Menu actions. The set of items shown depends on whether a game is in
// progress (a resumable game adds "Resume Game").
typedef enum {
    ACT_RETURN,
    ACT_RESUME,
    ACT_NEW,
    ACT_ONLINE,
    ACT_OPTIONS,
    ACT_SOUND,
    ACT_RECORD,
    ACT_EXIT,
} MenuAction;

// Upper bound on labels[]/actions[]: one slot per MenuAction (8). Each action
// appears at most once, so build_menu can never overflow.
#define MAX_MENU_ITEMS 8

// The Options screen exposes exactly what the plan allows: players, difficulty,
// bonus scoring, partners, target score. Everything else stays at the official
// defaults. The chosen values persist across games within a session.
static Rules s_options;
static bool  s_options_ready = false;

static Rules* current_options(void) {
    if (!s_options_ready) {
        s_options = rules_default();
        s_options_ready = true;
    }
    return &s_options;
}

// Map the events produced during a frame to sound effects.
static void play_event_sounds(const Game* g, unsigned events) {
    int human = g->rules.human_seat;
    if (events & EV_MATCH_END) { sound_play(SFX_MATCH_WIN); return; }
    if (events & EV_ROUND_END) {
        sound_play((human >= 0 && g->round_winner == human) ? SFX_ROUND_WIN
                                                            : SFX_ROUND_LOSE);
        if (events & EV_BONUS) sound_play(SFX_BONUS);
        return;
    }
    if (events & EV_PLACE)        sound_play(SFX_PLACE);
    else if (events & EV_DISCARD) sound_play(SFX_DISCARD);
    else if (events & EV_DRAW)    sound_play(SFX_DRAW);
    if (events & EV_TURN)         sound_play(SFX_TURN);
}

// Build the current menu. Returns the item count; fills labels[] and actions[],
// and sets *gap_before to the index that should have a blank line above it --
// Exit, which is set apart from the rest -- or -1 when this build has no Exit
// item at all (mobile and web, where the OS or the browser tab owns the
// lifecycle). A fixed "last item" index put the gap above whatever happened to
// be last, which on those builds was an ordinary setting.
static int build_menu(bool resumable, const char* labels[], MenuAction actions[],
                      int* gap_before) {
    int n = 0;
    *gap_before = -1;
    // A seat held in a live online match comes first: one tap back to it.
    if (net_available() && prefs_seat_key()[0]) {
        labels[n] = "Return to Game"; actions[n++] = ACT_RETURN;
    }
    if (resumable) { labels[n] = "Resume Game"; actions[n++] = ACT_RESUME; }
    labels[n] = "New Game"; actions[n++] = ACT_NEW;
    if (net_available()) { labels[n] = "Play Online"; actions[n++] = ACT_ONLINE; }
    labels[n] = "Options";  actions[n++] = ACT_OPTIONS;
    labels[n] = sound_is_enabled() ? "Sound: On" : "Sound: Off"; actions[n++] = ACT_SOUND;
#ifndef OR_TOUCH
    // The mp4 recorder is a desktop-only feature (stubbed out on mobile/web), so
    // the toggle would do nothing there — omit it.
    labels[n] = recorder_active() ? "Record: On" : "Record: Off"; actions[n++] = ACT_RECORD;
#endif
#if defined(PLATFORM_WEB)
    // A browser tab can't be closed from code, so no Exit on web. (Touch vs
    // desktop presentation is auto-detected by pointer type.)
#elif !defined(PLATFORM_IOS) && !defined(PLATFORM_ANDROID)
    // Mobile apps don't self-terminate (the OS owns the lifecycle: home gesture /
    // back button on Android, Apple guidelines on iOS), so no Exit on either.
    *gap_before = n;
    labels[n] = "Exit"; actions[n++] = ACT_EXIT;
#endif
    return n;
}

// The Options screen items. Values cycle with Left/Right (or Select); the last
// item returns to the menu. Labels are rebuilt every frame from the live Rules.
#define OPT_ITEMS 8
enum { OPT_NAME, OPT_PLAYERS, OPT_DIFFICULTY, OPT_BONUS, OPT_PARTNERS, OPT_TARGET,
       OPT_ROOM, OPT_BACK };

static int build_options(const char* labels[]) {
    static char buf[OPT_ITEMS][32];
    static const char* DIFF_NAMES[3] = {"Easy", "Normal", "Hard"};
    Rules* r = current_options();
    const char* nm = prefs_name();
    snprintf(buf[OPT_NAME],       sizeof buf[0], "Name: %s", (nm && nm[0]) ? nm : "(not set)");
    snprintf(buf[OPT_PLAYERS],    sizeof buf[0], "Players: %d", r->player_count);
    snprintf(buf[OPT_DIFFICULTY], sizeof buf[0], "Difficulty: %s", DIFF_NAMES[r->ai_difficulty]);
    snprintf(buf[OPT_BONUS],      sizeof buf[0], "Bonus Scoring: %s", r->bonus_scoring ? "On" : "Off");
    if (r->player_count == 4) {
        snprintf(buf[OPT_PARTNERS], sizeof buf[0], "Partners: %s", r->partners ? "On" : "Off");
    } else {
        snprintf(buf[OPT_PARTNERS], sizeof buf[0], "Partners: 4P only");
    }
    snprintf(buf[OPT_TARGET],     sizeof buf[0], "Play To: %d", r->target_score);
    if (prefs_room_code()[0])
        snprintf(buf[OPT_ROOM],   sizeof buf[0], "Room: %s (Forget)", prefs_room_code());
    else
        snprintf(buf[OPT_ROOM],   sizeof buf[0], "Room: none saved");
    snprintf(buf[OPT_BACK],       sizeof buf[0], "Back");
    for (int i = 0; i < OPT_ITEMS; i++) labels[i] = buf[i];
    return OPT_ITEMS;
}

// Cycle one Options value by `dir` (+1 / -1). Normalization keeps every
// combination legal (partners drops off below 4 players, etc.).
static void cycle_option(int item, int dir) {
    static const int TARGETS[] = {250, 500, 750, 1000};
    Rules* r = current_options();
    switch (item) {
    case OPT_PLAYERS:
        r->player_count += dir;
        if (r->player_count > 4) r->player_count = 2;
        if (r->player_count < 2) r->player_count = 4;
        break;
    case OPT_DIFFICULTY:
        r->ai_difficulty = (r->ai_difficulty + 3 + dir) % 3;
        break;
    case OPT_BONUS:
        r->bonus_scoring = !r->bonus_scoring;
        break;
    case OPT_PARTNERS:
        if (r->player_count == 4) r->partners = !r->partners;
        break;
    case OPT_TARGET: {
        int n = (int)(sizeof TARGETS / sizeof TARGETS[0]);
        int cur = 1; // default 500
        for (int i = 0; i < n; i++) if (TARGETS[i] == r->target_score) cur = i;
        r->target_score = TARGETS[(cur + n + dir) % n];
        break;
    }
    default:
        break;
    }
    rules_normalize(r);
}

// A fresh, uniquely-seeded game from the session options. The clock seeds the
// RNG here — never inside game.c, which must stay reproducible from its seed.
static Game* new_game(int human_seat) {
    Rules r = *current_options();
    r.human_seat = human_seat;
    r.seed = (uint64_t)time(NULL) ^ ((uint64_t)rand() << 32) ^ (uint64_t)rand();
    if (r.seed == 0) r.seed = 1;
    return game_create(&r);
}

#ifdef OR_SIMSTATS
// Real-device validation instrumentation (SIMSTATS=1 builds; compiles on
// desktop too for local smoke tests). Once per second of continuous play, log
// how many frames were rendered vs how many fixed 60 Hz sim steps ran, so
// scripts/devicefarm_run.py can assert from the device log that the
// fixed-timestep accumulator holds ~60 steps/s at whatever refresh rate the
// display actually delivers (the frames count is the evidence of that rate).
// Any gap between calls (menu, pause, a stall past the spiral clamp) starts a
// fresh window, so every logged line covers uninterrupted play.
#if defined(PLATFORM_ANDROID)
#include <android/log.h>
#define SIMSTATS_LOG(...) __android_log_print(ANDROID_LOG_INFO, "openrackem", __VA_ARGS__)
#else
#define SIMSTATS_LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#endif

static void simstats_count(double now, int steps) {
    static double win_start, last_call;
    static int frames, sim_steps;
    if (last_call == 0.0 || now - last_call > 0.25) { // gap: not continuous play
        win_start = now;
        frames = 0;
        sim_steps = 0;
    }
    last_call = now;
    frames++;
    sim_steps += steps;
    double span = now - win_start;
    if (span >= 1.0) {
        SIMSTATS_LOG("SIMSTATS window=%.3f frames=%d steps=%d", span, frames, sim_steps);
        win_start = now;
        frames = 0;
        sim_steps = 0;
    }
}
#endif // OR_SIMSTATS

// Typing into a slot picker (name / room code). Each keystroke since the cursor
// last moved is remembered as landed-in-a-slot or rejected (outside the
// alphabet), so a backspace undoes exactly what the keystroke did: native
// keyboards happily send spaces and punctuation, and deleting one of those must
// not eat a real letter.
#define TYPE_HIST 32
typedef struct {
    bool    tail;              // the last slot was just typed: backspace clears it in place
    uint8_t hist[TYPE_HIST];   // 1 = landed, 0 = rejected; oldest first
    int     hist_len;
} TypeState;

// App state carried across frames. Kept in one struct so the web build can drive
// the loop from an emscripten per-frame callback (browsers can't block).
typedef struct {
    Game* game;
    AppState state;
    int selected;      // menu cursor (shared by the main menu and Options)
    TableUi ui;        // slot cursor + round-over page, read by the renderers
    bool quit;
    SimClock clock;    // fixed-timestep accumulator (only advanced while playing)
    double prev_time;  // GetTime() at the previous frame; 0 before the first frame
    NetGame net;       // the online session (valid in STATE_ONLINE)
    OnlineScreen ol_screen;   // which online screen, where the session doesn't decide
    int ol_sel;        // cursor: room-list row / waiting-room button / menu row
    int ol_scroll;     // room list: first public row shown
    int ol_filter;     // room list: Play To filter (0 = any)
    NgState ol_prev;   // session state last frame (to catch transitions)
    long ol_last_room; // the public room we last left (cursor lands on it)
    bool ol_away;      // we stepped away because the app lost focus
    bool ol_was_focused;
    bool name_then_online;    // the name screen was the gate to Play Online
    char ol_notice[40];       // a local notice ("COPIED") ...
    int  ol_notice_frames;    // ...and how long it stays up
    char join_code[8]; // code being entered on the Join screen
    int code_cursor;   // active slot on the Join screen (0..3)
    char name_buf[16]; // player name being edited (STATE_NAME)
    int name_cursor;   // active slot on the Name screen (0..NAME_MAX-1)
    TypeState typing;  // keystroke history for whichever entry screen is up
} AppCtx;

// Where the pointer picked something this frame: a completed tap, or a mouse
// click. Hit-tested against the menu rows, the picker or the table zones.
static bool pointer_tap(const Input* in, Vector2* p) {
    if (in->touch_tap) { *p = (Vector2){in->tap_x, in->tap_y}; return true; }
    if (in->left_pressed) {
        *p = (Vector2){(float)in->mouse_x, (float)in->mouse_y};
        return true;
    }
    return false;
}

// Synthesize the acting player's Action for this frame from keyboard and
// pointer, for the seat `seat` of game `g` with cursor state `ui`. Returns true
// when an action was produced. Shared by offline play and the online client.
static bool input_to_action(Game* g, int seat, TableUi* ui, const Input* in, Action* out) {
    if (seat < 0) return false;                                // full-AI game
    if ((int)g->turn != seat) return false;                    // not our turn
    if (g->phase != PHASE_DRAW && g->phase != PHASE_PLACE) return false;

    // Keyboard.
    if (g->phase == PHASE_DRAW) {
        if (in->draw_stock_pressed)   { *out = (Action){ACTION_DRAW_STOCK, 0};   return true; }
        if (in->draw_discard_pressed) { *out = (Action){ACTION_DRAW_DISCARD, 0}; return true; }
    } else {
        if (in->cursor_up   && ui->cursor < RACK_SLOTS - 1) ui->cursor++;
        if (in->cursor_down && ui->cursor > 0)              ui->cursor--;
        if (in->confirm_pressed) { *out = (Action){ACTION_PLACE, (uint8_t)ui->cursor}; return true; }
        if (in->throw_pressed)   { *out = (Action){ACTION_DISCARD, 0}; return true; }
    }

    // Touch / mouse: taps and clicks against the zones the renderer captured
    // last frame.
    Vector2 tp;
    if (pointer_tap(in, &tp)) {
        int hit = render_table_hit_test(tp);
        if (g->phase == PHASE_DRAW) {
            if (hit == HIT_STOCK)   { *out = (Action){ACTION_DRAW_STOCK, 0};   return true; }
            if (hit == HIT_DISCARD) { *out = (Action){ACTION_DRAW_DISCARD, 0}; return true; }
        } else {
            if (hit >= 0 && hit < RACK_SLOTS) {
                ui->cursor = hit;
                *out = (Action){ACTION_PLACE, (uint8_t)hit};
                return true;
            }
            // Tapping the discard pile again throws a stock-drawn card away
            // (game_apply rejects it for a discard-drawn card).
            if (hit == HIT_DISCARD) { *out = (Action){ACTION_DISCARD, 0}; return true; }
        }
    }
    return false;
}

// --- Online ------------------------------------------------------------------
// The daemon address. Defaults to the public deployment over wss:// so "Play
// Online" works out of the box; override with the environment to reach a local
// or self-hosted daemon (e.g. OPENRACKEM_SERVER=127.0.0.1 OPENRACKEM_PORT=8080,
// which auto-selects plain ws:// on a non-443 port; force with OPENRACKEM_TLS).
#define DEFAULT_SERVER "openrackem-rooms.fly.dev"
#define DEFAULT_PORT   443

static const char* server_host(void) {
    const char* h = getenv("OPENRACKEM_SERVER");
    return (h && h[0]) ? h : DEFAULT_SERVER;
}
static int server_port(void) {
    const char* p = getenv("OPENRACKEM_PORT");
    int v = p ? atoi(p) : 0;
    return v > 0 ? v : DEFAULT_PORT;
}
// TLS on by default for the public endpoint; off for a plain local daemon.
// OPENRACKEM_TLS=1/0 forces it; otherwise it follows the port (443 = wss).
static bool server_tls(void) {
    const char* t = getenv("OPENRACKEM_TLS");
    if (t && (t[0] == '1' || t[0] == 'y' || t[0] == 'Y')) return true;
    if (t && (t[0] == '0' || t[0] == 'n' || t[0] == 'N')) return false;
    return server_port() == 443;
}

// The room-code alphabet the daemon uses (no ambiguous 0/O/1/I), for the Join
// slot picker, and the length of a private room's code.
static const char CODE_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
#define CODE_LEN 4

// Step one picker slot along its alphabet ring (+1 / -1, wrapping). Shared by
// the room-code and name screens, which are the same widget over different
// rings.
static void slot_cycle(char* slot, const char* alphabet, int dir) {
    int n = (int)strlen(alphabet);
    const char* pos = strchr(alphabet, *slot);
    int idx = pos ? (int)(pos - alphabet) : 0;
    idx = ((idx + (dir > 0 ? 1 : -1)) % n + n) % n;
    *slot = alphabet[idx];
}

// A tap on the slot picker, resolved against what the renderer drew. Returns 1
// to confirm, -1 to cancel, 0 if the tap only moved the cursor or cycled a
// slot (or missed everything).
static int picker_tap(const Input* in, char* slots, int nslots, int* cursor,
                      const char* alphabet) {
    Vector2 tp;
    if (!pointer_tap(in, &tp)) return 0;
    int hit = render_picker_hit_test(tp);
    if (hit >= 0 && hit < nslots) {
        *cursor = hit;
        softkbd_reshow(); // the player may have put the keyboard away
        sound_play(SFX_MENU_MOVE);
    } else if (hit == PICK_NEXT || hit == PICK_PREV) {
        slot_cycle(&slots[*cursor], alphabet, (hit == PICK_NEXT) ? 1 : -1);
        sound_play(SFX_MENU_MOVE);
    } else if (hit == PICK_OK) {
        return 1;
    } else if (hit == PICK_CANCEL) {
        return -1;
    }
    return 0;
}

static void type_reset(TypeState* t) { memset(t, 0, sizeof *t); }

// Apply this frame's typing (physical or on-screen keyboard) to `slots`:
// letters land at the cursor and advance it; backspace steps back and clears.
// Lowercase is folded up. `blank` is what a cleared slot holds.
static void type_into(TypeState* t, const Input* in, char* slots, int n, int* cursor,
                      const char* alphabet, char blank) {
    for (int i = 0; i < in->text_len; i++) {
        char ch = in->text_input[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        bool ok = (ch != ' ' && strchr(alphabet, ch) != NULL);
        if (t->hist_len == TYPE_HIST) { // forget the oldest keystroke
            memmove(t->hist, t->hist + 1, TYPE_HIST - 1);
            t->hist_len--;
        }
        t->hist[t->hist_len++] = ok;
        if (!ok) continue;
        slots[*cursor] = ch;
        if (*cursor < n - 1) (*cursor)++;
        else t->tail = true;
        sound_play(SFX_MENU_MOVE);
    }
    if (in->backspace_pressed) {
        if (t->hist_len > 0 && !t->hist[--t->hist_len]) return; // nothing landed
        if (t->tail) {
            t->tail = false;
        } else if (*cursor > 0) {
            (*cursor)--;
        }
        slots[*cursor] = blank;
    }
}

// Anything that moves the cursor or changes a slot by hand makes the keystroke
// history meaningless.
static void type_maybe_reset(TypeState* t, const Input* in) {
    if (in->touch_tap || in->left_pressed || in->menu_left || in->menu_right || in->menu_up || in->menu_down)
        type_reset(t);
}

// The picker's hint line: how to edit on this device.
static const char* picker_hint(void) {
    bool touch = render_use_portrait();
    if (softkbd_keyboard_detected())
        return touch ? "TYPE, OR TAP A SLOT TO MOVE THE CURSOR"
                     : "TYPE, OR CLICK A SLOT TO MOVE THE CURSOR";
    return touch ? "TAP A SLOT, THEN THE LETTER ABOVE OR BELOW IT"
                 : "CLICK A SLOT, THEN THE LETTER ABOVE OR BELOW IT";
}

// The Enter Code screen: a 4-slot private room code picked from the
// daemon's alphabet. Left/Right move the cursor, Up/Down cycle the letter,
// confirm joins. Returns 1 to join, -1 to go back, 0 to stay.
static int join_screen(AppCtx* c, const Input* in) {
    if (in->escape_pressed) return -1;
    if (c->join_code[0] == '\0') {
        for (int i = 0; i < CODE_LEN; i++) c->join_code[i] = 'A';
        c->join_code[CODE_LEN] = '\0';
    }
    // Keyboard (physical or on-screen): type the code directly, filtered to
    // the room-code alphabet. Coexists with the slot picker.
    type_maybe_reset(&c->typing, in);
    type_into(&c->typing, in, c->join_code, CODE_LEN, &c->code_cursor, CODE_ALPHABET, 'A');
    if (in->menu_left)  c->code_cursor = (c->code_cursor + CODE_LEN - 1) % CODE_LEN;
    if (in->menu_right) c->code_cursor = (c->code_cursor + 1) % CODE_LEN;
    if (in->menu_up || in->menu_down) {
        slot_cycle(&c->join_code[c->code_cursor], CODE_ALPHABET, in->menu_up ? 1 : -1);
        sound_play(SFX_MENU_MOVE);
    }
    int tap = picker_tap(in, c->join_code, CODE_LEN, &c->code_cursor, CODE_ALPHABET);
    if (tap != 0) return tap;
    if (in->confirm_pressed) return 1;
    return 0;
}

// --- Name entry (persisted player name) --------------------------------------
#define NAME_MAX 12
// Space (blank slot) first; kept identifier-safe to match the server's handle
// sanitizer. Keyboard fills slots left-to-right; touch swipes cycle/move.
static const char NAME_ALPHABET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

static void name_edit_begin(AppCtx* c) {
    const char* nm = prefs_name();
    int len = (int)strlen(nm);
    for (int i = 0; i < NAME_MAX; i++) c->name_buf[i] = (i < len) ? nm[i] : ' ';
    c->name_buf[NAME_MAX] = '\0';
    // Start after the existing name, so typing appends and backspace deletes
    // from its end, as in any text field.
    c->name_cursor = (len < NAME_MAX) ? len : NAME_MAX - 1;
    type_reset(&c->typing);
    c->typing.tail = (len >= NAME_MAX);
}

static void name_edit_save(const AppCtx* c) {
    // Concatenate the slots, drop blanks, and persist (blanks are only a slot
    // placeholder, never part of the name).
    char out[NAME_MAX + 1];
    int j = 0;
    for (int i = 0; i < NAME_MAX; i++)
        if (c->name_buf[i] != ' ') out[j++] = c->name_buf[i];
    out[j] = '\0';
    prefs_set_name(out);
    prefs_save();
}

// The Name screen: the keyboard (physical, or the device's own on-screen one)
// types alphanumerics into the slots and backspace deletes; a tap on a slot
// moves the cursor there, and SAVE / CANCEL (or the keyboard's Done) leave.
// Only without any keyboard (softkbd_keyboard_detected) do the spinner bands
// appear to cycle the active letter.
// Returns 1 to save+exit, -1 to cancel, 0 to stay.
static int name_screen(AppCtx* c, const Input* in) {
    if (in->escape_pressed) return -1;
    type_maybe_reset(&c->typing, in);
    type_into(&c->typing, in, c->name_buf, NAME_MAX, &c->name_cursor, NAME_ALPHABET, ' ');
    if (in->menu_left)  c->name_cursor = (c->name_cursor + NAME_MAX - 1) % NAME_MAX;
    if (in->menu_right) c->name_cursor = (c->name_cursor + 1) % NAME_MAX;
    if (in->menu_up || in->menu_down) {
        slot_cycle(&c->name_buf[c->name_cursor], NAME_ALPHABET, in->menu_up ? 1 : -1);
        sound_play(SFX_MENU_MOVE);
    }
    // A bare tap used to mean "save and leave", which made the picker unusable
    // on a phone: every attempt to reach another slot exited the screen. Taps
    // now go to the widget, and only its SAVE button confirms.
    int tap = picker_tap(in, c->name_buf, NAME_MAX, &c->name_cursor, NAME_ALPHABET);
    if (tap != 0) return tap;
    if (in->confirm_pressed) return 1;
    return 0;
}

// What a server notice means, for the screens.
static const char* notice_text(const char* code) {
    static const struct { const char* code; const char* text; } T[] = {
        {"no_room",   "NO SUCH ROOM"},
        {"full",      "THAT ROOM IS FULL"},
        {"started",   "THAT GAME HAS ALREADY STARTED"},
        {"rate",      "TOO MANY TRIES - WAIT A MINUTE"},
        {"team_full", "THAT TEAM IS FULL"},
        {"no_seat",   "THAT GAME HAS ENDED"},
        {"not_owner", "ONLY THE ROOM'S CREATOR CAN DO THAT"},
        {"idle",      "YOU WERE MOVED OUT FOR NOT BEING READY"},
        {"closed",    "THE ROOM CLOSED"},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (strcmp(code, T[i].code) == 0) return T[i].text;
    }
    return NULL;
}

static void online_notice(AppCtx* c, const char* text) {
    snprintf(c->ol_notice, sizeof c->ol_notice, "%s", text);
    c->ol_notice_frames = 180;
}

// The line under the screen: a local notice first, then the server's.
static const char* online_notice_line(const AppCtx* c) {
    if (c->ol_notice_frames > 0) return c->ol_notice;
    if (c->net.notice[0]) return notice_text(c->net.notice);
    return NULL;
}

// Open the session. With `rejoin_key` the session goes straight back to that
// seat (Return to Game); otherwise it lands on the room list. A seat still
// held elsewhere rides along, to be surrendered if a different game is joined.
static void online_begin(AppCtx* c, const char* rejoin_key) {
    netgame_start(&c->net, server_host(), server_port(), server_tls(), prefs_name(),
                  rejoin_key);
    if (!rejoin_key) {
        snprintf(c->net.stale_key, sizeof c->net.stale_key, "%s", prefs_seat_key());
    }
    c->ui = (TableUi){ .cursor = 0, .standings = false };
    c->state = STATE_ONLINE;
    c->ol_screen = OS_LIST;
    c->ol_sel = 0;
    c->ol_scroll = 0;
    c->ol_prev = NG_CONNECTING;
    c->ol_away = false;
    c->ol_notice_frames = 0;
}

// Play Online needs a name: ask first if there isn't one.
static void online_enter(AppCtx* c) {
    if (prefs_name()[0]) {
        online_begin(c, NULL);
    } else {
        c->name_then_online = true;
        name_edit_begin(c);
        c->state = STATE_NAME;
    }
}

static void online_leave_to_menu(AppCtx* c) {
    netgame_close(&c->net);   // mid-match this steps away; the seat is kept
    c->state = STATE_MENU;
    c->selected = 0;
}

// Keep the persisted bits in step with the session: the seat of a live match
// (Return to Game), the saved private room, and its owner key.
static void online_sync_prefs(AppCtx* c) {
    NetGame* ng = &c->net;
    bool dirty = false;
    if (ng->state == NG_PLAYING && ng->key[0] && !ng->result_pending) {
        if (strcmp(prefs_seat_key(), ng->key) != 0) { prefs_set_seat_key(ng->key); dirty = true; }
    } else if ((ng->state == NG_LOBBY || strcmp(ng->notice, "no_seat") == 0) &&
               prefs_seat_key()[0]) {
        // Back in a waiting room (the match ended, or another game was
        // joined), or the seat is gone: nothing left to return to.
        prefs_set_seat_key("");
        dirty = true;
    }
    if (ng->have_lobby && (ng->state == NG_LOBBY || ng->state == NG_PLAYING)) {
        const NgLobby* L = &ng->lobby;
        bool same = strcmp(prefs_room_code(), L->code) == 0;
        if (L->priv && L->code[0] && !same) {
            // A private room we created or joined becomes the saved room.
            prefs_set_room(L->code, ng->owner);
            dirty = true;
        } else if (same && L->code[0]) {
            if (ng->owner[0] && strcmp(prefs_room_owner(), ng->owner) != 0) {
                prefs_set_room(L->code, ng->owner);
                dirty = true;
            } else if (!ng->owner[0] && prefs_room_owner()[0]) {
                // Back in our own room in a later session: we still own it.
                snprintf(ng->owner, sizeof ng->owner, "%s", prefs_room_owner());
            }
        }
    }
    if (dirty) prefs_save();
}

static void online_view(const AppCtx* c, OnlineView* v) {
    const NetGame* ng = &c->net;
    memset(v, 0, sizeof *v);
    v->ng = ng;
    v->player = prefs_name();
    v->saved_code = prefs_room_code();
    v->has_return = prefs_seat_key()[0] != '\0';
    v->sel = c->ol_sel;
    v->scroll = c->ol_scroll;
    v->filter = c->ol_filter;
    v->notice = online_notice_line(c);
    v->is_owner = ng->owner[0] != '\0';
}

static void online_invite(const AppCtx* c) {
    char text[160];
    const char* code = c->net.lobby.code;
    snprintf(text, sizeof text,
             "Join my Rack 'Em table: %s - https://danheskett.com/dist/openrackem/?room=%s",
             code, code);
    ShareResult r = share_text(text);
    if (r == SHARE_COPIED) online_notice((AppCtx*)c, "INVITE COPIED - PASTE IT TO A FRIEND");
    else if (r == SHARE_NONE) online_notice((AppCtx*)c, "SHARE THE CODE ON SCREEN");
}

// The two small menus over the online screens.
#define NEW_ROOM_ITEMS 3
static const char* const NEW_ROOM_LABELS[NEW_ROOM_ITEMS] = {
    "Public (anyone can join)", "Private (by code)", "Back"
};
#define GAME_MENU_ITEMS 3
static const char* const GAME_MENU_LABELS[GAME_MENU_ITEMS] = {
    "Resume", "Step Away", "Forfeit"
};
#define FORFEIT_ITEMS 2
static const char* const FORFEIT_LABELS[FORFEIT_ITEMS] = { "Keep Playing", "Forfeit" };

// The owner's room settings, rebuilt each frame from the live waiting room.
#define SET_ITEMS 6
enum { SET_TARGET, SET_BONUS, SET_PARTNERS, SET_PRIVATE, SET_AWAY, SET_BACK };
static int build_room_settings(const NgLobby* L, const char* labels[]) {
    static char buf[SET_ITEMS][40];
    snprintf(buf[SET_TARGET],   sizeof buf[0], "Play To: %d", L->target);
    snprintf(buf[SET_BONUS],    sizeof buf[0], "Bonus Scoring: %s", L->bonus ? "On" : "Off");
    snprintf(buf[SET_PARTNERS], sizeof buf[0], "Partners: %s", L->partners ? "On" : "Off");
    snprintf(buf[SET_PRIVATE],  sizeof buf[0], "Room: %s", L->priv ? "Private" : "Public");
    snprintf(buf[SET_AWAY],     sizeof buf[0], "If Someone Leaves: %s",
             !L->priv ? "AI after 30s" : L->away_policy ? "Wait" : "AI after 30s");
    snprintf(buf[SET_BACK],     sizeof buf[0], "Back");
    for (int i = 0; i < SET_ITEMS; i++) labels[i] = buf[i];
    return SET_ITEMS;
}

// Plain list navigation for the small menus: returns the chosen row or -1.
static int menu_pick(int* sel, int count, const Input* in) {
    if (in->menu_up)   { *sel = (*sel + count - 1) % count; sound_play(SFX_MENU_MOVE); }
    if (in->menu_down) { *sel = (*sel + 1) % count; sound_play(SFX_MENU_MOVE); }
    bool pick = in->select_pressed;
    Vector2 p;
    if (pointer_tap(in, &p)) {
        int hit = menu_hit_test(p);
        if (hit >= 0 && hit < count) { *sel = hit; pick = true; }
    }
    return pick ? *sel : -1;
}

static void activate_list_row(AppCtx* c, const OlRow* row) {
    NetGame* ng = &c->net;
    sound_play(SFX_MENU_SELECT);
    switch (row->kind) {
    case OLR_RETURN:
        netgame_rejoin(ng, prefs_seat_key());
        break;
    case OLR_SAVED:
        if (ng->peek.known && !ng->peek.ok && strcmp(ng->peek.code, prefs_room_code()) == 0) {
            netgame_create(ng, true, current_options(), false);   // expired: a new one
        } else {
            netgame_join_code(ng, prefs_room_code());
        }
        break;
    case OLR_NEW:
        c->ol_screen = OS_NEW_ROOM;
        c->ol_sel = 0;
        break;
    case OLR_CODE:
        snprintf(c->join_code, sizeof c->join_code, "%s", prefs_room_code());
        if (strlen(c->join_code) != CODE_LEN) c->join_code[0] = '\0';
        c->code_cursor = 0;
        type_reset(&c->typing);
        c->ol_screen = OS_CODE;
        break;
    case OLR_BEST:
        netgame_best(ng, c->ol_filter);
        break;
    case OLR_ROOM:
        c->ol_last_room = ng->rooms[row->room].id;
        netgame_join_room(ng, c->ol_last_room);
        break;
    }
}

static void online_list_input(AppCtx* c, const Input* in) {
    static const int FILTERS[] = {0, 250, 500, 750, 1000};
    NetGame* ng = &c->net;
    OnlineView v;
    online_view(c, &v);
    OlRow rows[OL_MAX_ROWS];
    int n = online_list_rows(&v, rows);
    if (c->ol_sel >= n) c->ol_sel = n - 1;
    if (c->ol_sel < 0) c->ol_sel = 0;

    if (in->escape_pressed) { online_leave_to_menu(c); return; }
    if (in->menu_up && c->ol_sel > 0) { c->ol_sel--; sound_play(SFX_MENU_MOVE); }
    if (in->menu_down && c->ol_sel < n - 1) { c->ol_sel++; sound_play(SFX_MENU_MOVE); }
    int fdir = (in->menu_right ? 1 : 0) - (in->menu_left ? 1 : 0);

    Vector2 p;
    bool activate = in->select_pressed;
    if (pointer_tap(in, &p)) {
        int hit = online_hit_test(p);
        if (hit >= OL_HIT_ROW && hit < OL_HIT_ROW + n) { c->ol_sel = hit - OL_HIT_ROW; activate = true; }
        else if (hit == OL_HIT_FILTER) fdir = 1;
        else if (hit == OL_HIT_SHUFFLE) { netgame_shuffle(ng); sound_play(SFX_MENU_MOVE); }
        else if (hit == OL_HIT_BACK) { online_leave_to_menu(c); return; }
    }
    if (fdir) {
        int k = 0, nf = (int)(sizeof FILTERS / sizeof FILTERS[0]);
        for (int i = 0; i < nf; i++) if (FILTERS[i] == c->ol_filter) k = i;
        c->ol_filter = FILTERS[(k + nf + fdir) % nf];
        netgame_list(ng, c->ol_filter);
        sound_play(SFX_MENU_MOVE);
    }
    if (activate && n > 0) activate_list_row(c, &rows[c->ol_sel]);
}

static void activate_lobby_button(AppCtx* c, int id) {
    NetGame* ng = &c->net;
    const NgLobby* L = &ng->lobby;
    sound_play(SFX_MENU_SELECT);
    switch (id) {
    case OLB_READY:    netgame_ready(ng, !L->ready[L->me]); break;
    case OLB_FORCE:    netgame_force(ng); break;
    case OLB_SWITCH:   netgame_team(ng, 1 - L->team[L->me]); break;
    case OLB_SHARE:    online_invite(c); break;
    case OLB_SETTINGS: c->ol_screen = OS_SETTINGS; c->ol_sel = 0; break;
    case OLB_LEAVE:    netgame_leave(ng); break;
    case OLB_ACCEPT:   netgame_swap_answer(ng, L->swap_from, true); break;
    case OLB_DECLINE:  netgame_swap_answer(ng, L->swap_from, false); break;
    }
}

static void online_lobby_input(AppCtx* c, const Input* in) {
    NetGame* ng = &c->net;
    OnlineView v;
    online_view(c, &v);
    int ids[8];
    int n = online_lobby_buttons(&v, ids);
    if (c->ol_sel >= n || c->ol_sel < 0) c->ol_sel = 0;

    if (in->escape_pressed) {
        if (ng->lobby.swap_from >= 0) netgame_swap_answer(ng, ng->lobby.swap_from, false);
        else netgame_leave(ng);
        return;
    }
    if (in->menu_up || in->menu_left) { c->ol_sel = (c->ol_sel + n - 1) % n; sound_play(SFX_MENU_MOVE); }
    if (in->menu_down || in->menu_right) { c->ol_sel = (c->ol_sel + 1) % n; sound_play(SFX_MENU_MOVE); }
    Vector2 p;
    if (pointer_tap(in, &p)) {
        int hit = online_hit_test(p);
        if (hit >= OL_HIT_MEMBER && hit < OL_HIT_MEMBER + MAX_PLAYERS) {
            netgame_swap(ng, hit - OL_HIT_MEMBER);
            online_notice(c, "SWAP REQUEST SENT");
            sound_play(SFX_MENU_SELECT);
        } else if (hit >= OL_HIT_BUTTON && hit < OL_HIT_BUTTON + 16) {
            activate_lobby_button(c, hit - OL_HIT_BUTTON);
        }
        return;
    }
    if (in->select_pressed) activate_lobby_button(c, ids[c->ol_sel]);
}

static void online_settings_input(AppCtx* c, const Input* in) {
    static const int TARGETS[] = {250, 500, 750, 1000};
    NetGame* ng = &c->net;
    NgLobby L = ng->lobby;
    if (in->escape_pressed) { c->ol_screen = OS_LIST; c->ol_sel = 0; return; }
    if (in->menu_up)   { c->ol_sel = (c->ol_sel + SET_ITEMS - 1) % SET_ITEMS; sound_play(SFX_MENU_MOVE); }
    if (in->menu_down) { c->ol_sel = (c->ol_sel + 1) % SET_ITEMS; sound_play(SFX_MENU_MOVE); }
    int dir = (in->menu_right ? 1 : 0) - (in->menu_left ? 1 : 0);
    bool pick = in->select_pressed;
    Vector2 p;
    if (pointer_tap(in, &p)) {
        int hit = menu_hit_test(p);
        if (hit >= 0 && hit < SET_ITEMS) { c->ol_sel = hit; pick = true; }
    }
    if (pick && c->ol_sel == SET_BACK) { c->ol_screen = OS_LIST; c->ol_sel = 0; return; }
    if (!dir && !pick) return;
    if (!dir) dir = 1;
    switch (c->ol_sel) {
    case SET_TARGET: {
        int k = 1, nt = (int)(sizeof TARGETS / sizeof TARGETS[0]);
        for (int i = 0; i < nt; i++) if (TARGETS[i] == L.target) k = i;
        L.target = TARGETS[(k + nt + dir) % nt];
        break;
    }
    case SET_BONUS:    L.bonus = !L.bonus; break;
    case SET_PARTNERS: L.partners = !L.partners; break;
    case SET_PRIVATE:  L.priv = !L.priv; break;
    case SET_AWAY:     if (L.priv) L.away_policy = !L.away_policy; break;
    default: return;
    }
    netgame_settings(ng, L.target, L.bonus, L.partners, L.priv, L.away_policy != 0);
    sound_play(SFX_MENU_SELECT);
}

static void online_play_input(AppCtx* c, const Input* in, unsigned ev) {
    NetGame* ng = &c->net;
    play_event_sounds(&ng->game, ev);
    if (c->ol_screen == OS_GAME_MENU) {
        if (in->escape_pressed) { c->ol_screen = OS_LIST; return; }
        int pick = menu_pick(&c->ol_sel, GAME_MENU_ITEMS, in);
        if (pick == 0) { c->ol_screen = OS_LIST; }
        else if (pick == 1) { online_leave_to_menu(c); }
        else if (pick == 2) { c->ol_screen = OS_FORFEIT; c->ol_sel = 0; }
        return;
    }
    if (c->ol_screen == OS_FORFEIT) {
        if (in->escape_pressed) { c->ol_screen = OS_LIST; return; }
        int pick = menu_pick(&c->ol_sel, FORFEIT_ITEMS, in);
        if (pick == 0) c->ol_screen = OS_LIST;
        else if (pick == 1) {
            netgame_forfeit(ng);
            prefs_set_seat_key("");
            prefs_save();
            c->ol_screen = OS_LIST;
            c->ol_sel = 0;
        }
        return;
    }

    // Escape steps away: the seat is kept and Return to Game brings you back.
    if (in->escape_pressed) { online_leave_to_menu(c); return; }

    // The absent-player banner's button.
    Vector2 tp;
    if (ng->wait_seat >= 0 && pointer_tap(in, &tp) &&
        render_table_hit_test(tp) == HIT_LETAI) {
        netgame_letai(ng, ng->wait_seat);
        sound_play(SFX_MENU_SELECT);
        return;
    }

    if (ng->game.phase == PHASE_ROUND_OVER) {
        // First confirm shows standings; the second sends `next`.
        if (in->confirm_pressed || in->touch_tap || in->left_pressed) {
            if (!c->ui.standings) { c->ui.standings = true; sound_play(SFX_MENU_MOVE); }
            else { netgame_confirm(ng); c->ui.standings = false; }
        }
        return;
    }
    if (ng->game.phase == PHASE_MATCH_OVER) return;   // the room follows at once

    bool placing = netgame_my_turn(ng) && ng->game.phase == PHASE_PLACE;
    if (in->pause_pressed && !placing) {
        c->ol_screen = OS_GAME_MENU;
        c->ol_sel = 0;
        sound_play(SFX_PAUSE);
        return;
    }
    Action a;
    if (input_to_action(&ng->game, ng->my_seat, &c->ui, in, &a)) netgame_action(ng, a);
}

// One frame of Play Online.
static void online_frame(AppCtx* c, const Input* in, double dt) {
    NetGame* ng = &c->net;
    unsigned ev = netgame_update(ng);
    netgame_elapse(ng, (int)(dt * 1000.0));
    if (c->ol_notice_frames > 0) c->ol_notice_frames--;
    online_sync_prefs(c);

    // Transitions the screens follow.
    if (ng->state != c->ol_prev) {
        if (ng->state == NG_IDLE) {
            netgame_list(ng, c->ol_filter);
            if (prefs_room_code()[0]) netgame_peek(ng, prefs_room_code());
            if (c->ol_screen != OS_NEW_ROOM && c->ol_screen != OS_CODE) {
                c->ol_screen = OS_LIST;
                c->ol_sel = 0;
            }
        } else if (ng->state == NG_LOBBY || ng->state == NG_PLAYING) {
            c->ol_screen = OS_LIST;   // i.e. no overlay: the session decides
            c->ol_sel = 0;
            c->ui = (TableUi){ .cursor = 0, .standings = false };
        }
        c->ol_prev = ng->state;
    }
    // Back on the list after leaving a room: the cursor lands on it.
    if (ng->state == NG_IDLE && c->ol_screen == OS_LIST && c->ol_last_room && ng->have_rooms) {
        OnlineView v;
        online_view(c, &v);
        OlRow rows[OL_MAX_ROWS];
        int n = online_list_rows(&v, rows);
        for (int i = 0; i < n; i++) {
            if (rows[i].kind == OLR_ROOM && ng->rooms[rows[i].room].id == c->ol_last_room) {
                c->ol_sel = i;
            }
        }
        c->ol_last_room = 0;
    }

    switch (ng->state) {
    case NG_ERROR:
        if (strcmp(ng->err, "need_name") == 0 || strcmp(ng->err, "bad_name") == 0) {
            netgame_close(ng);
            if (strcmp(ng->err, "bad_name") == 0) prefs_set_name("");
            c->name_then_online = true;
            name_edit_begin(c);
            c->state = STATE_NAME;
        } else if (in->any_pressed && !in->fullscreen_toggle) {
            c->state = STATE_MENU;
            c->selected = 0;
        }
        break;
    case NG_CONNECTING:
    case NG_DISCONNECTED:
        if (in->escape_pressed) online_leave_to_menu(c);
        break;
    case NG_IDLE:
        if (c->ol_screen == OS_NEW_ROOM) {
            if (in->escape_pressed) { c->ol_screen = OS_LIST; break; }
            int pick = menu_pick(&c->ol_sel, NEW_ROOM_ITEMS, in);
            if (pick == 0 || pick == 1) {
                netgame_create(ng, pick == 1, current_options(), false);
                sound_play(SFX_MENU_SELECT);
            }
            if (pick >= 0) { c->ol_screen = OS_LIST; c->ol_sel = 0; }
        } else if (c->ol_screen == OS_CODE) {
            int r = join_screen(c, in);
            if (r < 0) { c->ol_screen = OS_LIST; break; }
            if (r > 0) {
                netgame_join_code(ng, c->join_code);
                c->ol_screen = OS_LIST;
                sound_play(SFX_MENU_SELECT);
            }
        } else {
            online_list_input(c, in);
        }
        break;
    case NG_LOBBY:
        if (ng->result_pending) {
            // The match result stays up until it's dismissed; then the room.
            if (in->confirm_pressed || in->touch_tap || in->left_pressed || in->escape_pressed) {
                netgame_dismiss_result(ng);
                sound_play(SFX_MENU_SELECT);
            }
        } else if (c->ol_screen == OS_SETTINGS) {
            online_settings_input(c, in);
        } else {
            online_lobby_input(c, in);
        }
        break;
    case NG_PLAYING:
        if (ng->have_game) online_play_input(c, in, ev);
        else if (in->escape_pressed) online_leave_to_menu(c);
        break;
    }
}

// The app lost focus (backgrounded, tab hidden, window deactivated): an
// online match steps away at once rather than burning turn clocks, and
// comes back with the app.
static void online_background(AppCtx* c) {
    if (c->state != STATE_ONLINE || c->net.state != NG_PLAYING) return;
    netgame_leave(&c->net);
    if (c->net.conn) net_poll(c->net.conn);   // flush before the OS suspends us
    c->ol_away = true;
}

static void online_foreground(AppCtx* c) {
    if (!c->ol_away) return;
    c->ol_away = false;
    if (c->state == STATE_ONLINE) netgame_here(&c->net);
}

static void render_online_status(const AppCtx* c) {
    const NetGame* ng = &c->net;
    char l0[48] = "", l1[48] = "";
    const char* title = "ONLINE";
    switch (ng->state) {
    case NG_CONNECTING:   snprintf(l0, sizeof l0, "Connecting..."); break;
    case NG_DISCONNECTED: snprintf(l0, sizeof l0, "Reconnecting..."); break;
    case NG_ERROR:
        title = "DISCONNECTED";
        snprintf(l0, sizeof l0, "%s", strcmp(ng->err, "upgrade") == 0
                 ? "Please update the app" : ng->err[0] ? ng->err : "connection failed");
        snprintf(l1, sizeof l1, "Press any key");
        break;
    default: break;
    }
    const char* lines[2];
    int n = 0;
    if (l0[0]) lines[n++] = l0;
    if (l1[0]) lines[n++] = l1;
    render_menu(title, lines, n, -1, -1);
}

static void render_online(AppCtx* c) {
    NetGame* ng = &c->net;
    OnlineView v;
    switch (ng->state) {
    case NG_IDLE:
        if (c->ol_screen == OS_NEW_ROOM) {
            render_menu("NEW ROOM", NEW_ROOM_LABELS, NEW_ROOM_ITEMS, c->ol_sel, NEW_ROOM_ITEMS - 1);
        } else if (c->ol_screen == OS_CODE) {
            render_picker("ENTER ROOM CODE", c->join_code, c->code_cursor, CODE_ALPHABET,
                          picker_hint(), "JOIN");
        } else {
            online_view(c, &v);
            online_render_list(&v);
            c->ol_scroll = v.scroll;
        }
        break;
    case NG_LOBBY:
        if (ng->result_pending && ng->have_game) {
            render_frame(&ng->game, &c->ui);
        } else if (c->ol_screen == OS_SETTINGS) {
            const char* labels[SET_ITEMS];
            build_room_settings(&ng->lobby, labels);
            render_menu("ROOM SETTINGS", labels, SET_ITEMS, c->ol_sel, SET_BACK);
        } else {
            online_view(c, &v);
            online_render_lobby(&v);
        }
        break;
    case NG_PLAYING:
        if (!ng->have_game) { render_online_status(c); break; }
        if (c->ol_screen == OS_GAME_MENU) {
            render_menu("GAME", GAME_MENU_LABELS, GAME_MENU_ITEMS, c->ol_sel, -1);
        } else if (c->ol_screen == OS_FORFEIT) {
            render_menu("FORFEIT THIS MATCH?", FORFEIT_LABELS, FORFEIT_ITEMS, c->ol_sel, -1);
        } else {
            render_frame(&ng->game, &c->ui);
        }
        break;
    default:
        render_online_status(c);
        break;
    }
}

// One iteration of the game loop. `arg` is an AppCtx* (void* to match the
// emscripten_set_main_loop callback signature).
static void frame_step(void* arg) {
    AppCtx* c = (AppCtx*)arg;

    // Load persisted prefs (player name) once, on the first frame — works on
    // every platform, including iOS where main() is compiled out.
    static bool prefs_ready = false;
    if (!prefs_ready) { prefs_load(); prefs_ready = true; }

    // Real seconds since the previous frame, feeding the fixed-timestep
    // accumulator so presentation runs at 60 Hz on any display refresh. The
    // first frame (prev_time == 0) is treated as exactly one step. The clock only
    // banks time while actually playing; any other state drains it so a pause or
    // menu can't hoard a burst of catch-up steps for the moment play resumes.
    double now = GetTime();
    double dt = (c->prev_time > 0.0) ? now - c->prev_time : SIM_DT;
    c->prev_time = now;
    if (c->state != STATE_PLAYING) sim_clock_reset(&c->clock);

    // Sampled every frame, not only while playing, so a stale "was focused"
    // cannot survive a menu visit and fire on the first frame of the next game.
    bool focus_lost = window_focus_lost();

    // An online match steps away when the app loses focus and comes back with
    // it. (iOS also signals this directly from the shell, since frames stop
    // once the app is suspended; a repeat here is harmless.)
    bool focused_now = IsWindowFocused();
    if (c->ol_was_focused && !focused_now) online_background(c);
    else if (!c->ol_was_focused && focused_now) online_foreground(c);
    c->ol_was_focused = focused_now;

    Input in = input_poll();
    if (in.fullscreen_toggle) window_toggle_fullscreen();

    bool resumable = (c->game != NULL && !game_is_over(c->game));
    const char* labels[MAX_MENU_ITEMS];
    MenuAction actions[MAX_MENU_ITEMS];
    int gap_before = -1;
    int menu_count = build_menu(resumable, labels, actions, &gap_before);

    switch (c->state) {
    case STATE_MENU: {
        if (c->selected >= menu_count) c->selected = 0;
        if (in.escape_pressed) {
            // Escape backs out: resume a game in progress, else quit (native).
            if (resumable) { c->state = STATE_PLAYING; break; }
            c->quit = true; return;
        }
        if (in.menu_up) {
            c->selected = (c->selected + menu_count - 1) % menu_count;
            sound_play(SFX_MENU_MOVE);
        }
        if (in.menu_down) {
            c->selected = (c->selected + 1) % menu_count;
            sound_play(SFX_MENU_MOVE);
        }
        // A tap or click on a row chooses it; a keyboard select activates the
        // highlighted row.
        bool do_select = in.select_pressed;
        Vector2 p;
        if (pointer_tap(&in, &p)) {
            int hit = menu_hit_test(p);
            if (hit >= 0 && hit < menu_count) { c->selected = hit; do_select = true; }
        }
        if (do_select) {
            switch (actions[c->selected]) {
            case ACT_RETURN:
                online_begin(c, prefs_seat_key());
                sound_play(SFX_MENU_SELECT);
                break;
            case ACT_RESUME:
                c->state = STATE_PLAYING;
                sound_play(SFX_MENU_SELECT);
                break;
            case ACT_NEW:
                if (c->game) game_destroy(c->game);
                c->game = new_game(0);
                c->ui = (TableUi){ .cursor = 0, .standings = false };
                if (recorder_active()) { recorder_stop(); recorder_start(NULL); }
                c->state = STATE_PLAYING;
                sound_play(SFX_MENU_SELECT);
                break;
            case ACT_ONLINE:
                online_enter(c);
                sound_play(SFX_MENU_SELECT);
                break;
            case ACT_OPTIONS:
                c->state = STATE_OPTIONS;
                c->selected = 0;
                sound_play(SFX_MENU_SELECT);
                break;
            case ACT_SOUND:
                sound_toggle();
                sound_play(SFX_MENU_SELECT); // audible only once enabled
                break;
            case ACT_RECORD:
                recorder_toggle();
                sound_play(SFX_MENU_SELECT);
                break;
            case ACT_EXIT:
                c->quit = true; return;
            }
        }
        break;
    }

    case STATE_OPTIONS: {
        const char* opt_labels[OPT_ITEMS];
        int opt_count = build_options(opt_labels);
        if (c->selected >= opt_count) c->selected = 0;
        if (in.escape_pressed) {
            c->state = STATE_MENU;
            c->selected = 0;
            break;
        }
        if (in.menu_up) {
            c->selected = (c->selected + opt_count - 1) % opt_count;
            sound_play(SFX_MENU_MOVE);
        }
        if (in.menu_down) {
            c->selected = (c->selected + 1) % opt_count;
            sound_play(SFX_MENU_MOVE);
        }
        int dir = (in.menu_right ? 1 : 0) - (in.menu_left ? 1 : 0);
        bool do_select = in.select_pressed;
        Vector2 p;
        if (pointer_tap(&in, &p)) {
            int hit = menu_hit_test(p);
            if (hit >= 0 && hit < opt_count) { c->selected = hit; do_select = true; }
        }
        if (do_select && c->selected == OPT_BACK) {
            c->state = STATE_MENU;
            c->selected = 0;
            sound_play(SFX_MENU_SELECT);
        } else if (do_select && c->selected == OPT_NAME) {
            name_edit_begin(c);
            c->state = STATE_NAME;
            sound_play(SFX_MENU_SELECT);
        } else if (do_select && c->selected == OPT_ROOM) {
            if (prefs_room_code()[0]) {
                prefs_set_room("", NULL);
                prefs_save();
                sound_play(SFX_MENU_SELECT);
            }
        } else if (c->selected == OPT_NAME || c->selected == OPT_ROOM) {
            // Name / room rows: left/right don't cycle a value.
        } else if (dir != 0 || do_select) {
            cycle_option(c->selected, dir ? dir : 1);
            sound_play(SFX_MENU_SELECT);
        }
        break; // rendered by the per-state dispatch below
    }

    case STATE_NAME: {
        int res = name_screen(c, &in);
        if (res == 1) {
            name_edit_save(c);
            sound_play(SFX_MENU_SELECT);
            if (c->name_then_online) {
                // The name was the way into Play Online: carry on there.
                c->name_then_online = false;
                if (prefs_name()[0]) online_begin(c, NULL);
                else { c->state = STATE_MENU; c->selected = 0; }
            } else {
                c->state = STATE_OPTIONS;
            }
        } else if (res == -1) {
            if (c->name_then_online) {
                c->name_then_online = false;
                c->state = STATE_MENU;
                c->selected = 0;
            } else {
                c->state = STATE_OPTIONS;
            }
        }
        break; // rendered by the per-state dispatch below
    }

    case STATE_PLAYING: {
        // Losing focus (app backgrounded, tab hidden, window deactivated)
        // returns to the menu; the game stays resumable.
        if (focus_lost) {
            c->state = STATE_MENU;
            c->selected = 0;
            break;
        }
        Game* g = c->game;
        bool human_turn = (g->rules.human_seat >= 0 &&
                           (int)g->turn == g->rules.human_seat &&
                           (g->phase == PHASE_DRAW || g->phase == PHASE_PLACE));
        if (in.escape_pressed) {
            c->state = STATE_MENU; // game stays alive and resumable
            c->selected = 0;
            break;
        }
        // Enter pauses only when it has nothing to confirm: during the deal,
        // an opponent's turn, or while waiting to draw. Holding a card, Enter
        // places it; on the round screens, Enter advances.
        if (in.pause_pressed && !(human_turn && g->phase == PHASE_PLACE) &&
            g->phase != PHASE_ROUND_OVER && g->phase != PHASE_MATCH_OVER) {
            c->state = STATE_PAUSED;
            sound_play(SFX_PAUSE);
            break;
        }

        // Run the fixed 60 Hz steps that have elapsed (animation + AI pacing).
        // game_update clears the event flags each step, so OR them into
        // frame_events to keep every sound when a frame runs more than one step.
        unsigned frame_events = 0;
        uint8_t phase_before = g->phase;
        int steps = sim_clock_advance(&c->clock, dt);
#ifdef OR_SIMSTATS
        simstats_count(now, steps);
#endif
        for (int s = 0; s < steps; s++) {
            game_update(g);
            frame_events |= g->events;
        }
        // The input above was released against the screen that existed BEFORE
        // these steps ran. If the phase changed during them (an AI went out,
        // the deal finished), that press must not act on a screen the player
        // has never seen: a tap-spam during "CPU THINKING" would skip the
        // round reveal, and a tap on the still-face-down discard outline could
        // commit a blind forced draw the instant the deal completes.
        bool phase_flipped = (g->phase != phase_before);

        // Round-over flow: first confirm shows the standings page, the second
        // starts the next round (or reveals the match result).
        if (g->phase == PHASE_ROUND_OVER) {
            if (!phase_flipped && (in.confirm_pressed || in.touch_tap || in.left_pressed)) {
                if (!c->ui.standings) {
                    c->ui.standings = true;
                    sound_play(SFX_MENU_MOVE);
                } else {
                    unsigned before = g->events;
                    game_next_round(g);
                    frame_events |= g->events & ~before;
                    c->ui.standings = false;
                }
            }
        } else if (g->phase == PHASE_MATCH_OVER) {
            if (!phase_flipped && (in.confirm_pressed || in.touch_tap || in.left_pressed)) {
#ifdef OR_AUTOPLAY
                c->game = new_game(-1);
#else
                c->state = STATE_MENU; // the finished game stays for the menu's
                c->selected = 0;       // non-resumable check
#endif
            }
        } else {
            Action a;
            if (!phase_flipped &&
                input_to_action(g, g->rules.human_seat, &c->ui, &in, &a)) {
                unsigned before = g->events;
                if (game_apply(g, a)) {
                    frame_events |= g->events & ~before;
                } else {
                    sound_play(SFX_INVALID);
                }
            }
        }
#ifdef OR_AUTOPLAY
        // Validation builds run hands-off: auto-advance the round screens so an
        // unattended device run plays match after match for its whole duration.
        if (c->game->phase == PHASE_MATCH_OVER) {
            c->game = new_game(-1);
        }
#endif
        play_event_sounds(c->game, frame_events);
        break;
    }

    case STATE_PAUSED:
        if (in.escape_pressed) {
            c->state = STATE_MENU; // game stays alive and resumable
            c->selected = 0;
        } else if (in.any_pressed && !in.fullscreen_toggle) {
            c->state = STATE_PLAYING;
        }
        break;

    case STATE_ONLINE:
        online_frame(c, &in, dt);
        break; // rendered via the dispatch below
    }

    // The device's keyboard is up exactly while a text-entry screen is.
    softkbd_set_visible(c->state == STATE_NAME ||
                        (c->state == STATE_ONLINE && c->net.state == NG_IDLE &&
                         c->ol_screen == OS_CODE));

    // Online opponents show their chosen names (server handles); offline seats
    // fall back to "CPU N".
    if (c->state == STATE_ONLINE && c->net.have_game) {
        const NetGame* ng = &c->net;
        render_set_seat_labels(ng->handles, ng->players);
        OnlineHud hud = { .on = true, .deadline_ms = ng->deadline_ms,
                          .wait_seat = ng->wait_seat, .wait_ms = ng->wait_ms,
                          .wait_timer = ng->wait_timer };
        for (int i = 0; i < MAX_PLAYERS; i++) hud.away[i] = ng->ctl[i];
        if (ng->back_frames > 0) snprintf(hud.back, sizeof hud.back, "%s", ng->back_name);
        render_set_online_hud(&hud);
    } else {
        render_clear_seat_labels();
        render_set_online_hud(NULL);
    }

    // Render for the state we ended the frame in. Every state must be handled
    // here: a same-frame transition (menu -> options) otherwise falls into a
    // branch whose data doesn't exist yet (there is no game before New Game).
    if (c->state == STATE_MENU) {
        render_menu("OPENRACKEM", labels, menu_count, c->selected, gap_before);
    } else if (c->state == STATE_OPTIONS) {
        const char* opt_labels[OPT_ITEMS];
        int opt_count = build_options(opt_labels);
        render_menu("OPTIONS", opt_labels, opt_count, c->selected, OPT_BACK);
    } else if (c->state == STATE_NAME) {
        render_picker("YOUR NAME", c->name_buf, c->name_cursor, NAME_ALPHABET,
                      picker_hint(), "SAVE");
    } else if (c->state == STATE_PAUSED) {
        render_pause(c->game, &c->ui);
    } else if (c->state == STATE_ONLINE) {
        render_online(c);
    } else {
        render_frame(c->game, &c->ui);
    }
}

#if defined(PLATFORM_IOS)

// iOS: UIKit provides main() and the run loop, so the normal main() below is
// compiled out. The app shell (ios_main.mm) sets up the Metal layer, calls
// app_init() once, then app_frame() from a CADisplayLink each frame.
static AppCtx ios_ctx;

void app_init(void) {
    srand((unsigned int)time(NULL));
    render_init();   // no-op on iOS (UIKit owns the window)
    sound_init();    // AVAudioEngine backend
    ios_ctx.game = NULL;
    ios_ctx.state = STATE_MENU;
    ios_ctx.selected = 0;
    ios_ctx.ui = (TableUi){ .cursor = 0, .standings = false };
    ios_ctx.quit = false;
    sim_clock_reset(&ios_ctx.clock);
    ios_ctx.prev_time = 0.0;
#ifdef OR_AUTOPLAY
    ios_ctx.game = new_game(-1);
    ios_ctx.state = STATE_PLAYING;
#endif
}

void app_frame(void) { frame_step(&ios_ctx); }
void app_on_background(void) { online_background(&ios_ctx); }
void app_on_foreground(void) { online_foreground(&ios_ctx); }

#else

int main(int argc, char** argv) {
    srand((unsigned int)time(NULL)); // seeds the game-seed mixer and sound noise

    // CLI: --record [path] starts recording immediately (auto-named if no path).
    bool cli_record = false;
    const char* cli_record_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--record") == 0) {
            cli_record = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') cli_record_path = argv[++i];
        }
    }

    render_init();
    sound_init();

#ifdef PLATFORM_WEB
    // Pick touch vs desktop by the primary pointer: coarse (phone/tablet) -> the
    // layout fills the screen and takes taps; fine (desktop / 2-in-1 laptop) ->
    // the layout sits in a centred column and takes the mouse and keyboard,
    // matching the native desktop app. Only pointer:coarse is used —
    // the maxTouchPoints/ontouchstart backstops wrongly flipped touchscreen
    // laptops to the touch layout.
    render_set_portrait(emscripten_run_script_int(
        "(window.matchMedia && window.matchMedia('(pointer: coarse)').matches) ? 1 : 0"));
#endif

    if (cli_record) recorder_start(cli_record_path);

    // Static so the pointer handed to emscripten stays valid after main()'s stack
    // is unwound on the web build (see the PLATFORM_WEB branch below).
    static AppCtx ctx;
    ctx.game = NULL;
    ctx.state = STATE_MENU;
    ctx.selected = 0;
    ctx.ui = (TableUi){ .cursor = 0, .standings = false };
    ctx.quit = false;
    sim_clock_reset(&ctx.clock);
    ctx.prev_time = 0.0;
#ifdef OR_AUTOPLAY
    // Validation builds skip the menu and start playing at boot (see SIMSTATS
    // in the Makefile); the AI fills every seat and keeps the simulation running.
    ctx.game = new_game(-1);
    ctx.state = STATE_PLAYING;
#endif

#ifdef PLATFORM_WEB
    // Browsers drive the loop via a per-frame callback; with the infinite-loop
    // flag this call does not return, so the native cleanup below never runs on
    // web (the browser tab owns the lifetime).
    emscripten_set_main_loop_arg(frame_step, &ctx, 0, 1);
#else
    while (!window_should_close() && !ctx.quit) {
        frame_step(&ctx);
    }
    recorder_stop(); // finalize the .mp4 if recording
    if (ctx.game) game_destroy(ctx.game);
    sound_shutdown();
    render_cleanup();
#endif
    return 0;
}

#endif // PLATFORM_IOS (main() is compiled out on iOS)
