// Unit tests for the multiplayer daemon: the ws/wire codecs and the whole
// game-pool core (public and private rooms, waiting rooms and ready-up, the
// live room list, member keys, turn clocks, the leave policy, redacted
// broadcast, snapshots), driven socket-free with injected time. Includes the
// sources directly, as every suite in this repo does.
//
// Built and run by `make test`. A non-zero exit means a failure.

#include "rules.c"
#include "game.c"
#include "ai.c"
#include "wire.c"
#include "ws.c"
#include "server_core.c"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// --- Captured transport -------------------------------------------------------
#define CAP_MSGS 1024
typedef struct {
    int  client;
    char text[8192];
} SentMsg;
static SentMsg s_sent[CAP_MSGS];
static int s_sent_n;

static int  s_kicked[256];
static int  s_kicked_n;
static char s_logs[64][4500];
static int  s_logs_n;

static void t_send(void* ud, int client, const char* text, size_t len) {
    (void)ud;
    int i = s_sent_n % CAP_MSGS;
    s_sent[i].client = client;
    size_t n = len < sizeof s_sent[i].text - 1 ? len : sizeof s_sent[i].text - 1;
    memcpy(s_sent[i].text, text, n);
    s_sent[i].text[n] = '\0';
    s_sent_n++;
}
static void t_kick(void* ud, int client) {
    (void)ud;
    if (s_kicked_n < 256) s_kicked[s_kicked_n++] = client;
}
static void t_log(void* ud, const char* line) {
    (void)ud;
    int i = s_logs_n % 64;
    snprintf(s_logs[i], sizeof s_logs[i], "%s", line);
    s_logs_n++;
}

static void cap_reset(void) { s_sent_n = 0; s_kicked_n = 0; }

// Latest message of a given "t" for a client, or NULL.
static const char* last_msg(int client, const char* type) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"t\":\"%s\"", type);
    int lo = s_sent_n > CAP_MSGS ? s_sent_n - CAP_MSGS : 0;
    for (int i = s_sent_n - 1; i >= lo; i--) {
        const SentMsg* m = &s_sent[i % CAP_MSGS];
        if (m->client == client && strstr(m->text, pat)) return m->text;
    }
    return NULL;
}

static bool was_kicked(int client) {
    for (int i = 0; i < s_kicked_n; i++) {
        if (s_kicked[i] == client) return true;
    }
    return false;
}

// Integer field from a flat JSON message ("key":123). -99999 when absent.
static long jfield(const char* json, const char* key) {
    if (!json) return -99999;
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char* p = strstr(json, pat);
    if (!p) return -99999;
    return strtol(p + strlen(pat), NULL, 10);
}

// String field ("key":"...") copied into out. false when absent.
static bool jstr(const char* json, const char* key, char* out, size_t cap) {
    if (!json) return false;
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < cap) { out[n] = p[n]; n++; }
    out[n] = '\0';
    return true;
}

// Flat int array ("key":[1,2,3]) into out; returns the count.
static int jints(const char* json, const char* key, long* out, int max) {
    if (!json) return 0;
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":[", key);
    const char* p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    int n = 0;
    while (*p && *p != ']') {
        char* end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        if (n < max) out[n++] = v;
        p = end;
        if (*p == ',') p++;
    }
    return n;
}

// --- Harness ------------------------------------------------------------------
static Srv* S;
static int64_t NOW;

static Srv* fresh_server(void) {
    SrvIo io = {t_send, t_kick, t_log, NULL};
    cap_reset();
    s_logs_n = 0;
    NOW = 1000000;
    S = srv_create(&io, 0xC0FFEE);
    return S;
}

static void say(int client, const char* json) {
    srv_client_msg(S, client, json, strlen(json), NOW);
}

static void sayf(int client, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static void sayf(int client, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say(client, buf);
}

// Connect + hello (with a name) in one step.
static void hello(int client, const char* ip, const char* name) {
    srv_client_connected(S, client, ip);
    sayf(client, "{\"t\":\"hello\",\"v\":%d,\"name\":\"%s\"}", SRV_PROTO_VERSION, name);
}

static void tick_ms(int64_t ms) {
    NOW += ms;
    srv_tick(S, NOW);
}

// Room slot and public id of the room a client is in (from its welcome).
static int room_of(int client) { return S->clients[client].room; }
static long id_of(int client) { return jfield(last_msg(client, "welcome"), "id"); }

static void key_of(int client, char out[40]) {
    out[0] = '\0';
    jstr(last_msg(client, "welcome"), "key", out, 40);
}

// Create a room from `client` and return its slot.
static int create(int client, const char* extra) {
    sayf(client, "{\"t\":\"create\"%s%s}", extra && extra[0] ? "," : "", extra ? extra : "");
    return room_of(client);
}

static void join_id(int client, long id) { sayf(client, "{\"t\":\"join\",\"room\":%ld}", id); }
static void join_code(int client, const char* code) {
    sayf(client, "{\"t\":\"join\",\"code\":\"%s\"}", code);
}
static void ready(int client) { say(client, "{\"t\":\"ready\",\"on\":1}"); }

// Ready every listed client, then run out the start countdown.
static void ready_all_and_start(const int* clients, int n) {
    for (int i = 0; i < n; i++) ready(clients[i]);
    tick_ms(SRV_COUNTDOWN_MS + 10);
}

static int seat_client(Room* rm, int seat) {
    int mi = rm->seat_member[seat];
    return (mi >= 0 && rm->m[mi].connected) ? rm->m[mi].client : -1;
}

// The seat's human plays the move the AI would pick for them. Returns false
// when it isn't a present human's turn.
static uint64_t s_play_rng = 12345;
static bool human_move(Room* rm) {
    if (rm->phase != PHASE_PLAYING) return false;
    if (rm->g.phase != PHASE_DRAW && rm->g.phase != PHASE_PLACE) return false;
    int c = seat_client(rm, rm->g.turn);
    if (c < 0) return false;
    GameView v = game_view_for(&rm->g, rm->g.turn);
    Action a = ai_choose(&v, &s_play_rng);
    if (!game_action_legal(&rm->g, a)) {
        a = (Action){ACTION_DRAW_STOCK, 0};
        if (rm->g.phase == PHASE_PLACE) a = (Action){ACTION_PLACE, 0};
    }
    sayf(c, "{\"t\":\"action\",\"a\":%d,\"slot\":%d}", a.type, a.slot);
    return true;
}

// Humans play every turn and confirm every round until the match ends and
// the room is back in its waiting room.
static void play_match_out(Room* rm) {
    for (int guard = 0; guard < 20000 && rm->active && rm->phase == PHASE_PLAYING; guard++) {
        if (rm->g.phase == PHASE_ROUND_OVER) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (rm->m[i].used && rm->m[i].connected) say(rm->m[i].client, "{\"t\":\"next\"}");
            }
            continue;
        }
        if (!human_move(rm)) tick_ms(SRV_AI_PACE_MS);
        NOW += 5;
    }
}

// --- ws codec -----------------------------------------------------------------
static void test_ws_codec(void) {
    // RFC 6455 §1.3's own handshake vector pins SHA-1 + base64 end to end.
    const char* req =
        "GET /chat HTTP/1.1\r\nHost: server\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    char resp[256];
    int n = ws_handshake(req, strlen(req), resp, sizeof resp);
    CHECK(n > 0);
    CHECK(strstr(resp, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    CHECK(strstr(resp, "101 Switching Protocols") != NULL);

    // No key -> rejected.
    const char* bad = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(ws_handshake(bad, strlen(bad), resp, sizeof resp) == -1);

    // Masked text frame round trip.
    uint8_t frame[64] = {0x81, 0x85, 1, 2, 3, 4};
    const char* msg = "hello";
    for (int i = 0; i < 5; i++) frame[6 + i] = (uint8_t)(msg[i] ^ frame[2 + (i & 3)]);
    int opcode;
    uint8_t* payload;
    size_t plen;
    CHECK(ws_decode(frame, 11, &opcode, &payload, &plen) == 11);
    CHECK(opcode == WS_TEXT && plen == 5 && memcmp(payload, "hello", 5) == 0);

    // Incomplete frame: no consumption.
    uint8_t part[3] = {0x81, 0x85, 1};
    CHECK(ws_decode(part, 3, &opcode, &payload, &plen) == 0);

    // Unmasked (server-style) client frame: protocol error.
    uint8_t unmasked[7] = {0x81, 0x05, 'h', 'e', 'l', 'l', 'o'};
    CHECK(ws_decode(unmasked, 7, &opcode, &payload, &plen) == -1);

    // Fragmented (FIN clear) and reserved bits: protocol error.
    uint8_t frag[8] = {0x01, 0x81, 1, 2, 3, 4, 0};
    CHECK(ws_decode(frag, 7, &opcode, &payload, &plen) == -1);
    uint8_t rsv[8] = {0xC1, 0x81, 1, 2, 3, 4, 0};
    CHECK(ws_decode(rsv, 7, &opcode, &payload, &plen) == -1);

    // 64-bit length and non-minimal 16-bit length: rejected.
    uint8_t big[16] = {0x81, 0xFF};
    CHECK(ws_decode(big, 16, &opcode, &payload, &plen) == -1);
    uint8_t nonmin[10] = {0x81, 0xFE, 0x00, 0x05, 1, 2, 3, 4, 0, 0};
    CHECK(ws_decode(nonmin, 10, &opcode, &payload, &plen) == -1);

    // Server frame headers.
    uint8_t hdr[4];
    CHECK(ws_encode_header(hdr, WS_TEXT, 5) == 2 && hdr[0] == 0x81 && hdr[1] == 5);
    CHECK(ws_encode_header(hdr, WS_TEXT, 300) == 4 && hdr[1] == 126 &&
          hdr[2] == 1 && hdr[3] == 44);
}

// --- wire codec ---------------------------------------------------------------
static void test_wire_codec(void) {
    WireKV kv[WIRE_MAX_KV];

    int n = wire_parse("{\"t\":\"action\",\"a\":2,\"slot\":7}", 29, kv);
    CHECK(n == 3);
    CHECK(strcmp(wire_str(kv, n, "t"), "action") == 0);
    CHECK(wire_int(kv, n, "a", -1) == 2);
    CHECK(wire_int(kv, n, "slot", -1) == 7);
    CHECK(wire_int(kv, n, "missing", -7) == -7);
    CHECK(wire_str(kv, n, "a") == NULL);   // int field is not a string

    CHECK(wire_parse("{}", 2, kv) == 0);
    CHECK(wire_parse(" { \"a\" : 1 } ", 13, kv) == 1);

    // Everything outside the grammar is rejected outright.
    static const char* bad[] = {
        "", "{", "[1]", "{\"a\":{\"b\":1}}", "{\"a\":[1]}", "{\"a\":-1}",
        "{\"a\":1.5}", "{\"a\":true}", "{\"a\":null}", "{\"a\":\"x\\ny\"}",
        "{\"a\":1}x", "{\"a\":1,,}", "{\"a\":9999999999}", "{\"\":1}",
        "{\"aaaaaaaaaaaaaaaaaaaaaaa\":1}",
        "{\"a\":\"0123456789012345678901234567890123456789012345\"}",
        "{\"a\":1,\"b\":1,\"c\":1,\"d\":1,\"e\":1,\"f\":1,\"g\":1,\"h\":1,\"i\":1}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(wire_parse(bad[i], strlen(bad[i]), kv) == -1);
    }

    // Deterministic garbage fuzz: never crash, mostly reject.
    uint32_t lcg = 77;
    char buf[128];
    for (int iter = 0; iter < 5000; iter++) {
        lcg = lcg * 1664525u + 1013904223u; int len = (int)(lcg % 100u);
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            buf[i] = (char)(lcg >> 24);
        }
        wire_parse(buf, (size_t)len, kv);
    }

    // Writer overflow is flagged, never truncated-and-sent.
    char small[16];
    JW w;
    jw_init(&w, small, sizeof small);
    jw_f(&w, "0123456789");
    CHECK(!w.overflow && w.len == 10);
    jw_f(&w, "0123456789");
    CHECK(w.overflow);
}

// --- Names ----------------------------------------------------------------------
static void test_names(void) {
    fresh_server();
    srv_client_connected(S, 0, "1.1.1.1");
    sayf(0, "{\"t\":\"hello\",\"v\":%d}", SRV_PROTO_VERSION);
    char code[16];
    jstr(last_msg(0, "error"), "code", code, sizeof code);
    CHECK(strcmp(code, "need_name") == 0);
    CHECK(S->clients[0].state == CL_NEW);          // still has to say hello

    sayf(0, "{\"t\":\"hello\",\"v\":%d,\"name\":\"xXFuCkXx\"}", SRV_PROTO_VERSION);
    jstr(last_msg(0, "error"), "code", code, sizeof code);
    CHECK(strcmp(code, "bad_name") == 0);

    sayf(0, "{\"t\":\"hello\",\"v\":%d,\"name\":\"Dan\"}", SRV_PROTO_VERSION);
    CHECK(last_msg(0, "hi") != NULL);
    CHECK(S->clients[0].state == CL_IDLE);

    // The old protocol is told to upgrade.
    srv_client_connected(S, 1, "2.2.2.2");
    say(1, "{\"t\":\"hello\",\"v\":1,\"name\":\"Old\"}");
    jstr(last_msg(1, "error"), "code", code, sizeof code);
    CHECK(strcmp(code, "upgrade") == 0);
    CHECK(was_kicked(1));

    // Duplicate names in one room get a suffix.
    hello(2, "3.3.3.3", "Dan");
    int r = create(0, "");
    join_id(2, id_of(0));
    CHECK(strcmp(S->rooms[r].m[0].name, "Dan") == 0);
    CHECK(strcmp(S->rooms[r].m[1].name, "Dan2") == 0);
}

// --- Waiting room: ready-up and the start countdown --------------------------
static void test_lobby_ready(void) {
    fresh_server();
    hello(0, "1.1.1.1", "Ann");
    hello(1, "2.2.2.2", "Bob");
    hello(2, "3.3.3.3", "Cy");
    int r = create(0, "\"target\":250");
    Room* rm = &S->rooms[r];
    CHECK(rm->active && rm->phase == PHASE_LOBBY && rm->kind == ROOM_PUBLIC);
    CHECK(rm->code[0] == '\0');                         // public rooms have no code

    // Alone and ready: not enough players.
    ready(0);
    char block[16];
    jstr(last_msg(0, "lobby"), "block", block, sizeof block);
    CHECK(strcmp(block, "need2") == 0);
    tick_ms(SRV_COUNTDOWN_MS * 3);
    CHECK(rm->phase == PHASE_LOBBY);

    // A second player arrives: everyone sees both names; they arrive not ready.
    join_id(1, id_of(0));
    const char* lob = last_msg(0, "lobby");
    CHECK(strstr(lob, "\"Ann\"") && strstr(lob, "\"Bob\""));
    long rd[4];
    CHECK(jints(lob, "ready", rd, 4) == 4 && rd[0] == 1 && rd[1] == 0);
    jstr(lob, "block", block, sizeof block);
    CHECK(strcmp(block, "ready") == 0);

    // Both ready: the countdown arms. A third arrival cancels it.
    ready(1);
    CHECK(rm->countdown_at != 0);
    CHECK(jfield(last_msg(1, "lobby"), "count_ms") > 0);
    join_id(2, id_of(0));
    CHECK(rm->countdown_at == 0);

    // Un-ready cancels too; re-ready re-arms; the deal happens on time.
    ready(2);
    CHECK(rm->countdown_at != 0);
    say(1, "{\"t\":\"ready\",\"on\":0}");
    CHECK(rm->countdown_at == 0);
    ready(1);
    tick_ms(SRV_COUNTDOWN_MS - 500);
    CHECK(rm->phase == PHASE_LOBBY);
    tick_ms(600);
    CHECK(rm->phase == PHASE_PLAYING);
    CHECK(rm->g.rules.player_count == 3);              // dealt for who's here
    CHECK(rm->g.rules.target_score == 250);            // the room's setting
    CHECK(last_msg(2, "state") != NULL);

    // A started room can't be joined.
    hello(3, "4.4.4.4", "Dee");
    join_id(3, id_of(0));
    jstr(last_msg(3, "error"), "code", block, sizeof block);
    CHECK(strcmp(block, "started") == 0);
}

// --- Leaving the waiting room, grace, deletion ---------------------------------
static void test_lobby_leave_and_grace(void) {
    fresh_server();
    hello(0, "1.1.1.1", "Ann");
    hello(1, "2.2.2.2", "Bob");
    int r = create(0, "");
    long id = id_of(0);
    join_id(1, id);
    Room* rm = &S->rooms[r];
    CHECK(member_count(rm) == 2);

    // Leave is instant, and the leaver is free to browse again.
    say(1, "{\"t\":\"leave\"}");
    CHECK(member_count(rm) == 1);
    CHECK(S->clients[1].state == CL_IDLE);
    CHECK(strstr(last_msg(0, "lobby"), "\"Bob\"") == NULL);

    // A dropped link is held for the grace period, then removed — and a public
    // room is deleted the moment its waiting room is empty.
    srv_client_gone(S, 0, NOW);
    CHECK(rm->active && member_count(rm) == 1);
    tick_ms(SRV_LOBBY_GRACE_MS - 1000);
    CHECK(rm->active);
    tick_ms(2000);
    CHECK(!rm->active);

    // A stale public id can't land in the slot's next occupant.
    hello(2, "3.3.3.3", "Cy");
    int r2 = create(2, "");
    CHECK(r2 == r);                                     // same slot reused
    join_id(1, id);
    char code[16];
    jstr(last_msg(1, "error"), "code", code, sizeof code);
    CHECK(strcmp(code, "no_room") == 0);
}

// --- Private rooms: codes, guessing limit, durability ---------------------------
static void test_private_rooms(void) {
    fresh_server();
    hello(0, "1.1.1.1", "Ann");
    hello(1, "2.2.2.2", "Bob");
    hello(9, "9.9.9.9", "Viewer");
    int r = create(0, "\"private\":1");
    Room* rm = &S->rooms[r];
    CHECK(rm->kind == ROOM_PRIVATE && strlen(rm->code) == SRV_CODE_LEN);
    char code[8];
    jstr(last_msg(0, "welcome"), "code", code, sizeof code);
    CHECK(strcmp(code, rm->code) == 0);
    CHECK(jstr(last_msg(0, "welcome"), "owner", (char[40]){0}, 40));   // owner key issued

    // Not in the public list.
    say(9, "{\"t\":\"list\"}");
    CHECK(jfield(last_msg(9, "rooms"), "total") == 0);

    // Peek and join by code.
    sayf(1, "{\"t\":\"peek\",\"code\":\"%s\"}", code);
    CHECK(jfield(last_msg(1, "peeked"), "ok") == 1);
    join_code(1, code);
    CHECK(room_of(1) == r);

    // Everyone leaves: the room stays (dormant), with the same code.
    say(1, "{\"t\":\"leave\"}");
    say(0, "{\"t\":\"leave\"}");
    CHECK(rm->active && member_count(rm) == 0);
    tick_ms(60000);
    CHECK(rm->active);
    sayf(1, "{\"t\":\"peek\",\"code\":\"%s\"}", code);
    CHECK(jfield(last_msg(1, "peeked"), "ok") == 1);

    // Guessing: ten misses from one address, then even the right code is refused.
    hello(5, "5.5.5.5", "Guess");
    for (int i = 0; i < MISS_MAX; i++) {
        join_code(5, "ZZZZ");
        NOW += 100;
    }
    join_code(5, code);
    char err[16];
    jstr(last_msg(5, "error"), "code", err, sizeof err);
    CHECK(strcmp(err, "rate") == 0);
    CHECK(room_of(5) != r);
    NOW += MISS_WINDOW_MS + 1000;                       // the window passes
    join_code(5, code);
    CHECK(room_of(5) == r);
    say(5, "{\"t\":\"leave\"}");

    // Expiry: 30 days after its last use with nobody in it.
    tick_ms(SRV_PRIVATE_TTL_MS + 1000);
    CHECK(!rm->active);
    sayf(1, "{\"t\":\"peek\",\"code\":\"%s\"}", code);
    CHECK(jfield(last_msg(1, "peeked"), "ok") == 0);
}

// --- Owner settings --------------------------------------------------------------
static void test_owner_settings(void) {
    fresh_server();
    hello(0, "1.1.1.1", "Ann");
    hello(1, "2.2.2.2", "Bob");
    int r = create(0, "");
    char owner[40];
    jstr(last_msg(0, "welcome"), "owner", owner, sizeof owner);
    join_id(1, id_of(0));
    Room* rm = &S->rooms[r];
    ready(0);
    ready(1);

    // Not the owner: refused.
    sayf(1, "{\"t\":\"settings\",\"owner\":\"%032d\",\"target\":1000}", 0);
    char err[16];
    jstr(last_msg(1, "error"), "code", err, sizeof err);
    CHECK(strcmp(err, "not_owner") == 0);
    CHECK(rm->rules.target_score == 500);

    // The owner changes the target and makes it private: a code appears and
    // everyone has to ready again.
    sayf(0, "{\"t\":\"settings\",\"owner\":\"%s\",\"target\":1000,\"private\":1,\"away\":1}", owner);
    CHECK(rm->rules.target_score == 1000);
    CHECK(rm->kind == ROOM_PRIVATE && strlen(rm->code) == SRV_CODE_LEN);
    CHECK(rm->away_policy == AWAY_WAIT);
    CHECK(!rm->m[0].ready && !rm->m[1].ready && rm->countdown_at == 0);
}

// --- Partners: balanced teams, consent to swap, seating ------------------------
static void test_partners(void) {
    fresh_server();
    const char* names[4] = {"Ann", "Bob", "Cy", "Dee"};
    int cl[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; i++) {
        char ip[24];
        snprintf(ip, sizeof ip, "1.1.1.%d", i + 1);
        hello(i, ip, names[i]);
    }
    int r = create(0, "\"partners\":1");
    Room* rm = &S->rooms[r];

    // Three players can never start a Partners room.
    join_id(1, id_of(0));
    join_id(2, id_of(0));
    for (int i = 0; i < 3; i++) ready(i);
    char block[16];
    jstr(last_msg(0, "lobby"), "block", block, sizeof block);
    CHECK(strcmp(block, "partners4") == 0);

    // Arrivals fill the smaller team: A, B, A, B.
    join_id(3, id_of(0));
    CHECK(rm->m[0].team == 0 && rm->m[1].team == 1 && rm->m[2].team == 0 && rm->m[3].team == 1);

    // Both teams full: a plain switch is refused.
    say(0, "{\"t\":\"team\",\"side\":1}");
    char err[16];
    jstr(last_msg(0, "error"), "code", err, sizeof err);
    CHECK(strcmp(err, "team_full") == 0);

    // A swap needs the other player's consent. Declined: nothing moves.
    ready(3);
    say(0, "{\"t\":\"swap\",\"seat\":1}");
    CHECK(jfield(last_msg(1, "lobby"), "swap_from") == 0);
    say(1, "{\"t\":\"swap_no\",\"seat\":0}");
    CHECK(rm->m[0].team == 0 && rm->m[1].team == 1);

    // Accepted: they trade places and both must ready again.
    say(0, "{\"t\":\"swap\",\"seat\":1}");
    say(1, "{\"t\":\"swap_ok\",\"seat\":0}");
    CHECK(rm->m[0].team == 1 && rm->m[1].team == 0);
    CHECK(!rm->m[0].ready && !rm->m[1].ready);
    jstr(last_msg(2, "lobby"), "block", block, sizeof block);
    CHECK(strcmp(block, "ready") == 0);

    // Start: partners sit opposite (team A in seats 0 and 2).
    ready_all_and_start(cl, 4);
    CHECK(rm->phase == PHASE_PLAYING && rm->g.rules.partners);
    CHECK(rm->m[rm->seat_member[0]].team == rm->m[rm->seat_member[2]].team);
    CHECK(rm->m[rm->seat_member[1]].team == rm->m[rm->seat_member[3]].team);
    CHECK(rm->m[rm->seat_member[0]].team != rm->m[rm->seat_member[1]].team);
}

// --- Room list: sampling, stability, weighting, best ---------------------------
static int list_ids(int client, long* ids) {
    return jints(last_msg(client, "rooms"), "ids", ids, SRV_LIST_SHOW + 1);
}

static void test_room_list(void) {
    fresh_server();
    // 30 public rooms of one player each.
    for (int i = 0; i < 30; i++) {
        char ip[20];
        snprintf(ip, sizeof ip, "10.0.0.%d", i + 1);
        hello(100 + i, ip, "Host");
        create(100 + i, i < 5 ? "\"target\":250" : "");
    }
    hello(0, "1.1.1.1", "Viewer");
    say(0, "{\"t\":\"list\"}");
    const char* rooms = last_msg(0, "rooms");
    CHECK(jfield(rooms, "total") == 30);
    long ids[SRV_LIST_SHOW + 1], ids2[SRV_LIST_SHOW + 1];
    int n = list_ids(0, ids);
    CHECK(n == SRV_LIST_SHOW);

    // Stable: nothing changed, so the next ticks keep the same sample.
    int before = s_sent_n;
    tick_ms(SRV_LIST_PUSH_MS * 2);
    tick_ms(SRV_LIST_PUSH_MS * 2);
    CHECK(s_sent_n == before);                          // no pointless pushes

    // A shown room fills up: it leaves the list (replaced from the rest), and
    // the others stay put.
    int victim = (int)(ids[0] & 0xFFF);
    for (int k = 0; k < 3; k++) {
        char ip[20];
        snprintf(ip, sizeof ip, "10.1.0.%d", k + 1);
        hello(200 + k, ip, "Fill");
        join_id(200 + k, ids[0]);
    }
    CHECK(member_count(&S->rooms[victim]) == 4);
    tick_ms(SRV_LIST_PUSH_MS + 10);
    int n2 = list_ids(0, ids2);
    CHECK(n2 == SRV_LIST_SHOW);
    int kept = 0;
    bool victim_gone = true;
    for (int i = 0; i < n2; i++) {
        if (ids2[i] == ids[0]) victim_gone = false;
        for (int j = 1; j < n; j++) if (ids2[i] == ids[j]) kept++;
    }
    CHECK(victim_gone);
    CHECK(kept == SRV_LIST_SHOW - 1);
    CHECK(jfield(last_msg(0, "rooms"), "total") == 29);

    // Play To filter.
    say(0, "{\"t\":\"list\",\"target\":250}");
    CHECK(jfield(last_msg(0, "rooms"), "total") == 5);
    CHECK(list_ids(0, ids) == 5);

    // Weighting: a 3-player room is shown far more often than a 1-player one.
    fresh_server();
    for (int i = 0; i < 40; i++) {
        char ip[20];
        snprintf(ip, sizeof ip, "10.0.1.%d", i + 1);
        hello(100 + i, ip, "Host");
        create(100 + i, "");
    }
    long full_id = id_of(100);
    hello(300, "10.2.0.1", "A");
    hello(301, "10.2.0.2", "B");
    join_id(300, full_id);
    join_id(301, full_id);
    long one_id = id_of(139);
    int full_hits = 0, one_hits = 0;
    hello(0, "1.1.1.1", "Viewer");
    for (int trial = 0; trial < 200; trial++) {
        say(0, "{\"t\":\"shuffle\"}");
        int k = list_ids(0, ids);
        for (int i = 0; i < k; i++) {
            if (ids[i] == full_id) full_hits++;
            if (ids[i] == one_id) one_hits++;
        }
        NOW += 20;
    }
    CHECK(full_hits > 180);                             // ~always in the sample
    CHECK(one_hits < 140);                              // ~half the time

    // Join Best Room: the fullest.
    hello(1, "1.1.1.2", "Best");
    say(1, "{\"t\":\"best\"}");
    CHECK(id_of(1) == full_id);

    // Nothing joinable: best says so (the client offers New Room).
    fresh_server();
    hello(1, "1.1.1.2", "Best");
    say(1, "{\"t\":\"best\"}");
    char err[16];
    jstr(last_msg(1, "error"), "code", err, sizeof err);
    CHECK(strcmp(err, "no_room") == 0);
}

// --- Idle not-ready players ------------------------------------------------------
static void test_idle_not_ready(void) {
    fresh_server();
    hello(0, "1.1.1.1", "Ann");
    hello(1, "2.2.2.2", "Bob");
    hello(2, "3.3.3.3", "Idle");
    create(0, "");
    join_id(1, id_of(0));
    join_id(2, id_of(0));
    ready(0);
    ready(1);
    tick_ms(SRV_IDLE_READY_MS - 1000);
    CHECK(room_of(2) >= 0);
    tick_ms(2000);
    // Public: the idle player is sent back to the list, and the rest start.
    CHECK(S->clients[2].state == CL_IDLE);
    char why[16];
    jstr(last_msg(2, "removed"), "why", why, sizeof why);
    CHECK(strcmp(why, "idle") == 0);
    tick_ms(SRV_COUNTDOWN_MS + 10);
    CHECK(S->rooms[room_of(0)].phase == PHASE_PLAYING);

    // Private: the ready players are offered "Start without them".
    fresh_server();
    hello(0, "1.1.1.1", "Ann");
    hello(1, "2.2.2.2", "Bob");
    hello(2, "3.3.3.3", "Idle");
    int r = create(0, "\"private\":1");
    join_code(1, S->rooms[r].code);
    join_code(2, S->rooms[r].code);
    ready(0);
    ready(1);
    tick_ms(SRV_IDLE_READY_MS + 1000);
    CHECK(room_of(2) == r);                             // nobody removed
    CHECK(jfield(last_msg(0, "lobby"), "force") == 1);
    CHECK(jfield(last_msg(2, "lobby"), "force") == 0);  // only ready players see it
    say(0, "{\"t\":\"force\"}");
    CHECK(S->clients[2].state == CL_IDLE);
    tick_ms(SRV_COUNTDOWN_MS + 10);
    CHECK(S->rooms[r].phase == PHASE_PLAYING && S->rooms[r].g.rules.player_count == 2);
}

// --- Leaving mid-match: wait, Let AI play, return -------------------------------
static void two_player_match(int* r_out, int priv, const char* extra) {
    hello(0, "1.1.1.1", "Ann");
    hello(1, "2.2.2.2", "Bob");
    char opts[96];
    snprintf(opts, sizeof opts, "\"private\":%d%s%s", priv, extra ? "," : "", extra ? extra : "");
    int r = create(0, opts);
    if (priv) join_code(1, S->rooms[r].code);
    else join_id(1, id_of(0));
    int cl[2] = {0, 1};
    ready_all_and_start(cl, 2);
    *r_out = r;
}

// Play humans' turns until it is `seat`'s turn to draw.
static void until_turn(Room* rm, int seat) {
    for (int g = 0; g < 400 && !(rm->g.turn == seat && rm->g.phase == PHASE_DRAW); g++) {
        if (rm->g.phase == PHASE_ROUND_OVER) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (rm->m[i].used && rm->m[i].connected) say(rm->m[i].client, "{\"t\":\"next\"}");
            }
            continue;
        }
        if (!human_move(rm)) tick_ms(SRV_AI_PACE_MS);
    }
}

static void test_leave_policy(void) {
    fresh_server();
    int r;
    two_player_match(&r, 0, NULL);
    Room* rm = &S->rooms[r];
    int bob_mi = 1, bob_seat = rm->m[bob_mi].seat;
    int ann_seat = rm->m[0].seat;
    until_turn(rm, ann_seat);

    // Bob steps away. Play carries on to his turn, then the table waits.
    say(1, "{\"t\":\"leave\"}");
    CHECK(rm->m[bob_mi].away && !rm->m[bob_mi].ai_control);
    const char* st = last_msg(0, "state");
    long ctl[4];
    CHECK(jints(st, "ctl", ctl, 4) == 2 && ctl[bob_seat] == 1 && ctl[ann_seat] == 0);
    until_turn(rm, bob_seat);
    CHECK(rm->wait_seat == bob_seat && rm->wait_deadline != 0);
    st = last_msg(0, "state");
    CHECK(jfield(st, "wait_seat") == bob_seat && jfield(st, "wait_ms") > 0);
    CHECK(jfield(st, "wait_timer") == 1);
    uint32_t acts = rm->actions;
    tick_ms(5000);
    CHECK(rm->actions == acts);                         // waiting, not playing

    // Ann taps Let AI play: the AI takes Bob's seat right away.
    sayf(0, "{\"t\":\"letai\",\"seat\":%d}", bob_seat);
    CHECK(rm->m[bob_mi].ai_control && rm->wait_seat == -1);
    tick_ms(SRV_AI_PACE_MS + 10);
    CHECK(rm->actions == acts + 1);

    // Once the AI has the seat it doesn't pause again on Bob's next turn.
    until_turn(rm, ann_seat);
    until_turn(rm, bob_seat);
    tick_ms(SRV_AI_PACE_MS + 10);
    CHECK(rm->wait_seat == -1);

    // Bob comes back: Ann hears about it, and control returns.
    say(1, "{\"t\":\"here\"}");
    CHECK(!rm->m[bob_mi].away && !rm->m[bob_mi].ai_control);
    CHECK(jfield(last_msg(0, "back"), "seat") == bob_seat);

    // The timer path: absent again, his turn, nobody taps — 30 s, then AI.
    until_turn(rm, ann_seat);
    say(1, "{\"t\":\"leave\"}");
    until_turn(rm, bob_seat);
    CHECK(rm->wait_seat == bob_seat);
    tick_ms(SRV_AWAY_WAIT_MS + 10);
    CHECK(rm->m[bob_mi].ai_control);

    // Dropping the link counts as stepping away; rejoining by key returns.
    char key[40];
    key_of(1, key);
    srv_client_gone(S, 1, NOW);
    CHECK(!rm->m[bob_mi].connected && rm->m[bob_mi].away);
    hello(7, "2.2.2.2", "Bob");
    sayf(7, "{\"t\":\"rejoin\",\"key\":\"%s\"}", key);
    CHECK(rm->m[bob_mi].connected && rm->m[bob_mi].client == 7);
    CHECK(!rm->m[bob_mi].away && !rm->m[bob_mi].ai_control);
    CHECK(last_msg(7, "state") != NULL);
}

static void test_wait_policy(void) {
    fresh_server();
    int r;
    two_player_match(&r, 1, "\"away\":1");
    Room* rm = &S->rooms[r];
    CHECK(rm->away_policy == AWAY_WAIT);
    int bob_seat = rm->m[1].seat;
    until_turn(rm, rm->m[0].seat);
    say(1, "{\"t\":\"leave\"}");
    until_turn(rm, bob_seat);
    CHECK(rm->wait_seat == bob_seat && rm->wait_deadline == 0);
    CHECK(jfield(last_msg(0, "state"), "wait_timer") == 0);
    uint32_t acts = rm->actions;
    for (int i = 0; i < 10; i++) tick_ms(60000);       // ten minutes: still waiting
    CHECK(rm->actions == acts && !rm->m[1].ai_control);
    sayf(0, "{\"t\":\"letai\",\"seat\":%d}", bob_seat);  // the button still works
    tick_ms(SRV_AI_PACE_MS + 10);
    CHECK(rm->actions == acts + 1);
}

// --- Everyone away: paused; public closes, private keeps it ---------------------
static void test_everyone_away(void) {
    fresh_server();
    int r;
    two_player_match(&r, 0, NULL);
    Room* rm = &S->rooms[r];
    char key0[40];
    key_of(0, key0);
    say(0, "{\"t\":\"leave\"}");
    say(1, "{\"t\":\"leave\"}");
    uint32_t acts = rm->actions;
    tick_ms(60000);
    CHECK(rm->actions == acts);                         // paused
    // Ann returns: play resumes.
    say(0, "{\"t\":\"here\"}");
    CHECK(any_present(rm));
    // Everyone gone for good: a public table closes after the abandon limit.
    say(0, "{\"t\":\"leave\"}");
    tick_ms(SRV_ABANDON_MS + 2000);
    CHECK(!rm->active);
    CHECK(strstr(last_msg(0, "removed"), "closed") != NULL);

    // Private: the paused match is kept.
    fresh_server();
    two_player_match(&r, 1, NULL);
    rm = &S->rooms[r];
    srv_client_gone(S, 0, NOW);
    srv_client_gone(S, 1, NOW);
    acts = rm->actions;
    tick_ms(SRV_ABANDON_MS * 4);
    CHECK(rm->active && rm->phase == PHASE_PLAYING && rm->actions == acts);
}

// --- Forfeit and joining another game ------------------------------------------
static void test_forfeit_and_switch(void) {
    fresh_server();
    int r;
    two_player_match(&r, 0, NULL);
    Room* rm = &S->rooms[r];
    char key1[40];
    key_of(1, key1);
    say(1, "{\"t\":\"forfeit\"}");
    CHECK(rm->m[1].forfeited && rm->m[1].ai_control);
    CHECK(S->clients[1].state == CL_IDLE);
    sayf(1, "{\"t\":\"rejoin\",\"key\":\"%s\"}", key1);
    char err[16];
    jstr(last_msg(1, "error"), "code", err, sizeof err);
    CHECK(strcmp(err, "no_seat") == 0);                 // the key is spent

    // Joining another game with a live key surrenders the old seat.
    fresh_server();
    two_player_match(&r, 0, NULL);
    rm = &S->rooms[r];
    key_of(1, key1);
    srv_client_gone(S, 1, NOW);                         // app closed mid-match
    hello(5, "2.2.2.2", "Bob");
    sayf(5, "{\"t\":\"create\",\"key\":\"%s\"}", key1);  // starts something new
    CHECK(rm->m[1].forfeited);
    CHECK(room_of(5) != r);
}

// --- After a match: same waiting room, same players --------------------------------
static void test_rematch(void) {
    fresh_server();
    int r;
    two_player_match(&r, 1, "\"target\":50");
    Room* rm = &S->rooms[r];
    char key0[40];
    key_of(0, key0);
    play_match_out(rm);
    CHECK(rm->active && rm->phase == PHASE_LOBBY);
    CHECK(member_count(rm) == 2 && !rm->m[0].ready && !rm->m[1].ready);
    CHECK(last_msg(0, "lobby") != NULL);
    // The final state reached everyone before the lobby did.
    CHECK(jfield(last_msg(1, "state"), "phase") == PHASE_MATCH_OVER);
    CHECK(rm->g.match_winner != NO_WINNER);

    // The member key carries over to the next match.
    srv_client_gone(S, 0, NOW);
    hello(8, "1.1.1.1", "Ann");
    sayf(8, "{\"t\":\"rejoin\",\"key\":\"%s\"}", key0);
    CHECK(rm->m[0].connected && rm->m[0].client == 8);
    int cl[2] = {8, 1};
    ready_all_and_start(cl, 2);
    CHECK(rm->phase == PHASE_PLAYING);
    play_match_out(rm);
    CHECK(rm->phase == PHASE_LOBBY);
}

// --- Redaction on the wire ----------------------------------------------------
static void test_wire_redaction(void) {
    fresh_server();
    int r;
    two_player_match(&r, 0, NULL);
    Room* rm = &S->rooms[r];
    int actor = seat_client(rm, rm->g.turn);
    int other = 1 - actor;

    // Mid-round: no "racks" array for anyone, and a stock draw is private.
    say(actor, "{\"t\":\"action\",\"a\":0}");
    const char* st_actor = last_msg(actor, "state");
    const char* st_other = last_msg(other, "state");
    CHECK(strstr(st_actor, "\"racks\":") == NULL);
    CHECK(strstr(st_other, "\"racks\":") == NULL);
    CHECK(jfield(st_actor, "held") == rm->g.held_card);
    CHECK(jfield(st_other, "held") == 0);              // hidden from the table
    CHECK(jfield(st_other, "phase") == PHASE_PLACE);   // but its existence shows
    CHECK(jfield(st_other, "card") == 0);              // a stock draw shows a back

    // A discard exchange is public: the displaced card is the echo.
    uint8_t displaced = rm->g.players[rm->g.turn].rack.slots[4];
    say(actor, "{\"t\":\"action\",\"a\":2,\"slot\":4}");
    st_other = last_msg(other, "state");
    CHECK(jfield(st_other, "card") == displaced);

    // Nothing anywhere on the wire ever carries the seed.
    CHECK(strstr(st_actor, "seed") == NULL && strstr(st_other, "seed") == NULL);

    // A hostile out-of-range slot must be rejected without an OOB rack read
    // (observed_card runs pre-validation): the action errors, state unchanged.
    int mover = seat_client(rm, rm->g.turn);
    say(mover, "{\"t\":\"action\",\"a\":0}");
    uint8_t phase_before = rm->g.phase;
    int turn_before = rm->g.turn;
    say(mover, "{\"t\":\"action\",\"a\":2,\"slot\":250}");
    CHECK(last_msg(mover, "error") != NULL);
    CHECK(rm->g.phase == phase_before && rm->g.turn == turn_before);

    // The same call on a heap Room, so a slot past the rack would read off the
    // end of the allocation — ASan traps the regression here.
    Room* hr = malloc(sizeof *hr);
    memcpy(hr, rm, sizeof *hr);
    CHECK(observed_card(hr, (Action){ACTION_PLACE, 250}) == 0);
    CHECK(observed_card(hr, (Action){ACTION_PLACE, 255}) == 0);
    free(hr);

    // Out-of-turn actions are refused.
    int idle = seat_client(rm, 1 - rm->g.turn);
    say(idle, "{\"t\":\"action\",\"a\":0}");
    char err[24];
    jstr(last_msg(idle, "error"), "code", err, sizeof err);
    CHECK(strcmp(err, "not_your_turn") == 0);
}

// --- Turn clock: a connected idle player strikes out -----------------------------
static void test_turn_clock(void) {
    fresh_server();
    int r;
    two_player_match(&r, 0, NULL);
    Room* rm = &S->rooms[r];
    for (int strike = 0; strike < STRIKES_TO_AI; strike++) {
        int seat = rm->g.turn;
        Member* mb = seat_mem(rm, seat);
        CHECK(rm->turn_deadline != 0);
        tick_ms(SRV_TURN_MS + 10);                      // draw times out
        // The other seat answers immediately so only this seat strikes.
        for (int g = 0; g < 50 && rm->g.turn != seat; g++) {
            if (!human_move(rm)) tick_ms(SRV_AI_PACE_MS);
        }
        (void)mb;
    }
    bool any_struck = rm->m[0].ai_control || rm->m[1].ai_control;
    CHECK(any_struck);
    // Any message from the struck-out player reclaims the seat.
    int mi = rm->m[0].ai_control ? 0 : 1;
    say(rm->m[mi].client, "{\"t\":\"here\"}");
    CHECK(!rm->m[mi].ai_control);
}

// --- Reveal backstop survives a flapping client ------------------------------
static void test_reveal_backstop(void) {
    fresh_server();
    int r;
    two_player_match(&r, 0, NULL);
    Room* rm = &S->rooms[r];
    char tok1[40];
    key_of(1, tok1);
    for (int i = 0; i < 2000 && rm->g.phase != PHASE_ROUND_OVER; i++) {
        if (!human_move(rm)) tick_ms(SRV_AI_PACE_MS);
        NOW += 50;
    }
    CHECK(rm->g.phase == PHASE_ROUND_OVER);

    // Ann confirms; capture the reveal deadline.
    say(0, "{\"t\":\"next\"}");
    CHECK(rm->m[0].confirmed);
    int64_t deadline0 = rm->reveal_deadline;
    CHECK(deadline0 != 0);

    // Bob flaps his socket repeatedly: Ann's vote and the timer both hold.
    int nid = 7;
    for (int f = 0; f < 6; f++) {
        srv_client_gone(S, f == 0 ? 1 : nid - 1, NOW);
        CHECK(rm->m[0].confirmed);
        CHECK(rm->reveal_deadline == deadline0);
        hello(nid, "2.2.2.2", "Bob");
        sayf(nid, "{\"t\":\"rejoin\",\"key\":\"%s\"}", tok1);
        CHECK(rm->m[0].confirmed);
        CHECK(rm->reveal_deadline == deadline0);
        nid++;
    }
    CHECK(rm->m[1].connected && !rm->m[1].confirmed);
    while (rm->active && rm->phase == PHASE_PLAYING && rm->g.phase == PHASE_ROUND_OVER &&
           NOW < deadline0 + 60000) {
        tick_ms(1000);
    }
    CHECK(rm->phase != PHASE_PLAYING || rm->g.phase != PHASE_ROUND_OVER);
}

// --- Audit log replays byte-for-byte ------------------------------------------
static void test_audit_replay(void) {
    fresh_server();
    int r;
    two_player_match(&r, 0, "\"target\":50");
    Room* rm = &S->rooms[r];
    Rules rules = rm->g.rules;
    play_match_out(rm);
    CHECK(rm->phase == PHASE_LOBBY);

    char* line = NULL;
    for (int i = 0; i < s_logs_n && i < 64; i++) {
        if (strstr(s_logs[i], "why=finished")) line = s_logs[i];
    }
    CHECK(line != NULL);
    if (!line) return;

    unsigned long long seed = 0;
    sscanf(strstr(line, "seed="), "seed=%llu", &seed);
    int sc[4] = {0};
    sscanf(strstr(line, "scores="), "scores=%d,%d,%d,%d", &sc[0], &sc[1], &sc[2], &sc[3]);
    const char* hex = strstr(line, "alog=");
    CHECK(hex != NULL && !strstr(line, "..."));
    hex += 5;

    Game g;
    rules.seed = seed;
    game_init(&g, &rules);
    for (int i = 0; i < 4096 && g.phase == PHASE_DEAL; i++) game_update(&g);
    for (const char* p = hex; p[0] && p[1] && p[0] != ' '; p += 2) {
        unsigned b;
        sscanf(p, "%2x", &b);
        if (b == ALOG_NEXT_ROUND) {
            game_next_round(&g);
            for (int i = 0; i < 4096 && g.phase == PHASE_DEAL; i++) game_update(&g);
        } else {
            Action a = {(uint8_t)(b & 3), (uint8_t)(b >> 2)};
            CHECK(game_apply(&g, a));
        }
    }
    CHECK(g.phase == PHASE_MATCH_OVER);
    CHECK(g.players[0].score == sc[0] && g.players[1].score == sc[1]);
    printf("  (audit log replayed to identical final scores)\n");
}

// --- Snapshot: private rooms and live matches survive a restart -----------------
static uint8_t s_snap[sizeof(SnapHeader) + 8 * (sizeof(uint16_t) + sizeof(Room))];

static void test_snapshot(void) {
    fresh_server();
    int r;
    two_player_match(&r, 1, NULL);
    Room* rm = &S->rooms[r];
    for (int i = 0; i < 9; i++) if (!human_move(rm)) tick_ms(SRV_AI_PACE_MS);
    char key0[40], key1[40], code[8];
    key_of(0, key0);
    key_of(1, key1);
    snprintf(code, sizeof code, "%s", rm->code);
    // A dormant private room and a public waiting room (not kept) too.
    hello(2, "3.3.3.3", "Cy");
    int r2 = create(2, "\"private\":1");
    say(2, "{\"t\":\"leave\"}");
    hello(3, "4.4.4.4", "Dee");
    int r3 = create(3, "");
    CHECK(srv_dirty(S));

    Game before;
    memcpy(&before, &rm->g, sizeof before);
    size_t len = srv_snapshot(S, s_snap, sizeof s_snap, NOW);
    CHECK(len == sizeof(SnapHeader) + 2 * (sizeof(uint16_t) + sizeof(Room)));
    CHECK(!srv_dirty(S));

    // "Reboot": a fresh core, some time later.
    fresh_server();
    NOW += 90000;
    CHECK(srv_restore(S, s_snap, len, NOW));
    rm = &S->rooms[r];
    CHECK(rm->active && rm->phase == PHASE_PLAYING);
    CHECK(memcmp(&before, &rm->g, GAME_SERIAL_SIZE) == 0);  // byte-identical game
    CHECK(strcmp(rm->code, code) == 0);
    CHECK(!rm->m[0].connected && rm->m[0].away);
    CHECK(S->rooms[r2].active && S->rooms[r2].kind == ROOM_PRIVATE);
    CHECK(!S->rooms[r3].active);                        // public lobby not kept

    // Paused until someone returns; both rejoin by key and play on.
    uint32_t acts = rm->actions;
    tick_ms(10000);
    CHECK(rm->actions == acts);
    hello(10, "1.1.1.1", "Ann");
    hello(11, "2.2.2.2", "Bob");
    sayf(10, "{\"t\":\"rejoin\",\"key\":\"%s\"}", key0);
    sayf(11, "{\"t\":\"rejoin\",\"key\":\"%s\"}", key1);
    CHECK(rm->m[0].connected && rm->m[1].connected && !rm->m[0].away);
    CHECK(human_move(rm));
    CHECK(rm->actions == acts + 1);

    // A corrupt or foreign image is discarded, never half-loaded.
    fresh_server();
    SnapHeader h;
    memcpy(&h, s_snap, sizeof h);
    h.version++;
    memcpy(s_snap, &h, sizeof h);
    CHECK(!srv_restore(S, s_snap, len, NOW));
    CHECK(!S->rooms[r].active);
    CHECK(!srv_restore(S, s_snap, 3, NOW));
}

// --- Silence: clients ping; the quiet are dropped ---------------------------------
static void test_silence(void) {
    fresh_server();
    hello(0, "1.1.1.1", "Ann");
    hello(1, "2.2.2.2", "Bob");
    for (int i = 0; i < 6; i++) {
        say(0, "{\"t\":\"ping\"}");
        tick_ms(10000);
    }
    CHECK(!was_kicked(0));
    CHECK(was_kicked(1));
    CHECK(last_msg(0, "pong") != NULL);
}

// --- Abuse handling -----------------------------------------------------------
static void test_abuse(void) {
    fresh_server();

    // Message flood: the rate bucket kicks the connection once the burst is
    // spent (all at one instant, so no refill intervenes).
    hello(0, "1.1.1.1", "Ann");
    for (int i = 0; i < RATE_BURST + 10 && !was_kicked(0); i++) say(0, "{\"t\":\"ping\"}");
    CHECK(was_kicked(0));
    srv_client_gone(S, 0, NOW);

    // Malformed JSON: error + kick, never a crash.
    hello(1, "2.2.2.2", "Bob");
    say(1, "{\"t\":\"act");
    CHECK(was_kicked(1));
    srv_client_gone(S, 1, NOW);

    // Per-IP create cap.
    int hits = 0;
    for (int i = 0; i < IPCAP_MAX + 1; i++) {
        int id = 100 + i;
        hello(id, "6.6.6.6", "Spam");
        say(id, "{\"t\":\"create\"}");
        if (last_msg(id, "welcome")) hits++;
        NOW += 200;
    }
    CHECK(hits == IPCAP_MAX);

    // Deterministic garbage at hello'd clients, in and out of rooms.
    fresh_server();
    uint32_t lcg = 4242;
    for (int iter = 0; iter < 3000; iter++) {
        int id = iter % 32;
        if (!S->clients[id].used) {
            hello(id, "7.7.7.7", "Fuzz");
            if (iter % 3 == 0) say(id, "{\"t\":\"create\"}");
        }
        char buf[100];
        lcg = lcg * 1664525u + 1013904223u; int len = (int)(lcg % 96u);
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            buf[i] = (char)(lcg >> 16);
        }
        srv_client_msg(S, id, buf, (size_t)len, NOW);
        if (was_kicked(id)) {
            srv_client_gone(S, id, NOW);
            s_kicked_n = 0;
        }
        NOW += 150;
    }

    // Well-formed messages with hostile values, in every state.
    fresh_server();
    int r;
    two_player_match(&r, 0, NULL);
    static const char* hostile[] = {
        "{\"t\":\"letai\",\"seat\":999}", "{\"t\":\"swap\",\"seat\":200}",
        "{\"t\":\"swap_ok\",\"seat\":9}", "{\"t\":\"join\",\"room\":999999999}",
        "{\"t\":\"action\",\"a\":200,\"slot\":200}", "{\"t\":\"team\",\"side\":7}",
        "{\"t\":\"settings\",\"owner\":\"x\"}", "{\"t\":\"peek\"}", "{\"t\":\"rejoin\"}",
        "{\"t\":\"list\",\"target\":99999}",
    };
    for (size_t i = 0; i < sizeof hostile / sizeof hostile[0]; i++) {
        say(0, hostile[i]);
        say(1, hostile[i]);
    }
    CHECK(S->rooms[r].active);
}

// --- Load: many concurrent tables to completion -------------------------------
static void test_many_tables(void) {
    fresh_server();
    // 200 two-player tables whose humans never move: the turn clock strikes
    // them out and the AI plays every match to the end.
    for (int i = 0; i < 200; i++) {
        char ip[24];
        snprintf(ip, sizeof ip, "10.9.%d.%d", i / 100, i % 100);
        int a = 2 * i, b = 2 * i + 1;
        hello(a, ip, "A");
        hello(b, ip, "B");
        sayf(a, "{\"t\":\"create\",\"private\":1,\"target\":50}");
        join_code(b, S->rooms[room_of(a)].code);
        ready(a);
        ready(b);
        NOW += 10;
    }
    tick_ms(SRV_COUNTDOWN_MS + 10);
    int live = 0;
    for (int r = 0; r < SRV_MAX_ROOMS; r++) live += S->rooms[r].active && S->rooms[r].phase == PHASE_PLAYING;
    CHECK(live == 200);

    int64_t guard = NOW + (int64_t)12 * 60 * 60 * 1000;
    int playing = live;
    while (playing > 0 && NOW < guard) {
        for (int i = 0; i < 400; i++) srv_client_msg(S, i, "{\"t\":\"ping\"}", 12, NOW);
        tick_ms(1000);
        playing = 0;
        for (int r = 0; r < SRV_MAX_ROOMS; r++) {
            playing += S->rooms[r].active && S->rooms[r].phase == PHASE_PLAYING;
        }
    }
    CHECK(playing == 0);
    printf("  (200 concurrent tables all completed and returned to their rooms)\n");
}

// --- /status ------------------------------------------------------------------
static void test_status(void) {
    fresh_server();
    hello(0, "1.1.1.1", "Ann");
    hello(1, "2.2.2.2", "Bob");
    create(0, "");
    create(1, "\"private\":1");
    say(1, "{\"t\":\"leave\"}");
    char buf[256];
    srv_status_json(S, buf, sizeof buf, NOW);
    CHECK(jfield(buf, "public") == 1);
    CHECK(jfield(buf, "private") == 1);
    CHECK(jfield(buf, "dormant") == 1);
    CHECK(jfield(buf, "clients") == 2);
}

int main(void) {
    printf("test_server: ws + wire codecs, names, waiting rooms, private rooms,\n");
    printf("             owner settings, partners, room list, idle players,\n");
    printf("             leave policy, pause, forfeit, rematch, redaction,\n");
    printf("             turn clock, reveal backstop, audit replay, snapshot,\n");
    printf("             silence, abuse, 200-table load, status\n");
    test_ws_codec();
    test_wire_codec();
    test_names();
    test_lobby_ready();
    test_lobby_leave_and_grace();
    test_private_rooms();
    test_owner_settings();
    test_partners();
    test_room_list();
    test_idle_not_ready();
    test_leave_policy();
    test_wait_policy();
    test_everyone_away();
    test_forfeit_and_switch();
    test_rematch();
    test_wire_redaction();
    test_turn_clock();
    test_reveal_backstop();
    test_audit_replay();
    test_snapshot();
    test_silence();
    test_abuse();
    test_many_tables();
    test_status();
    if (failures == 0) {
        printf("OK: all checks passed\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
