# openrackem v2 — multiplayer plan

## 1. Scope

The hosted game-pool engine promised in PLAN.md §1: real people at the same
table over the network, on every platform v1 ships (desktop, web, Android,
iOS), cross-playing. Two ways in:

- **Play with friends:** create a table, share a short room code, others join.
- **Quick match:** a public queue that seats 2–4 strangers, backfilling empty
  seats with AI after a short wait so a game always starts.

Decisions taken (2026-08-26): authoritative **C daemon on a small VPS**,
**anonymous per-match session tokens** (no accounts), public matchmaking
included. No chat and no user-entered names anywhere — seats get generated
table handles — so the public mode carries **zero moderation surface**. AI
takeover covers leavers and disconnects, so no penalty system exists either.

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
 native app ──┐                       ┌────────────────────────────┐
 web (WASM) ──┼── WSS ── caddy ── WS ─┤ orserverd (C, one thread)  │
 native app ──┘      (TLS terminator) │  rooms[] { Game, seats[4] }│
                                      │  queue, timers, audit log  │
                                      └────────────────────────────┘
```

- **Transport: WebSocket everywhere.** It is the only option for the WASM
  build, and native speaks it too, so web/native cross-play is free. TLS is
  terminated by caddy (or nginx) in front; the daemon serves plain WS on
  localhost and stays crypto-free.
- **Wire format: versioned JSON.** At card-game rates (a few messages per
  second per table, tops) readability beats bytes. Every message carries
  `v`; the server rejects mismatched clients with an upgrade notice.
- **The daemon** is `src-server/orserverd.c` + the v1 engine objects. Event
  loop from libwebsockets (or mongoose); no threads, no allocation per
  message. A fixed pool (`MAX_ROOMS`, e.g. 4096) bounds everything.

## 4. Protocol

Client → server:

| msg | fields | notes |
|---|---|---|
| `hello` | `v`, `client` | first message; server replies `welcome` or `upgrade` |
| `create` | rules subset (players, bonus, partners, target) | returns a room code |
| `join` | `code` | seat assigned in join order |
| `quick` | preferred player count (or any) | enters the matchmaking queue |
| `rejoin` | `seat_token` | resumes a live seat after a disconnect |
| `action` | `type`, `slot` | v1's `Action`, verbatim |
| `next_round` | — | the round-over confirm (any human may advance? no: each seat confirms; table advances when all connected humans confirmed or the reveal timer lapses) |
| `leave` | — | seat degrades to AI immediately |

Server → client:

| msg | fields | notes |
|---|---|---|
| `welcome` | `seat`, `seat_token`, `code`, `handles[]` | token is the only identity; valid for this match |
| `state` | redacted game state, `events`, `last_action {seat, type, slot, card?}`, `deadline` | after every applied action and on join/rejoin |
| `error` | `code`, `text` | includes illegal-action rejections |

**Redacted state** is the semantic fields of `Game`, filtered per seat by a
new engine function (§6): own rack in full; opponents' racks as counts only —
except in `PHASE_ROUND_OVER`/`MATCH_OVER`, when racks are public (the
physical reveal); full discard pile (public information, as in
`game_view_for`); held card under the existing visibility rule; never the
stock contents, `rng`, or seed. `last_action` carries what an observer at a
real table would see, and is what drives client sounds and card-flight
animation.

## 5. Server behavior

- **Turn clock:** 45 s per decision (draw and place each). At expiry the
  seat's move is played by `ai_choose` — the seat is *not* forfeited; the
  token still owns it and any reconnect hands control back. A seat that has
  timed out 3 consecutive times flips to AI until it acts again.
- **Disconnects:** socket loss marks the seat absent; AI plays it; `rejoin`
  with the token resumes. Seats stay reserved until the match ends.
- **Quick match:** queue seats up to 4; a table launches when full, or after
  15 s with ≥2 humans (AI backfill), or after 30 s with 1 human (they get an
  ordinary AI game, served locally-equivalent). Preferred-count requests are
  best-effort.
- **Handles:** generated, not chosen — e.g. compass seats ("North") or a
  small card-themed word list. No free text exists in the protocol.
- **Pacing:** the engine is compiled with the pacing macros at zero; the
  server sleeps ~1 s between AI actions at mixed tables so play stays
  legible, and not at all at all-AI tables.
- **Audit log:** per match, `(timestamp, seed, Rules, action list, seats)` —
  a few hundred bytes. Kept N days for abuse/bug investigation, then deleted.
  This is the only thing the server ever writes to disk.
- **Rate limits:** per-IP caps on `create`/`quick` (rooms are the only
  resource an attacker can burn); per-connection message cap; oversized or
  malformed frames close the socket.

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
- Menu grows **Play Online** → Quick Match / Create Table / Join Code (code
  entry via the existing cursor UI; no keyboard text on touch — codes are
  short and digit/letter cycled).
- In a network session the frame loop holds the latest redacted `Game` and
  renders it with the existing renderers (they already draw opponents as
  backs and respect held-card visibility). Input produces the same `Action`s;
  instead of `game_apply`, they go to the socket. `last_action` + `events`
  drive the existing sounds and flight animations.
- The turn clock renders in the status line; "reconnecting…" overlays reuse
  the pause panel.

## 8. Operations and privacy

- One static `orserverd` binary + caddy on a small VPS; systemd unit;
  `/status` (rooms live, queue depth, uptime) for monitoring.
- No accounts, no names, no persistent identity. Tokens die with the match;
  audit logs expire. **The store listings change**: "never touches the
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

- **TLS in C** — avoided entirely by the caddy front; the daemon never sees
  a certificate.
- **Queue liquidity** — AI backfill makes an empty queue invisible; the
  15/30 s rule guarantees a game.
- **Round-over consensus** — "all humans confirm or timer lapses" needs care
  so one AFK player can't hold the table; the reveal timer (already in the
  engine for full-AI games) is the backstop.
- **Store review friction** — adding networking changes Play data-safety and
  App Store privacy declarations; schedule review time in N4.
- **The static instance habit** — server code must never touch
  `game_create`; a lint/grep in CI keeps `orserverd` on `game_init`.
