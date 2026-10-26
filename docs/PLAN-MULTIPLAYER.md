# openrackem v2 — multiplayer plan

## 1. Scope

The hosted game-pool engine promised in PLAN.md §1: real people at the same
table over the network, on every platform v1 ships (desktop, web, Android,
iOS), cross-playing. Everything happens in **rooms** (protocol v2,
2026-10-26; Quick Match and creator-started tables were v1):

- **Public rooms** are listed by who's in them and have no code; they exist
  while anyone is in them.
- **Private rooms** are unlisted, joined by a 4-character code, and last 30
  days after their last match, so a group keeps one code.
- A **waiting room** shows every name with a ready flag; when everyone
  present (2–4) is ready, the match deals for exactly those players. After a
  match the table returns to the same waiting room.

Decisions taken (2026-08-26, revised 2026-10-26): authoritative **C daemon
on fly.io** (a dedicated app on one tiny machine — see `fly.toml`),
**anonymous member keys** (no accounts). Players choose a name (required to
play online, checked against a small blocklist); there is no chat, so names
are the only text strangers see. Leaving keeps your seat with an AI standing
in, so no penalty system exists.

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
  threads, no allocation after startup; fixed pools (1024 rooms, 2048
  connections, ~40 MB total) bound everything.

## 4. Protocol (v2)

Client → server messages are flat JSON objects (≤ 8 keys, non-negative ints
and identifier strings — `wire.c`); anything else closes the socket.

| msg | fields | notes |
|---|---|---|
| `hello` | `v`=2, `name` | `need_name` / `bad_name` / `upgrade` errors |
| `list` / `shuffle` / `unlist` | `target` (0 = any) | subscribe to `rooms` pushes |
| `peek` | `code` | status of a private code (the saved-room row) |
| `create` | `private`, `target`, `bonus`, `partners`, `away`, `key` | |
| `join` | `room` (public id) or `code`, `key` | `key` surrenders a seat held elsewhere |
| `best` | `target`, `key` | the joinable room closest to starting |
| `rejoin` | `key` | back to a seat (Return to Game, reconnect) |
| `ready` | `on` | waiting room |
| `team` / `swap` / `swap_ok` / `swap_no` | `side` / `seat` | Partners teams; a swap needs consent |
| `force` | — | private: Start without them |
| `settings` | `owner`, rules, `private`, `away` | owner key required, waiting room only |
| `action`, `next` | `a`, `slot` | as v1 |
| `letai` | `seat` | hand an absent player's seat to the AI |
| `leave` | — | waiting room: leave; match: step away (seat kept) |
| `here` / `forfeit` | — | back from stepping away / give the seat up |
| `ping` | — | every 10 s; 25 s of silence drops the client |

Server → client: `hi`, `rooms` (total + a sample of 20: ids, names, ready,
rules), `peeked`, `lobby` (the waiting room: names, ready, teams, countdown,
why it can't start, swap requests), `welcome` (member key, owner key for the
creator), `state` (v1 fields + `ctl[]` away flags + the absent-player wait),
`back`, `removed`, `error`.

Redacted state is unchanged from v1: own rack in full, others only at the
reveal, never the stock order, `rng`, or seed.

## 5. Server behavior

- **Starting:** everyone present ready, ≥ 2 players (Partners: exactly 4,
  two teams of two) → 2 s countdown → deal. A new arrival or an un-ready
  cancels it. Once everyone else has been ready 60 s, an idle player is
  removed (public) or the ready players may Start without them (private).
- **Turn clock:** 45 s per decision; at expiry the AI plays that decision; 3
  consecutive timeouts hand the seat to the AI until the player acts.
- **Leaving:** stepping away or dropping keeps the seat. At that player's
  turn the table waits 30 s (or indefinitely, in a private room set to Wait),
  any present player can hand the seat to the AI, and the AI plays it until
  they return. With nobody present the match pauses; a public one is closed
  after 5 min.
- **Room list:** each viewer gets a stable sample of 20 joinable public rooms,
  weighted toward fuller ones, refilled only as rooms fill, start, or empty;
  pushed at most every 2 s.
- **Durability:** private rooms and every live match are snapshotted to the
  volume (`OR_DATA_DIR`) every few seconds and on shutdown and reloaded at
  boot, so a deploy no longer ends games.
- **Audit log:** per match, `(seed, Rules, action list, scores)` to stdout.
- **Rate limits:** per-IP room creates; 10 wrong codes per minute per IP;
  per-connection message cap; malformed frames close the socket.

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
  emscripten's WebSocket API on web, libwebsockets client (vendored, like
  minih264) on native. Compiled out of the iOS/Android builds until N2 lands
  there.
- **Play Online** (name required) opens the room list: Return to Game, the
  saved private room, New Room, Enter Code, Join Best Room, then public
  rooms. Rooms open into the waiting room; Share sends a private room's
  invite (share sheet on phones, clipboard on desktop). The saved room, its
  owner key, and a live seat's member key persist in prefs.
- In a network session the frame loop holds the latest redacted `Game` and
  renders it with the existing renderers (they already draw opponents as
  backs and respect held-card visibility). Input produces the same `Action`s;
  instead of `game_apply`, they go to the socket. `last_action` + `events`
  drive the existing sounds and flight animations.
- The turn clock rides the turn line; away seats show an "AI" pill; the
  absent-player wait is a banner with Let AI play; round screens show their
  auto-advance countdown.
- Losing focus steps away at once (iOS signals it from the app shell, since
  frames stop on suspend) and returning reclaims the seat.

## 8. Operations and privacy

- One static `orserverd` binary (`make server`) in a FROM-scratch image
  (`Dockerfile.server`), deployed with `fly deploy` on a shared-cpu-1x /
  256 MB machine pinned to exactly one instance (`fly.toml` explains why),
  as the `openrackem-rooms` app with a 1 GB volume for the snapshot; the v1
  `openrackem-server` app stays up for old clients until it is retired.
  `/status` (public / private / dormant / playing / paused rooms, clients,
  uptime) doubles as the health check.
- No accounts and no persistent identity beyond member keys. Player names
  and room membership are stored server-side for private rooms (up to 30
  days after the last match); audit logs expire. **The store listings change**: "never touches the
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

Status 2026-10-26: **protocol v2 (rooms) is implemented and tested** —
public and private rooms, ready-up, Partners teams, leave-and-return,
snapshots — on every client. `make net-multi` plays every room flow (2/3/4
public, private rematch, Partners swap, step-away, daemon restart) against
the real daemon and gates the deploy.

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
