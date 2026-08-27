#include "game.h"
#include "rules.h"
#include "render.h"
#include "input.h"
#include "sound.h"
#include "recorder.h"
#include "app.h"
#include "tick.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
#endif

#include "netgame.h"
#include "prefs.h"

typedef enum {
    STATE_MENU,
    STATE_OPTIONS,
    STATE_NAME,         // editing the persisted player name
    STATE_PLAYING,
    STATE_PAUSED,
    STATE_ONLINE_MENU,  // choose quick / create / join (net builds only)
    STATE_ONLINE_JOIN,  // entering a room code
    STATE_ONLINE,       // connected: lobby, waiting, or a live online match
} AppState;

// Menu actions. The set of items shown depends on whether a game is in
// progress (a resumable game adds "Resume Game").
typedef enum {
    ACT_RESUME,
    ACT_NEW,
    ACT_ONLINE,
    ACT_OPTIONS,
    ACT_SOUND,
    ACT_RECORD,
    ACT_EXIT,
} MenuAction;

// Upper bound on labels[]/actions[]: one slot per MenuAction (7). Each action
// appears at most once, so build_menu can never overflow.
#define MAX_MENU_ITEMS 7

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

// Build the current menu. Returns the item count; fills labels[] and actions[].
static int build_menu(bool resumable, const char* labels[], MenuAction actions[]) {
    int n = 0;
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
    // A browser tab can't be closed from code, so no Exit on web. (The renderer —
    // portrait touch vs desktop landscape — is auto-detected by pointer type.)
#elif !defined(PLATFORM_IOS) && !defined(PLATFORM_ANDROID)
    // Mobile apps don't self-terminate (the OS owns the lifecycle: home gesture /
    // back button on Android, Apple guidelines on iOS), so no Exit on either.
    labels[n] = "Exit"; actions[n++] = ACT_EXIT;
#endif
    return n;
}

// The Options screen items. Values cycle with Left/Right (or Select); the last
// item returns to the menu. Labels are rebuilt every frame from the live Rules.
#define OPT_ITEMS 7
enum { OPT_NAME, OPT_PLAYERS, OPT_DIFFICULTY, OPT_BONUS, OPT_PARTNERS, OPT_TARGET, OPT_BACK };

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
    int online_sel;    // cursor on the online submenu
    char join_code[8]; // code being entered on the Join screen
    int code_cursor;   // active slot on the Join screen (0..5)
    char name_buf[16]; // player name being edited (STATE_NAME)
    int name_cursor;   // active slot on the Name screen (0..NAME_MAX-1)
} AppCtx;

// Synthesize the acting player's Action for this frame from keyboard and
// touch, for the seat `seat` of game `g` with cursor state `ui`. Returns true
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

    // Touch: taps against the zones the renderer captured last frame.
    if (in->touch_tap) {
        int hit = render_table_hit_test((Vector2){in->tap_x, in->tap_y});
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

// --- Online submenu + status --------------------------------------------------
#define ONLINE_ITEMS 4
enum { ONL_QUICK = 0, ONL_CREATE, ONL_JOIN, ONL_BACK };
static const char* const ONLINE_LABELS[ONLINE_ITEMS] = {
    "Quick Match", "Create Table", "Join by Code", "Back"
};

// The daemon address. Defaults to the public deployment over wss:// so "Play
// Online" works out of the box; override with the environment to reach a local
// or self-hosted daemon (e.g. OPENRACKEM_SERVER=127.0.0.1 OPENRACKEM_PORT=8080,
// which auto-selects plain ws:// on a non-443 port; force with OPENRACKEM_TLS).
#define DEFAULT_SERVER "openrackem-server.fly.dev"
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
// slot picker.
static const char CODE_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

// Drive the online submenu one frame (plain navigation). Returns the chosen
// item (ONL_*) on select, or -1.
static int online_menu(AppCtx* c, const Input* in) {
    if (in->escape_pressed) return ONL_BACK;
    if (in->menu_up) {
        c->online_sel = (c->online_sel + ONLINE_ITEMS - 1) % ONLINE_ITEMS;
        sound_play(SFX_MENU_MOVE);
    }
    if (in->menu_down) {
        c->online_sel = (c->online_sel + 1) % ONLINE_ITEMS;
        sound_play(SFX_MENU_MOVE);
    }
    bool sel = in->select_pressed;
    if (in->touch_tap) {
        int hit = render_menu_hit_test((Vector2){in->tap_x, in->tap_y});
        if (hit >= 0 && hit < ONLINE_ITEMS) { c->online_sel = hit; sel = true; }
    }
    return sel ? c->online_sel : -1;
}

// The Join-by-code entry screen: a 6-slot code picked from the daemon's
// alphabet. Left/Right move the cursor, Up/Down cycle the letter, confirm
// joins. Returns 1 to join, -1 to go back, 0 to stay.
static int join_screen(AppCtx* c, const Input* in) {
    if (in->escape_pressed) return -1;
    if (c->join_code[0] == '\0') {
        for (int i = 0; i < 6; i++) c->join_code[i] = 'A';
        c->join_code[6] = '\0';
    }
    // Keyboard: type the code directly (case-insensitive, filtered to the
    // room-code alphabet); backspace steps back. Coexists with the slot picker.
    for (int i = 0; i < in->text_len; i++) {
        char ch = in->text_input[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (strchr(CODE_ALPHABET, ch)) {
            c->join_code[c->code_cursor] = ch;
            if (c->code_cursor < 5) c->code_cursor++;
            sound_play(SFX_MENU_MOVE);
        }
    }
    if (in->backspace_pressed) {
        if (c->code_cursor > 0) c->code_cursor--;
        c->join_code[c->code_cursor] = 'A';
    }
    if (in->menu_left)  c->code_cursor = (c->code_cursor + 5) % 6;
    if (in->menu_right) c->code_cursor = (c->code_cursor + 1) % 6;
    if (in->menu_up || in->menu_down) {
        const char* pos = strchr(CODE_ALPHABET, c->join_code[c->code_cursor]);
        int idx = pos ? (int)(pos - CODE_ALPHABET) : 0;
        int n = (int)(sizeof CODE_ALPHABET - 1);
        idx = (idx + (in->menu_up ? 1 : n - 1)) % n;
        c->join_code[c->code_cursor] = CODE_ALPHABET[idx];
        sound_play(SFX_MENU_MOVE);
    }
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
    c->name_cursor = 0;
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

// The Name screen: keyboard types alphanumerics into the slots (backspace
// deletes); touch swipes move the slot (left/right) and cycle the letter
// (up/down). Returns 1 to save+exit, -1 to cancel, 0 to stay.
static int name_screen(AppCtx* c, const Input* in) {
    if (in->escape_pressed) return -1;
    for (int i = 0; i < in->text_len; i++) {
        char ch = in->text_input[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (ch != ' ' && strchr(NAME_ALPHABET, ch)) {
            c->name_buf[c->name_cursor] = ch;
            if (c->name_cursor < NAME_MAX - 1) c->name_cursor++;
            sound_play(SFX_MENU_MOVE);
        }
    }
    if (in->backspace_pressed) {
        if (c->name_cursor > 0) c->name_cursor--;
        c->name_buf[c->name_cursor] = ' ';
    }
    if (in->menu_left)  c->name_cursor = (c->name_cursor + NAME_MAX - 1) % NAME_MAX;
    if (in->menu_right) c->name_cursor = (c->name_cursor + 1) % NAME_MAX;
    if (in->menu_up || in->menu_down) {
        const char* pos = strchr(NAME_ALPHABET, c->name_buf[c->name_cursor]);
        int idx = pos ? (int)(pos - NAME_ALPHABET) : 0;
        int n = (int)(sizeof NAME_ALPHABET - 1);
        idx = (idx + (in->menu_up ? 1 : n - 1)) % n;
        c->name_buf[c->name_cursor] = NAME_ALPHABET[idx];
        sound_play(SFX_MENU_MOVE);
    }
    if (in->confirm_pressed || in->touch_tap) return 1;
    return 0;
}

// Render the online submenu / status. `code_cursor` (0..5, -1 hidden) shows
// which Join slot is active.
static void render_online(const AppCtx* c) {
    const NetGame* ng = &c->net;
    if (c->state == STATE_ONLINE_MENU) {
        render_menu("PLAY ONLINE", ONLINE_LABELS, ONLINE_ITEMS, c->online_sel, ONL_BACK);
        return;
    }
    // STATE_ONLINE: playing, or a status card while connecting/waiting.
    if (ng->state == NG_PLAYING && ng->have_game) {
        render_frame(&ng->game, &c->ui);
        return;
    }
    char l0[48] = "", l1[48] = "";
    const char* title = "ONLINE";
    switch (ng->state) {
    case NG_CONNECTING: snprintf(l0, sizeof l0, "Connecting..."); break;
    case NG_LOBBY:      snprintf(l0, sizeof l0, "Connected"); break;
    case NG_QUEUED:
        snprintf(l0, sizeof l0, "Finding a match...");
        snprintf(l1, sizeof l1, "Queue position %d", ng->queue_pos);
        break;
    case NG_WAITING:
        title = "WAITING FOR PLAYERS";
        snprintf(l0, sizeof l0, "Room code: %s", ng->code);
        snprintf(l1, sizeof l1, "%d / %d joined", ng->joined, ng->players);
        break;
    case NG_DISCONNECTED: snprintf(l0, sizeof l0, "Reconnecting..."); break;
    case NG_ERROR:
        title = "DISCONNECTED";
        snprintf(l0, sizeof l0, "%s", ng->err[0] ? ng->err : "connection failed");
        snprintf(l1, sizeof l1, "Press any key");
        break;
    default: break;
    }
    const char* lines[2]; int n = 0;
    if (l0[0]) lines[n++] = l0;
    if (l1[0]) lines[n++] = l1;
    render_menu(title, lines, n, -1, -1);
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

    Input in = input_poll();
    if (in.fullscreen_toggle) render_toggle_fullscreen();

    bool resumable = (c->game != NULL && !game_is_over(c->game));
    const char* labels[MAX_MENU_ITEMS];
    MenuAction actions[MAX_MENU_ITEMS];
    int menu_count = build_menu(resumable, labels, actions);

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
        // Touch: a tap directly on a menu item selects it. Keyboard select
        // activates the highlighted item.
        bool do_select = in.select_pressed;
        if (in.touch_tap) {
            int hit = render_menu_hit_test((Vector2){in.tap_x, in.tap_y});
            if (hit >= 0 && hit < menu_count) { c->selected = hit; do_select = true; }
        }
        if (do_select) {
            switch (actions[c->selected]) {
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
                c->state = STATE_ONLINE_MENU;
                c->online_sel = 0;
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
        if (in.touch_tap) {
            int hit = render_menu_hit_test((Vector2){in.tap_x, in.tap_y});
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
        } else if (c->selected == OPT_NAME) {
            // Name row: left/right don't cycle a value; only select opens it.
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
            c->state = STATE_OPTIONS;
            sound_play(SFX_MENU_SELECT);
        } else if (res == -1) {
            c->state = STATE_OPTIONS;
        }
        break; // rendered by the per-state dispatch below
    }

    case STATE_PLAYING: {
#ifdef OR_TOUCH
        // Auto-pause when the app is backgrounded (Android) or the browser tab
        // loses focus (web), so the player returns paused, not mid-turn.
        if (!render_window_focused()) {
            c->state = STATE_PAUSED;
            sound_play(SFX_PAUSE);
            break;
        }
#endif
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
            if (!phase_flipped && (in.confirm_pressed || in.touch_tap)) {
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
            if (!phase_flipped && (in.confirm_pressed || in.touch_tap)) {
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

    case STATE_ONLINE_MENU: {
        // Choose how to get into a game. Quick Match needs no code; Create
        // hands back a code to share; Join enters one with the slot cursor.
        int oc = online_menu(c, &in);
        if (oc == ONL_BACK) { c->state = STATE_MENU; c->selected = 0; break; }
        if (oc == ONL_JOIN) {
            c->state = STATE_ONLINE_JOIN;
            c->code_cursor = 0;
            sound_play(SFX_MENU_SELECT);
        } else if (oc >= 0) {
            NgJoin j = (oc == ONL_CREATE) ? NG_JOIN_CREATE : NG_JOIN_QUICK;
            netgame_start(&c->net, server_host(), server_port(), server_tls(), j,
                          NULL, current_options(), prefs_name());
            c->ui = (TableUi){ .cursor = 0, .standings = false };
            c->state = STATE_ONLINE;
            sound_play(SFX_MENU_SELECT);
        }
        break; // rendered via the dispatch below
    }

    case STATE_ONLINE_JOIN: {
        int r = join_screen(c, &in);
        if (r < 0) { c->state = STATE_ONLINE_MENU; break; }
        if (r > 0) {
            netgame_start(&c->net, server_host(), server_port(), server_tls(),
                          NG_JOIN_CODE, c->join_code, current_options(), prefs_name());
            c->ui = (TableUi){ .cursor = 0, .standings = false };
            c->state = STATE_ONLINE;
            sound_play(SFX_MENU_SELECT);
        }
        break; // rendered via the dispatch below
    }

    case STATE_ONLINE: {
        unsigned ev = netgame_update(&c->net);
        NetGame* ng = &c->net;
        if (in.escape_pressed) {
            netgame_close(ng);
            c->state = STATE_MENU;
            c->selected = 0;
            break;
        }
        if (ng->state == NG_PLAYING) {
            play_event_sounds(&ng->game, ev);
            if (ng->game.phase == PHASE_ROUND_OVER) {
                // First confirm shows standings; the second sends `next`.
                if (in.confirm_pressed || in.touch_tap) {
                    if (!c->ui.standings) { c->ui.standings = true; sound_play(SFX_MENU_MOVE); }
                    else { netgame_confirm(ng); c->ui.standings = false; }
                }
            } else if (ng->game.phase == PHASE_MATCH_OVER) {
                if (in.confirm_pressed || in.touch_tap) {
                    netgame_close(ng);
                    c->state = STATE_MENU;
                    c->selected = 0;
                }
            } else {
                Action a;
                if (input_to_action(&ng->game, ng->my_seat, &c->ui, &in, &a)) {
                    netgame_action(ng, a);
                }
            }
        } else if (ng->state == NG_ERROR) {
            // Any key returns to the menu once the failure is shown.
            if (in.any_pressed && !in.fullscreen_toggle) { c->state = STATE_MENU; c->selected = 0; }
        }
        break;
    }
    }

    // Online opponents show their chosen names (server handles); offline seats
    // fall back to "CPU N".
    if (c->state == STATE_ONLINE && c->net.have_game)
        render_set_seat_labels(c->net.handles, c->net.players);
    else
        render_clear_seat_labels();

    // Render for the state we ended the frame in. Every state must be handled
    // here: a same-frame transition (menu -> options) otherwise falls into a
    // branch whose data doesn't exist yet (there is no game before New Game).
    if (c->state == STATE_MENU) {
        render_menu("OPENRACKEM", labels, menu_count, c->selected, menu_count - 1);
    } else if (c->state == STATE_OPTIONS) {
        const char* opt_labels[OPT_ITEMS];
        int opt_count = build_options(opt_labels);
        render_menu("OPTIONS", opt_labels, opt_count, c->selected, OPT_BACK);
    } else if (c->state == STATE_NAME) {
        // Show the name with the active slot bracketed, blanks as underscores.
        char shown[40] = {0};
        int p = 0;
        for (int i = 0; i < NAME_MAX && p < 36; i++) {
            char ch = (c->name_buf[i] == ' ') ? '_' : c->name_buf[i];
            if (i == c->name_cursor) p += snprintf(shown + p, sizeof shown - p, "[%c]", ch);
            else                     p += snprintf(shown + p, sizeof shown - p, "%c", ch);
        }
        const char* lines[] = { shown,
                                "Type your name, or swipe to edit",
                                "Enter / tap: save    Esc: cancel" };
        render_menu("YOUR NAME", lines, 3, -1, -1);
    } else if (c->state == STATE_PAUSED) {
        render_pause(c->game, &c->ui);
    } else if (c->state == STATE_ONLINE_MENU || c->state == STATE_ONLINE) {
        render_online(c);
    } else if (c->state == STATE_ONLINE_JOIN) {
        // Show the code with the active slot bracketed, e.g. "A B [C] D E F".
        char shown[24] = {0};
        int p = 0;
        for (int i = 0; i < 6 && p < 20; i++) {
            char ch = c->join_code[i] ? c->join_code[i] : 'A';
            if (i == c->code_cursor) p += snprintf(shown + p, sizeof shown - p, "[%c]", ch);
            else p += snprintf(shown + p, sizeof shown - p, " %c ", ch);
        }
        const char* lines[] = { shown, "Up/Down: letter   Left/Right: slot", "Enter: join" };
        render_menu("JOIN BY CODE", lines, 3, -1, -1);
    } else {
        render_frame(c->game, &c->ui);
    }
}

#if defined(PLATFORM_IOS)

// iOS: UIKit provides main() and the run loop, so the normal main() below is
// compiled out. The app shell (ios_main.mm) sets up the Metal layer, calls
// ob_app_init() once, then ob_app_frame() from a CADisplayLink each frame.
static AppCtx ios_ctx;

void ob_app_init(void) {
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

void ob_app_frame(void) { frame_step(&ios_ctx); }

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
    // Pick the renderer by the primary pointer: coarse (phone/tablet) -> portrait
    // touch layout; fine (desktop / 2-in-1 laptop) -> desktop landscape layout +
    // keyboard, matching the native desktop app. Only pointer:coarse is used —
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
    while (!render_window_should_close() && !ctx.quit) {
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
