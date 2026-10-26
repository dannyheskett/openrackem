#include "server_core.h"
#include "ai.h"
#include "wire.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// --- Tunables that are not pacing -------------------------------------------
#define STRIKES_TO_AI    3      // consecutive turn timeouts before AI takeover
// Per-connection message budget. Sized well above legitimate play — two
// humans trading turns as fast as they physically can peaks near 40 msg/s
// each (measured), and a normal game with server-paced AI opponents is a few
// msg/s — while still tripping a real flood (hundreds/sec of garbage or
// reconnect spam) within the burst, in under a second. The turn/phase gate
// already makes out-of-turn actions cheap no-ops, so this only has to stop
// connection-level abuse, not gameplay pace.
#define RATE_BURST       100    // per-connection message budget...
#define RATE_REFILL_MS   10     // ...refilling 1 per this many ms (~100 msg/s)
#define IPCAP_WINDOW_MS  600000 // per-IP room-create budget window...
#define IPCAP_MAX        30     // ...and how many creates it allows
#define MISS_WINDOW_MS   60000  // per-IP wrong-code window...
#define MISS_MAX         10     // ...and how many misses it allows
#define ALOG_CAP         2048   // audit action log per match (then truncated)
#define ALOG_NEXT_ROUND  0xFF   // pseudo-action marking a round advance
#define NAME_CAP         16     // display name buffer (15 chars + NUL)

#define MSG_BUF          8192   // one outbound message, cap-checked

static const char* const HANDLES[MAX_PLAYERS] = {"North", "East", "South", "West"};

// Names a public table should never show. Matched case-insensitively as
// substrings; deliberately short (no chat exists, so names are the only text
// a stranger can put on screen).
static const char* const BLOCKED[] = {
    "fuck", "shit", "cunt", "nigg", "fag", "bitch", "whore", "slut", "rape",
    "nazi", "hitler", "pussy", "asshole", "penis", "vagina", "porn", "kike",
    "retard",
};

// --- Types -------------------------------------------------------------------
enum { ROOM_PUBLIC = 0, ROOM_PRIVATE = 1 };
enum { PHASE_LOBBY = 0, PHASE_PLAYING = 1 };
enum { AWAY_AI = 0, AWAY_WAIT = 1 };   // leave policy (private rooms choose)

typedef struct {
    bool     used;
    bool     connected;
    int      client;      // valid while connected
    char     key[33];     // member key: the only identity, rejoin by it
    char     name[NAME_CAP];
    bool     ready;
    uint8_t  team;        // Partners: 0 = A, 1 = B
    int8_t   seat;        // game seat while playing, else -1
    bool     away;        // stepped away or dropped during a match
    bool     ai_control;  // AI plays the seat until the member acts again
    bool     forfeited;   // surrendered: AI to the end, removed after
    uint8_t  strikes;     // consecutive turn timeouts
    bool     confirmed;   // round-over confirmation
    int8_t   swap_from;   // Partners: member index asking to swap with us
    int64_t  gone_ms;     // waiting room: when the link dropped (grace)
} Member;

typedef struct {
    bool     active;
    uint8_t  kind;            // ROOM_PUBLIC / ROOM_PRIVATE
    uint8_t  phase;           // PHASE_LOBBY / PHASE_PLAYING
    uint8_t  away_policy;     // AWAY_AI / AWAY_WAIT (private rooms only)
    bool     can_force;       // private: "Start without them" offered
    uint16_t gen;             // bumps on every reuse of this slot
    char     code[SRV_CODE_LEN + 1];   // private rooms ("" for public)
    char     owner_key[33];
    Rules    rules;           // room settings; player_count set at deal
    Game     g;               // valid while playing (and its final state)
    Member   m[MAX_PLAYERS];
    int8_t   seat_member[MAX_PLAYERS];  // game seat -> member index
    int64_t  countdown_at;    // everyone ready: deal at this time (0 = none)
    int64_t  idle_since;      // others ready, someone not: since when
    int64_t  turn_deadline;   // human decision clock (0 = none armed)
    int64_t  ai_due;          // when the AI-controlled actor moves (0 = none)
    int64_t  reveal_deadline; // round-over auto-advance (0 = none)
    int8_t   wait_seat;       // seat whose absence the table is waiting out
    int64_t  wait_deadline;   // ...until then (0 = no timer: Wait policy)
    int64_t  empty_since;     // playing, nobody present: since when
    int64_t  created_ms;
    int64_t  last_used_ms;    // last deal / move / match end (private expiry)
    uint32_t rev;             // bumps on any change a viewer can see
    uint32_t actions;
    uint8_t  alog[ALOG_CAP];
    uint16_t alog_len;
    bool     alog_full;
} Room;

typedef enum { CL_NEW = 0, CL_IDLE, CL_ROOM } ClState;

typedef struct {
    bool    used;
    ClState state;
    int     room, member;    // valid when CL_ROOM
    char    ip[46];
    char    name[NAME_CAP];  // from hello
    int     tokens;          // rate bucket
    int64_t bucket_ms;
    int64_t last_msg_ms;     // silence detection
    // Room-list subscription: a stable sample of public rooms.
    bool    listing;
    bool    list_force;      // push on the next tick regardless of spacing
    bool    list_pending;    // something changed; push once spacing allows
    int     list_target;     // Play To filter (0 = any)
    int16_t shown[SRV_LIST_SHOW];
    uint16_t shown_gen[SRV_LIST_SHOW];
    uint32_t shown_rev[SRV_LIST_SHOW];
    int     shown_total;
    int64_t last_push_ms;
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
    IpCap    ipcap[256];
    IpCap    misses[256];
    int16_t  joinable[SRV_MAX_ROOMS];   // scratch: this tick's listable rooms
    int64_t  boot_ms;
    bool     boot_set;
    bool     dirty;
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
    char buf[4400];
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

// Private-room codes from an unambiguous 32-symbol alphabet (no 0/O/1/I),
// unique among active rooms. Generated into a local first: `out` is the
// (already active) room's own code field, and writing the candidate there
// before the clash scan made every candidate collide with itself, forever.
static void make_code(Srv* s, char out[SRV_CODE_LEN + 1]) {
    static const char AB[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    char cand[SRV_CODE_LEN + 1];
    for (;;) {
        uint64_t v = rng_next64(&s->rng);
        for (int i = 0; i < SRV_CODE_LEN; i++) cand[i] = AB[(v >> (i * 5)) & 31];
        cand[SRV_CODE_LEN] = '\0';
        bool clash = false;
        for (int r = 0; r < SRV_MAX_ROOMS && !clash; r++) {
            if (s->rooms[r].active && strcmp(s->rooms[r].code, cand) == 0) clash = true;
        }
        if (!clash) {
            memcpy(out, cand, SRV_CODE_LEN + 1);
            return;
        }
    }
}

static int room_id(const Srv* s, int r) {
    return ((int)s->rooms[r].gen << 12) | r;
}

// A public room id from the wire back to its slot, or -1 when stale.
static int room_from_id(const Srv* s, long id) {
    if (id < 0) return -1;
    int r = (int)(id & 0xFFF);
    if (r >= SRV_MAX_ROOMS || !s->rooms[r].active) return -1;
    if (s->rooms[r].gen != (uint16_t)(id >> 12)) return -1;
    return r;
}

static int find_room_by_code(const Srv* s, const char* code) {
    if (!code || strlen(code) != SRV_CODE_LEN) return -1;
    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        const Room* rm = &s->rooms[r];
        if (rm->active && rm->code[0] && strcmp(rm->code, code) == 0) return r;
    }
    return -1;
}

static int member_count(const Room* rm) {
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) n += rm->m[i].used;
    return n;
}

static bool member_present(const Member* mb) {
    return mb->used && mb->connected && !mb->away;
}

static bool any_present(const Room* rm) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (member_present(&rm->m[i])) return true;
    }
    return false;
}

static Member* seat_mem(Room* rm, int seat) {
    if (seat < 0 || seat >= MAX_PLAYERS) return NULL;
    int mi = rm->seat_member[seat];
    return (mi >= 0 && mi < MAX_PLAYERS && rm->m[mi].used) ? &rm->m[mi] : NULL;
}

// A seat whose human is present and in control: the turn clock applies.
static bool seat_human_active(Room* rm, int seat) {
    Member* mb = seat_mem(rm, seat);
    return mb && member_present(mb) && !mb->ai_control;
}

static void alog_push(Room* rm, uint8_t b) {
    if (rm->alog_len < ALOG_CAP) rm->alog[rm->alog_len++] = b;
    else rm->alog_full = true;
}

static void touch(Srv* s, Room* rm) {
    rm->rev++;
    s->dirty = true;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool name_blocked(const char* nm) {
    char lo[NAME_CAP];
    size_t n = 0;
    for (; nm[n] && n + 1 < sizeof lo; n++) lo[n] = lower(nm[n]);
    lo[n] = '\0';
    for (size_t i = 0; i < sizeof BLOCKED / sizeof BLOCKED[0]; i++) {
        if (strstr(lo, BLOCKED[i])) return true;
    }
    return false;
}

static bool name_eq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) if (lower(*a) != lower(*b)) return false;
    return *a == *b;
}

// --- Per-IP limiters ---------------------------------------------------------
static IpCap* ip_bucket(IpCap* table, const char* ip, int64_t now, int64_t window) {
    uint32_t h = 2166136261u;
    for (const char* p = ip; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    IpCap* c = &table[h & 255];
    if (strcmp(c->ip, ip) != 0 || now - c->window_ms > window) {
        snprintf(c->ip, sizeof c->ip, "%s", ip);
        c->window_ms = now;
        c->count = 0;
    }
    return c;
}

static bool ip_allow_create(Srv* s, const char* ip, int64_t now) {
    IpCap* c = ip_bucket(s->ipcap, ip, now, IPCAP_WINDOW_MS);
    if (c->count >= IPCAP_MAX) return false;
    c->count++;
    return true;
}

// Wrong-code guessing: once an IP has missed MISS_MAX codes in the window,
// every code lookup (right or wrong) is refused until the window passes.
static bool ip_code_blocked(Srv* s, const char* ip, int64_t now) {
    return ip_bucket(s->misses, ip, now, MISS_WINDOW_MS)->count >= MISS_MAX;
}
static void ip_code_miss(Srv* s, const char* ip, int64_t now) {
    ip_bucket(s->misses, ip, now, MISS_WINDOW_MS)->count++;
}

// --- Messages ----------------------------------------------------------------
static void jw_names(JW* w, const Room* rm) {
    jw_f(w, "\"names\":[");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        jw_f(w, "%s\"%s\"", i ? "," : "", rm->m[i].used ? rm->m[i].name : "");
    }
    jw_f(w, "]");
}

// Why the waiting room can't start right now ("" when it can).
static const char* lobby_block(const Room* rm, int* present_out) {
    int present = 0, ready = 0, team[2] = {0, 0};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Member* mb = &rm->m[i];
        if (!mb->used || !mb->connected) continue;
        present++;
        ready += mb->ready;
        team[mb->team & 1]++;
    }
    if (present_out) *present_out = present;
    if (present < 2) return "need2";
    if (rm->rules.partners && present != 4) return "partners4";
    if (rm->rules.partners && (team[0] != 2 || team[1] != 2)) return "teams";
    if (ready < present) return "ready";
    return "";
}

static void emit_lobby(Srv* s, Room* rm, int r, int mi, int64_t now) {
    Member* me = &rm->m[mi];
    if (!me->used || !me->connected) return;
    char buf[MSG_BUF];
    JW w;
    jw_init(&w, buf, sizeof buf);
    int64_t cd = rm->countdown_at ? rm->countdown_at - now : -1;
    if (rm->countdown_at && cd < 0) cd = 0;
    jw_f(&w, "{\"t\":\"lobby\",\"v\":%d,\"id\":%d,\"code\":\"%s\",\"private\":%d,"
             "\"target\":%d,\"bonus\":%d,\"partners\":%d,\"away\":%d,\"me\":%d,",
        SRV_PROTO_VERSION, room_id(s, r), rm->code, rm->kind == ROOM_PRIVATE,
        rm->rules.target_score, rm->rules.bonus_scoring ? 1 : 0,
        rm->rules.partners ? 1 : 0, rm->away_policy, mi);
    jw_names(&w, rm);
    jw_f(&w, ",\"used\":[");
    for (int i = 0; i < MAX_PLAYERS; i++) jw_f(&w, "%s%d", i ? "," : "", rm->m[i].used);
    jw_f(&w, "],\"ready\":[");
    for (int i = 0; i < MAX_PLAYERS; i++) jw_f(&w, "%s%d", i ? "," : "", rm->m[i].ready);
    jw_f(&w, "],\"team\":[");
    for (int i = 0; i < MAX_PLAYERS; i++) jw_f(&w, "%s%d", i ? "," : "", rm->m[i].team);
    jw_f(&w, "],\"on\":[");
    for (int i = 0; i < MAX_PLAYERS; i++) jw_f(&w, "%s%d", i ? "," : "", rm->m[i].connected);
    jw_f(&w, "],\"count_ms\":%lld,\"block\":\"%s\",\"force\":%d,\"swap_from\":%d}",
        (long long)cd, lobby_block(rm, NULL), rm->can_force && me->ready ? 1 : 0,
        me->swap_from);
    if (w.overflow) {
        logf_line(s, "BUG lobby overflow room=%d", r);
        return;
    }
    send_text(s, me->client, buf, w.len);
}

static void broadcast_lobby(Srv* s, int r, int64_t now) {
    Room* rm = &s->rooms[r];
    for (int i = 0; i < MAX_PLAYERS; i++) emit_lobby(s, rm, r, i, now);
}

static void send_welcome(Srv* s, int r, int mi, bool owner) {
    Room* rm = &s->rooms[r];
    Member* mb = &rm->m[mi];
    char buf[256];
    JW w;
    jw_init(&w, buf, sizeof buf);
    jw_f(&w, "{\"t\":\"welcome\",\"v\":%d,\"key\":\"%s\",\"id\":%d,\"code\":\"%s\","
             "\"private\":%d,\"playing\":%d",
        SRV_PROTO_VERSION, mb->key, room_id(s, r), rm->code,
        rm->kind == ROOM_PRIVATE, rm->phase == PHASE_PLAYING);
    if (owner) jw_f(&w, ",\"owner\":\"%s\"", rm->owner_key);
    jw_f(&w, "}");
    if (!w.overflow) send_text(s, mb->client, buf, w.len);
}

// One redacted snapshot per seat, emitted FROM the redacted copy so a
// formatting bug can never leak what game_redact_for already removed.
static void emit_state(Srv* s, Room* rm, int seat, int last_seat, int last_a,
                       int last_slot, int last_card, int64_t now) {
    Member* mb = seat_mem(rm, seat);
    if (!mb || !mb->connected) return;
    Game red;
    game_redact_for(&red, &rm->g, seat);

    char buf[MSG_BUF];
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
    // ctl: 0 = a human is playing the seat, 1 = away (AI standing in or the
    // table waiting for them).
    jw_f(&w, "],\"ctl\":[");
    for (int p = 0; p < n; p++) {
        Member* pm = seat_mem(rm, p);
        bool away = !pm || !member_present(pm) || pm->ai_control;
        jw_f(&w, "%s%d", p ? "," : "", away ? 1 : 0);
    }
    jw_f(&w, "],\"handles\":[");
    for (int p = 0; p < n; p++) {
        Member* pm = seat_mem(rm, p);
        jw_f(&w, "%s\"%s\"", p ? "," : "", pm ? pm->name : HANDLES[p]);
    }
    jw_f(&w, "],\"winner\":%d,\"match_winner\":%d,\"recycles\":%d,\"events\":%u,",
        red.round_winner, red.match_winner, red.recycles, red.events);

    if (last_seat >= 0) {
        jw_f(&w, "\"last\":{\"seat\":%d,\"a\":%d,\"slot\":%d,\"card\":%d},",
             last_seat, last_a, last_slot, last_card);
    }

    int64_t wait_ms = -1;
    if (rm->wait_seat >= 0) {
        wait_ms = rm->wait_deadline ? rm->wait_deadline - now : 0;
        if (wait_ms < 0) wait_ms = 0;
    }
    int64_t deadline = 0;
    if (rm->turn_deadline) deadline = rm->turn_deadline - now;
    else if (rm->reveal_deadline) deadline = rm->reveal_deadline - now;
    if (deadline < 0) deadline = 0;
    // wait_timer: 1 when wait_ms counts down, 0 when the table waits
    // indefinitely (the Wait-for-them policy).
    jw_f(&w, "\"wait_seat\":%d,\"wait_ms\":%lld,\"wait_timer\":%d,\"deadline_ms\":%lld}",
         rm->wait_seat, (long long)wait_ms, rm->wait_deadline ? 1 : 0, (long long)deadline);

    if (w.overflow) {
        logf_line(s, "BUG state overflow seat=%d", seat);
        return;
    }
    send_text(s, mb->client, buf, w.len);
}

// last_card: what an observer at the table saw move — the drawn card if it
// was public (discard draw), the discarded/displaced card otherwise, 0 if
// face down. Drives client sounds and flight animation.
static void broadcast_state(Srv* s, Room* rm, int last_seat, int last_a,
                            int last_slot, int last_card, int64_t now) {
    for (int p = 0; p < rm->g.rules.player_count; p++) {
        emit_state(s, rm, p, last_seat, last_a, last_slot, last_card, now);
    }
}

static void send_to_room(Srv* s, Room* rm, const char* text, int skip_member) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == skip_member || !rm->m[i].used || !rm->m[i].connected) continue;
        send_text(s, rm->m[i].client, text, strlen(text));
    }
}

// --- Room lifecycle ----------------------------------------------------------
static void drive_deal(Room* rm) {
    for (int i = 0; i < 4096 && rm->g.phase == PHASE_DEAL; i++) game_update(&rm->g);
}

static void log_match(Srv* s, Room* rm, int r, const char* why) {
    // Audit line: everything needed to replay the match byte-for-byte.
    char hex[ALOG_CAP * 2 + 1];
    for (int i = 0; i < rm->alog_len; i++) {
        snprintf(hex + i * 2, 3, "%02x", rm->alog[i]);
    }
    hex[rm->alog_len * 2] = '\0';
    logf_line(s, "match room=%d why=%s private=%d players=%d seed=%llu actions=%u "
                 "scores=%d,%d,%d,%d alog=%s%s",
        r, why, rm->kind == ROOM_PRIVATE, rm->g.rules.player_count,
        (unsigned long long)rm->g.rules.seed, rm->actions,
        rm->g.players[0].score, rm->g.players[1].score,
        rm->g.players[2].score, rm->g.players[3].score,
        hex, rm->alog_full ? "..." : "");
}

static void client_unbind(Srv* s, int client) {
    if (!client_ok(s, client)) return;
    Client* c = &s->clients[client];
    c->state = CL_IDLE;
    c->room = c->member = -1;
}

static void delete_room(Srv* s, int r) {
    Room* rm = &s->rooms[r];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (rm->m[i].used && rm->m[i].connected) {
            static const char CLOSED[] = "{\"t\":\"removed\",\"why\":\"closed\"}";
            send_text(s, rm->m[i].client, CLOSED, sizeof CLOSED - 1);
            client_unbind(s, rm->m[i].client);
        }
    }
    uint16_t gen = rm->gen;
    memset(rm, 0, sizeof *rm);
    rm->gen = (uint16_t)(gen + 1);
    s->dirty = true;
}

// Take a member out of a room (lobby leave, removal, end of grace, forfeit
// sweep). Deletes a public room the moment its waiting room is empty.
static void remove_member(Srv* s, int r, int mi, const char* why, int64_t now) {
    Room* rm = &s->rooms[r];
    Member* mb = &rm->m[mi];
    if (!mb->used) return;
    if (mb->connected) {
        if (why) {
            char buf[64];
            snprintf(buf, sizeof buf, "{\"t\":\"removed\",\"why\":\"%s\"}", why);
            send_text(s, mb->client, buf, strlen(buf));
        }
        client_unbind(s, mb->client);
    }
    memset(mb, 0, sizeof *mb);
    mb->seat = -1;
    mb->swap_from = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (rm->m[i].swap_from == mi) rm->m[i].swap_from = -1;
    }
    touch(s, rm);
    if (rm->phase == PHASE_LOBBY && member_count(rm) == 0 && rm->kind == ROOM_PUBLIC) {
        delete_room(s, r);
        return;
    }
    if (rm->phase == PHASE_LOBBY) broadcast_lobby(s, r, now);
}

// Arm whichever clock the current phase needs. The engine's own pacing/anim
// is presentation and plays no part here: the server never calls game_update
// outside the deal skip, so engine AI never fires on its own.
static void schedule(Srv* s, Room* rm, int64_t now) {
    (void)s;
    rm->turn_deadline = 0;
    rm->ai_due = 0;
    if (rm->phase != PHASE_PLAYING) return;

    // Nobody present: the match is paused. No clocks, no AI moves; the first
    // member back re-arms everything.
    if (!any_present(rm)) {
        rm->wait_seat = -1;
        rm->wait_deadline = 0;
        if (!rm->empty_since) rm->empty_since = now;
        return;
    }
    rm->empty_since = 0;

    if (rm->g.phase == PHASE_DRAW || rm->g.phase == PHASE_PLACE) {
        rm->reveal_deadline = 0;
        int seat = rm->g.turn;
        Member* mb = seat_mem(rm, seat);
        if (mb && member_present(mb) && !mb->ai_control) {
            rm->wait_seat = -1;
            rm->wait_deadline = 0;
            rm->turn_deadline = now + SRV_TURN_MS;
        } else if (!mb || mb->ai_control) {
            rm->wait_seat = -1;
            rm->wait_deadline = 0;
            rm->ai_due = now + SRV_AI_PACE_MS;
        } else {
            // An absent player's turn: wait for them (once per absence — a
            // re-schedule from some other seat's event must not restart it).
            if (rm->wait_seat != seat) {
                rm->wait_seat = (int8_t)seat;
                bool wait_forever = rm->kind == ROOM_PRIVATE && rm->away_policy == AWAY_WAIT;
                rm->wait_deadline = wait_forever ? 0 : now + SRV_AWAY_WAIT_MS;
            }
        }
    } else if (rm->g.phase == PHASE_ROUND_OVER) {
        rm->wait_seat = -1;
        rm->wait_deadline = 0;
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
            for (int i = 0; i < MAX_PLAYERS; i++) rm->m[i].confirmed = false;
        }
    } else {
        rm->wait_seat = -1;
        rm->wait_deadline = 0;
        rm->reveal_deadline = 0;
    }
}

// Back to the waiting room after a match: same members, everyone not ready.
static void return_to_lobby(Srv* s, int r, int64_t now) {
    Room* rm = &s->rooms[r];
    rm->phase = PHASE_LOBBY;
    rm->turn_deadline = rm->ai_due = rm->reveal_deadline = rm->wait_deadline = 0;
    rm->wait_seat = -1;
    rm->empty_since = 0;
    rm->countdown_at = rm->idle_since = 0;
    rm->can_force = false;
    rm->last_used_ms = now;
    for (int i = 0; i < MAX_PLAYERS; i++) rm->seat_member[i] = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Member* mb = &rm->m[i];
        if (!mb->used) continue;
        if (mb->forfeited) {
            memset(mb, 0, sizeof *mb);
            mb->seat = -1;
            mb->swap_from = -1;
            continue;
        }
        mb->ready = false;
        mb->seat = -1;
        mb->away = false;
        mb->ai_control = false;
        mb->strikes = 0;
        mb->confirmed = false;
        mb->swap_from = -1;
        if (!mb->connected) mb->gone_ms = now;   // the lobby grace applies
    }
    touch(s, rm);
    if (member_count(rm) == 0 && rm->kind == ROOM_PUBLIC) {
        delete_room(s, r);
        return;
    }
    broadcast_lobby(s, r, now);
}

static void start_match(Srv* s, int r, int64_t now) {
    Room* rm = &s->rooms[r];
    // Members who dropped from the waiting room don't get dealt in.
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (rm->m[i].used && !rm->m[i].connected) remove_member(s, r, i, NULL, now);
    }
    if (!s->rooms[r].active) return;

    // Seats: join order; under Partners team A takes 0 and 2, team B 1 and 3,
    // so partners sit opposite.
    int order[MAX_PLAYERS], n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) rm->seat_member[i] = -1;
    if (rm->rules.partners) {
        int a = 0, b = 1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!rm->m[i].used) continue;
            int seat = rm->m[i].team ? b : a;
            if (rm->m[i].team) b += 2; else a += 2;
            if (seat < MAX_PLAYERS) rm->seat_member[seat] = (int8_t)i;
            n++;
        }
    } else {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (rm->m[i].used) order[n++] = i;
        }
        for (int k = 0; k < n; k++) rm->seat_member[k] = (int8_t)order[k];
    }

    Rules rules = rm->rules;
    rules.player_count = n;
    rules.human_seat = 0;        // engine-side flag; the server drives all AI itself
    rules.ai_difficulty = 1;     // stand-ins play at the middle tier
    rules.seed = rng_next64(&s->rng);
    if (rules.seed == 0) rules.seed = 1;
    rules_normalize(&rules);

    for (int seat = 0; seat < n; seat++) {
        Member* mb = seat_mem(rm, seat);
        if (!mb) continue;
        mb->seat = (int8_t)seat;
        mb->ready = false;
        mb->away = false;
        mb->ai_control = false;
        mb->forfeited = false;
        mb->strikes = 0;
        mb->confirmed = false;
        mb->swap_from = -1;
    }

    rm->phase = PHASE_PLAYING;
    rm->countdown_at = rm->idle_since = 0;
    rm->can_force = false;
    rm->wait_seat = -1;
    rm->wait_deadline = rm->reveal_deadline = rm->empty_since = 0;
    rm->actions = 0;
    rm->alog_len = 0;
    rm->alog_full = false;
    rm->last_used_ms = now;
    game_init(&rm->g, &rules);
    drive_deal(rm);
    touch(s, rm);
    logf_line(s, "start room=%d private=%d players=%d seed=%llu",
        r, rm->kind == ROOM_PRIVATE, n, (unsigned long long)rules.seed);
    schedule(s, rm, now);
    broadcast_state(s, rm, -1, 0, 0, 0, now);
}

// Re-evaluate the waiting room after any change: arm or cancel the start
// countdown and the idle-not-ready clock, then show everyone the result.
static void lobby_changed(Srv* s, int r, int64_t now) {
    Room* rm = &s->rooms[r];
    if (!rm->active || rm->phase != PHASE_LOBBY) return;
    int present = 0;
    const char* block = lobby_block(rm, &present);
    if (block[0] == '\0') {
        if (!rm->countdown_at) rm->countdown_at = now + SRV_COUNTDOWN_MS;
    } else {
        rm->countdown_at = 0;
    }
    int ready = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (rm->m[i].used && rm->m[i].connected && rm->m[i].ready) ready++;
    }
    bool idle_case = ready >= 2 && ready < present;
    if (idle_case) {
        if (!rm->idle_since) rm->idle_since = now;
    } else {
        rm->idle_since = 0;
        rm->can_force = false;
    }
    touch(s, rm);
    broadcast_lobby(s, r, now);
}

// Apply one action for the seat whose turn it is, log it, broadcast, and
// re-arm the clocks. `card_pub` tells observers what they saw move.
static void applied_action(Srv* s, Room* rm, int seat, Action a, int card_pub,
                           int64_t now) {
    rm->actions++;
    rm->last_used_ms = now;
    alog_push(rm, (uint8_t)((a.type & 0x3) | (a.slot << 2)));
    s->dirty = true;
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

static void advance_round(Srv* s, int r, int64_t now);

// One AI-controlled move for the current actor (a struck-out or absent
// human, or a forfeited seat). Mirrors the engine's own fallback ladder so a
// misbehaving evaluator can never wedge a table.
static void ai_move(Srv* s, int r, int64_t now) {
    Room* rm = &s->rooms[r];
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
        // Unreachable by construction; end the match rather than spin.
        logf_line(s, "BUG ai wedge room=%d phase=%d", r, g->phase);
        log_match(s, rm, r, "wedge");
        return_to_lobby(s, r, now);
    }
}

static void advance_round(Srv* s, int r, int64_t now) {
    Room* rm = &s->rooms[r];
    alog_push(rm, ALOG_NEXT_ROUND);
    game_next_round(&rm->g);
    if (rm->g.phase == PHASE_DEAL) drive_deal(rm);
    s->dirty = true;
    schedule(s, rm, now);
    broadcast_state(s, rm, -1, 0, 0, 0, now);
    if (rm->g.phase == PHASE_MATCH_OVER) {
        log_match(s, rm, r, "finished");
        return_to_lobby(s, r, now);
    }
}

static void maybe_advance(Srv* s, int r, int64_t now) {
    Room* rm = &s->rooms[r];
    for (int p = 0; p < rm->g.rules.player_count; p++) {
        Member* mb = seat_mem(rm, p);
        if (seat_human_active(rm, p) && mb && !mb->confirmed) return;
    }
    advance_round(s, r, now);
}

// A member came back (rejoin, or `here` after stepping away): control returns
// at the next decision, and everyone else is told.
static void member_back(Srv* s, int r, int mi, int64_t now) {
    Room* rm = &s->rooms[r];
    Member* mb = &rm->m[mi];
    bool was_away = mb->away || mb->ai_control;
    mb->away = false;
    mb->ai_control = false;
    mb->strikes = 0;
    if (rm->wait_seat == mb->seat) {
        rm->wait_seat = -1;
        rm->wait_deadline = 0;
    }
    if (was_away) {
        char buf[64];
        snprintf(buf, sizeof buf, "{\"t\":\"back\",\"seat\":%d,\"name\":\"%s\"}",
                 mb->seat, mb->name);
        send_to_room(s, rm, buf, mi);
    }
    s->dirty = true;
    schedule(s, rm, now);
    broadcast_state(s, rm, -1, 0, 0, 0, now);
}

// A member stepped away or dropped mid-match: the seat is kept, and the leave
// policy takes over when their turn comes.
static void member_away(Srv* s, int r, int mi, int64_t now) {
    Room* rm = &s->rooms[r];
    rm->m[mi].away = true;
    s->dirty = true;
    schedule(s, rm, now);
    broadcast_state(s, rm, -1, 0, 0, 0, now);
}

// Display name unique within the room: "Dan", "Dan2", "Dan3". `skip` is the
// new member's own slot, which must not count as a clash with itself.
static void unique_name(const Room* rm, int skip, const char* want, char out[NAME_CAP]) {
    char cand[NAME_CAP];
    snprintf(cand, sizeof cand, "%s", want);
    for (int k = 2; k <= MAX_PLAYERS + 1; k++) {
        bool clash = false;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i != skip && rm->m[i].used && name_eq(rm->m[i].name, cand)) clash = true;
        }
        if (!clash) break;
        size_t base = strlen(want);
        if (base > NAME_CAP - 2) base = NAME_CAP - 2;   // room for one digit
        memcpy(cand, want, base);
        cand[base] = (char)('0' + k);
        cand[base + 1] = '\0';
    }
    memcpy(out, cand, NAME_CAP);
}

// Seat `client` in room `r` as a new member. Returns the member index or -1.
static int add_member(Srv* s, int r, int client, int64_t now) {
    Room* rm = &s->rooms[r];
    int mi = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!rm->m[i].used) { mi = i; break; }
    }
    if (mi < 0) return -1;
    Client* c = &s->clients[client];
    Member* mb = &rm->m[mi];
    memset(mb, 0, sizeof *mb);
    mb->used = true;
    mb->connected = true;
    mb->client = client;
    mb->seat = -1;
    mb->swap_from = -1;
    make_token(s, mb->key);
    unique_name(rm, mi, c->name, mb->name);
    // Partners: arrivals join the smaller team.
    int team[2] = {0, 0};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i != mi && rm->m[i].used) team[rm->m[i].team & 1]++;
    }
    mb->team = (team[1] < team[0]) ? 1 : 0;
    c->state = CL_ROOM;
    c->room = r;
    c->member = mi;
    c->listing = false;
    (void)now;
    return mi;
}

// Find a member by key across all rooms.
static bool find_key(Srv* s, const char* key, int* r_out, int* mi_out) {
    if (!key || strlen(key) != 32) return false;
    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        Room* rm = &s->rooms[r];
        if (!rm->active) continue;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (rm->m[i].used && rm->m[i].key[0] && strcmp(rm->m[i].key, key) == 0) {
                *r_out = r;
                *mi_out = i;
                return true;
            }
        }
    }
    return false;
}

// Joining or creating another room surrenders any seat this device held:
// gone from a waiting room, forfeited mid-match.
static void surrender_key(Srv* s, const char* key, int64_t now) {
    int r, mi;
    if (!find_key(s, key, &r, &mi)) return;
    Room* rm = &s->rooms[r];
    Member* mb = &rm->m[mi];
    if (mb->connected && client_ok(s, mb->client)) {
        client_unbind(s, mb->client);
        s->io.kick(s->io.ud, mb->client);
        mb->connected = false;
        mb->client = -1;
    }
    if (rm->phase == PHASE_LOBBY) {
        remove_member(s, r, mi, NULL, now);
        if (s->rooms[r].active) lobby_changed(s, r, now);
    } else {
        mb->forfeited = true;
        mb->ai_control = true;
        mb->away = true;
        mb->key[0] = '\0';
        s->dirty = true;
        schedule(s, rm, now);
        broadcast_state(s, rm, -1, 0, 0, 0, now);
    }
}

// --- Room list ---------------------------------------------------------------
static bool room_joinable(const Room* rm, int target) {
    if (!rm->active || rm->kind != ROOM_PUBLIC || rm->phase != PHASE_LOBBY) return false;
    int n = member_count(rm);
    if (n < 1 || n >= MAX_PLAYERS) return false;
    return target == 0 || rm->rules.target_score == target;
}

static int room_ready_count(const Room* rm) {
    int k = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) k += rm->m[i].used && rm->m[i].ready;
    return k;
}

static void push_rooms(Srv* s, int client, int64_t now) {
    Client* c = &s->clients[client];
    // Fullest first, for scanning; membership of the sample stays stable.
    int idx[SRV_LIST_SHOW], n = 0;
    for (int i = 0; i < SRV_LIST_SHOW; i++) if (c->shown[i] >= 0) idx[n++] = c->shown[i];
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && member_count(&s->rooms[idx[j]]) < member_count(&s->rooms[v])) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = v;
    }
    char buf[MSG_BUF];
    JW w;
    jw_init(&w, buf, sizeof buf);
    jw_f(&w, "{\"t\":\"rooms\",\"total\":%d,\"target\":%d,\"ids\":[", c->shown_total,
         c->list_target);
    for (int i = 0; i < n; i++) jw_f(&w, "%s%d", i ? "," : "", room_id(s, idx[i]));
    jw_f(&w, "],\"targets\":[");
    for (int i = 0; i < n; i++) jw_f(&w, "%s%d", i ? "," : "", s->rooms[idx[i]].rules.target_score);
    jw_f(&w, "],\"bonus\":[");
    for (int i = 0; i < n; i++) jw_f(&w, "%s%d", i ? "," : "", s->rooms[idx[i]].rules.bonus_scoring ? 1 : 0);
    jw_f(&w, "],\"partners\":[");
    for (int i = 0; i < n; i++) jw_f(&w, "%s%d", i ? "," : "", s->rooms[idx[i]].rules.partners ? 1 : 0);
    jw_f(&w, "],\"names\":[");
    for (int i = 0; i < n; i++) {
        const Room* rm = &s->rooms[idx[i]];
        jw_f(&w, "%s[", i ? "," : "");
        int k = 0;
        for (int m = 0; m < MAX_PLAYERS; m++) {
            if (rm->m[m].used) jw_f(&w, "%s\"%s\"", k++ ? "," : "", rm->m[m].name);
        }
        jw_f(&w, "]");
    }
    jw_f(&w, "],\"ready\":[");
    for (int i = 0; i < n; i++) {
        const Room* rm = &s->rooms[idx[i]];
        jw_f(&w, "%s[", i ? "," : "");
        int k = 0;
        for (int m = 0; m < MAX_PLAYERS; m++) {
            if (rm->m[m].used) jw_f(&w, "%s%d", k++ ? "," : "", rm->m[m].ready);
        }
        jw_f(&w, "]");
    }
    jw_f(&w, "]}");
    if (w.overflow) {
        logf_line(s, "BUG rooms overflow");
        return;
    }
    send_text(s, client, buf, w.len);
    c->last_push_ms = now;
    c->list_force = false;
    c->list_pending = false;
}

// Refresh one viewer's sample against this tick's joinable rooms: drop what
// stopped being joinable, refill by weighted random (fuller rooms likelier),
// and push when anything they can see changed.
static void refresh_list(Srv* s, int client, int njoin, int64_t now) {
    Client* c = &s->clients[client];
    bool changed = c->list_force;
    int target = c->list_target;
    int total = 0;
    for (int k = 0; k < njoin; k++) {
        if (target == 0 || s->rooms[s->joinable[k]].rules.target_score == target) total++;
    }
    if (total != c->shown_total) { c->shown_total = total; changed = true; }

    for (int i = 0; i < SRV_LIST_SHOW; i++) {
        int r = c->shown[i];
        if (r < 0) continue;
        if (!room_joinable(&s->rooms[r], target) || s->rooms[r].gen != c->shown_gen[i]) {
            c->shown[i] = -1;
            changed = true;
        } else if (s->rooms[r].rev != c->shown_rev[i]) {
            c->shown_rev[i] = s->rooms[r].rev;
            changed = true;
        }
    }
    for (int i = 0; i < SRV_LIST_SHOW; i++) {
        if (c->shown[i] >= 0) continue;
        // Weighted pick among joinable rooms not already shown.
        uint64_t sum = 0;
        for (int k = 0; k < njoin; k++) {
            int r = s->joinable[k];
            if (target && s->rooms[r].rules.target_score != target) continue;
            bool dup = false;
            for (int j = 0; j < SRV_LIST_SHOW && !dup; j++) dup = c->shown[j] == r;
            if (dup) continue;
            int m = member_count(&s->rooms[r]);
            sum += (uint64_t)(m * m + 1);
        }
        if (sum == 0) break;
        uint64_t pick = rng_next64(&s->rng) % sum;
        for (int k = 0; k < njoin; k++) {
            int r = s->joinable[k];
            if (target && s->rooms[r].rules.target_score != target) continue;
            bool dup = false;
            for (int j = 0; j < SRV_LIST_SHOW && !dup; j++) dup = c->shown[j] == r;
            if (dup) continue;
            int m = member_count(&s->rooms[r]);
            uint64_t wgt = (uint64_t)(m * m + 1);
            if (pick < wgt) {
                c->shown[i] = (int16_t)r;
                c->shown_gen[i] = s->rooms[r].gen;
                c->shown_rev[i] = s->rooms[r].rev;
                changed = true;
                break;
            }
            pick -= wgt;
        }
    }
    if (changed) c->list_pending = true;
    if (c->list_pending && (c->list_force || now - c->last_push_ms >= SRV_LIST_PUSH_MS)) {
        push_rooms(s, client, now);
    }
}

static void list_reset(Client* c) {
    for (int i = 0; i < SRV_LIST_SHOW; i++) c->shown[i] = -1;
    c->shown_total = -1;
    c->list_force = true;
}

static void process_lists(Srv* s, int64_t now) {
    bool any = false;
    for (int i = 0; i < SRV_MAX_CLIENTS && !any; i++) any = s->clients[i].used && s->clients[i].listing;
    if (!any) return;
    int njoin = 0;
    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        if (room_joinable(&s->rooms[r], 0)) s->joinable[njoin++] = (int16_t)r;
    }
    for (int i = 0; i < SRV_MAX_CLIENTS; i++) {
        if (s->clients[i].used && s->clients[i].listing) refresh_list(s, i, njoin, now);
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
    c->room = c->member = -1;
    c->tokens = RATE_BURST;
    list_reset(c);
    c->list_force = false;
    snprintf(c->ip, sizeof c->ip, "%s", ip ? ip : "?");
}

void srv_client_gone(Srv* s, int client, int64_t now_ms) {
    if (!client_ok(s, client)) return;
    Client* c = &s->clients[client];
    if (c->state == CL_ROOM && c->room >= 0 && s->rooms[c->room].active) {
        int r = c->room, mi = c->member;
        Room* rm = &s->rooms[r];
        Member* mb = &rm->m[mi];
        if (mb->used && mb->client == client) {
            mb->connected = false;
            mb->client = -1;
            if (rm->phase == PHASE_LOBBY) {
                mb->gone_ms = now_ms;
                mb->ready = false;
                memset(c, 0, sizeof *c);
                lobby_changed(s, r, now_ms);
                return;
            }
            memset(c, 0, sizeof *c);
            member_away(s, r, mi, now_ms);
            return;
        }
    }
    memset(c, 0, sizeof *c);
}

// Bind `client` to its member by key. Returns true on success.
static bool try_rejoin(Srv* s, int client, const char* key, int64_t now) {
    int r, mi;
    if (!find_key(s, key, &r, &mi)) return false;
    Room* rm = &s->rooms[r];
    Member* mb = &rm->m[mi];
    if (mb->connected && client_ok(s, mb->client) && mb->client != client) {
        // The key holder reconnected elsewhere: the new socket wins.
        client_unbind(s, mb->client);
        s->io.kick(s->io.ud, mb->client);
    }
    mb->connected = true;
    mb->client = client;
    mb->gone_ms = 0;
    Client* c = &s->clients[client];
    c->state = CL_ROOM;
    c->room = r;
    c->member = mi;
    c->listing = false;
    send_welcome(s, r, mi, false);
    if (rm->phase == PHASE_LOBBY) {
        lobby_changed(s, r, now);
    } else {
        member_back(s, r, mi, now);
    }
    return true;
}

static int parse_target(long t) {
    return (t >= 50 && t <= 1000) ? (int)t : 500;
}

void srv_client_msg(Srv* s, int client, const char* text, size_t len, int64_t now) {
    if (!client_ok(s, client)) return;
    Client* c = &s->clients[client];
    c->last_msg_ms = now;

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
        const char* nm = wire_str(kv, n, "name");
        if (!nm || !nm[0]) { send_error(s, client, "need_name"); return; }
        if (name_blocked(nm)) { send_error(s, client, "bad_name"); return; }
        snprintf(c->name, sizeof c->name, "%.15s", nm);
        c->state = CL_IDLE;
        c->room = c->member = -1;
        char buf[40];
        int m = snprintf(buf, sizeof buf, "{\"t\":\"hi\",\"v\":%d}", SRV_PROTO_VERSION);
        send_text(s, client, buf, (size_t)m);
        return;
    }

    if (strcmp(t, "ping") == 0) {
        send_text(s, client, "{\"t\":\"pong\"}", 12);
        return;
    }

    // --- Out of a room: browse, create, join, rejoin -----------------------
    if (c->state == CL_IDLE) {
        if (strcmp(t, "list") == 0 || strcmp(t, "shuffle") == 0) {
            c->listing = true;
            c->list_target = (int)wire_int(kv, n, "target", 0);
            if (c->list_target < 0) c->list_target = 0;
            list_reset(c);
            process_lists(s, now);
            return;
        }
        if (strcmp(t, "unlist") == 0) { c->listing = false; return; }

        if (strcmp(t, "peek") == 0) {
            const char* code = wire_str(kv, n, "code");
            if (ip_code_blocked(s, c->ip, now)) { send_error(s, client, "rate"); return; }
            int r = find_room_by_code(s, code);
            char buf[MSG_BUF];
            JW w;
            jw_init(&w, buf, sizeof buf);
            if (r < 0) {
                ip_code_miss(s, c->ip, now);
                jw_f(&w, "{\"t\":\"peeked\",\"code\":\"%s\",\"ok\":0}", code ? code : "");
            } else {
                Room* rm = &s->rooms[r];
                jw_f(&w, "{\"t\":\"peeked\",\"code\":\"%s\",\"ok\":1,\"playing\":%d,"
                         "\"target\":%d,", rm->code, rm->phase == PHASE_PLAYING,
                     rm->rules.target_score);
                jw_names(&w, rm);
                jw_f(&w, ",\"ready\":[");
                for (int i = 0; i < MAX_PLAYERS; i++) jw_f(&w, "%s%d", i ? "," : "", rm->m[i].ready);
                jw_f(&w, "]}");
            }
            if (!w.overflow) send_text(s, client, buf, w.len);
            return;
        }

        if (strcmp(t, "rejoin") == 0) {
            if (!try_rejoin(s, client, wire_str(kv, n, "key"), now)) {
                send_error(s, client, "no_seat");
            }
            return;
        }

        if (strcmp(t, "create") == 0) {
            if (!ip_allow_create(s, c->ip, now)) { send_error(s, client, "rate"); return; }
            int r = -1;
            for (int i = 0; i < SRV_MAX_ROOMS; i++) {
                if (!s->rooms[i].active) { r = i; break; }
            }
            if (r < 0) { send_error(s, client, "full"); return; }
            surrender_key(s, wire_str(kv, n, "key"), now);
            Room* rm = &s->rooms[r];
            uint16_t gen = rm->gen;
            memset(rm, 0, sizeof *rm);
            rm->gen = gen;
            rm->active = true;
            rm->phase = PHASE_LOBBY;
            rm->kind = wire_int(kv, n, "private", 0) ? ROOM_PRIVATE : ROOM_PUBLIC;
            rm->away_policy = (rm->kind == ROOM_PRIVATE && wire_int(kv, n, "away", 0)) ? AWAY_WAIT : AWAY_AI;
            rm->wait_seat = -1;
            rm->created_ms = rm->last_used_ms = now;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                rm->seat_member[i] = -1;
                rm->m[i].seat = -1;
                rm->m[i].swap_from = -1;
            }
            if (rm->kind == ROOM_PRIVATE) make_code(s, rm->code);
            make_token(s, rm->owner_key);
            rm->rules = rules_default();
            rm->rules.bonus_scoring = wire_int(kv, n, "bonus", 0) != 0;
            rm->rules.partners = wire_int(kv, n, "partners", 0) != 0;
            rm->rules.target_score = parse_target(wire_int(kv, n, "target", 500));
            int mi = add_member(s, r, client, now);
            logf_line(s, "create room=%d private=%d", r, rm->kind == ROOM_PRIVATE);
            send_welcome(s, r, mi, true);
            lobby_changed(s, r, now);
            return;
        }

        if (strcmp(t, "join") == 0 || strcmp(t, "best") == 0) {
            int r = -1;
            if (strcmp(t, "best") == 0) {
                int target = (int)wire_int(kv, n, "target", 0);
                int best_n = -1, best_ready = -1;
                int64_t best_ms = 0;
                for (int i = 0; i < SRV_MAX_ROOMS; i++) {
                    const Room* rm = &s->rooms[i];
                    if (!room_joinable(rm, target < 0 ? 0 : target)) continue;
                    int m = member_count(rm), rd = room_ready_count(rm);
                    if (m > best_n || (m == best_n && rd > best_ready) ||
                        (m == best_n && rd == best_ready && rm->created_ms > best_ms)) {
                        r = i; best_n = m; best_ready = rd; best_ms = rm->created_ms;
                    }
                }
                if (r < 0) { send_error(s, client, "no_room"); return; }
            } else if (wire_str(kv, n, "code")) {
                if (ip_code_blocked(s, c->ip, now)) { send_error(s, client, "rate"); return; }
                r = find_room_by_code(s, wire_str(kv, n, "code"));
                if (r < 0) {
                    ip_code_miss(s, c->ip, now);
                    send_error(s, client, "no_room");
                    return;
                }
            } else {
                r = room_from_id(s, wire_int(kv, n, "room", -1));
                if (r < 0 || s->rooms[r].kind != ROOM_PUBLIC) {
                    send_error(s, client, "no_room");
                    return;
                }
            }
            Room* rm = &s->rooms[r];
            if (rm->phase != PHASE_LOBBY) { send_error(s, client, "started"); return; }
            if (member_count(rm) >= MAX_PLAYERS) { send_error(s, client, "full"); return; }
            surrender_key(s, wire_str(kv, n, "key"), now);
            if (!s->rooms[r].active) { send_error(s, client, "no_room"); return; }
            int mi = add_member(s, r, client, now);
            if (mi < 0) { send_error(s, client, "full"); return; }
            send_welcome(s, r, mi, false);
            lobby_changed(s, r, now);
            return;
        }

        send_error(s, client, "not_in_room");
        return;
    }

    // --- In a room ------------------------------------------------------------
    int r = c->room, mi = c->member;
    if (r < 0 || r >= SRV_MAX_ROOMS || !s->rooms[r].active ||
        mi < 0 || mi >= MAX_PLAYERS || !s->rooms[r].m[mi].used) {
        client_unbind(s, client);
        send_error(s, client, "not_in_room");
        return;
    }
    Room* rm = &s->rooms[r];
    Member* me = &rm->m[mi];

    if (rm->phase == PHASE_LOBBY) {
        if (strcmp(t, "leave") == 0) {
            remove_member(s, r, mi, NULL, now);
            if (s->rooms[r].active) lobby_changed(s, r, now);
            return;
        }
        if (strcmp(t, "ready") == 0) {
            me->ready = wire_int(kv, n, "on", 1) != 0;
            lobby_changed(s, r, now);
            return;
        }
        if (strcmp(t, "team") == 0 && rm->rules.partners) {
            int side = wire_int(kv, n, "side", 0) ? 1 : 0;
            if (me->team == side) return;
            int count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) count += rm->m[i].used && rm->m[i].team == side;
            if (count >= 2) { send_error(s, client, "team_full"); return; }
            me->team = (uint8_t)side;
            me->ready = false;
            lobby_changed(s, r, now);
            return;
        }
        if (strcmp(t, "swap") == 0 && rm->rules.partners) {
            int other = (int)wire_int(kv, n, "seat", -1);
            if (other < 0 || other >= MAX_PLAYERS || !rm->m[other].used ||
                rm->m[other].team == me->team) {
                send_error(s, client, "bad_msg");
                return;
            }
            rm->m[other].swap_from = (int8_t)mi;
            lobby_changed(s, r, now);
            return;
        }
        if ((strcmp(t, "swap_ok") == 0 || strcmp(t, "swap_no") == 0) && rm->rules.partners) {
            int from = (int)wire_int(kv, n, "seat", -1);
            if (from < 0 || from >= MAX_PLAYERS || me->swap_from != from || !rm->m[from].used) {
                me->swap_from = -1;
                lobby_changed(s, r, now);
                return;
            }
            me->swap_from = -1;
            if (strcmp(t, "swap_ok") == 0 && rm->m[from].team != me->team) {
                uint8_t tm = me->team;
                me->team = rm->m[from].team;
                rm->m[from].team = tm;
                me->ready = false;
                rm->m[from].ready = false;
            }
            lobby_changed(s, r, now);
            return;
        }
        if (strcmp(t, "force") == 0) {
            if (!rm->can_force || !me->ready) return;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (rm->m[i].used && !rm->m[i].ready) remove_member(s, r, i, "idle", now);
            }
            if (s->rooms[r].active) {
                rm->can_force = false;
                lobby_changed(s, r, now);
            }
            return;
        }
        if (strcmp(t, "settings") == 0) {
            const char* ok = wire_str(kv, n, "owner");
            if (!ok || strcmp(ok, rm->owner_key) != 0) { send_error(s, client, "not_owner"); return; }
            rm->rules.target_score = parse_target(wire_int(kv, n, "target", rm->rules.target_score));
            rm->rules.bonus_scoring = wire_int(kv, n, "bonus", rm->rules.bonus_scoring) != 0;
            bool partners = wire_int(kv, n, "partners", rm->rules.partners) != 0;
            if (partners && !rm->rules.partners) {
                // Turning Partners on: balance teams in join order.
                int k = 0;
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (rm->m[i].used) rm->m[i].team = (uint8_t)(k++ & 1);
                }
            }
            rm->rules.partners = partners;
            long priv = wire_int(kv, n, "private", rm->kind == ROOM_PRIVATE);
            rm->kind = priv ? ROOM_PRIVATE : ROOM_PUBLIC;
            if (rm->kind == ROOM_PRIVATE && !rm->code[0]) make_code(s, rm->code);
            if (rm->kind == ROOM_PUBLIC) rm->code[0] = '\0';
            rm->away_policy = (rm->kind == ROOM_PRIVATE && wire_int(kv, n, "away", rm->away_policy))
                              ? AWAY_WAIT : AWAY_AI;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                rm->m[i].ready = false;
                rm->m[i].swap_from = -1;
            }
            lobby_changed(s, r, now);
            return;
        }
        if (strcmp(t, "here") == 0 || strcmp(t, "next") == 0) return;   // harmless
        send_error(s, client, "bad_msg");
        return;
    }

    // --- Playing --------------------------------------------------------------
    if (strcmp(t, "leave") == 0) {
        member_away(s, r, mi, now);
        return;
    }
    if (strcmp(t, "forfeit") == 0) {
        me->forfeited = true;
        me->ai_control = true;
        me->away = true;
        me->key[0] = '\0';
        me->connected = false;
        me->client = -1;
        client_unbind(s, client);
        s->dirty = true;
        schedule(s, rm, now);
        broadcast_state(s, rm, -1, 0, 0, 0, now);
        return;
    }
    if (strcmp(t, "letai") == 0) {
        int seat = (int)wire_int(kv, n, "seat", -1);
        Member* am = seat_mem(rm, seat);
        if (seat == rm->wait_seat && am) {
            am->ai_control = true;
            rm->wait_seat = -1;
            rm->wait_deadline = 0;
            schedule(s, rm, now);
            broadcast_state(s, rm, -1, 0, 0, 0, now);
        }
        return;
    }

    // Any live message from the seat's owner reclaims it.
    if (me->away || me->ai_control) {
        if (!me->forfeited) member_back(s, r, mi, now);
    } else {
        me->strikes = 0;
    }
    if (strcmp(t, "here") == 0) return;

    if (strcmp(t, "action") == 0) {
        if (rm->g.turn != me->seat ||
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
        applied_action(s, rm, me->seat, a, card, now);
        return;
    }

    if (strcmp(t, "next") == 0) {
        if (rm->g.phase != PHASE_ROUND_OVER) return;
        me->confirmed = true;
        maybe_advance(s, r, now);
        return;
    }

    send_error(s, client, "bad_msg");
}

void srv_tick(Srv* s, int64_t now) {
    if (!s->boot_set) { s->boot_set = true; s->boot_ms = now; }

    for (int i = 0; i < SRV_MAX_CLIENTS; i++) {
        Client* c = &s->clients[i];
        if (!c->used) continue;
        if (!c->last_msg_ms) c->last_msg_ms = now;
        else if (now - c->last_msg_ms > SRV_SILENCE_MS) {
            s->io.kick(s->io.ud, i);
            c->last_msg_ms = now;   // one kick per silence window
        }
    }

    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        Room* rm = &s->rooms[r];
        if (!rm->active) continue;

        if (rm->phase == PHASE_LOBBY) {
            // Waiting-room members whose link dropped get a short grace.
            for (int i = 0; i < MAX_PLAYERS && rm->active; i++) {
                Member* mb = &rm->m[i];
                if (mb->used && !mb->connected && mb->gone_ms &&
                    now - mb->gone_ms > SRV_LOBBY_GRACE_MS) {
                    remove_member(s, r, i, NULL, now);
                    if (rm->active) lobby_changed(s, r, now);
                }
            }
            if (!rm->active) continue;
            if (rm->kind == ROOM_PRIVATE && member_count(rm) == 0 &&
                now - rm->last_used_ms > SRV_PRIVATE_TTL_MS) {
                logf_line(s, "expire room=%d", r);
                delete_room(s, r);
                continue;
            }
            if (rm->idle_since && !rm->can_force && now - rm->idle_since > SRV_IDLE_READY_MS) {
                if (rm->kind == ROOM_PUBLIC) {
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (rm->m[i].used && rm->m[i].connected && !rm->m[i].ready) {
                            remove_member(s, r, i, "idle", now);
                        }
                    }
                    if (rm->active) lobby_changed(s, r, now);
                } else {
                    rm->can_force = true;
                    broadcast_lobby(s, r, now);
                }
                continue;
            }
            if (rm->countdown_at && now >= rm->countdown_at) {
                if (lobby_block(rm, NULL)[0] == '\0') start_match(s, r, now);
                else rm->countdown_at = 0;
            }
            continue;
        }

        // Playing. Nobody present: paused; a public table is closed after a
        // while, a private one keeps its game within its life.
        if (!any_present(rm)) {
            if (!rm->empty_since) rm->empty_since = now;
            int64_t limit = rm->kind == ROOM_PUBLIC ? SRV_ABANDON_MS : SRV_PRIVATE_TTL_MS;
            if (now - rm->empty_since > limit) {
                log_match(s, rm, r, "abandoned");
                delete_room(s, r);
            }
            continue;
        }

        if (rm->ai_due && now >= rm->ai_due) {
            ai_move(s, r, now);
            continue;   // one move per tick per room keeps AI play legible
        }
        if (rm->turn_deadline && now >= rm->turn_deadline) {
            Member* mb = seat_mem(rm, rm->g.turn);
            if (mb && ++mb->strikes >= STRIKES_TO_AI) mb->ai_control = true;
            ai_move(s, r, now);
            continue;
        }
        if (rm->wait_seat >= 0 && rm->wait_deadline && now >= rm->wait_deadline) {
            Member* mb = seat_mem(rm, rm->wait_seat);
            if (mb) mb->ai_control = true;
            rm->wait_seat = -1;
            rm->wait_deadline = 0;
            schedule(s, rm, now);
            broadcast_state(s, rm, -1, 0, 0, 0, now);
            continue;
        }
        if (rm->reveal_deadline && now >= rm->reveal_deadline &&
            rm->g.phase == PHASE_ROUND_OVER) {
            advance_round(s, r, now);
        }
    }

    process_lists(s, now);
}

void srv_status_json(Srv* s, char* out, size_t cap, int64_t now) {
    int pub = 0, priv = 0, dormant = 0, playing = 0, paused = 0, clients = 0;
    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        const Room* rm = &s->rooms[r];
        if (!rm->active) continue;
        if (rm->kind == ROOM_PUBLIC) pub++; else priv++;
        if (rm->phase == PHASE_PLAYING) {
            if (any_present(rm)) playing++; else paused++;
        } else if (member_count(rm) == 0) {
            dormant++;
        }
    }
    for (int i = 0; i < SRV_MAX_CLIENTS; i++) clients += s->clients[i].used;
    snprintf(out, cap,
             "{\"public\":%d,\"private\":%d,\"dormant\":%d,\"playing\":%d,"
             "\"paused\":%d,\"clients\":%d,\"uptime_s\":%lld}",
             pub, priv, dormant, playing, paused, clients,
             s->boot_set ? (long long)((now - s->boot_ms) / 1000) : 0);
}

// --- Persistence ---------------------------------------------------------------
#define SNAP_MAGIC   0x4F525331u   // "ORS1"
#define SNAP_VERSION 1u

typedef struct {
    uint32_t magic, version, room_size, count;
    int64_t  saved_at;
} SnapHeader;

size_t srv_snapshot_cap(void) {
    return sizeof(SnapHeader) + (size_t)SRV_MAX_ROOMS * (sizeof(uint16_t) + sizeof(Room));
}

bool srv_dirty(const Srv* s) { return s->dirty; }

static bool room_durable(const Room* rm) {
    return rm->active && (rm->kind == ROOM_PRIVATE || rm->phase == PHASE_PLAYING);
}

size_t srv_snapshot(Srv* s, uint8_t* buf, size_t cap, int64_t now) {
    SnapHeader h = {SNAP_MAGIC, SNAP_VERSION, (uint32_t)sizeof(Room), 0, now};
    size_t off = sizeof h;
    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        if (!room_durable(&s->rooms[r])) continue;
        if (off + sizeof(uint16_t) + sizeof(Room) > cap) return 0;
        uint16_t idx = (uint16_t)r;
        memcpy(buf + off, &idx, sizeof idx);
        off += sizeof idx;
        memcpy(buf + off, &s->rooms[r], sizeof(Room));
        off += sizeof(Room);
        h.count++;
    }
    if (cap < sizeof h) return 0;
    memcpy(buf, &h, sizeof h);
    s->dirty = false;
    return off;
}

static int64_t rebase(int64_t t, int64_t delta) { return t ? t + delta : 0; }

bool srv_restore(Srv* s, const uint8_t* buf, size_t len, int64_t now) {
    SnapHeader h;
    if (!buf || len < sizeof h) return false;
    memcpy(&h, buf, sizeof h);
    if (h.magic != SNAP_MAGIC || h.version != SNAP_VERSION || h.room_size != sizeof(Room) ||
        len != sizeof h + (size_t)h.count * (sizeof(uint16_t) + sizeof(Room))) {
        logf_line(s, "snapshot discarded (magic/version/size mismatch)");
        return false;
    }
    int64_t delta = now - h.saved_at;
    size_t off = sizeof h;
    int restored = 0;
    for (uint32_t k = 0; k < h.count; k++) {
        uint16_t idx;
        memcpy(&idx, buf + off, sizeof idx);
        off += sizeof idx;
        if (idx >= SRV_MAX_ROOMS) { off += sizeof(Room); continue; }
        Room* rm = &s->rooms[idx];
        memcpy(rm, buf + off, sizeof(Room));
        off += sizeof(Room);
        if (!rm->active) continue;
        rm->countdown_at = 0;
        rm->idle_since = 0;
        rm->can_force = false;
        rm->turn_deadline = rm->ai_due = rm->reveal_deadline = 0;
        rm->wait_seat = -1;
        rm->wait_deadline = 0;
        rm->empty_since = 0;
        rm->created_ms = rebase(rm->created_ms, delta);
        rm->last_used_ms = rebase(rm->last_used_ms, delta);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            Member* mb = &rm->m[i];
            if (!mb->used) continue;
            mb->connected = false;
            mb->client = -1;
            mb->swap_from = -1;
            if (rm->phase == PHASE_PLAYING) {
                mb->away = true;
            } else {
                mb->ready = false;
                mb->gone_ms = now;
            }
        }
        if (rm->phase == PHASE_PLAYING) schedule(s, rm, now);
        restored++;
    }
    logf_line(s, "snapshot restored rooms=%d", restored);
    s->dirty = false;
    return true;
}
