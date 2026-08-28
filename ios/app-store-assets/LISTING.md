# openrackem — App Store listing

Copy/paste into App Store Connect. Mirrors `android/play-assets/LISTING.md`, with
the differences Apple requires (subtitle, keywords, promotional text).

Two rules that differ from Play:

- **Never mention Android, Google Play, or another platform** in the description.
  Apple rejects listings that reference competing stores.
- **The name of the classic rack-sorting card game appears nowhere.** openrackem is
  an original implementation and must not imply affiliation or endorsement. See
  `NOTICE`. Describe it as a "rack-sorting card game".

## New App form (My Apps -> + -> New App)

| Field | Value |
| --- | --- |
| Platform | iOS |
| Name | `openrackem` (must be unique App Store-wide; see fallbacks below) |
| Primary Language | English (U.S.) |
| Bundle ID | `com.danheskett.openrackem` |
| SKU | `openrackem-free` |
| User Access | Full Access |

The app record now exists. Its **Apple ID is `6806156215`** (App Store Connect ->
App Information). Nothing in the release pipeline needs it -- `altool` reads the
bundle ID out of the .ipa, and `scripts/testflight_notes.py` looks the app up by
`filter[bundleId]` -- but it is the id Apple support and the store URL use.

If `openrackem` is taken, in order of preference: `Openrackem Cards`,
`Openrackem Card Game`, `Openrackem Game`. The name is public, capped at 30
characters, and can be changed with any later version — the SKU and bundle ID
cannot.

## Subtitle (<=30 chars)

```
Sort your rack. Call it first.
```

## Promotional text (<=170 chars)

Editable anytime without submitting a new build — use it for release notes or
seasonal copy.

```
No ads, no tracking, no accounts. Play the computer offline, or take on friends online with a room code.
```

## Keywords (<=100 chars, comma-separated, no spaces after commas)

Do not repeat the app name — it is already indexed.

```
cards,card game,rack,sequence,order,sorting,multiplayer,online,family,ascending,offline
```

## Description (<=4000 chars)

```
Draw a card. Slot it in. Get all ten in order before anyone else does.

You hold ten cards in a rack, and they need to run low to high. Each turn you draw — from the face-down stock or the face-up discard — and choose which card in your rack it replaces. The card you bump lands face up, where your opponents can take it. That's the whole game. It is harder than it sounds.

Get all ten ascending and you call RACK 'EM, bank 75 points, and everyone else scores whatever partial run they had built. First to 500 takes the match.

No ads. No tracking. No accounts. No in-app purchases.

PLAY YOUR WAY
• Offline against AI opponents in three strengths
• Online for 2 to 4 players — quick match, or share a room code with friends
• Full official rules, including the two-player run-of-three requirement
• Optional Bonus scoring and Partners team play

BUILT FOR ONE HAND
• Everything is a tap; portrait only
• Two-finger tap returns to the menu
• Clean, readable cards that stay out of your way

FREE AND OPEN SOURCE
openrackem is open source. Read the code, report a bug, or build it yourself: https://github.com/dannyheskett/openrackem

No dark patterns, no "energy" timers, no paywalled decks. Just the card game, done properly.
```

## App information

- **Category (primary):** Games -> Card
- **Category (secondary):** Games -> Family
- **Content Rights:** does not contain third-party content. (The bundled Nunito
  typeface is licensed under the SIL OFL — see `NOTICE` — which is a license to
  redistribute, not third-party *content* in the sense this question asks about.)
- **Copyright:** `2026 Daniel Heskett`
- **Support URL:** https://danheskett.com
- **Marketing URL:** https://danheskett.com/projects/openrackem/
- **Privacy Policy URL:** https://danheskett.com/app/privacy-policy/

## Age Rating

No gambling (no wagering, no simulated-gambling loop, no purchasable chips), no
ads, no unrestricted web access, no violence, no chat and no messaging.

The one thing openblocks does not have: online players pick a display name the
others can see (`netgame.h:43`, alphanumerics only, 15 chars). Answer the
user-generated-content question with that in mind.

## App Privacy (App Store Connect -> App Privacy)

No accounts, no analytics, no advertising, no third-party SDKs, nothing stored
server-side — matches are played in memory and discarded when they end. See
`android/play-assets/PRIVACY.md` for the wording used on the Play listing.

## Pricing

Free. No in-app purchases. Requires the **Free Applications agreement** to be
Active under Business / Agreements, Tax, and Banking.

## Export compliance

`ITSAppUsesNonExemptEncryption = false` is already set in `ios/Info.plist`, so App
Store Connect will not ask the encryption question on each upload.

The app *does* use encryption — online play is `wss://` over TLS — but only the
TLS the OS provides (Network.framework), for standard secure communications,
which is exempt. `false` answers the question actually being asked, which is about
**non-exempt** encryption. No crypto ships in the iOS binary; the bundled OpenSSL
is an Android-only dependency.
