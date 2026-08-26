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
`Alt+Enter` toggles fullscreen. On touch screens everything is a tap; a
two-finger tap returns to the menu.

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

A desktop browser gets the landscape keyboard layout; a phone gets the
portrait touch layout — same binary, chosen by pointer type.

### macOS / Android / iOS

`make mac` (Xcode toolchain), `make android` (SDK + NDK, no Gradle; `make
android-play` builds the Play `.aab`), `make ios` / `make ios-sim` (hand
assembled `.ipa`, no Xcode project). CI builds all of these on every PR; see
`.github/workflows/ci.yml` for the exact toolchain setup each platform needs.

## Multiplayer server (v2, in progress)

`make server` builds `orserverd`, the authoritative multiplayer daemon
(Linux): rooms with join codes, a public quick-match queue with AI backfill,
anonymous seat tokens with reconnect, turn clocks with AI takeover, and
per-seat redacted state over WebSocket. Single-threaded with fixed pools —
one tiny fly.io machine holds a thousand concurrent tables (`fly deploy`
with the included `fly.toml` / `Dockerfile.server`). The desktop app has a
**Play Online** menu (Quick Match, Create Table, Join by Code) that speaks to
it; point it at a daemon with `OPENRACKEM_SERVER` / `OPENRACKEM_PORT`
(default `127.0.0.1:8080`). See `docs/PLAN-MULTIPLAYER.md`.

## Architecture notes

- `game.c` / `rules.c` / `ai.c` are pure logic: no rendering, no raylib, no
  `rand()`, no clock. All randomness flows from one seeded xorshift64* state,
  so a match replays byte-for-byte from `(seed, rules, action list)`.
- Game state is a flat POD with no pointers; a snapshot is a `memcpy`
  (`GAME_SERIAL_SIZE`), which tests enforce.
- The AI consumes a redacted `GameView` — it structurally cannot see another
  rack, the stock order, or the RNG seed.
- Two renderers behind one dispatch: fixed 640x480 landscape (desktop) and an
  adaptive portrait layout (touch), both drawing through the `gfx.h`
  primitive layer so the iOS Metal backend swaps in cleanly.
- Sound effects are synthesized at startup (square waves, sweeps, noise) —
  no audio files. Off by default.
- The desktop builds include a frame-fidelity `.mp4` recorder (`--record
  [path]`, or the Record menu toggle).

## License

MIT (see `LICENSE`). Bundled third-party components are listed in `NOTICE`.
