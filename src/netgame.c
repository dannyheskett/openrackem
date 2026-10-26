// The online session (see netgame.h). Reconstructs the authoritative game
// from each `state` message into a redacted Game, so the existing renderers
// draw an online table with no changes, and keeps the room list and waiting
// room as plain data for the online screens. The only JSON this reads is our
// own server's output, but the reader stays defensive (bounded, no
// allocation, every index range-checked).
#include "netgame.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define REJOIN_FRAMES  120   // ~2 s between auto-rejoin attempts
#define PING_FRAMES    600   // ~10 s between keep-alive pings
#define NOTICE_FRAMES  240   // ~4 s for a transient notice
#define MSG_CAP        8192  // one inbound message (matches the server)

// --- Minimal JSON reads --------------------------------------------------------
// Messages are flat objects of scalars, int arrays, string arrays, and arrays
// of int/string arrays. These helpers pull exactly those shapes; anything
// unexpected yields a default, never a crash.
static const char* find_key(const char* j, const char* key) {
    char pat[24];
    int m = snprintf(pat, sizeof pat, "\"%s\":", key);
    if (m < 0 || (size_t)m >= sizeof pat) return NULL;
    const char* p = strstr(j, pat);
    return p ? p + m : NULL;   // just past the colon
}

static const char* js_skip_ws(const char* p) {
    while (*p == ' ') p++;
    return p;
}

static long js_int(const char* j, const char* key, long absent) {
    const char* p = find_key(j, key);
    if (!p) return absent;
    p = js_skip_ws(p);
    if (*p != '-' && (*p < '0' || *p > '9')) return absent;
    return strtol(p, NULL, 10);
}

static bool js_str(const char* j, const char* key, char* out, size_t cap) {
    const char* p = find_key(j, key);
    if (!p) return false;
    p = js_skip_ws(p);
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
    return true;
}

// Read an int array starting at `start` ('[') into out[0..max), returning the
// count; *endp (optional) receives the position just past the closing ']'.
static int js_ints_at(const char* start, int* out, int max, const char** endp) {
    const char* p = start;
    if (endp) *endp = p;
    if (*p != '[') return 0;
    p++;
    int n = 0;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            long v = strtol(p, (char**)&p, 10);
            if (n < max) out[n++] = (int)v;
        } else if (*p == '[' || *p == '"') {
            break;   // not an int array
        } else {
            p++;     // skip anything unexpected
        }
    }
    if (*p == ']') p++;
    if (endp) *endp = p;
    return n;
}

static int js_ints(const char* j, const char* key, int* out, int max) {
    const char* p = find_key(j, key);
    if (!p) return 0;
    return js_ints_at(js_skip_ws(p), out, max, NULL);
}

// A string array at `start` into out[max][NG_NAME]; same contract.
static int js_strs_at(const char* start, char out[][NG_NAME], int max, const char** endp) {
    const char* p = start;
    if (endp) *endp = p;
    if (*p != '[') return 0;
    p++;
    int n = 0;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '"') break;
        p++;
        char tmp[NG_NAME];
        size_t k = 0;
        while (*p && *p != '"') {
            if (k + 1 < sizeof tmp) tmp[k++] = *p;
            p++;
        }
        tmp[k] = '\0';
        if (*p == '"') p++;
        if (n < max) memcpy(out[n++], tmp, NG_NAME);
    }
    if (*p == ']') p++;
    if (endp) *endp = p;
    return n;
}

static int js_strs(const char* j, const char* key, char out[][NG_NAME], int max) {
    const char* p = find_key(j, key);
    if (!p) return 0;
    return js_strs_at(js_skip_ws(p), out, max, NULL);
}

// The position of the first inner array of "key":[[...],[...]], or NULL.
static const char* js_outer(const char* j, const char* key) {
    const char* p = find_key(j, key);
    if (!p) return NULL;
    p = js_skip_ws(p);
    return (*p == '[') ? p + 1 : NULL;
}

static const char* next_inner(const char* p) {
    while (*p == ' ' || *p == ',') p++;
    return (*p == '[') ? p : NULL;
}

static void set_notice(NetGame* ng, const char* what) {
    snprintf(ng->notice, sizeof ng->notice, "%s", what);
    ng->notice_frames = NOTICE_FRAMES;
}

// --- State reconstruction ----------------------------------------------------
bool netgame_parse_state(NetGame* ng, const char* json, size_t len) {
    (void)len;
    if (js_int(json, "v", -1) != 2) return false;

    int players = (int)js_int(json, "players", 0);
    if (players < 2 || players > MAX_PLAYERS) return false;
    int seat = (int)js_int(json, "seat", -1);
    if (seat < 0 || seat >= players) return false;
    int phase = (int)js_int(json, "phase", -1);
    if (phase < PHASE_DEAL || phase > PHASE_MATCH_OVER) return false;

    // Seat indices from an untrusted server MUST be range-checked before the
    // renderers use them: seat_name() indexes a static buf[MAX_PLAYERS][8], so
    // a hostile turn/dealer/winner of e.g. 200 would be an out-of-bounds write.
    // turn/dealer must be a real seat; the winners may also be NO_WINNER.
    long turn = js_int(json, "turn", 0);
    long dealer = js_int(json, "dealer", 0);
    long winner = js_int(json, "winner", NO_WINNER);
    long mwin = js_int(json, "match_winner", NO_WINNER);
    if (turn < 0 || turn >= players) return false;
    if (dealer < 0 || dealer >= players) return false;
    if (winner != NO_WINNER && (winner < 0 || winner >= players)) return false;
    if (mwin != NO_WINNER && (mwin < 0 || mwin >= players)) return false;

    Game* g = &ng->game;
    memset(g, 0, sizeof *g);
    ng->players = players;
    ng->my_seat = seat;

    g->rules.player_count = players;
    g->rules.human_seat = seat;   // "YOU" is our seat in the renderer
    g->rules.target_score = (int)js_int(json, "target", 500);
    g->rules.bonus_scoring = js_int(json, "bonus", 0) != 0;
    g->rules.partners = js_int(json, "partners", 0) != 0;
    g->phase = (uint8_t)phase;
    g->turn = (uint8_t)turn;
    g->dealer = (uint8_t)dealer;
    g->round_winner = (uint8_t)winner;
    g->match_winner = (uint8_t)mwin;

    g->round_no = (uint8_t)js_int(json, "round", 1);
    g->stock_count = (uint8_t)js_int(json, "stock", 0);
    g->held_card = (uint8_t)js_int(json, "held", 0);
    g->held_from_discard = js_int(json, "hfd", 0) != 0;
    g->recycles = (uint8_t)js_int(json, "recycles", 0);
    g->events = (unsigned)js_int(json, "events", 0);

    int discard[MAX_CARDS];
    int dn = js_ints(json, "discard", discard, MAX_CARDS);
    g->discard_count = (uint8_t)dn;
    for (int i = 0; i < dn; i++) g->discard[i] = (uint8_t)discard[i];

    int rack[RACK_SLOTS];
    int rn = js_ints(json, "rack", rack, RACK_SLOTS);
    for (int i = 0; i < rn; i++) g->players[seat].rack.slots[i] = (uint8_t)rack[i];

    // At the reveal every rack is present as "racks":[[...],...].
    const char* rp = js_outer(json, "racks");
    for (int p = 0; rp && p < players; p++) {
        rp = next_inner(rp);
        if (!rp) break;
        int one[RACK_SLOTS];
        int on = js_ints_at(rp, one, RACK_SLOTS, &rp);
        // Only copy the slots actually present; a short subarray from a
        // buggy/hostile server otherwise leaks uninitialized stack.
        for (int i = 0; i < RACK_SLOTS; i++) {
            g->players[p].rack.slots[i] = (i < on) ? (uint8_t)one[i] : 0;
        }
    }

    int vals[MAX_PLAYERS];
    int sn = js_ints(json, "scores", vals, MAX_PLAYERS);
    for (int i = 0; i < sn; i++) g->players[i].score = (uint16_t)vals[i];
    int pn = js_ints(json, "round_points", vals, MAX_PLAYERS);
    for (int i = 0; i < pn; i++) g->round_points[i] = (uint16_t)vals[i];
    int tn = js_ints(json, "last_taken", vals, MAX_PLAYERS);
    for (int i = 0; i < tn; i++) g->last_taken[i] = (uint8_t)vals[i];

    int cn = js_ints(json, "ctl", vals, MAX_PLAYERS);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        ng->ctl[i] = (i < cn && vals[i]) ? 1 : 0;
        g->players[i].is_ai = ng->ctl[i] != 0;
    }

    char hs[MAX_PLAYERS][NG_NAME];
    int hn = js_strs(json, "handles", hs, MAX_PLAYERS);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i < hn) memcpy(ng->handles[i], hs[i], NG_NAME);   // NUL-terminated by js_strs
        else ng->handles[i][0] = '\0';
    }

    ng->deadline_ms = (int)js_int(json, "deadline_ms", 0);
    long ws = js_int(json, "wait_seat", -1);
    ng->wait_seat = (ws >= 0 && ws < players) ? (int)ws : -1;
    ng->wait_ms = (int)js_int(json, "wait_ms", 0);
    ng->wait_timer = js_int(json, "wait_timer", 0) != 0;

    // The last action drives the client-side flight tween: set up game.anim so
    // the existing renderers animate the move. `card` is what an observer saw
    // (0 for a face-down stock draw). The frame loop decrements anim.frames.
    const char* la = find_key(json, "last");
    if (la) {
        int lseat = (int)js_int(la, "seat", -1);
        int la_type = (int)js_int(la, "a", -1);
        int la_slot = (int)js_int(la, "slot", 0);
        int la_card = (int)js_int(la, "card", 0);
        if (lseat >= 0 && lseat < players && la_slot >= 0 && la_slot < RACK_SLOTS) {
            g->anim.seat = (uint8_t)lseat;
            g->anim.slot = (uint8_t)la_slot;
            g->anim.card = (uint8_t)la_card;
            g->anim.frames = g->anim.total = 10;
            switch (la_type) {
            case ACTION_DRAW_STOCK:   g->anim.kind = ANIM_DRAW_STOCK; break;
            case ACTION_DRAW_DISCARD: g->anim.kind = ANIM_DRAW_DISCARD; break;
            case ACTION_PLACE:        g->anim.kind = ANIM_PLACE; break;
            case ACTION_DISCARD:      g->anim.kind = ANIM_DISCARD; break;
            default:                  g->anim.kind = ANIM_NONE; break;
            }
        }
    }

    ng->have_game = true;
    return true;
}

bool netgame_parse_lobby(NetGame* ng, const char* json) {
    if (js_int(json, "v", -1) != 2) return false;
    NgLobby* L = &ng->lobby;
    NgLobby nl;
    memset(&nl, 0, sizeof nl);
    nl.id = js_int(json, "id", -1);
    js_str(json, "code", nl.code, sizeof nl.code);
    nl.priv = js_int(json, "private", 0) != 0;
    nl.target = (int)js_int(json, "target", 500);
    nl.bonus = js_int(json, "bonus", 0) != 0;
    nl.partners = js_int(json, "partners", 0) != 0;
    nl.away_policy = js_int(json, "away", 0) ? 1 : 0;
    long me = js_int(json, "me", -1);
    if (me < 0 || me >= MAX_PLAYERS) return false;
    nl.me = (int)me;
    js_strs(json, "names", nl.names, MAX_PLAYERS);
    int v[MAX_PLAYERS];
    int n = js_ints(json, "used", v, MAX_PLAYERS);
    for (int i = 0; i < n; i++) nl.used[i] = v[i] != 0;
    n = js_ints(json, "ready", v, MAX_PLAYERS);
    for (int i = 0; i < n; i++) nl.ready[i] = v[i] != 0;
    n = js_ints(json, "team", v, MAX_PLAYERS);
    for (int i = 0; i < n; i++) nl.team[i] = v[i] ? 1 : 0;
    n = js_ints(json, "on", v, MAX_PLAYERS);
    for (int i = 0; i < n; i++) nl.on[i] = v[i] != 0;
    nl.count_ms = (int)js_int(json, "count_ms", -1);
    js_str(json, "block", nl.block, sizeof nl.block);
    nl.can_force = js_int(json, "force", 0) != 0;
    long sf = js_int(json, "swap_from", -1);
    nl.swap_from = (sf >= 0 && sf < MAX_PLAYERS && nl.used[sf]) ? (int)sf : -1;
    *L = nl;
    snprintf(ng->code, sizeof ng->code, "%s", nl.code);
    ng->room_id = nl.id;
    ng->have_lobby = true;
    return true;
}

bool netgame_parse_rooms(NetGame* ng, const char* json) {
    int ids[NG_LIST_MAX], tg[NG_LIST_MAX], bo[NG_LIST_MAX], pa[NG_LIST_MAX];
    int n = js_ints(json, "ids", ids, NG_LIST_MAX);
    js_ints(json, "targets", tg, NG_LIST_MAX);
    int nb = js_ints(json, "bonus", bo, NG_LIST_MAX);
    int np = js_ints(json, "partners", pa, NG_LIST_MAX);
    memset(ng->rooms, 0, sizeof ng->rooms);
    const char* np_ = js_outer(json, "names");
    const char* rp_ = js_outer(json, "ready");
    for (int i = 0; i < n; i++) {
        NgRoomRow* row = &ng->rooms[i];
        row->id = ids[i];
        row->target = tg[i];
        row->bonus = i < nb && bo[i];
        row->partners = i < np && pa[i];
        if (np_ && (np_ = next_inner(np_)) != NULL) {
            row->n = js_strs_at(np_, row->names, MAX_PLAYERS, &np_);
        }
        if (rp_ && (rp_ = next_inner(rp_)) != NULL) {
            int r[MAX_PLAYERS];
            int k = js_ints_at(rp_, r, MAX_PLAYERS, &rp_);
            for (int m = 0; m < k; m++) row->ready[m] = r[m] != 0;
        }
    }
    ng->room_count = n;
    ng->rooms_total = (int)js_int(json, "total", n);
    if (ng->rooms_total < n) ng->rooms_total = n;
    ng->have_rooms = true;
    return true;
}

static void parse_peek(NetGame* ng, const char* json) {
    NgPeek pk;
    memset(&pk, 0, sizeof pk);
    js_str(json, "code", pk.code, sizeof pk.code);
    pk.known = true;
    pk.ok = js_int(json, "ok", 0) != 0;
    pk.playing = js_int(json, "playing", 0) != 0;
    pk.target = (int)js_int(json, "target", 500);
    char nm[MAX_PLAYERS][NG_NAME];
    int n = js_strs(json, "names", nm, MAX_PLAYERS);
    int rd[MAX_PLAYERS];
    int k = js_ints(json, "ready", rd, MAX_PLAYERS);
    for (int i = 0; i < n; i++) {
        if (!nm[i][0]) continue;   // empty seat
        memcpy(pk.names[pk.n], nm[i], NG_NAME);
        pk.ready[pk.n] = i < k && rd[i];
        pk.n++;
    }
    ng->peek = pk;
}

// --- Protocol send helpers ---------------------------------------------------
static bool send_json(NetGame* ng, const char* json) {
    return ng->conn && net_send(ng->conn, json, strlen(json));
}

static void sendf(NetGame* ng, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static void sendf(NetGame* ng, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof buf) send_json(ng, buf);
}

// Codes and keys from prefs are user-editable: keep them wire-safe.
static void copy_ident(char* out, size_t cap, const char* in) {
    size_t j = 0;
    if (in) for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        char ch = in[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
            out[j++] = ch;
    }
    out[j] = '\0';
}

// The ",\"key\":\"...\"" suffix that surrenders a seat held elsewhere.
static void stale_suffix(NetGame* ng, char* out, size_t cap) {
    out[0] = '\0';
    if (ng->stale_key[0]) snprintf(out, cap, ",\"key\":\"%s\"", ng->stale_key);
}

static void open_conn(NetGame* ng) {
    ng->conn = net_connect(ng->host, ng->port, "/", ng->tls);
    ng->greeted = false;
    if (!ng->conn) {
        ng->state = NG_ERROR;
        snprintf(ng->err, sizeof ng->err, "no connection");
    } else {
        ng->state = NG_CONNECTING;
    }
}

void netgame_start(NetGame* ng, const char* host, int port, bool tls,
                   const char* name, const char* rejoin_key) {
    memset(ng, 0, sizeof *ng);
    ng->my_seat = -1;
    ng->wait_seat = -1;
    ng->lobby.swap_from = -1;
    copy_ident(ng->name, sizeof ng->name, name);
    snprintf(ng->host, sizeof ng->host, "%s", host ? host : "127.0.0.1");
    ng->port = port;
    ng->tls = tls;
    copy_ident(ng->key, sizeof ng->key, rejoin_key);
    ng->rejoining = ng->key[0] != '\0';
    ng->ping_frames = PING_FRAMES;
    open_conn(ng);
}

// Leaving a room by any path: back to browsing, with nothing to rejoin.
static void to_idle(NetGame* ng) {
    ng->state = NG_IDLE;
    ng->in_room = false;
    ng->key[0] = '\0';
    ng->owner[0] = '\0';
    ng->code[0] = '\0';
    ng->have_lobby = false;
    ng->have_game = false;
    ng->result_pending = false;
    ng->pending = false;
    if (ng->listing) netgame_list(ng, ng->list_target);   // resubscribe
}

static void handle_message(NetGame* ng, const char* msg, size_t n, unsigned* events) {
    char type[16];
    if (!js_str(msg, "t", type, sizeof type)) return;

    if (strcmp(type, "hi") == 0) {
        if (ng->rejoining && ng->key[0]) {
            sendf(ng, "{\"t\":\"rejoin\",\"key\":\"%s\"}", ng->key);
        } else {
            ng->state = NG_IDLE;
            if (ng->listing) netgame_list(ng, ng->list_target);
        }
        ng->rejoining = false;
    } else if (strcmp(type, "welcome") == 0) {
        js_str(msg, "key", ng->key, sizeof ng->key);
        char owner[NG_KEY];
        if (js_str(msg, "owner", owner, sizeof owner)) memcpy(ng->owner, owner, NG_KEY);
        js_str(msg, "code", ng->code, sizeof ng->code);
        ng->room_id = js_int(msg, "id", -1);
        ng->in_room = true;
        ng->stale_key[0] = '\0';   // surrendered (or reclaimed) by now
        ng->listing = false;
        ng->state = js_int(msg, "playing", 0) ? NG_PLAYING : NG_LOBBY;
    } else if (strcmp(type, "lobby") == 0) {
        if (netgame_parse_lobby(ng, msg)) {
            // The match just ended: keep the result on screen until dismissed.
            if (ng->state == NG_PLAYING && ng->have_game &&
                ng->game.phase == PHASE_MATCH_OVER) {
                ng->result_pending = true;
            }
            ng->state = NG_LOBBY;
            ng->pending = false;
        }
    } else if (strcmp(type, "state") == 0) {
        if (netgame_parse_state(ng, msg, n)) {
            ng->state = NG_PLAYING;
            ng->result_pending = false;
            if (ng->game.phase != PHASE_ROUND_OVER) ng->confirmed_local = false;
            ng->pending = false;   // the authoritative result landed
            *events = ng->game.events;
        }
    } else if (strcmp(type, "rooms") == 0) {
        netgame_parse_rooms(ng, msg);
    } else if (strcmp(type, "peeked") == 0) {
        parse_peek(ng, msg);
    } else if (strcmp(type, "back") == 0) {
        js_str(msg, "name", ng->back_name, sizeof ng->back_name);
        ng->back_frames = NOTICE_FRAMES;
    } else if (strcmp(type, "removed") == 0) {
        char why[16] = {0};
        js_str(msg, "why", why, sizeof why);
        set_notice(ng, why[0] ? why : "removed");
        to_idle(ng);
    } else if (strcmp(type, "error") == 0) {
        char code[16] = {0};
        js_str(msg, "code", code, sizeof code);
        ng->pending = false;       // a rejection frees the next attempt
        if (strcmp(code, "upgrade") == 0 || strcmp(code, "need_name") == 0 ||
            strcmp(code, "bad_name") == 0) {
            ng->state = NG_ERROR;
            snprintf(ng->err, sizeof ng->err, "%s", code);
        } else if (strcmp(code, "no_seat") == 0) {
            // The seat is gone (match over, forfeited, room expired).
            set_notice(ng, code);
            to_idle(ng);
        } else if (strcmp(code, "not_your_turn") != 0 && strcmp(code, "illegal") != 0) {
            set_notice(ng, code);  // no_room, full, started, rate, team_full...
        }
    }
}

unsigned netgame_update(NetGame* ng) {
    unsigned events = 0;
    if (ng->notice_frames > 0 && --ng->notice_frames == 0) ng->notice[0] = '\0';
    if (ng->back_frames > 0) ng->back_frames--;

    // Auto-rejoin: after a drop with a member key, wait a moment, then dial
    // back and reclaim the seat.
    if (ng->state == NG_DISCONNECTED && !ng->conn) {
        if (ng->reconnect_delay > 0) { ng->reconnect_delay--; return 0; }
        ng->rejoining = true;
        open_conn(ng);
        if (!ng->conn) snprintf(ng->err, sizeof ng->err, "reconnect failed");
        return 0;
    }
    if (!ng->conn) return 0;

    // Local flight tween runs down between authoritative states.
    if (ng->game.anim.frames > 0) {
        ng->game.anim.frames--;
        if (ng->game.anim.frames == 0) ng->game.anim.kind = ANIM_NONE;
    }

    net_poll(ng->conn);
    NetStatus ns = net_status(ng->conn);

    if (ns == NET_ERROR || ns == NET_CLOSED) {
        // Drop into reconnect if we hold a seat to reclaim, else fail.
        net_close(ng->conn);
        ng->conn = NULL;
        if (ng->key[0]) {
            ng->state = NG_DISCONNECTED;
            ng->reconnect_delay = REJOIN_FRAMES;
        } else {
            ng->state = NG_ERROR;
            snprintf(ng->err, sizeof ng->err, "connection lost");
        }
        return 0;
    }
    if (ns != NET_OPEN) return 0;   // still handshaking

    // First frame after the socket opens: say hello with our name.
    if (!ng->greeted) {
        sendf(ng, "{\"t\":\"hello\",\"v\":2,\"name\":\"%s\"}", ng->name);
        ng->greeted = true;
    }
    if (--ng->ping_frames <= 0) {
        send_json(ng, "{\"t\":\"ping\"}");
        ng->ping_frames = PING_FRAMES;
    }

    static char msg[MSG_CAP];
    int n;
    while (ng->conn && (n = net_recv(ng->conn, msg, sizeof msg)) >= 0) {
        handle_message(ng, msg, (size_t)n, &events);
    }
    return events;
}

void netgame_elapse(NetGame* ng, int ms) {
    if (ms <= 0) return;
    if (ng->deadline_ms > 0) ng->deadline_ms = ng->deadline_ms > ms ? ng->deadline_ms - ms : 0;
    if (ng->wait_timer && ng->wait_ms > 0) ng->wait_ms = ng->wait_ms > ms ? ng->wait_ms - ms : 0;
    if (ng->lobby.count_ms > 0) ng->lobby.count_ms = ng->lobby.count_ms > ms ? ng->lobby.count_ms - ms : 0;
}

// --- Browsing -------------------------------------------------------------------
void netgame_list(NetGame* ng, int target) {
    ng->listing = true;
    ng->list_target = target < 0 ? 0 : target;
    if (ng->state == NG_IDLE) sendf(ng, "{\"t\":\"list\",\"target\":%d}", ng->list_target);
}

void netgame_shuffle(NetGame* ng) {
    if (ng->state == NG_IDLE) sendf(ng, "{\"t\":\"shuffle\",\"target\":%d}", ng->list_target);
}

void netgame_unlist(NetGame* ng) {
    if (ng->listing && ng->state == NG_IDLE) send_json(ng, "{\"t\":\"unlist\"}");
    ng->listing = false;
}

void netgame_peek(NetGame* ng, const char* code) {
    char c[NG_CODE];
    copy_ident(c, sizeof c, code);
    if (!c[0] || ng->state != NG_IDLE) return;
    memset(&ng->peek, 0, sizeof ng->peek);
    snprintf(ng->peek.code, sizeof ng->peek.code, "%s", c);
    sendf(ng, "{\"t\":\"peek\",\"code\":\"%s\"}", c);
}

void netgame_create(NetGame* ng, bool priv, const Rules* rules, bool wait_policy) {
    if (ng->state != NG_IDLE) return;
    Rules r = rules ? *rules : rules_default();
    char sk[64];
    stale_suffix(ng, sk, sizeof sk);
    sendf(ng, "{\"t\":\"create\",\"private\":%d,\"target\":%d,\"bonus\":%d,"
              "\"partners\":%d,\"away\":%d%s}",
          priv ? 1 : 0, r.target_score, r.bonus_scoring ? 1 : 0,
          r.partners ? 1 : 0, wait_policy ? 1 : 0, sk);
}

void netgame_join_room(NetGame* ng, long id) {
    if (ng->state != NG_IDLE) return;
    char sk[64];
    stale_suffix(ng, sk, sizeof sk);
    sendf(ng, "{\"t\":\"join\",\"room\":%ld%s}", id, sk);
}

void netgame_join_code(NetGame* ng, const char* code) {
    if (ng->state != NG_IDLE) return;
    char c[NG_CODE], sk[64];
    copy_ident(c, sizeof c, code);
    stale_suffix(ng, sk, sizeof sk);
    sendf(ng, "{\"t\":\"join\",\"code\":\"%s\"%s}", c, sk);
}

void netgame_best(NetGame* ng, int target) {
    if (ng->state != NG_IDLE) return;
    char sk[64];
    stale_suffix(ng, sk, sizeof sk);
    sendf(ng, "{\"t\":\"best\",\"target\":%d%s}", target < 0 ? 0 : target, sk);
}

void netgame_rejoin(NetGame* ng, const char* key) {
    if (ng->state != NG_IDLE) return;
    copy_ident(ng->key, sizeof ng->key, key);
    if (ng->key[0]) sendf(ng, "{\"t\":\"rejoin\",\"key\":\"%s\"}", ng->key);
}

// --- Waiting room ---------------------------------------------------------------
void netgame_ready(NetGame* ng, bool on) {
    if (ng->state == NG_LOBBY) sendf(ng, "{\"t\":\"ready\",\"on\":%d}", on ? 1 : 0);
}

void netgame_team(NetGame* ng, int side) {
    if (ng->state == NG_LOBBY) sendf(ng, "{\"t\":\"team\",\"side\":%d}", side ? 1 : 0);
}

void netgame_swap(NetGame* ng, int member) {
    if (ng->state == NG_LOBBY && member >= 0 && member < MAX_PLAYERS)
        sendf(ng, "{\"t\":\"swap\",\"seat\":%d}", member);
}

void netgame_swap_answer(NetGame* ng, int from, bool accept) {
    if (ng->state == NG_LOBBY && from >= 0 && from < MAX_PLAYERS)
        sendf(ng, "{\"t\":\"%s\",\"seat\":%d}", accept ? "swap_ok" : "swap_no", from);
}

void netgame_force(NetGame* ng) {
    if (ng->state == NG_LOBBY) send_json(ng, "{\"t\":\"force\"}");
}

void netgame_settings(NetGame* ng, int target, bool bonus, bool partners,
                      bool priv, bool wait_policy) {
    if (ng->state != NG_LOBBY || !ng->owner[0]) return;
    sendf(ng, "{\"t\":\"settings\",\"owner\":\"%s\",\"target\":%d,\"bonus\":%d,"
              "\"partners\":%d,\"private\":%d,\"away\":%d}",
          ng->owner, target, bonus ? 1 : 0, partners ? 1 : 0, priv ? 1 : 0,
          wait_policy ? 1 : 0);
}

// --- Leaving and returning --------------------------------------------------------
void netgame_leave(NetGame* ng) {
    if (ng->state == NG_LOBBY) {
        send_json(ng, "{\"t\":\"leave\"}");
        to_idle(ng);
    } else if (ng->state == NG_PLAYING) {
        send_json(ng, "{\"t\":\"leave\"}");   // step away; the seat is kept
    }
}

void netgame_here(NetGame* ng) {
    if (ng->state == NG_PLAYING) send_json(ng, "{\"t\":\"here\"}");
}

void netgame_forfeit(NetGame* ng) {
    if (ng->state != NG_PLAYING) return;
    send_json(ng, "{\"t\":\"forfeit\"}");
    to_idle(ng);
}

void netgame_letai(NetGame* ng, int seat) {
    if (ng->state == NG_PLAYING && seat >= 0 && seat < MAX_PLAYERS)
        sendf(ng, "{\"t\":\"letai\",\"seat\":%d}", seat);
}

// --- Playing ------------------------------------------------------------------------
bool netgame_my_turn(const NetGame* ng) {
    if (ng->state != NG_PLAYING || !ng->have_game || ng->result_pending) return false;
    return ng->game.turn == ng->my_seat &&
           (ng->game.phase == PHASE_DRAW || ng->game.phase == PHASE_PLACE);
}

void netgame_action(NetGame* ng, Action a) {
    if (!netgame_my_turn(ng) || ng->pending) return;
    char buf[64];
    snprintf(buf, sizeof buf, "{\"t\":\"action\",\"a\":%d,\"slot\":%d}", a.type, a.slot);
    if (send_json(ng, buf)) ng->pending = true;
}

void netgame_confirm(NetGame* ng) {
    if (ng->state != NG_PLAYING || ng->game.phase != PHASE_ROUND_OVER) return;
    if (ng->confirmed_local) return;
    ng->confirmed_local = true;
    send_json(ng, "{\"t\":\"next\"}");
}

void netgame_dismiss_result(NetGame* ng) {
    ng->result_pending = false;
    ng->have_game = false;
}

void netgame_close(NetGame* ng) {
    if (ng->conn) {
        if (ng->state == NG_PLAYING) send_json(ng, "{\"t\":\"leave\"}");
        net_poll(ng->conn);   // give the step-away a chance to go out
        net_close(ng->conn);
        ng->conn = NULL;
    }
    ng->state = NG_ERROR;
    snprintf(ng->err, sizeof ng->err, "closed");
}
