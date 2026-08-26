// Unit tests for the multiplayer daemon: the ws/wire codecs and the whole
// game-pool core (rooms, tokens, turn clocks, AI takeover, matchmaking,
// redacted broadcast), driven socket-free with injected time — the loopback
// harness the multiplayer plan calls N0. Includes the sources directly, as
// every suite in this repo does.
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
#define CAP_MSGS 512
typedef struct {
    int  client;
    char text[2048];
} SentMsg;
static SentMsg s_sent[CAP_MSGS];
static int s_sent_n;

static int  s_kicked[64];
static int  s_kicked_n;
static char s_logs[64][4200];
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
    if (s_kicked_n < 64) s_kicked[s_kicked_n++] = client;
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

// Connect + hello in one step.
static void connect_hello(int client, const char* ip) {
    srv_client_connected(S, client, ip);
    say(client, "{\"t\":\"hello\",\"v\":1}");
}

static void tick_ms(int64_t ms) {
    NOW += ms;
    srv_tick(S, NOW);
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
        int len = (int)(lcg = lcg * 1664525u + 1013904223u) % 100;
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

// --- Rooms: create/join/play/finish ------------------------------------------
static void test_room_lifecycle(void) {
    fresh_server();
    connect_hello(0, "1.1.1.1");
    connect_hello(1, "2.2.2.2");
    CHECK(last_msg(0, "hi") != NULL);

    say(0, "{\"t\":\"create\",\"players\":2,\"target\":500}");
    const char* w0 = last_msg(0, "welcome");
    CHECK(w0 && jfield(w0, "seat") == 0 && jfield(w0, "waiting") == 1);
    char code[8], tok0[40], tok1[40];
    CHECK(jstr(w0, "code", code, sizeof code));
    CHECK(jstr(w0, "token", tok0, sizeof tok0));
    CHECK(strlen(tok0) == 32);

    // Second seat joins by code; a 2-player table then auto-starts.
    char join[64];
    snprintf(join, sizeof join, "{\"t\":\"join\",\"code\":\"%s\"}", code);
    say(1, join);
    const char* w1 = last_msg(1, "welcome");
    CHECK(w1 && jfield(w1, "seat") == 1);
    CHECK(jstr(w1, "token", tok1, sizeof tok1));
    const char* st0 = last_msg(0, "state");
    CHECK(st0 && jfield(st0, "phase") == PHASE_DRAW);   // deal skipped server-side
    CHECK(jfield(st0, "players") == 2);
    CHECK(jfield(st0, "seat") == 0);

    // Play scripted turns until the round ends: draw stock, place at a
    // rotating slot (the same shape test_game's driver uses).
    Room* rm = &S->rooms[0];
    CHECK(rm->active && !rm->waiting);
    int slot = 0;
    for (int i = 0; i < 500 && rm->g.phase != PHASE_ROUND_OVER; i++) {
        int actor = rm->g.turn;
        say(actor, "{\"t\":\"action\",\"a\":0}");                  // DRAW_STOCK
        char place[64];
        snprintf(place, sizeof place, "{\"t\":\"action\",\"a\":2,\"slot\":%d}",
                 slot);
        slot = (slot + 1) % RACK_SLOTS;
        if (rm->g.phase == PHASE_PLACE) say(actor, place);
        NOW += 100;
    }
    CHECK(rm->g.phase == PHASE_ROUND_OVER);
    const char* over = last_msg(1, "state");
    CHECK(jfield(over, "phase") == PHASE_ROUND_OVER);
    CHECK(strstr(over, "\"racks\":[[") != NULL);   // the reveal is public

    // Acting out of turn / after the round is over is rejected cleanly.
    say(0, "{\"t\":\"action\",\"a\":0}");
    CHECK(last_msg(0, "error") != NULL);

    // Both confirm: next round deals.
    say(0, "{\"t\":\"next\"}");
    CHECK(rm->g.phase == PHASE_ROUND_OVER);   // one confirm is not enough
    say(1, "{\"t\":\"next\"}");
    CHECK(rm->g.phase == PHASE_DRAW);
    CHECK(rm->g.round_no == 2);

    // Both leave: seats fall to AI; the abandoned-room GC closes it.
    say(0, "{\"t\":\"leave\"}");
    say(1, "{\"t\":\"leave\"}");
    CHECK(rm->active);
    int64_t deadline = NOW + SRV_ABANDON_MS + 10000;
    while (rm->active && NOW < deadline) tick_ms(1000);
    CHECK(!rm->active);
    CHECK(S->clients[0].state == CL_LOBBY);
}

// --- Redaction on the wire ----------------------------------------------------
static void test_wire_redaction(void) {
    fresh_server();
    connect_hello(0, "1.1.1.1");
    connect_hello(1, "2.2.2.2");
    say(0, "{\"t\":\"create\",\"players\":2}");
    char code[8], join[64];
    jstr(last_msg(0, "welcome"), "code", code, sizeof code);
    snprintf(join, sizeof join, "{\"t\":\"join\",\"code\":\"%s\"}", code);
    say(1, join);

    Room* rm = &S->rooms[0];
    int actor = rm->g.turn;
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

    // The last-action echo for a stock draw shows only a card back.
    CHECK(jfield(st_other, "card") == 0);

    // A discard exchange is public: the displaced card is the echo.
    uint8_t displaced = rm->g.players[actor].rack.slots[4];
    say(actor, "{\"t\":\"action\",\"a\":2,\"slot\":4}");
    st_other = last_msg(other, "state");
    CHECK(jfield(st_other, "card") == displaced);

    // Nothing anywhere on the wire ever carries the seed.
    CHECK(strstr(st_actor, "seed") == NULL && strstr(st_other, "seed") == NULL);
}

// --- Turn clock, takeover, disconnect, rejoin --------------------------------
static void test_timeouts_and_rejoin(void) {
    fresh_server();
    connect_hello(0, "1.1.1.1");
    connect_hello(1, "2.2.2.2");
    say(0, "{\"t\":\"create\",\"players\":2}");
    char code[8], join[64], tok[40];
    jstr(last_msg(0, "welcome"), "code", code, sizeof code);
    snprintf(join, sizeof join, "{\"t\":\"join\",\"code\":\"%s\"}", code);
    say(1, join);
    jstr(last_msg(1, "welcome"), "token", tok, sizeof tok);

    Room* rm = &S->rooms[0];
    int first = rm->g.turn;

    // The human dawdles: at the deadline the AI plays their whole turn.
    tick_ms(SRV_TURN_MS / 2);
    CHECK(rm->g.turn == first);            // clock still running
    tick_ms(SRV_TURN_MS);                  // past the deadline
    tick_ms(1);                            // (place decision may need a 2nd tick)
    tick_ms(SRV_TURN_MS + 1);
    CHECK(rm->seats[first].strikes >= 1);

    // Three strikes: the seat flips to AI and plays at AI pace.
    for (int i = 0; i < 20 && !rm->seats[first].ai_control; i++) {
        tick_ms(SRV_TURN_MS + 1);
    }
    CHECK(rm->seats[first].ai_control);
    const char* st = last_msg(1 - first, "state");
    CHECK(st && strstr(st, "\"ai\":[") != NULL);

    // Any live message from the struck-out human reclaims the seat.
    say(first, "{\"t\":\"next\"}");
    CHECK(!rm->seats[first].ai_control);

    // Disconnect seat 1 entirely: the AI takes over on its own clock.
    srv_client_gone(S, 1, NOW);
    CHECK(!rm->seats[1].connected);
    uint32_t before = rm->actions;
    for (int i = 0; i < 400 && !(rm->actions > before + 4); i++) tick_ms(1000);
    CHECK(rm->actions > before + 4);       // the table kept playing

    // Rejoin by token on a new socket: seat rebinds, play hands back.
    connect_hello(7, "9.9.9.9");
    char rejoin[80];
    snprintf(rejoin, sizeof rejoin, "{\"t\":\"rejoin\",\"token\":\"%s\"}", tok);
    say(7, rejoin);
    CHECK(rm->seats[1].connected && rm->seats[1].client == 7);
    CHECK(jfield(last_msg(7, "welcome"), "seat") == 1);
    CHECK(last_msg(7, "state") != NULL);

    // A bogus token finds nothing.
    connect_hello(8, "9.9.9.8");
    say(8, "{\"t\":\"rejoin\",\"token\":\"00000000000000000000000000000000\"}");
    CHECK(last_msg(8, "error") != NULL);
}

// --- Quick match --------------------------------------------------------------
static void test_quick_match(void) {
    fresh_server();
    for (int i = 0; i < 4; i++) {
        char ip[16];
        snprintf(ip, sizeof ip, "10.0.0.%d", i);
        connect_hello(i, ip);
        say(i, "{\"t\":\"quick\"}");
    }
    // Four in the queue: instant table, four humans.
    for (int i = 0; i < 4; i++) {
        CHECK(last_msg(i, "welcome") != NULL);
        const char* st = last_msg(i, "state");
        CHECK(st && strstr(st, "\"ai\":[0,0,0,0]") != NULL);
    }

    // Two in the queue: backfilled with AI after the pairing window.
    connect_hello(10, "10.0.1.1");
    connect_hello(11, "10.0.1.2");
    say(10, "{\"t\":\"quick\"}");
    say(11, "{\"t\":\"quick\"}");
    CHECK(last_msg(10, "welcome") == NULL);
    tick_ms(SRV_QUEUE_PAIR_MS + 1000);
    const char* st10 = last_msg(10, "state");
    CHECK(st10 && strstr(st10, "\"ai\":[0,0,1,1]") != NULL);

    // A loner gets a table against three AI after the solo window.
    connect_hello(12, "10.0.2.1");
    say(12, "{\"t\":\"quick\"}");
    tick_ms(SRV_QUEUE_SOLO_MS + 1000);
    const char* st12 = last_msg(12, "state");
    CHECK(st12 && strstr(st12, "\"ai\":[0,1,1,1]") != NULL);

    // Leaving the queue by disconnect removes the entry.
    connect_hello(13, "10.0.3.1");
    say(13, "{\"t\":\"quick\"}");
    CHECK(S->queue_len == 1);
    srv_client_gone(S, 13, NOW);
    CHECK(S->queue_len == 0);
}

// --- Abuse handling -----------------------------------------------------------
static void test_abuse(void) {
    fresh_server();

    // Message flood: the rate bucket kicks the connection.
    connect_hello(0, "1.1.1.1");
    for (int i = 0; i < 40 && !was_kicked(0); i++) say(0, "{\"t\":\"ping\"}");
    CHECK(was_kicked(0));
    srv_client_gone(S, 0, NOW);

    // Malformed JSON: error + kick, never a crash.
    connect_hello(1, "2.2.2.2");
    say(1, "{\"t\":\"act");
    CHECK(was_kicked(1));
    srv_client_gone(S, 1, NOW);

    // Wrong protocol version: told to upgrade.
    srv_client_connected(S, 2, "3.3.3.3");
    say(2, "{\"t\":\"hello\",\"v\":99}");
    CHECK(last_msg(2, "error") != NULL);
    srv_client_gone(S, 2, NOW);

    // Per-IP start cap: the 31st create from one address is refused.
    int hits = 0;
    for (int i = 0; i < IPCAP_MAX + 1; i++) {
        int id = 100 + i;
        connect_hello(id, "6.6.6.6");
        say(id, "{\"t\":\"create\",\"players\":2}");
        if (last_msg(id, "welcome")) hits++;
        NOW += 200;   // stay inside the window, off the rate bucket
    }
    CHECK(hits == IPCAP_MAX);

    // Deterministic garbage at a fresh, hello'd client: server survives all.
    fresh_server();
    uint32_t lcg = 4242;
    for (int iter = 0; iter < 2000; iter++) {
        int id = iter % 32;
        if (!S->clients[id].used) connect_hello(id, "7.7.7.7");
        char buf[100];
        int len = (int)(lcg = lcg * 1664525u + 1013904223u) % 96;
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            buf[i] = (char)(lcg >> 16);
        }
        srv_client_msg(S, id, buf, (size_t)len, NOW);
        // Kicked clients really disconnect, then reconnect fresh.
        if (was_kicked(id)) {
            srv_client_gone(S, id, NOW);
            s_kicked_n = 0;
        }
        NOW += 150;
    }
}

// --- Load: many concurrent tables to completion -------------------------------
static void test_many_tables(void) {
    fresh_server();

    // 200 private tables, each started solo against three AI. The humans
    // then vanish, and every table must still play itself to a finished
    // match (or abandoned-GC) without wedging the pool.
    for (int i = 0; i < 200; i++) {
        char ip[24];
        snprintf(ip, sizeof ip, "10.9.%d.%d", i / 100, i % 100);
        connect_hello(i, ip);
        say(i, "{\"t\":\"create\",\"players\":4}");
        say(i, "{\"t\":\"start\"}");
        NOW += 10;
    }
    int live = 0;
    for (int r = 0; r < SRV_MAX_ROOMS; r++) {
        if (S->rooms[r].active) live++;
    }
    CHECK(live == 200);

    for (int i = 0; i < 200; i++) srv_client_gone(S, i, NOW);

    int64_t guard = NOW + (int64_t) 4 * 60 * 60 * 1000;   // 4 virtual hours
    int remaining = live;
    while (remaining > 0 && NOW < guard) {
        tick_ms(1000);
        remaining = 0;
        for (int r = 0; r < SRV_MAX_ROOMS; r++) {
            if (S->rooms[r].active) remaining++;
        }
    }
    CHECK(remaining == 0);
    printf("  (200 concurrent tables all completed and recycled)\n");
}

// --- Audit log replays byte-for-byte ------------------------------------------
static void test_audit_replay(void) {
    fresh_server();
    connect_hello(0, "1.1.1.1");
    say(0, "{\"t\":\"create\",\"players\":2,\"target\":50}");
    say(0, "{\"t\":\"start\"}");   // seat 1 stays AI; tiny target ends fast

    Room* rm = &S->rooms[0];
    CHECK(rm->active && !rm->waiting);
    uint64_t seed = rm->rules.seed;
    Rules rules = rm->rules;

    // Let the AI play seat 1 and the timeout clock play seat 0 to the end.
    int64_t guard = NOW + (int64_t) 8 * 60 * 60 * 1000;
    while (rm->active && NOW < guard) tick_ms(1000);
    CHECK(!rm->active);

    // The close log line carries seed + the action log; replay it.
    char* line = NULL;
    for (int i = 0; i < s_logs_n && i < 64; i++) {
        if (strstr(s_logs[i], "why=finished")) line = s_logs[i];
    }
    CHECK(line != NULL);
    if (!line) return;

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

// --- /status ------------------------------------------------------------------
static void test_status(void) {
    fresh_server();
    connect_hello(0, "1.1.1.1");
    say(0, "{\"t\":\"create\",\"players\":2}");
    char buf[192];
    srv_status_json(S, buf, sizeof buf, NOW);
    CHECK(jfield(buf, "rooms") == 1);
    CHECK(jfield(buf, "waiting") == 1);
    CHECK(jfield(buf, "clients") == 1);
}

// --- Creator leaves a waiting table -------------------------------------------
static void test_creator_leaves(void) {
    fresh_server();
    connect_hello(0, "1.1.1.1");
    connect_hello(1, "2.2.2.2");
    say(0, "{\"t\":\"create\",\"players\":3}");
    char code[8], join[64];
    jstr(last_msg(0, "welcome"), "code", code, sizeof code);
    snprintf(join, sizeof join, "{\"t\":\"join\",\"code\":\"%s\"}", code);
    say(1, join);
    CHECK(S->rooms[0].active && S->rooms[0].waiting);

    srv_client_gone(S, 0, NOW);            // creator vanishes pre-start
    CHECK(!S->rooms[0].active);            // the table dies promptly
    CHECK(S->clients[1].state == CL_LOBBY);
    say(1, "{\"t\":\"create\",\"players\":2}");   // and the joiner is free again
    CHECK(last_msg(1, "welcome") != NULL);
}

int main(void) {
    printf("test_server: ws + wire codecs, room lifecycle, wire redaction,\n");
    printf("             turn clock/takeover/rejoin, quick match, abuse,\n");
    printf("             200-table load, audit replay, status, creator-leave\n");
    test_ws_codec();
    test_wire_codec();
    test_room_lifecycle();
    test_wire_redaction();
    test_timeouts_and_rejoin();
    test_quick_match();
    test_abuse();
    test_many_tables();
    test_audit_replay();
    test_status();
    test_creator_leaves();
    if (failures == 0) {
        printf("OK: all checks passed\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
