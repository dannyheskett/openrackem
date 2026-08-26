# openrackem — implementation plan

## 1. Scope

A clone of the classic rack-sorting card game, in C, structurally cloned from `openrackem`: platform-independent game logic, raylib on every platform except iOS (native Metal), landscape + portrait renderers, targets for Linux / Windows / macOS / Web (WASM) / Android / iOS.

Release-1 requires **all** of: full official rules, full scoring, AI opponents, multiplatform native builds, WebAssembly build, CI, and release packaging. Work is staged (§13) but nothing is deferred past release-1.

Version 2, post-release, adds a hosted game-pool engine on a server. No networking code ships in v1, but §14 lists the design constraints v1 must honor so `game.c` is reusable server-side unchanged.

## 2. Naming and trademark

RACK-O is a Hasbro trademark, licensed to Winning Moves. The name is unavailable, hence **openrackem**.

| Thing | Value |
|---|---|
| Repo / binary | `openrackem` |
| Header guards | `OPENRACKEM_*_H` |
| Feature macros | `OR_TOUCH`, `OR_PORTRAIT`, `OR_LANDSCAPE`, `OR_RUNTIME_RENDERER`, `OR_SIMSTATS`, `OR_AUTOPLAY` |
| Android package | `com.danheskett.openrackem` |
| iOS bundle id / app name | `com.danheskett.openrackem` / `Openrackem` |
| Title bar / menu title | `OPENRACKEM` |
| Winning call (UI text) | `RACK 'EM!` |

The trademarked name appears nowhere in source, UI, README, or store listings. `NOTICE` carries the same original-implementation disclaimer openrackem uses, adapted: an original implementation of a rack-sorting card game, not affiliated with or endorsed by any other game or its rights holders. The rack slot labels (5, 10, … 50) are generic numbers and are kept, since they carry the scoring language.

## 3. Rules specification

Sourced from the official Winning Moves 2023 rulebook. This section is the test oracle.

**Cards.** A deck numbered 1–60. Deck subset depends on player count: 2 players use 1–40, 3 players use 1–50, 4 players use 1–60.

**Rack.** Ten slots, labeled 5, 10, 15 … 50. Internally `slots[0..9]`, where index 0 is the #5 slot (the low end) and index 9 is the #50 slot (the high end).

**Deal.** Ten cards to each player, one at a time. Each card goes into the rack as it is dealt: first card to slot #50 (index 9), second to #45 (index 8), and so on down to #5 (index 0). Cards are never rearranged; the only way the rack changes is an exchange.

**Setup.** Remaining cards form the face-down stockpile. Its top card is turned face up to start the discard pile.

**Turn.** In turn order starting left of the dealer, a player takes exactly one card:

- **From the discard pile:** must exchange it into the rack. The displaced card goes face up on the discard pile.
- **From the stockpile:** may exchange it into the rack (displaced card discarded), or discard it outright.

An exchange **must** place the new card in the slot the old card came from. Exactly one card lands on the discard pile per turn.

**Stock exhaustion.** If the stockpile empties before anyone goes out, the discard pile is turned over and becomes the new stockpile.

**Winning a round.** A player goes out when all ten slots read strictly ascending from #5 to #50, in any increasing combination.

**Scoring.**

- Winner: 75 points (5 per card, plus 25 for going out).
- Everyone else: 5 points per card in ascending sequence starting at slot #5, stopping at the first break. Cards past the break score nothing even if they happen to be ordered. A player whose #10 card is lower than their #5 card scores 5.
- First player to reach 500 or more wins the match.

**Two-player rule (official, mandatory at 2 players).** A player may not go out unless their rack includes a run of at least 3 cards in consecutive numerical sequence.

**Bonus variant (official, optional).** Only the player going out scores a bonus, for their longest run of consecutive numbers. Two or more runs score only the longest.

| Longest run | Total |
|---|---|
| 3 cards | 75 + 50 = 125 |
| 4 cards | 75 + 100 = 175 |
| 5 cards | 75 + 200 = 275 |
| 6+ cards | 75 + 400 = 475 |

**Partners variant (official, optional, 4 players only).** Two against two, seated alternately. Partner scores are summed after each round; the first pair to 500 wins.

**Stated assumptions** (not specified by the rulebook, needed for a digital implementation):

1. The first dealer is chosen at random from the seeded RNG rather than by cutting for low card. The deal rotates one seat left each round.
2. When the stockpile is recycled, the current top discard stays face up and everything beneath it is turned over to form the new stockpile, preserving order (the rulebook says "turned over", not "shuffled", so no shuffle occurs). A config flag can reshuffle instead.
3. A stalemate cutoff ends a round with no winner if the stockpile is recycled more than three times. Every player scores their base sequence points and no one scores 75. Config-gated, on by default. This is an addition to the official rules; it exists because "turn over, do not shuffle" plus deterministic AI admits a theoretical non-terminating round.

## 4. Configuration surface

Official rules are the defaults. Everything tunable sits in one struct at the top of the stack, set from the menu and passed once into `game_create`, never read from globals inside the logic.

```c
typedef struct {
    int  player_count;        // 2..4 (default 4)
    int  human_seat;          // 0..player_count-1, or -1 for full AI (autoplay/tests)
    int  target_score;        // default 500
    bool bonus_scoring;       // Bonus variant (default off)
    bool partners;            // Partners variant, 4 players only (default off)
    bool reshuffle_on_recycle;// non-official convenience (default off)
    bool stalemate_cutoff;    // see assumption 3 (default on)
    int  ai_difficulty;       // 0 easy, 1 normal, 2 hard (default 1)
    uint64_t seed;            // 0 = seed from clock at create
} Rules;

Rules rules_default(void);
// Clamps and resolves derived values: deck size from player_count, the
// mandatory 2-player run requirement, partners forced off below 4 players.
void rules_normalize(Rules* r);
```

`rules_normalize` is the single place the official derivations live (deck subset from player count, the 2-player run requirement always on, partners requires exactly 4). It is directly unit-tested.

The Options menu exposes: players, difficulty, bonus scoring, partners, target score. Nothing else.

## 5. Architecture

Cloned from openrackem, with one structural difference: this is a turn-based game, so the simulation is event-driven rather than a 60 Hz physics loop. The 60 Hz fixed-timestep loop is **kept anyway** and drives presentation only, so `main.c` keeps openrackem' exact shape (`AppCtx`, `frame_step`, `SimClock`, emscripten callback, iOS `ob_app_init`/`ob_app_frame`).

- **Rules and state** (`game.c` / `game.h`): pure, allocation-free after create, no rendering, no raylib, no `rand()`. Advances only when an action is applied.
- **Animation and AI pacing** (`game_update`, one call per 60 Hz step): card-slide tweens, the AI think delay, the round-end reveal timer. These live in a presentation sub-struct that never affects rules outcomes.
- **AI** (`ai.c` / `ai.h`): a separate translation unit that consumes a redacted view and returns an `Action`. No access to hidden state, enforced by the view type, not by discipline.
- **Renderers**: `render.c` (shared helpers, dispatch) + `render_landscape.c` + `render_portrait.c`, behind `render.h`, drawing through `gfx.h` so iOS Metal swaps in cleanly.
- **Input** (`input.c` / `input.h`): normalizes keyboard and touch into a single `Input` struct, exactly as openrackem does.
- **Audio, recorder, tick, safe_area, font atlas, platform**: taken from openrackem with the sound effect set replaced.

## 6. State model

All state is a flat POD with no pointers, so `memcpy` is a valid snapshot and the future server can serialize it with a single write.

```c
#define RACK_SLOTS   10
#define MAX_PLAYERS   4
#define MAX_CARDS    60

typedef struct {
    uint8_t slots[RACK_SLOTS];   // card values, index 0 = slot #5 ... 9 = slot #50
} Rack;

typedef struct {
    Rack rack;
    uint16_t score;              // running match score
    bool is_ai;
} Player;

typedef enum {
    PHASE_DEAL = 0,   // animated deal, no input accepted
    PHASE_DRAW,       // current player must draw (stock or discard)
    PHASE_PLACE,      // holding a drawn card: choose a slot, or discard it
    PHASE_ROUND_OVER, // scoring screen
    PHASE_MATCH_OVER,
} Phase;

typedef struct {
    Rules   rules;
    uint64_t rng;                 // xorshift64* state; the ONLY randomness source
    Player  players[MAX_PLAYERS];
    uint8_t stock[MAX_CARDS];     // stock[0] is the top
    uint8_t stock_count;
    uint8_t discard[MAX_CARDS];   // discard[discard_count-1] is the face-up top
    uint8_t discard_count;
    uint8_t dealer;
    uint8_t turn;                 // seat to act
    uint8_t phase;
    uint8_t held_card;            // valid in PHASE_PLACE, 0 = none
    bool    held_from_discard;    // if true, it MUST be placed (cannot be discarded)
    uint8_t recycles;             // stockpile turn-overs this round
    uint8_t round_winner;         // 0xFF = none (stalemate)
    uint16_t round_points[MAX_PLAYERS];
    unsigned events;              // EV_* flags for this step, cleared each step
    /* presentation-only, excluded from the serialized form */
    Anim    anim;
} Game;
```

`held_from_discard` is the mechanism that enforces the "a discard draw must be exchanged" rule, and it is what makes the illegal move unrepresentable rather than merely rejected.

## 7. Action model

A turn is a sequence of validated actions. This is the interface the v2 server will speak.

```c
typedef enum {
    ACTION_DRAW_STOCK,
    ACTION_DRAW_DISCARD,
    ACTION_PLACE,     // slot 0..9; the displaced card is discarded
    ACTION_DISCARD,   // throw the held card away (illegal if held_from_discard)
} ActionType;

typedef struct { uint8_t type; uint8_t slot; } Action;

bool game_action_legal(const Game* g, Action a);
bool game_apply(Game* g, Action a);          // false = rejected, state untouched
void game_update(Game* g);                   // one 60 Hz step: animation + AI pacing
```

Every rule check funnels through `game_action_legal`. The renderer greys out illegal affordances by calling it, so the UI and the engine cannot disagree.

Going out is not an action. `game_apply` checks the rack after each `ACTION_PLACE` and, if it reads ascending (and satisfies the two-player run requirement when it applies), ends the round immediately. This matches the physical game, where the rack is public once claimed and cannot be declined.

Scoring functions are pure and separately testable:

```c
int score_sequence(const Rack* r);              // 5 per ascending card from slot #5
int score_longest_run(const Rack* r);           // length of longest consecutive run
int score_bonus(int run_len);                   // 0 / 50 / 100 / 200 / 400
bool rack_is_out(const Rack* r);                // strictly ascending, all ten
```

## 8. AI

`ai.c` receives a redacted view and returns one `Action`.

```c
typedef struct {
    Rules   rules;
    Rack    own_rack;
    uint8_t top_discard;
    uint8_t stock_count;
    uint8_t seat, player_count;
    uint8_t held_card, held_from_discard;
    uint8_t discard_history[MAX_CARDS], discard_history_count;
    uint8_t opponent_out_risk[MAX_PLAYERS];  // turns elapsed, public info only
} GameView;

GameView game_view_for(const Game* g, int seat);   // hides all other racks
Action   ai_choose(const GameView* v, uint64_t* rng);
```

**Heuristic.** Each card has an ideal slot: `slot = floor((card - 1) * 10 / deck_size)`. The AI scores a candidate rack by (a) the number of ascending adjacent pairs, (b) the length of the ascending prefix from slot #5, weighted highest since that is what scores when it loses, (c) per-slot distance from each card's ideal slot, and (d) at 2 players or with bonus scoring on, a term for consecutive runs. It evaluates the top discard against all ten slots plus "decline", then commits.

**Difficulty** is a deliberate degradation of that single evaluator, not three separate AIs:

- *Easy*: only considers the ascending-prefix term, and takes a random legal slot 25% of the time.
- *Normal*: full evaluator, no lookahead.
- *Hard*: full evaluator, plus discard-history tracking (avoids feeding a card an opponent is visibly collecting) and endgame urgency (takes a smaller improvement when an opponent has had many turns).

The AI takes its turn after a think delay counted in frames, so play is legible rather than instant.

## 9. Presentation

**Screens.** Menu → Options → Table → Round Scoring → Match Standings → Match Over. Pause and resume behave exactly as openrackem (Escape returns to the menu with the game resumable; the touch build auto-pauses on focus loss).

**Landscape (fixed 640×480, letterboxed).** Three columns, mirroring openrackem' layout discipline: opponent racks as face-down backs with names and running scores on the left; the player's ten slots as a labeled vertical column, centered, with the slot number (5…50) on the left of each card; stock, discard, and the held card on the right, above match score and turn indicator.

**Portrait (adaptive).** OPENRACKEM title bar pinned top; a compact opponents band beneath it; the ten rack slots filling the middle at the largest row height that fits; stock and discard as a fixed bottom band within the safe area, which is where the thumb is.

**Input.**

- Keyboard: `S` or Left draws stock, `D` or Right draws discard, Up/Down move the slot cursor, Enter/Space places into the cursor slot, `X` discards a held stock card, Escape to menu, Enter pauses, Alt+Enter fullscreen.
- Touch: tap the stock or discard pile to draw; the drawn card follows to a held position; tap a rack slot to place; tap the discard pile again to throw a stock-drawn card away; two-finger tap returns to the menu.

Both paths produce the same `Action` values, so a keyboard and a thumb are literally indistinguishable to the engine.

**Font.** Reuse `scripts/gen_font_atlas.c` to generate `font_atlas.h`. Rack cards need two-digit numbers legible at small portrait row heights, which is the tightest typographic constraint in the project and is validated on a real device via the existing Device Farm path.

## 10. Audio

Procedural, synthesized at startup, no audio files, sound off by default. Effect set: `SFX_DRAW`, `SFX_PLACE`, `SFX_DISCARD`, `SFX_TURN`, `SFX_INVALID`, `SFX_ROUND_WIN`, `SFX_ROUND_LOSE`, `SFX_BONUS`, `SFX_MATCH_WIN`, `SFX_MENU_MOVE`, `SFX_MENU_SELECT`, `SFX_PAUSE`.

## 11. Testing

`make test` builds and runs each binary; non-zero exit fails CI.

**`tests/test_game.c`** (includes `game.c` directly, as openrackem does, to reach file-static helpers):

- `rules_normalize`: deck subsets 1–40 / 1–50 / 1–60, the 2-player run requirement forced on, partners rejected below 4 players, clamping.
- Deal: ten cards each, slot #50 filled first down to #5, no duplicates, stock plus discard plus hands equals deck size, one card face up.
- Legality: a discard-drawn card cannot be discarded; placing outside 0–9 rejected; acting out of turn rejected; every action rejected in `PHASE_ROUND_OVER`.
- Exchange: the new card lands in exactly the slot vacated, the displaced card is on top of the discard pile, and no other slot moves.
- Stock exhaustion: recycle preserves order under the official rule, reshuffles under the flag, and the face-up top discard is retained.
- Going out: strictly ascending detected, equal-adjacent rejected, nine-of-ten rejected. At 2 players, an ascending rack with no 3-run does **not** end the round; adding a 3-run does.
- Base scoring: winner 75; the rulebook's Figure 3 case scores 30; the "#10 lower than #5" case scores 5; a fully ordered loser scores 50.
- Bonus scoring: the exact 50 / 100 / 200 / 400 table; two runs score only the longest; equal-length runs score once.
- Match: accumulation to 500, first-past-the-post, partners summing, stalemate cutoff awards base points and no 75.
- Determinism: two games with the same seed and the same action sequence produce byte-identical state.
- Serialization: `memcpy` round-trip through a buffer reproduces state exactly (guards the no-pointers invariant for v2).

**`tests/test_ai.c`**: `ai_choose` returns a legal action from every reachable phase across thousands of seeded positions; a full AI-only match always terminates; Normal beats Easy over a large seeded sample; the AI never reads a hidden rack (enforced structurally, since `GameView` has no field for one).

**`tests/test_input.c`**: the touch gesture recognizer, compiled `-DPLATFORM_IOS` against a scripted touch and clock surface, as in openrackem.

**Web smoke test**: the existing Playwright MCP path loads the built WASM page and drives a full round.

## 12. Build and CI

`Makefile` cloned from openrackem with the names swapped and the source list changed. Targets kept verbatim in structure: `all`, `release`, `run`, `windows` (x64 + x86 mingw), `mac` (universal), `android` (APK, no Gradle), `android-play` (AAB via aapt2 + bundletool), `web` (emcc + `web/shell.html`), `web-serve`, `ios` (unsigned .ipa, hand-assembled), `ios-sim`, `test`, `dist-*`, `clean`.

Carried over unchanged in shape: `scripts/build_raylib_*.sh` (pinned raylib 6.0), `check_elf_align.sh` (16 KB page alignment for Play), `devicefarm_run.py`, `testflight_notes.py`, `gen_font_atlas.c`, the `.github/workflows/ci.yml` job matrix (linux, windows, mac, web, android, ios), `release.yml` (guard, prepare, per-platform builds, publish, publish-play, publish-testflight, testflight-notes), `attribution-guard.yml`, `dependabot.yml`, `.editorconfig`, `.gitignore`, `LICENSE` (MIT), `NOTICE`, `run.sh`.

The recorder (`recorder.c`, `encode_h264.c`, `encode_mux.c`, minih264 + minimp4 vendored) is carried over as-is: desktop-only, stubbed on mobile and web.

`OR_SIMSTATS` / `OR_AUTOPLAY` are retained. Autoplay seats an AI in every chair (`human_seat = -1`) and starts a match at boot, which gives the Device Farm run a self-driving workload, exactly as gravity does for openrackem.

## 13. Milestones

Each milestone ends green: builds clean, `make test` passes, CI passes.

- **M0 — Scaffold.** Repo created, openrackem files copied and renamed, build system and CI green with a stub `game.c`. Every platform target compiles before any rules exist.
- **M1 — Rules engine.** `rules.c`, `game.c`, `game.h` complete and headless. `test_game.c` covers §11 in full. No rendering.
- **M2 — AI.** `ai.c`, `GameView` redaction, difficulty tiers, `test_ai.c`. Full AI-only matches run to 500 under `OR_AUTOPLAY`.
- **M3 — Landscape and keyboard.** Table, round scoring, standings, menu, options. One complete human-versus-AI match playable on desktop.
- **M4 — Portrait and touch.** Adaptive layout, safe area, gesture input, `test_input.c`. Android and web builds playable by thumb.
- **M5 — Platform completion.** macOS universal, Windows x64/x86, WASM, Android APK + AAB, iOS Metal backend and .ipa, all six CI jobs green.
- **M6 — Polish.** Sound effects, card animation, recorder wiring, README, screenshots, device validation via SIMSTATS.
- **M7 — release-1.** Store assets (Play listing, privacy, feature graphic, icons; App Store listing and icon), signing, `release.yml` end to end, tag `release-1`.

## 14. Constraints for the v2 hosted pool

Honored from the first line of v1, so the server reuses `game.c` without a rewrite:

1. `Game` is a flat POD with no pointers and no heap references. A snapshot is a `memcpy`; a test enforces this.
2. All randomness comes from the seeded `rng` field. `rand()`, `srand()`, and wall-clock reads never appear in `game.c` or `ai.c`. A game is fully reproducible from `(seed, Rules, action list)`, which is exactly what a server needs for replay, audit, and reconnect.
3. Rules logic never calls raylib, the renderer, audio, or platform code. `game.c` + `rules.c` + `ai.c` compile standalone with only libc.
4. Hidden information is redacted by construction through `GameView`. The same function that feeds the local AI will feed a remote client, so v1's AI is a working proof that the redaction is complete.
5. `game_apply` is total and validating: it never trusts its input and never partially mutates on rejection. Untrusted network input is the same code path as a mis-clicked button.
6. Presentation state (`Anim`) is isolated in one sub-struct and excluded from the serialized form, so the server carries no rendering baggage.

No socket, protocol, or serialization-format code ships in v1. The above is design discipline, not dead code.

## 15. Risks and open items

- **Trademark.** The name is avoided everywhere, but store listings need a description that says what the game is without using the trademarked name. Worth a review pass before the Play and App Store submissions.
- **Two-digit legibility in portrait.** Ten rack rows plus a stock and discard band on a phone is denser than a falling-block playfield. If the atlas font does not hold up on a real device, the fallback is a wider card with the number left-aligned rather than centered.
- **AI difficulty tuning.** "Normal beats Easy over a large sample" is a weak test. Real tuning needs a batch harness that plays thousands of seeded matches and reports win rates per tier; planned as a `make ai-bench` target under M2.
- **Hidden-information animation.** Opponent turns need to read clearly (a card leaves the discard pile, a card arrives) without revealing which slot it entered. Design decision deferred to M3.
- **WASM size.** openrackem builds `-Os` with a fixed 64 MiB heap. Card rendering adds little, so this should hold, but it is checked at M5 rather than assumed.

## 16. File inventory

```
openrackem/
  Makefile  README.md  LICENSE  NOTICE  run.sh  .editorconfig  .gitignore
  docs/PLAN.md
  src/
    main.c            app state machine, 60 Hz loop, emscripten + iOS entry
    rules.c rules.h   Rules struct, defaults, normalization
    game.c game.h     deck, deal, actions, legality, scoring, round/match flow
    ai.c ai.h         GameView consumer, heuristic evaluator, difficulty tiers
    tick.c tick.h     fixed-timestep accumulator (from openrackem)
    input.c input.h   keyboard + touch -> Input
    render.c render.h render_internal.h
    render_landscape.c  render_portrait.c
    gfx.h gfx_raylib.c  ob_types.h  platform.h  safe_area.c safe_area.h
    sound.c sound.h  audio.h audio_raylib.c
    recorder.c recorder.h encode_h264.c encode_mux.c
    font_atlas.h
  ios/    ios_main.mm gfx_metal.mm plat_ios.mm audio_ios.mm Info.plist Assets.xcassets
  android/ AndroidManifest.xml java/com/danheskett/openrackem/ res/ play-assets/
  web/    shell.html
  scripts/ build_raylib_*.sh check_elf_align.sh devicefarm_run.py
           testflight_notes.py gen_font_atlas.c requirements.txt
  tests/  test_game.c test_ai.c test_input.c
  .github/workflows/ ci.yml release.yml attribution-guard.yml
```

## 17. Rules sources

- Winning Moves official rulebook, 2023 printing: https://winning-moves.com/images/RackO_Rules_2023.pdf
- UltraBoardGames: https://ultraboardgames.com/rack-o/game-rules.php
- Wikipedia: https://en.wikipedia.org/wiki/Rack-O
- officialgamerules.org: https://officialgamerules.org/game-rules/rack-o/
