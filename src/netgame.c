// The online session (see netgame.h). Reconstructs the authoritative game
// from each `state` message into a redacted Game, so the existing renderers
// draw an online table with no changes. The only JSON this reads is our own
// server's output, but the reader stays defensive (bounded, no allocation).
#include "netgame.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define REJOIN_FRAMES 120   // ~2 s between auto-rejoin attempts

// --- Minimal JSON reads (scalars + int arrays by key) -----------------------
// The state message is a flat object of scalars, int arrays, and one array of
// int arrays ("racks"). These helpers pull exactly those shapes; anything
// unexpected yields a default, never a crash.
static const char* find_key(const char* j, const char* key) {
    char pat[24];
    int m = snprintf(pat, sizeof pat, "\"%s\":", key);
    if (m < 0 || (size_t)m >= sizeof pat) return NULL;
    return strstr(j, pat);
}

static long js_int(const char* j, const char* key, long absent) {
    const char* p = find_key(j, key);
    if (!p) return absent;
    p += strlen(key) + 3;
    while (*p == ' ') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return absent;
    return strtol(p, NULL, 10);
}

static bool js_str(const char* j, const char* key, char* out, size_t cap) {
    const char* p = find_key(j, key);
    if (!p) return false;
    p += strlen(key) + 3;
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
    return true;
}

// Read the int array named `key` into out[0..max), returning the count. `*endp`
// (optional) receives the position just past the closing ']', so the caller
// can walk repeated arrays.
static int js_array_at(const char* start, int* out, int max) {
    const char* p = start;
    if (*p != '[') return 0;
    p++;
    int n = 0;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            long v = strtol(p, (char**)&p, 10);
            if (n < max) out[n++] = (int)v;
        } else {
            p++;   // skip anything unexpected
        }
    }
    return n;
}

static int js_array(const char* j, const char* key, int* out, int max) {
    const char* p = find_key(j, key);
    if (!p) return 0;
    p += strlen(key) + 3;
    while (*p == ' ') p++;
    return js_array_at(p, out, max);
}

// --- State reconstruction ----------------------------------------------------
bool netgame_parse_state(NetGame* ng, const char* json, size_t len) {
    (void)len;
    if (js_int(json, "v", -1) != 1) return false;

    Game* g = &ng->game;
    memset(g, 0, sizeof *g);

    int players = (int)js_int(json, "players", 4);
    if (players < 2 || players > MAX_PLAYERS) return false;
    ng->players = players;
    ng->my_seat = (int)js_int(json, "seat", 0);
    if (ng->my_seat < 0 || ng->my_seat >= players) return false;

    g->rules.player_count = players;
    g->rules.human_seat = ng->my_seat;   // "YOU" is our seat in the renderer
    g->rules.target_score = (int)js_int(json, "target", 500);
    g->rules.bonus_scoring = js_int(json, "bonus", 0) != 0;
    g->rules.partners = js_int(json, "partners", 0) != 0;
    g->phase = (uint8_t)js_int(json, "phase", PHASE_DRAW);
    if (g->phase > PHASE_MATCH_OVER) return false;

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
    int dn = js_array(json, "discard", discard, MAX_CARDS);
    if (dn > MAX_CARDS) dn = MAX_CARDS;
    g->discard_count = (uint8_t)dn;
    for (int i = 0; i < dn; i++) g->discard[i] = (uint8_t)discard[i];

    int rack[RACK_SLOTS];
    int rn = js_array(json, "rack", rack, RACK_SLOTS);
    for (int i = 0; i < rn && i < RACK_SLOTS; i++) {
        g->players[ng->my_seat].rack.slots[i] = (uint8_t)rack[i];
    }

    // At the reveal every rack is present as "racks":[[...],...].
    const char* rp = find_key(json, "racks");
    if (rp) {
        rp += strlen("racks") + 3;
        while (*rp == ' ') rp++;
        if (*rp == '[') {
            rp++;   // into the outer array
            for (int p = 0; p < players && *rp && *rp != ']'; p++) {
                while (*rp == ' ' || *rp == ',') rp++;
                if (*rp != '[') break;
                int one[RACK_SLOTS];
                int on = js_array_at(rp, one, RACK_SLOTS);
                // Only copy the slots actually present; a short subarray from a
                // buggy/hostile server otherwise leaks uninitialized stack.
                for (int i = 0; i < RACK_SLOTS; i++) {
                    g->players[p].rack.slots[i] = (i < on) ? (uint8_t)one[i] : 0;
                }
                // Skip to the end of this subarray.
                int depth = 0;
                do { if (*rp == '[') depth++; else if (*rp == ']') depth--; rp++; }
                while (*rp && depth > 0);
            }
        }
    }

    int scores[MAX_PLAYERS];
    int sn = js_array(json, "scores", scores, MAX_PLAYERS);
    for (int i = 0; i < sn && i < MAX_PLAYERS; i++) {
        g->players[i].score = (uint16_t)scores[i];
    }
    int rpts[MAX_PLAYERS];
    int pn = js_array(json, "round_points", rpts, MAX_PLAYERS);
    for (int i = 0; i < pn && i < MAX_PLAYERS; i++) {
        g->round_points[i] = (uint16_t)rpts[i];
    }
    int taken[MAX_PLAYERS];
    int tn = js_array(json, "last_taken", taken, MAX_PLAYERS);
    for (int i = 0; i < tn && i < MAX_PLAYERS; i++) {
        g->last_taken[i] = (uint8_t)taken[i];
    }

    int aiflags[MAX_PLAYERS];
    int an = js_array(json, "ai", aiflags, MAX_PLAYERS);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        ng->ai[i] = (i < an) ? (uint8_t)aiflags[i] : 0;
        g->players[i].is_ai = (i < an) ? (aiflags[i] != 0) : false;
    }

    ng->deadline_ms = (int)js_int(json, "deadline_ms", 0);

    // The last action drives the client-side flight tween: set up game.anim so
    // the existing renderers animate the move. `card` is what an observer saw
    // (0 for a face-down stock draw). The frame loop decrements anim.frames.
    const char* la = find_key(json, "last");
    if (la) {
        int lseat = (int)js_int(la, "seat", -1);
        int la_type = (int)js_int(la, "a", -1);
        int la_slot = (int)js_int(la, "slot", 0);
        int la_card = (int)js_int(la, "card", 0);
        if (lseat >= 0) {
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

// --- Protocol send helpers ---------------------------------------------------
static void send_json(NetGame* ng, const char* json) {
    if (ng->conn) net_send(ng->conn, json, strlen(json));
}

static void send_join_intent(NetGame* ng) {
    char buf[160];
    switch (ng->join) {
    case NG_JOIN_QUICK:
        snprintf(buf, sizeof buf, "{\"t\":\"quick\",\"name\":\"%s\"}", ng->name);
        send_json(ng, buf);
        break;
    case NG_JOIN_CREATE:
        snprintf(buf, sizeof buf,
                 "{\"t\":\"create\",\"players\":%d,\"bonus\":%d,\"partners\":%d,"
                 "\"target\":%d,\"name\":\"%s\"}",
                 ng->rules.player_count, ng->rules.bonus_scoring ? 1 : 0,
                 ng->rules.partners ? 1 : 0, ng->rules.target_score, ng->name);
        send_json(ng, buf);
        break;
    case NG_JOIN_CODE:
        snprintf(buf, sizeof buf, "{\"t\":\"join\",\"code\":\"%s\",\"name\":\"%s\"}",
                 ng->code, ng->name);
        send_json(ng, buf);
        break;
    }
}

void netgame_start(NetGame* ng, const char* host, int port, bool tls,
                   NgJoin join, const char* code, const Rules* rules,
                   const char* name) {
    memset(ng, 0, sizeof *ng);
    ng->state = NG_CONNECTING;
    ng->my_seat = -1;
    ng->join = join;
    snprintf(ng->code, sizeof ng->code, "%s", code ? code : "");
    // Sanitize the name to alphanumerics (JSON-safe, matches the server's handle
    // rules) so a hand-edited prefs file can't inject into the wire message.
    {
        int j = 0;
        if (name) for (int i = 0; name[i] && j < (int)sizeof ng->name - 1; i++) {
            char ch = name[i];
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9'))
                ng->name[j++] = ch;
        }
        ng->name[j] = '\0';
    }
    ng->rules = rules ? *rules : rules_default();
    rules_normalize(&ng->rules);
    snprintf(ng->host, sizeof ng->host, "%s", host ? host : "127.0.0.1");
    ng->port = port;
    ng->tls = tls;
    ng->rejoining = false;
    ng->greeted = false;
    ng->conn = net_connect(host, port, "/", tls);
    if (!ng->conn) {
        ng->state = NG_ERROR;
        snprintf(ng->err, sizeof ng->err, "no connection");
    }
}

// Parse a "handles":["A","B",...] array into ng->handles. Sent in the welcome
// and in every state broadcast, so opponent names stay current as players join.
static void parse_handles(NetGame* ng, const char* msg) {
    const char* h = find_key(msg, "handles");
    if (!h) return;
    h += strlen("handles") + 3;
    for (int i = 0; i < MAX_PLAYERS && *h && *h != ']'; ) {
        while (*h && *h != '"' && *h != ']') h++;
        if (*h != '"') break;
        h++;
        size_t k = 0;
        while (*h && *h != '"' && k + 1 < sizeof ng->handles[i]) ng->handles[i][k++] = *h++;
        ng->handles[i][k] = '\0';
        if (*h == '"') h++;
        i++;
    }
}

unsigned netgame_update(NetGame* ng) {
    unsigned events = 0;

    // Auto-rejoin: after a drop with a seat token, wait a moment, then dial
    // back and reclaim the seat. Fail for good once the token is refused.
    if (ng->state == NG_DISCONNECTED && !ng->conn) {
        if (ng->reconnect_delay > 0) { ng->reconnect_delay--; return 0; }
        ng->rejoining = true;
        ng->conn = net_connect(ng->host, ng->port, "/", ng->tls);
        if (!ng->conn) {
            ng->state = NG_ERROR;
            snprintf(ng->err, sizeof ng->err, "reconnect failed");
        } else {
            ng->state = NG_CONNECTING;
        }
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

    if (ns == NET_ERROR) {
        // Drop into reconnect if we had a seat to reclaim, else fail.
        if (ng->token[0]) {
            ng->state = NG_DISCONNECTED;
            ng->reconnect_delay = REJOIN_FRAMES;
        } else {
            ng->state = NG_ERROR;
            snprintf(ng->err, sizeof ng->err, "connection lost");
        }
        net_close(ng->conn);
        ng->conn = NULL;
        return 0;
    }
    if (ns == NET_CLOSED) {
        ng->state = NG_ERROR;
        snprintf(ng->err, sizeof ng->err, "closed");
        net_close(ng->conn);
        ng->conn = NULL;
        return 0;
    }
    if (ns != NET_OPEN) return 0;   // still handshaking

    // First frame after the socket opens: greet, then declare intent.
    if (ng->state == NG_CONNECTING) {
        send_json(ng, "{\"t\":\"hello\",\"v\":1}");
        ng->greeted = false;
    }

    char msg[2048];
    int n;
    while ((n = net_recv(ng->conn, msg, sizeof msg)) >= 0) {
        // Dispatch by the "t" field.
        char type[16];
        if (!js_str(msg, "t", type, sizeof type)) continue;

        if (strcmp(type, "hi") == 0) {
            if (!ng->greeted) {
                if (ng->rejoining && ng->token[0]) {
                    char rj[80];
                    snprintf(rj, sizeof rj, "{\"t\":\"rejoin\",\"token\":\"%s\"}", ng->token);
                    send_json(ng, rj);
                    ng->rejoining = false;
                } else {
                    send_join_intent(ng);
                }
                ng->greeted = true;
            }
            if (ng->state == NG_CONNECTING) ng->state = NG_LOBBY;
        } else if (strcmp(type, "welcome") == 0) {
            ng->my_seat = (int)js_int(msg, "seat", 0);
            js_str(msg, "code", ng->code, sizeof ng->code);
            js_str(msg, "token", ng->token, sizeof ng->token);
            ng->players = (int)js_int(msg, "players", 4);
            parse_handles(ng, msg);
            ng->state = js_int(msg, "waiting", 0) ? NG_WAITING : NG_PLAYING;
        } else if (strcmp(type, "room") == 0) {
            js_str(msg, "code", ng->code, sizeof ng->code);
            ng->players = (int)js_int(msg, "players", 4);
            ng->joined = (int)js_int(msg, "joined", 1);
            ng->state = NG_WAITING;
        } else if (strcmp(type, "queue") == 0) {
            ng->queue_pos = (int)js_int(msg, "pos", 1);
            ng->state = NG_QUEUED;
        } else if (strcmp(type, "state") == 0) {
            if (netgame_parse_state(ng, msg, (size_t)n)) {
                parse_handles(ng, msg);   // keep opponent names current
                ng->state = NG_PLAYING;
                ng->confirmed_local = false;
                ng->pending = false;   // the authoritative result landed
                events = ng->game.events;
            }
        } else if (strcmp(type, "error") == 0) {
            char code[16] = {0};
            js_str(msg, "code", code, sizeof code);
            ng->pending = false;       // a rejection frees the next attempt
            // A rejected action is transient; a protocol/seat error is fatal.
            if (strcmp(code, "upgrade") == 0 || strcmp(code, "no_room") == 0 ||
                strcmp(code, "no_seat") == 0 || strcmp(code, "full") == 0) {
                ng->state = NG_ERROR;
                snprintf(ng->err, sizeof ng->err, "%s", code);
            }
        }
    }
    return events;
}

bool netgame_my_turn(const NetGame* ng) {
    if (ng->state != NG_PLAYING || !ng->have_game) return false;
    return ng->game.turn == ng->my_seat &&
           (ng->game.phase == PHASE_DRAW || ng->game.phase == PHASE_PLACE);
}

void netgame_action(NetGame* ng, Action a) {
    if (!netgame_my_turn(ng) || ng->pending) return;
    char buf[64];
    snprintf(buf, sizeof buf, "{\"t\":\"action\",\"a\":%d,\"slot\":%d}", a.type, a.slot);
    if (net_send(ng->conn, buf, strlen(buf))) ng->pending = true;
}

void netgame_confirm(NetGame* ng) {
    if (ng->state != NG_PLAYING || ng->game.phase != PHASE_ROUND_OVER) return;
    if (ng->confirmed_local) return;
    ng->confirmed_local = true;
    send_json(ng, "{\"t\":\"next\"}");
}

void netgame_close(NetGame* ng) {
    if (ng->conn) {
        send_json(ng, "{\"t\":\"leave\"}");
        net_close(ng->conn);
        ng->conn = NULL;
    }
    ng->state = NG_ERROR;
}
