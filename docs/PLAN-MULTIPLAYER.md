# openrackem v2 — multiplayer plan

## 1. Scope

The hosted game-pool engine promised in PLAN.md §1: real people at the same
table over the network, on every platform v1 ships (desktop, web, Android,
iOS), cross-playing. Everything happens in **rooms** (protocol v3,
2026-11-01; v2 had public and private rooms, v1 Quick Match):

- There is **one kind of room**. It is listed by who's in it while it waits,
  joinable while it isn't full, and gone the moment it's empty. Its settings
  (Seats, Play To, Bonus, Partners) are fixed when it's made. Seats is the
  room's capacity, 2 to 4 (4 unless asked for; Partners needs 4).
- A **waiting room** shows every name with a ready flag; when everyone
  present (2–4) is ready, the match deals for exactly those players. After a
  match the table returns to the same waiting room, with the result on show,
  and the room is listed again.

Decisions taken (2026-08-26, revised 2026-11-01): authoritative **C daemon
on fly.io** (a dedicated app on one tiny machine — see `fly.toml`),
**anonymous member keys** (no accounts). Players choose a name (required to
play online, checked against a small blocklist); there is no chat, so names
are the only text strangers see. Leaving keeps your seat with an AI standing
in, so no penalty system exists. A tie at the target plays another round.

Single-player v1 continues to work fully offline; multiplayer is a menu item,
not a rewrite.

## 2. What v1 already guarantees

Every §14 constraint was honored and is test-enforced, so the server work
starts from here rather than working toward it:

1. `Game` is a flat, pointer-free POD (~400 bytes): a pool slot is a struct
   member, a snapshot is a `memcpy`.
2. A match replays byte-for-byte from `(seed, Rules, action list)`: free
   audit logs, bug repros, and reconnect.
3. `game.c`/`rules.c`/`ai.c` compile standalone with libc, integer math only:
   the daemon links them unchanged, deterministically, on any host.
4. `GameView` redacts by construction (rack privacy, stock order, and the RNG
   seed — the reviewed leak class): per-seat client payloads are a solved
   problem.
5. `game_apply` is total and validating: a hostile socket is the same code
   path as a mis-clicked button. The engine has no seat field in `Action` by
   design — **the server checks `msg.seat == g->turn` before applying**.
6. Presentation state (`Anim`) is excluded from the serialized form, and the
   pacing constants are compile-time overridable: the server build sets them
   to zero and paces turns with its own wall clock.

## 3. Architecture

**Authoritative server, no exceptions.** Hidden information (racks, stock
order) makes lockstep/P2P impossible without a trusted dealer. One
single-threaded C daemon owns every `Game`; clients hold only what their seat
may see.

```
 native app ──┐                          ┌────────────────────────────┐
 web (WASM) ──┼── WSS ── fly edge ── WS ──┤ orserverd (C, one thread)  │
 native app ──┘      (TLS terminates      │  rooms[] { Game, seats[4] }│
                      at Fly's proxy)     │  queue, timers, audit log  │
                                          └────────────────────────────┘
```

- **Transport: WebSocket everywhere.** It is the only option for the WASM
  build, and native speaks it too, so web/native cross-play is free. TLS is
  terminated at Fly's edge proxy; the daemon serves plain WS on the internal
  port and stays crypto-free.
- **Wire format: versioned JSON.** At card-game rates (a few messages per
  second per table, tops) readability beats bytes. Every message carries
  `v`; the server rejects mismatched clients with an upgrade notice.
- **The daemon** is `src-server/` + the v1 engine objects: an epoll loop
  and a from-scratch RFC 6455 subset (`ws.c` — the repo vendors nothing), a
  strict flat-JSON codec (`wire.c`), and the socket-free, time-injected pool
  core (`server_core.c`) that tests/test_server.c drives directly. No
  threads, no allocation after startup; fixed pools (4096 rooms, 2048
  connections, ~40 MB total) bound everything.

## 4. Protocol (v3)

Client → server messages are flat JSON objects (≤ 8 keys, non-negative ints
and identifier strings — `wire.c`); anything else closes the socket.

| msg | fields | notes |
|---|---|---|
| `hello` | `v`=3, `name` | `need_name` / `bad_name` / `upgrade` errors |
| `list` / `unlist` | — | subscribe to `rooms` pushes |
| `create` | `target`, `bonus`, `partners`, `seats`, `key` | settings fixed for the room's life; `seats` 2–4, default 4; a request the room can't honor is refused with `bad_rules`, never adjusted; `busy` when no room is free |
| `join` | `room`, `key` | `key` surrenders a seat held elsewhere; `full`, `started` (also while the room counts down), `rate` |
| `rejoin` | `key` | back to a seat (Return to Game, reconnect) |
| `ready` | `on` | waiting room |
| `team` | — | Partners: switch sides, trading places if the other side is full |
| `force` | — | Start without them (offered after a minute of waiting) |
| `action`, `next` | `a`, `slot` | as v1 |
| `letai` | `seat` | hand an absent player's seat to the AI |
| `leave` | — | waiting room: leave; match: step away (seat kept) |
| `here` / `forfeit` | — | back from stepping away / give the seat up |
| `ping` | — | every 10 s; 25 s of silence drops the client |

Server → client: `hi`, `rooms` (every waiting room that isn't full or
counting down, fullest first, up to 20: ids, names, ready, rules, seats),
`lobby` (the waiting room: names, ready, teams, seats, countdown, why it can't
start, games played), `welcome` (member key, room id), `state` (v1 fields +
`ctl[]` away flags + the absent-player wait), `back`, `removed`, `error`.

`seats` was added within v3 (2026-11-05) as an optional field in both
directions: a client that doesn't send it gets a four-seat room, and a client
that doesn't read it shows four. Nothing else on the wire changed, so there
was no version bump.

Member keys and match seeds are drawn from the operating system's random
source (`SrvIo.entropy`, `getrandom` in the daemon, which won't start without
it). Either one reconstructs hidden state — a key is a seat, a seed is every
card — so neither may be derivable from anything a client is sent. Until
2026-11-05 both came from one seeded xorshift whose whole state a single key
gave away; that generator now only stands in for the random source in the
unit tests.

The shuffle is hardened to match (2026-11-05): the engine deals from a
ChaCha20 keystream keyed by the match seed instead of from xorshift, so the
cards a seat is shown don't lead back to the seed or to anyone else's cards
(`docs/PLAN.md` §14.2). Replay from `(seed, Rules, action list)` and the
audit log are unchanged.

Redacted state is unchanged from v1: own rack in full, others only at the
reveal, never the stock order, `rng`, or seed. The engine's events word is
sent exactly once, on the broadcast that produced it; a re-send (someone
stepping away or back) carries none, so no client replays a sound.

## 5. Server behavior

- **Starting:** everyone present ready, ≥ 2 players (Partners: exactly 4,
  two teams of two) → 2 s countdown → deal. The countdown is a commitment:
  the room is off the list, a join is refused as `started`, and un-ready and
  Switch Team are ignored. Leaving still works, and stops it only if too few
  are left to play; a link that drops and returns during it is dealt in.
  (A join or an un-ready used to cancel it, which let one member hold a room
  in the waiting room indefinitely.) Once two players have been ready 60 s
  with someone present not ready, the ready players may Start without them
  (the idle ones are removed).
- **Not ready:** a connected member who stays not ready for 10 min is moved
  out (`removed`, `idle`), so a room someone walked away from doesn't sit on
  the list as a table nobody can start. Ready is the sign of someone waiting:
  a ready player keeps a room for as long as they like.
- **Turn clock:** 45 s per decision; at expiry the AI plays that decision; 3
  consecutive timeouts hand the seat to the AI until the player acts.
- **Round over:** the next round deals when every present human has
  confirmed, or after 30 s.
- **Leaving:** stepping away or dropping keeps the seat. At that player's
  turn the table waits 30 s, any present player can hand the seat to the AI,
  and the AI plays it until they return. With nobody present the match
  pauses and is closed after 5 min.
- **Between games:** the match ends into the same waiting room, nobody
  ready, the room listed again. A member whose link is down at that moment
  (or who drops in the waiting room) keeps the seat for 2 min, is shown as
  away, and doesn't count toward starting. An empty waiting room is deleted.
- **Room list:** every joinable room, fullest first then newest, up to 20,
  pushed to subscribers when anything changed, at most every 2 s.
- **Durability:** live matches and occupied waiting rooms are snapshotted to
  the volume (`OR_DATA_DIR`) every few seconds and on shutdown and reloaded
  at boot, so a deploy neither ends games nor scatters a group between them.
- **Audit log:** per match, `(seed, Rules, action list, scores)` to stdout.
- **Limits:** per address, 30 room creates and 60 joins per 10 min (`rate`)
  and 32 connections at once (`busy`, then closed); per connection, a message
  budget; malformed frames close the socket; a socket that hasn't finished
  the WebSocket upgrade in 10 s is closed. An IPv6 address is counted by its
  /64.

## 6. Engine additions (all v1-compatible, all tiny)

```c
// Pool-friendly construction (game_create keeps its static single instance
// for the local game).
void game_init(Game* out, const Rules* rules);

// Per-seat snapshot redaction: the GameView discipline applied to a whole
// Game, with the round-over reveal built in. What this returns is the ONLY
// thing a client ever holds.
void game_redact_for(Game* out, const Game* g, int seat);
```

`game_redact_for` gets the same adversarial treatment `game_view_for` got:
tests that reconstruct-from-snapshot cannot recover any hidden card, in any
phase, plus the seed/rng zeroing pinned. Nothing else in the engine changes.

## 7. Client changes

- `net.c`/`net.h`: a WebSocket session behind a small interface —
  emscripten's WebSocket API on web, a from-scratch plain-WebSocket client
  (`net_posix.c`, OpenSSL for wss) on native, Network.framework on Apple.
- **Play Online** (name required) opens the room list: Return to Game (when
  a seat is held), New Room, then the rooms. A room opens into the waiting
  room: Ready, Leave, Switch Team under Partners, Start Without Them when
  offered (only Leave while the countdown runs). Nothing else. The seat key
  of a live match persists in prefs.
- **New Room** opens the room's settings — Seats, Play To, Bonus Scoring,
  Partners — then Create Room (2026-11-05). Before that a room silently took
  its rules from the Options screen, where Players and Difficulty meant
  nothing online and Partners could only be switched on with the offline
  Players at 4. The settings are the room's own (`NgRoomSetup`), remembered
  in prefs, and independent of Options, which is now for CPU games only. This
  is not the New Room screen v3 removed: no codes, no privacy, no owner, and
  nothing can be changed once the room exists.
- A create, join or rejoin that hasn't been answered yet holds back the next
  one, so a double tap sends one request. The standings screen names the
  Bonus variant when it is on.
- In a network session the frame loop holds the latest redacted `Game` and
  renders it with the existing renderers (they already draw opponents as
  backs and respect held-card visibility). Input produces the same `Action`s;
  instead of `game_apply`, they go to the socket. `last_action` + `events`
  drive the existing sounds and flight animations.
- The turn clock rides the turn line; away seats show an "AI" pill; the
  absent-player wait is a banner with Let AI play; round screens show their
  auto-advance countdown. After a match the waiting room shows the result
  line ("ALEX WON 515 - 430 - 300") until the next deal.
- Losing focus steps away at once (iOS signals it from the app shell, since
  frames stop on suspend) and returning reclaims the seat.

## 8. Operations and privacy

- One static `orserverd` binary (`make server`) in a FROM-scratch image
  (`Dockerfile.server`), deployed with `fly deploy` on a shared-cpu-1x /
  256 MB machine pinned to exactly one instance (`fly.toml` explains why),
  as the `openrackem-rooms` app with a 1 GB volume for the snapshot; the v1
  `openrackem-server` app stays up for old clients until it is retired.
  `/status` (rooms, waiting / playing / paused, clients,
  uptime) doubles as the health check.
- No accounts and no persistent identity beyond member keys. Player names
  and room membership live server-side only while a room exists (as long as
  someone is in it); audit logs expire. **The store listings change**: "never touches the
  network" and the Play data-safety answers must be rewritten when
  multiplayer ships (network yes; personal data still none). PRIVACY.md gets
  a section saying exactly what a match transmits and how long the audit log
  lives.
- Server versioning: protocol `v` bump = old clients get a "please update"
  error; the store release precedes the server flip.

## 9. Testing

- **N0 loopback:** a headless harness runs daemon + N scripted clients in
  one process over a socketpair; full matches, byte-compared against a local
  `game.c` replay of the same action log. CI job.
- **Redaction:** adversarial tests on `game_redact_for` (the seed-leak
  lesson, institutionalized).
- **Chaos:** scripted disconnect/rejoin/timeout at every phase; a fuzzing
  client that sends malformed JSON and illegal actions (the daemon must
  never crash or leak a hidden card — run under ASan in CI).
- **Load smoke:** 1000 concurrent AI tables on the daemon in CI, memory
  flat, no timer drift.
- **Web e2e:** the existing Playwright path, pointed at a local daemon: two
  browser tabs play each other.

## 10. Milestones

Status 2026-11-05: room creation hardened — random keys and seeds, a
keystream shuffle, the committed countdown, Seats, validated settings, the
not-ready clock and the per-address limits on the server (§4, §5), and a New
Room screen with the room's own settings on the client (§7). The snapshot
format moved to 3, so that deploy starts with no saved rooms.

Status 2026-11-01: **protocol v3 (one room kind) is implemented and
tested** — the listed room, ready-up, Partners teams, leave-and-return, the
between-games flow, snapshots — on every client. `make net-multi` plays every
room flow (2/3/4 players, three games in one room, Partners switch,
step-away, away at match end, daemon restarts mid-match and between games)
against the real daemon and gates the deploy.

Status 2026-10-26: protocol v2 (public and private rooms) was implemented
and tested; v3 collapsed the two kinds into one.

Status 2026-08-26: **N0, N1, and N2 are implemented and tested**. The daemon
(`make server`, `Dockerfile.server`, `fly.toml`) and the desktop online client
(Play Online → Quick Match / Create Table / Join by Code, wired to the real
daemon) both work; a full match plays over loopback in CI (`make net-e2e`).
Native uses a from-scratch plain-WebSocket client (`net_posix.c`); the web
build stubs online for now (browser wss + emscripten WebSocket is the N3
follow-up), and a TLS client for the public fly.io wss endpoint is pending —
today the native client targets a plain-ws daemon set via OPENRACKEM_SERVER /
OPENRACKEM_PORT (default 127.0.0.1:8080).

- **N0 — Protocol on loopback.** `game_init` + `game_redact_for` + tests;
  protocol spec frozen; headless daemon core with the loopback harness in CI.
  No UI, no sockets to the world.
- **N1 — The daemon.** Rooms, codes, tokens, turn clock, AI takeover,
  rejoin, audit log, rate limits. Chaos + fuzz tests green under ASan.
- **N2 — The client.** Play Online on desktop + web against a local daemon;
  two-tab Playwright e2e; then Android/iOS enablement.
- **N3 — Quick match.** The queue, AI backfill, handles, `/status`.
- **N4 — Ship.** VPS + caddy deploy, soak, privacy/listing updates, client
  store releases with the server address baked in, protocol-version flip
  rehearsed.

## 11. Risks

- **TLS in C** — avoided entirely by Fly's edge; the daemon never sees a
  certificate.
- **Deploys end live tables** — state is in one machine's memory by design;
  ship server updates in quiet hours and bump the protocol version when the
  wire format changes.
- **Queue liquidity** — AI backfill makes an empty queue invisible; the
  15/30 s rule guarantees a game.
- **Round-over consensus** — "all humans confirm or timer lapses" needs care
  so one AFK player can't hold the table; the reveal timer (already in the
  engine for full-AI games) is the backstop.
- **Store review friction** — adding networking changes Play data-safety and
  App Store privacy declarations; schedule review time in N4.
- **The static instance habit** — server code must never touch
  `game_create`; a lint/grep in CI keeps `orserverd` on `game_init`.
