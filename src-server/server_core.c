#include "server_core.h"
#include "ai.h"
#include "wire.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// --- Tunables that are not pacing -------------------------------------------
#define STRIKES_TO_AI    3      // consecutive turn timeouts before AI takeover
#define RATE_BURST       20     // per-connection message budget...
#define RATE_REFILL_MS   100    // ...refilling 1 per this many ms (~10 msg/s)
#define IPCAP_WINDOW_MS  600000 // per-IP room-create/queue budget window...
#define IPCAP_MAX        30     // ...and how many starts it allows
#define ALOG_CAP         2048   // audit action log per room (then truncated)
#define ALOG_NEXT_ROUND  0xFF   // pseudo-action marking a round advance

#define STATE_BUF        2048   // one outbound message, cap-checked

static const char* const HANDLES[MAX_PLAYERS] = {"North", "East", "South", "West"};

// --- Types -------------------------------------------------------------------
typedef struct {
    bool     used;        // a token has been issued for this seat
    bool     connected;
    int      client;      // valid while connected
    char     token[33];   // empty = rejoin disabled (explicit leave)
    uint8_t  strikes;     // consecutive turn timeouts
    bool     ai_control;  // struck out or left: AI plays until they act again
    bool     confirmed;   // round-over confirmation
} Seat;

typedef struct {
    bool     active;
    bool     waiting;     // created, not yet started (friends still joining)
    bool     public_room; // born from the quick-match queue
    char     code[8];
    Rules    rules;       // as configured at create (seed filled at start)
    Game     g;           // valid once !waiting
    Seat     seats[MAX_PLAYERS];
    int64_t  turn_deadline;   // human decision clock (0 = none armed)
    int64_t  ai_due;          // when the AI-controlled actor moves (0 = none)
    int64_t  reveal_deadline; // round-over auto-advance (0 = none)
    int64_t  last_human_ms;   // abandoned-room GC
    uint32_t actions;
    uint8_t  alog[ALOG_CAP];
    uint16_t alog_len;
    bool     alog_full;
} Room;

typedef enum { CL_NEW = 0, CL_LOBBY, CL_QUEUED, CL_SEATED } ClState;

typedef struct {
    bool    used;
    ClState state;
    int     room, seat;      // valid when CL_SEATED
    char    ip[46];
    int     tokens;          // rate bucket
    int64_t bucket_ms;
    int64_t queued_ms;       // when they entered the quick queue
} Client;

typedef struct {
    char    ip[46];
    int64_t window_ms;
    int     count;
} IpCap;

struct Srv {
    SrvIo    io;
    uint64_t rng;
    Room     rooms[SRV_MAX_ROOMS];
    Client   clients[SRV_MAX_CLIENTS];
    int      queue[SRV_MAX_CLIENTS];
    int      queue_len;
    IpCap    ipcap[256];
    int64_t  boot_ms;
    bool     boot_set;
};

// --- Small helpers -----------------------------------------------------------
static uint64_t rng_next64(uint64_t* s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}

static bool client_ok(const Srv* s, int client) {
    return client >= 0 && client < SRV_MAX_CLIENTS && s->clients[client].used;
}

static void send_text(Srv* s, int client, const char* text, size_t len) {
    if (client_ok(s, client)) s->io.send(s->io.ud, client, text, len);
}

static void send_error(Srv* s, int client, const char* code) {
    char buf[96];
    int n = snprintf(buf, sizeof buf, "{\"t\":\"error\",\"code\":\"%s\"}", code);
    if (n > 0) send_text(s, client, buf, (size_t)n);
}

static void logf_line(Srv* s, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static void logf_line(Srv* s, const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) s->io.log(s->io.ud, buf);
}

static void make_token(Srv* s, char out[33]) {
    static const char HEX[] = "0123456789abcdef";
    uint64_t a = rng_next64(&s->rng), b = rng_next64(&s->rng);
    for (int i = 0; i < 16; i++) out[i]      = HEX[(a >> (i * 4)) & 15];
    for (int i = 0; i < 16; i++) out[16 + i] = HEX[(b >> (i * 4)) & 15];
    out[32] = '\0';
}

// Room codes from an unambiguous 32-letter alphabet (no 0/O/1/I), unique
// among active rooms. Generated into a local first: `out` is the (already
// active) room's own code field, and writing the candidate there before the
// clash scan made every candidate collide with itself, forever.
static void make_code(Srv* s, char out[8]) {
    static const char AB[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    char cand[8];
    for (;;) {
        uint64_t v = rng_next64(&s->rng);
        for (int i = 0; i < 6; i++) {
            cand[i] = AB[(v >> (i * 5)) & 31];
        }
        cand[6] = '\0';
        bool clash = false;
        for (int r = 0; r < SRV_MAX_ROOMS; r++) {
            if (s->rooms[r].active && strcmp(s->rooms[r].code, cand) == 0) clash = true;
        }
        if (!clash) {
            memcpy(out, cand, 7);
            return;
        }
    }
}

static int find_room_slot(Srv* s) {
    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        if (!s->rooms[r].active) return r;
    }
    return -1;
}

static bool any_connected_human(const Room* rm) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (rm->seats[i].used && rm->seats[i].connected) return true;
    }
    return false;
}

// A seat whose human is present and hasn't struck out: the turn clock applies.
static bool seat_human_active(const Room* rm, int seat) {
    const Seat* st = &rm->seats[seat];
    return st->used && st->connected && !st->ai_control;
}

static void alog_push(Room* rm, uint8_t b) {
    if (rm->alog_len < ALOG_CAP) rm->alog[rm->alog_len++] = b;
    else rm->alog_full = true;
}

// --- Per-IP start limiter ----------------------------------------------------
static bool ip_allow_start(Srv* s, const char* ip, int64_t now) {
    uint32_t h = 2166136261u;
    for (const char* p = ip; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    IpCap* c = &s->ipcap[h & 255];
    if (strcmp(c->ip, ip) != 0 || now - c->window_ms > IPCAP_WINDOW_MS) {
        snprintf(c->ip, sizeof c->ip, "%s", ip);
        c->window_ms = now;
        c->count = 0;
    }
    if (c->count >= IPCAP_MAX) return false;
    c->count++;
    return true;
}

// --- State broadcast ---------------------------------------------------------
// One redacted snapshot per seat, emitted FROM the redacted copy so a
// formatting bug can never leak what game_redact_for already removed.
static void emit_state(Srv* s, Room* rm, int seat, int last_seat, int last_a,
                       int last_slot, int last_card, int64_t now) {
    Game red;
    game_redact_for(&red, &rm->g, seat);

    char buf[STATE_BUF];
    JW w;
    jw_init(&w, buf, sizeof buf);
    int n = red.rules.player_count;

    jw_f(&w, "{\"t\":\"state\",\"v\":%d,\"seat\":%d,\"players\":%d,\"phase\":%d,"
             "\"turn\":%d,\"dealer\":%d,\"round\":%d,\"target\":%d,"
             "\"bonus\":%d,\"partners\":%d,\"stock\":%d,",
        SRV_PROTO_VERSION, seat, n, red.phase, red.turn, red.dealer,
        red.round_no, red.rules.target_score, red.rules.bonus_scoring ? 1 : 0,
        red.rules.partners ? 1 : 0, red.stock_count);

    jw_f(&w, "\"discard\":[");
    for (int i = 0; i < red.discard_count; i++) {
        jw_f(&w, "%s%d", i ? "," : "", red.discard[i]);
    }
    jw_f(&w, "],\"held\":%d,\"hfd\":%d,", red.held_card, red.held_from_discard ? 1 : 0);

    jw_f(&w, "\"rack\":[");
    for (int i = 0; i < RACK_SLOTS; i++) {
        jw_f(&w, "%s%d", i ? "," : "", red.players[seat].rack.slots[i]);
    }
    jw_f(&w, "],");

    // Other racks appear only when the redacted copy carries them (reveal).
    if (red.phase == PHASE_ROUND_OVER || red.phase == PHASE_MATCH_OVER) {
        jw_f(&w, "\"racks\":[");
        for (int p = 0; p < n; p++) {
            jw_f(&w, "%s[", p ? "," : "");
            for (int i = 0; i < RACK_SLOTS; i++) {
                jw_f(&w, "%s%d", i ? "," : "", red.players[p].rack.slots[i]);
            }
            jw_f(&w, "]");
        }
        jw_f(&w, "],");
    }

    jw_f(&w, "\"scores\":[");
    for (int p = 0; p < n; p++) jw_f(&w, "%s%d", p ? "," : "", red.players[p].score);
    jw_f(&w, "],\"round_points\":[");
    for (int p = 0; p < n; p++) jw_f(&w, "%s%d", p ? "," : "", red.round_points[p]);
    jw_f(&w, "],\"last_taken\":[");
    for (int p = 0; p < n; p++) jw_f(&w, "%s%d", p ? "," : "", red.last_taken[p]);
    jw_f(&w, "],\"ai\":[");
    for (int p = 0; p < n; p++) {
        bool ai = !rm->seats[p].used || rm->seats[p].ai_control || !rm->seats[p].connected;
        jw_f(&w, "%s%d", p ? "," : "", ai ? 1 : 0);
    }
    jw_f(&w, "],\"winner\":%d,\"match_winner\":%d,\"recycles\":%d,\"events\":%u,",
        red.round_winner, red.match_winner, red.recycles, red.events);

    if (last_seat >= 0) {
        jw_f(&w, "\"last\":{\"seat\":%d,\"a\":%d,\"slot\":%d,\"card\":%d},",
             last_seat, last_a, last_slot, last_card);
    }

    int64_t deadline = 0;
    if (rm->turn_deadline) deadline = rm->turn_deadline - now;
    else if (rm->reveal_deadline) deadline = rm->reveal_deadline - now;
    if (deadline < 0) deadline = 0;
    jw_f(&w, "\"deadline_ms\":%lld}", (long long)deadline);

    if (w.overflow) {
        logf_line(s, "BUG state overflow room=%s seat=%d", rm->code, seat);
        return;
    }
    send_text(s, rm->seats[seat].client, buf, w.len);
}

// last_card: what an observer at the table saw move — the drawn card if it
// was public (discard draw), the discarded/displaced card otherwise, 0 if
// face down. Drives client sounds and flight animation.
static void broadcast_state(Srv* s, Room* rm, int last_seat, int last_a,
                            int last_slot, int last_card, int64_t now) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (rm->seats[i].used && rm->seats[i].connected) {
            emit_state(s, rm, i, last_seat, last_a, last_slot, last_card, now);
        }
    }
}

static void send_welcome(Srv* s, Room* rm, int room_idx, int seat) {
    char buf[256];
    JW w;
    jw_init(&w, buf, sizeof buf);
    jw_f(&w, "{\"t\":\"welcome\",\"v\":%d,\"seat\":%d,\"token\":\"%s\","
             "\"code\":\"%s\",\"players\":%d,\"waiting\":%d,\"handles\":[",
        SRV_PROTO_VERSION, seat, rm->seats[seat].token, rm->code,
        rm->rules.player_count, rm->waiting ? 1 : 0);
    for (int i = 0; i < rm->rules.player_count; i++) {
        jw_f(&w, "%s\"%s\"", i ? "," : "", HANDLES[i]);
    }
    jw_f(&w, "]}");
    (void)room_idx;
    if (!w.overflow) send_text(s, rm->seats[seat].client, buf, w.len);
}

// Waiting-room roster update for everyone seated so far.
static void broadcast_room(Srv* s, Room* rm) {
    char buf[160];
    int joined = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (rm->seats[i].used) joined++;
    }
    int n = snprintf(buf, sizeof buf,
                     "{\"t\":\"room\",\"code\":\"%s\",\"players\":%d,\"joined\":%d}",
                     rm->code, rm->rules.player_count, joined);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (rm->seats[i].used && rm->seats[i].connected && n > 0) {
            send_text(s, rm->seats[i].client, buf, (size_t)n);
        }
    }
}

// --- Room lifecycle ----------------------------------------------------------
static void drive_deal(Room* rm) {
    for (int i = 0; i < 4096 && rm->g.phase == PHASE_DEAL; i++) game_update(&rm->g);
}

// Arm whichever clock the current phase needs. The engine's own pacing/anim
// is presentation and plays no part here: the server never calls game_update
// outside the deal skip, so engine AI never fires on its own.
static void schedule(Srv* s, Room* rm, int64_t now) {
    (void)s;
    rm->turn_deadline = 0;
    rm->ai_due = 0;
    if (rm->g.phase == PHASE_DRAW || rm->g.phase == PHASE_PLACE) {
        rm->reveal_deadline = 0;
        if (seat_human_active(rm, rm->g.turn)) {
            rm->turn_deadline = now + SRV_TURN_MS;
        } else {
            rm->ai_due = now + (any_connected_human(rm) ? SRV_AI_PACE_MS : 0);
        }
    } else if (rm->g.phase == PHASE_ROUND_OVER) {
        // Arm the reveal backstop and clear confirmations exactly ONCE, on
        // entry (reveal_deadline is 0 coming out of play). schedule() also
        // fires on every seat event — disconnect, leave, rejoin — and those
        // must NOT push the deadline forward or wipe confirmations: otherwise
        // a client that flaps its socket faster than SRV_REVEAL_MS keeps
        // nulling the other humans' `next` votes and resetting the timer,
        // holding the whole table in the reveal screen forever (the backstop
        // this branch exists to guarantee). Idempotent past the first call.
        if (rm->reveal_deadline == 0) {
            rm->reveal_deadline = now + SRV_REVEAL_MS;
            for (int i = 0; i < MAX_PLAYERS; i++) rm->seats[i].confirmed = false;
        }
    } else {
        rm->reveal_deadline = 0;
    }
}

static void close_room(Srv* s, Room* rm, const char* why) {
    // Audit line: everything needed to replay the match byte-for-byte.
    char hex[ALOG_CAP * 2 + 1];
    for (int i = 0; i < rm->alog_len; i++) {
        snprintf(hex + i * 2, 3, "%02x", rm->alog[i]);
    }
    hex[rm->alog_len * 2] = '\0';
    logf_line(s, "match code=%s why=%s public=%d players=%d seed=%llu actions=%u "
                 "scores=%d,%d,%d,%d alog=%s%s",
        rm->code, why, rm->public_room ? 1 : 0, rm->rules.player_count,
        (unsigned long long)rm->rules.seed, rm->actions,
        rm->g.players[0].score, rm->g.players[1].score,
        rm->g.players[2].score, rm->g.players[3].score,
        hex, rm->alog_full ? "..." : "");

    for (int i = 0; i < MAX_PLAYERS; i++) {
        Seat* st = &rm->seats[i];
        if (st->used && st->connected && client_ok(s, st->client)) {
            Client* c = &s->clients[st->client];
            c->state = CL_LOBBY;
            c->room = c->seat = -1;
        }
    }
    memset(rm, 0, sizeof *rm);
}

static void start_match(Srv* s, Room* rm, int64_t now) {
    rm->waiting = false;
    rm->rules.seed = rng_next64(&s->rng);
    if (rm->rules.seed == 0) rm->rules.seed = 1;
    rm->rules.human_seat = 0;   // engine-side flag; the server drives all AI itself
    game_init(&rm->g, &rm->rules);
    drive_deal(rm);
    rm->last_human_ms = now;
    logf_line(s, "start code=%s public=%d players=%d seed=%llu",
        rm->code, rm->public_room ? 1 : 0, rm->rules.player_count,
        (unsigned long long)rm->rules.seed);
    schedule(s, rm, now);
    broadcast_state(s, rm, -1, 0, 0, 0, now);
}

// Apply one action for the seat whose turn it is, log it, broadcast, and
// re-arm the clocks. `card_pub` tells observers what they saw move.
static void applied_action(Srv* s, Room* rm, int seat, Action a, int card_pub,
                           int64_t now) {
    rm->actions++;
    alog_push(rm, (uint8_t)((a.type & 0x3) | (a.slot << 2)));
    schedule(s, rm, now);
    broadcast_state(s, rm, seat, a.type, a.slot, card_pub, now);
}

// The card an observer saw during this action, before it is applied. Runs on
// UNVALIDATED client input by design (it captures the pre-apply table), so it
// must bounds-check every client-controlled index itself — a hostile
// {"a":2,"slot":250} otherwise reads past the 10-slot rack. An out-of-range
// slot returns 0; game_apply then rejects the action anyway.
static int observed_card(const Room* rm, Action a) {
    const Game* g = &rm->g;
    switch (a.type) {
    case ACTION_DRAW_DISCARD:
        return g->discard_count ? g->discard[g->discard_count - 1] : 0;
    case ACTION_PLACE:
        // The displaced card lands face up on the pile.
        return (a.slot < RACK_SLOTS) ? g->players[g->turn].rack.slots[a.slot] : 0;
    case ACTION_DISCARD:
        return g->held_card; // thrown face up
    default:
        return 0;            // a stock draw shows only a card back
    }
}

// One AI-controlled move for the current actor (a true AI seat, a struck-out
// human, or a timed-out turn). Mirrors the engine's own fallback ladder so a
// misbehaving evaluator can never wedge a table.
static void ai_move(Srv* s, Room* rm, int64_t now) {
    Game* g = &rm->g;
    GameView v = game_view_for(g, g->turn);
    Action a = ai_choose(&v, &g->rng);
    if (!game_action_legal(g, a)) {
        a = (Action){ACTION_DRAW_STOCK, 0};
        if (g->phase == PHASE_PLACE) a = (Action){ACTION_PLACE, 0};
        else if (!game_action_legal(g, a)) a = (Action){ACTION_DRAW_DISCARD, 0};
    }
    int seat = g->turn;
    int card = observed_card(rm, a);
    g->events = 0;
    if (game_apply(g, a)) {
        applied_action(s, rm, seat, a, card, now);
    } else {
        // Unreachable by construction; close rather than spin.
        logf_line(s, "BUG ai wedge code=%s phase=%d", rm->code, g->phase);
        close_room(s, rm, "wedge");
    }
}

static void advance_round(Srv* s, Room* rm, int64_t now) {
    alog_push(rm, ALOG_NEXT_ROUND);
    game_next_round(&rm->g);
    if (rm->g.phase == PHASE_DEAL) drive_deal(rm);
    schedule(s, rm, now);
    broadcast_state(s, rm, -1, 0, 0, 0, now);
    if (rm->g.phase == PHASE_MATCH_OVER) {
        close_room(s, rm, "finished");
    }
}

static void maybe_advance(Srv* s, Room* rm, int64_t now) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (seat_human_active(rm, i) && !rm->seats[i].confirmed) return;
    }
    advance_round(s, rm, now);
}

// Seat a client in a room: token, bookkeeping, welcome.
static void seat_client(Srv* s, int room_idx, int seat, int client) {
    Room* rm = &s->rooms[room_idx];
    Seat* st = &rm->seats[seat];
    st->used = true;
    st->connected = true;
    st->client = client;
    st->strikes = 0;
    st->ai_control = false;
    st->confirmed = false;
    make_token(s, st->token);
    Client* c = &s->clients[client];
    c->state = CL_SEATED;
    c->room = room_idx;
    c->seat = seat;
    send_welcome(s, rm, room_idx, seat);
}

// --- Queue -------------------------------------------------------------------
static void queue_remove(Srv* s, int client) {
    for (int i = 0; i < s->queue_len; i++) {
        if (s->queue[i] == client) {
            memmove(&s->queue[i], &s->queue[i + 1],
                    (size_t)(s->queue_len - i - 1) * sizeof s->queue[0]);
            s->queue_len--;
            return;
        }
    }
}

static void launch_public(Srv* s, int humans, int64_t now) {
    int slot = find_room_slot(s);
    if (slot < 0) return;
    Room* rm = &s->rooms[slot];
    memset(rm, 0, sizeof *rm);
    rm->active = true;
    rm->public_room = true;
    rm->waiting = true;               // flipped by start_match below
    rm->last_human_ms = now;
    make_code(s, rm->code);
    rm->rules = rules_default();      // 4 seats, official defaults
    for (int i = 0; i < humans; i++) {
        int client = s->queue[0];
        queue_remove(s, client);
        seat_client(s, slot, i, client);
    }
    start_match(s, rm, now);
}

static void process_queue(Srv* s, int64_t now) {
    while (s->queue_len >= MAX_PLAYERS) {
        launch_public(s, MAX_PLAYERS, now);
        if (find_room_slot(s) < 0) return;
    }
    if (s->queue_len == 0) return;
    int64_t waited = now - s->clients[s->queue[0]].queued_ms;
    if (s->queue_len >= 2 && waited >= SRV_QUEUE_PAIR_MS) {
        launch_public(s, s->queue_len, now);
    } else if (s->queue_len == 1 && waited >= SRV_QUEUE_SOLO_MS) {
        launch_public(s, 1, now);
    }
}

// --- Public API --------------------------------------------------------------
Srv* srv_create(const SrvIo* io, uint64_t rng_seed) {
    static Srv srv;
    memset(&srv, 0, sizeof srv);
    srv.io = *io;
    srv.rng = rng_seed ? rng_seed : 0x9E3779B97F4A7C15ULL;
    return &srv;
}

void srv_client_connected(Srv* s, int client, const char* ip) {
    if (client < 0 || client >= SRV_MAX_CLIENTS) return;
    Client* c = &s->clients[client];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->state = CL_NEW;
    c->room = c->seat = -1;
    c->tokens = RATE_BURST;
    snprintf(c->ip, sizeof c->ip, "%s", ip ? ip : "?");
}

void srv_client_gone(Srv* s, int client, int64_t now_ms) {
    if (!client_ok(s, client)) return;
    Client* c = &s->clients[client];
    if (c->state == CL_QUEUED) queue_remove(s, client);
    if (c->state == CL_SEATED && c->room >= 0 && s->rooms[c->room].active) {
        Room* rm = &s->rooms[c->room];
        Seat* st = &rm->seats[c->seat];
        st->connected = false;
        st->client = -1;
        if (rm->waiting && c->seat == 0) {
            // Only the creator can start a waiting table; without them the
            // joiners would wait forever. Close it now, not at GC time.
            memset(c, 0, sizeof *c);
            close_room(s, rm, "creator_left");
            return;
        }
        // If it was their turn, the AI clock takes over.
        if (!rm->waiting) schedule(s, rm, now_ms);
        if (!any_connected_human(rm)) rm->last_human_ms = now_ms;
    }
    memset(c, 0, sizeof *c);
}

// Bind `client` to a live seat found by token. Returns true on success.
static bool try_rejoin(Srv* s, int client, const char* token, int64_t now) {
    if (!token || strlen(token) != 32) return false;
    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        Room* rm = &s->rooms[r];
        if (!rm->active) continue;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            Seat* st = &rm->seats[i];
            if (!st->used || strcmp(st->token, token) != 0) continue;
            if (st->connected && client_ok(s, st->client)) {
                // The token holder reconnected elsewhere: the new socket wins.
                s->clients[st->client].state = CL_LOBBY;
                s->clients[st->client].room = s->clients[st->client].seat = -1;
                s->io.kick(s->io.ud, st->client);
            }
            st->connected = true;
            st->client = client;
            st->ai_control = false;
            st->strikes = 0;
            Client* c = &s->clients[client];
            c->state = CL_SEATED;
            c->room = r;
            c->seat = i;
            rm->last_human_ms = now;
            send_welcome(s, rm, r, i);
            if (!rm->waiting) {
                schedule(s, rm, now);
                emit_state(s, rm, i, -1, 0, 0, 0, now);
            }
            return true;
        }
    }
    return false;
}

void srv_client_msg(Srv* s, int client, const char* text, size_t len, int64_t now) {
    if (!client_ok(s, client)) return;
    Client* c = &s->clients[client];

    // Per-connection rate bucket: refill, then spend. Overdraft closes.
    if (c->bucket_ms == 0) c->bucket_ms = now;
    int64_t refill = (now - c->bucket_ms) / RATE_REFILL_MS;
    if (refill > 0) {
        c->tokens = (c->tokens + (int)(refill > RATE_BURST ? RATE_BURST : refill));
        if (c->tokens > RATE_BURST) c->tokens = RATE_BURST;
        c->bucket_ms = now;
    }
    if (--c->tokens < 0) {
        send_error(s, client, "rate");
        s->io.kick(s->io.ud, client);
        return;
    }

    WireKV kv[WIRE_MAX_KV];
    int n = wire_parse(text, len, kv);
    const char* t = (n >= 0) ? wire_str(kv, n, "t") : NULL;
    if (!t) {
        send_error(s, client, "bad_msg");
        s->io.kick(s->io.ud, client);
        return;
    }

    if (c->state == CL_NEW) {
        if (strcmp(t, "hello") != 0 || wire_int(kv, n, "v", -1) != SRV_PROTO_VERSION) {
            send_error(s, client, "upgrade");
            s->io.kick(s->io.ud, client);
            return;
        }
        c->state = CL_LOBBY;
        char buf[40];
        int m = snprintf(buf, sizeof buf, "{\"t\":\"hi\",\"v\":%d}", SRV_PROTO_VERSION);
        send_text(s, client, buf, (size_t)m);
        return;
    }

    if (strcmp(t, "ping") == 0) {
        send_text(s, client, "{\"t\":\"pong\"}", 12);
        return;
    }

    if (strcmp(t, "create") == 0 && c->state == CL_LOBBY) {
        if (!ip_allow_start(s, c->ip, now)) { send_error(s, client, "rate"); return; }
        int slot = find_room_slot(s);
        if (slot < 0) { send_error(s, client, "full"); return; }
        Room* rm = &s->rooms[slot];
        memset(rm, 0, sizeof *rm);
        rm->active = true;
        rm->waiting = true;
        rm->last_human_ms = now;
        make_code(s, rm->code);
        rm->rules = rules_default();
        rm->rules.player_count = (int)wire_int(kv, n, "players", 4);
        rm->rules.bonus_scoring = wire_int(kv, n, "bonus", 0) != 0;
        rm->rules.partners = wire_int(kv, n, "partners", 0) != 0;
        long target = wire_int(kv, n, "target", 500);
        rm->rules.target_score = (target >= 50 && target <= 1000) ? (int)target : 500;
        rules_normalize(&rm->rules);
        seat_client(s, slot, 0, client);
        broadcast_room(s, rm);
        return;
    }

    if (strcmp(t, "join") == 0 && c->state == CL_LOBBY) {
        const char* code = wire_str(kv, n, "code");
        if (!code) { send_error(s, client, "bad_msg"); return; }
        for (int r = 0; r < SRV_MAX_ROOMS; r++) {
            Room* rm = &s->rooms[r];
            if (!rm->active || !rm->waiting || strcmp(rm->code, code) != 0) continue;
            int seat = -1;
            for (int i = 0; i < rm->rules.player_count; i++) {
                if (!rm->seats[i].used) { seat = i; break; }
            }
            if (seat < 0) { send_error(s, client, "full"); return; }
            seat_client(s, r, seat, client);
            broadcast_room(s, rm);
            // Table complete: play.
            if (seat == rm->rules.player_count - 1) start_match(s, rm, now);
            return;
        }
        send_error(s, client, "no_room");
        return;
    }

    if (strcmp(t, "start") == 0 && c->state == CL_SEATED && c->seat == 0 &&
        c->room >= 0 && s->rooms[c->room].active && s->rooms[c->room].waiting) {
        start_match(s, &s->rooms[c->room], now);
        return;
    }

    if (strcmp(t, "quick") == 0 && c->state == CL_LOBBY) {
        if (!ip_allow_start(s, c->ip, now)) { send_error(s, client, "rate"); return; }
        c->state = CL_QUEUED;
        c->queued_ms = now;
        s->queue[s->queue_len++] = client;
        char buf[40];
        int m = snprintf(buf, sizeof buf, "{\"t\":\"queue\",\"pos\":%d}", s->queue_len);
        send_text(s, client, buf, (size_t)m);
        process_queue(s, now);
        return;
    }

    if (strcmp(t, "rejoin") == 0 && c->state == CL_LOBBY) {
        if (!try_rejoin(s, client, wire_str(kv, n, "token"), now)) {
            send_error(s, client, "no_seat");
        }
        return;
    }

    if (c->state != CL_SEATED || c->room < 0 || !s->rooms[c->room].active) {
        send_error(s, client, "not_in_game");
        return;
    }
    Room* rm = &s->rooms[c->room];
    rm->last_human_ms = now;
    Seat* st = &rm->seats[c->seat];
    st->ai_control = false;   // any live message reclaims a struck-out seat
    st->strikes = 0;

    if (strcmp(t, "action") == 0) {
        if (rm->waiting) { send_error(s, client, "not_started"); return; }
        if (rm->g.turn != c->seat ||
            (rm->g.phase != PHASE_DRAW && rm->g.phase != PHASE_PLACE)) {
            send_error(s, client, "not_your_turn");
            return;
        }
        Action a = {(uint8_t)wire_int(kv, n, "a", 255),
                    (uint8_t)wire_int(kv, n, "slot", 0)};
        int card = observed_card(rm, a);
        rm->g.events = 0;
        if (!game_apply(&rm->g, a)) {
            send_error(s, client, "illegal");
            return;
        }
        applied_action(s, rm, c->seat, a, card, now);
        return;
    }

    if (strcmp(t, "next") == 0) {
        if (rm->g.phase != PHASE_ROUND_OVER || rm->waiting) return;
        st->confirmed = true;
        maybe_advance(s, rm, now);
        return;
    }

    if (strcmp(t, "leave") == 0) {
        int seat = c->seat;
        st->connected = false;
        st->client = -1;
        st->token[0] = '\0';   // an explicit leave surrenders the seat for good
        st->ai_control = true;
        c->state = CL_LOBBY;
        c->room = c->seat = -1;
        if (rm->waiting && seat == 0) {
            close_room(s, rm, "creator_left");   // see srv_client_gone
            return;
        }
        if (!rm->waiting) schedule(s, rm, now);
        if (!any_connected_human(rm)) rm->last_human_ms = now;
        return;
    }

    send_error(s, client, "bad_msg");
}

void srv_tick(Srv* s, int64_t now) {
    if (!s->boot_set) { s->boot_set = true; s->boot_ms = now; }

    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        Room* rm = &s->rooms[r];
        if (!rm->active) continue;

        if (any_connected_human(rm)) rm->last_human_ms = now;
        else if (now - rm->last_human_ms > SRV_ABANDON_MS) {
            close_room(s, rm, "abandoned");
            continue;
        }
        if (rm->waiting) continue;

        if (rm->ai_due && now >= rm->ai_due) {
            ai_move(s, rm, now);
            continue;   // one move per tick per room keeps AI play legible
        }
        if (rm->turn_deadline && now >= rm->turn_deadline) {
            Seat* st = &rm->seats[rm->g.turn];
            if (++st->strikes >= STRIKES_TO_AI) st->ai_control = true;
            ai_move(s, rm, now);
            continue;
        }
        if (rm->reveal_deadline && now >= rm->reveal_deadline &&
            rm->g.phase == PHASE_ROUND_OVER) {
            advance_round(s, rm, now);
        }
    }

    process_queue(s, now);
}

void srv_status_json(Srv* s, char* out, size_t cap, int64_t now) {
    int rooms = 0, waiting = 0, clients = 0;
    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        if (s->rooms[r].active) { rooms++; if (s->rooms[r].waiting) waiting++; }
    }
    for (int i = 0; i < SRV_MAX_CLIENTS; i++) {
        if (s->clients[i].used) clients++;
    }
    snprintf(out, cap,
             "{\"rooms\":%d,\"waiting\":%d,\"clients\":%d,\"queue\":%d,"
             "\"uptime_s\":%lld}",
             rooms, waiting, clients, s->queue_len,
             s->boot_set ? (long long)((now - s->boot_ms) / 1000) : 0);
}
