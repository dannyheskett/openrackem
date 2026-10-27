# openrackem

A rack-sorting card game written in C with raylib. Draw from the stock or the
discard pile and exchange into your rack of ten: first player to read strictly
ascending from slot #5 to slot #50 calls **RACK 'EM!** and banks 75 points.
First to 500 wins the match.

An original implementation of the classic rack-sorting card game — full
official rules and scoring, including the mandatory two-player run rule and
the optional Bonus and Partners variants — with AI opponents in three
strengths. Not affiliated with or endorsed by any other game or its rights
holders.

Targets: **Linux**, **Windows** (x64/x86), **macOS** (universal), **Web**
(WASM), **Android**, and **iOS** (native Metal, no raylib).

## Playing

- **Draw** from the face-down stock (`S`/`Left`, or tap it) or take the
  face-up discard (`D`/`Right`, or tap it).
- **Exchange** the drawn card into any slot (`Up`/`Down` + `Enter`, or tap the
  slot); the displaced card goes face up onto the discard pile. A card taken
  from the discard pile *must* be exchanged.
- A stock draw you don't want can be thrown away (`X`, or tap the discard
  pile again).
- Go out by getting all ten slots strictly ascending. At 2 players you also
  need a run of three consecutive numbers (official rule).
- Losers score 5 per card in ascending sequence from slot #5, stopping at the
  first break. Winner scores 75. Options add the official Bonus (run bonuses:
  125/175/275/475 totals) and Partners (4 players, pairs) variants.

`Escape` returns to the menu (game stays resumable), `Enter` pauses,
`Alt+Enter` toggles fullscreen. Click a menu row to choose it. On touch
screens everything is a tap; a two-finger tap returns to the menu.

## Menu and window

These behave identically in every game in this family (openblocks, openrackem,
openklondike, opencheckers, openpairs, opensweeper). The code for them
(`src/menu.c`, `src/window.c`, `src/present.c`, and the gfx, safe-area,
timing, audio and recorder layers) is the same file in every repo.

- **Menu**: Resume Game (when a game is in progress), New Game, Options (when
  the game has settings), Sound, Record (desktop only), Exit (desktop only, set
  apart by a blank line). Options holds the settings and Back.
- **Menu input**: Up / Down (or W / S) move, Enter / Space choose, Left / Right
  (or A / D) cycle an Options value, Escape backs out. A mouse click or a tap on
  a row chooses it. Swipes move the selection and cycle values.
- **Menu size**: derived from the long edge of the view, so it is the same size
  upright and sideways and grows with the window; it shrinks only when its rows
  would not otherwise fit.
- **Back to the menu**: Escape, Android Back, or a two-finger tap. Losing focus
  (app backgrounded, tab hidden, window deactivated) also returns to the menu;
  the game stays resumable.
- **Window**: desktop opens at 960×720, resizes freely down to 640×480, and
  Alt+Enter toggles borderless fullscreen and back to the previous window.
  Web fills the browser viewport. Android and iOS are fullscreen.

## Building

### Linux / WSL2

```bash
./scripts/build_raylib_linux.sh   # once: build the pinned raylib
make          # dev build -> build/openrackem
make run
make release  # optimized -> build/openrackem-release
```

### Tests (headless, no window needed)

```bash
make test     # rules engine, AI legality/termination, touch recognizer
make ai-bench # tier-vs-tier AI win rates (thousands of seeded matches)
make shots    # deterministic screenshots of every screen (needs a display)
```

### Windows (cross-compile from Linux)

```bash
./scripts/build_raylib_windows.sh   # needs mingw-w64
make windows                        # build/openrackem-x64.exe + -x86.exe
```

### Web (WASM)

```bash
./scripts/build_raylib_web.sh       # needs emsdk on PATH
make web
make web-serve                      # http://localhost:8080/openrackem.html
```

Every platform draws the same layout. A phone gets it full screen with touch
controls; a desktop browser (like the native desktop app) gets it in a centred
column, played with the mouse or keyboard — same binary, chosen by pointer
type.

### macOS / Android / iOS

`make mac` (Xcode toolchain), `make android` (SDK + NDK, no Gradle; `make
android-play` builds the Play `.aab`), `make ios` / `make ios-sim` (hand
assembled `.ipa`, no Xcode project). CI builds all of these on every PR; see
`.github/workflows/ci.yml` for the exact toolchain setup each platform needs.

## Playing online

**Play Online** needs a player name (it asks for one first). It opens a room
list:

- **Public rooms** are listed by who's in them (no code). Pick one to step
  into its waiting room; Leave puts you straight back on the list. Join Best
  Room picks the one closest to starting; the list can be filtered by Play To.
- **Private rooms** are unlisted and joined by a 4-character code, a Share
  link, or the saved room at the top of the list. They last 30 days after
  their last game, so a group keeps the same code.
- **Waiting room:** everyone's name with a ready dot (filled = ready). When
  everyone present (2–4 players) is ready, the game starts. Partners needs 4
  in two teams of two; players pick teams, and a swap needs the other
  player's OK. After a match everyone returns to the same waiting room.
- **Leaving mid-game** keeps your seat: an AI stands in (shown with an "AI"
  tag) and **Return to Game** brings you back. When it's an absent player's
  turn the table waits 30 s, and anyone can tap **Let AI play**. Forfeit (from
  the in-game menu) gives the seat up.

## Multiplayer server

`make server` builds `orserverd`, the authoritative multiplayer daemon
(Linux): public and private rooms, waiting rooms with ready-up, a live room
list, member keys with reconnect, turn clocks and the leave policy, and
per-seat redacted state over WebSocket. Single-threaded with fixed pools on
one tiny fly.io machine (`fly.toml` / `Dockerfile.server`). With
`OR_DATA_DIR` set it snapshots private rooms and live matches to disk, so a
restart or deploy doesn't end games. Point a client at a local daemon with
`OPENRACKEM_SERVER` / `OPENRACKEM_PORT` (default
`openrackem-rooms.fly.dev:443`). `make net-multi` plays every room flow over
loopback against the real daemon. See `docs/PLAN-MULTIPLAYER.md`.

## Build and release

- Work branches merge into `staging` (squash); `main` takes merges from
  `staging` only. Every CI job must pass for either merge.
- **Build:** merging `staging` into `main` runs `build.yml`, which builds every
  platform, tags `release-N` and publishes the GitHub Release. It never ships
  to a store.
- **Release:** `release.yml` is run by hand and ships a chosen `release-N` to
  Google Play, then the App Store (replacing any version still pending App
  Review), then the Fly server. Any failure stops it and fails the run.
  See [docs/STORE-SETUP.md](docs/STORE-SETUP.md).

## Architecture notes

- `game.c` / `rules.c` / `ai.c` are pure logic: no rendering, no raylib, no
  `rand()`, no clock. All randomness flows from one seeded xorshift64* state,
  so a match replays byte-for-byte from `(seed, rules, action list)`.
- Game state is a flat POD with no pointers; a snapshot is a `memcpy`
  (`GAME_SERIAL_SIZE`), which tests enforce.
- The AI consumes a redacted `GameView` — it structurally cannot see another
  rack, the stock order, or the RNG seed.
- One adaptive portrait layout everywhere: full screen on phones, a centred
  3:5 column on desktop. It draws through the `gfx.h` primitive layer so the
  iOS Metal backend swaps in cleanly.
- Sound effects are synthesized at startup (square waves, sweeps, noise) —
  no audio files. Off by default.
- The desktop builds include a frame-fidelity `.mp4` recorder (`--record
  [path]`, or the Record menu toggle).

## License

MIT (see `LICENSE`). Bundled third-party components are listed in `NOTICE`.
