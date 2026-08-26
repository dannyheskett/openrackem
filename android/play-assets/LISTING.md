# openrackem — Google Play store listing

Copy/paste these into the Play Console (**Grow → Store presence → Main store
listing**, plus **Store settings** for category). The images referenced below
are release-1 work: regenerate icon, feature graphic, and screenshots from the
finished game before submission (the PNGs currently in this folder are
placeholders carried from the scaffold).

## Assets (this folder)

| File | Play field | Spec |
|------|-----------|------|
| `icon-512.png` | App icon | 512×512 PNG (32-bit) |
| `feature-graphic-1024x500.png` | Feature graphic | 1024×500 PNG/JPG |
| `screenshots/phone/` | Phone screenshots | 4× 1080×1920 PNG (9:16, promo-eligible) |
| `screenshots/tablet/` | 7-inch and 10-inch tablet screenshots | 4× 2160×3840 PNG (9:16, same files fit both slots) |

## App name (≤30 chars)

```
openrackem
```

## Short description (≤80 chars)

```
Classic rack-sorting card game. Free, open source, no ads, no tracking.
```

## Full description (≤4000 chars)

```
Draw a card, slot it into your rack, and race to get all ten in ascending order before your opponents do — the classic rack-sorting card game, with none of the junk that clutters mobile card games.

No ads. No tracking. No accounts. No in-app purchases. openrackem requests zero permissions and never touches the network. It's just the game.

THE CLASSIC RACK GAME
• Draw from the stock or take the face-up discard, exchange into your rack, and be first to read low-to-high
• Full official rules and scoring: 75 points for going out, 5 per card in sequence for everyone else, first to 500 wins the match
• The mandatory two-player run rule, plus the official Bonus (run bonuses) and Partners (two vs two) variants
• Three AI strengths, from casual to card-counting

BUILT RIGHT
• Big, thumb-friendly cards tuned for one-handed play
• Crisp, minimal table that stays out of your way
• Fully offline — perfect for flights, commutes, anywhere
• Tiny download, easy on your battery

FREE AND OPEN SOURCE
openrackem is open source. Read the code, report a bug, or build it yourself: https://github.com/dannyheskett/openrackem

No dark patterns, no "energy" timers, no paywalled decks. An original implementation of the timeless rack-sorting card game, done properly. Not affiliated with or endorsed by any other game or its rights holders.
```

## Categorization (Store settings)

- **App or game:** Game
- **Category:** Card
- **Tags:** card, casual, retro, classic
- **Email:** dan@danheskett.com
- **Website:** https://danheskett.com
- **Content rating:** Everyone (no objectionable content; IARC questionnaire —
  answer "no" to all violence/adult/gambling items)

## Data safety (Policy → App content)

- Data collected: **None**
- Data shared: **None**
- App has no `INTERNET` permission (verify in the manifest) → "no data
  transmitted off the device" is truthful.
- Privacy policy URL: **https://danheskett.com/app/privacy-policy/** (live)

## Screenshots

Capture from the CI web build's portrait renderer (pixel-identical to
Android) in a headless browser: menu, mid-round table, a placement, and the
round-scoring reveal. 4× 1080×1920 for phone; 4× 2160×3840 for both tablet
slots.

## Trademark note

The name of the commercial rack-sorting game is a trademark of its rights
holder and appears nowhere in the app, the listing, or the assets. Keep it
that way in any listing edits: describe the mechanics ("rack-sorting card
game"), never the trademarked title.
