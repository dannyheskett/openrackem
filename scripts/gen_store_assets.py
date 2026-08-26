#!/usr/bin/env python3
"""Generate openrackem's store/launcher art from code (no design tools).

The motif: three cards stepped upward showing 1-2-3 — the whole game (get your
rack ascending) in one glance, legible from 48px to 1024px. Palette matches
the in-game table (render.c): felt green, cream faces, dark ink, red back.

Outputs (run from the repo root):
  android/res/mipmap-*dpi/ic_launcher.png            48/72/96/144/192
  android/res/mipmap-*dpi/ic_launcher_foreground.png 108/162/216/324/432 (adaptive)
  android/play-assets/icon-512.png                   512x512
  android/play-assets/feature-graphic-1024x500.png   1024x500
  ios/Assets.xcassets/AppIcon.appiconset/icon-1024.png (opaque: App Store
      rejects alpha in the marketing icon)

Requires Pillow and the DejaVu fonts (both stock on Ubuntu).
"""

import os
from PIL import Image, ImageDraw, ImageFont

FELT = (16, 48, 32, 255)
FACE = (236, 232, 220, 255)
INK = (30, 30, 40, 255)
EDGE = (90, 90, 90, 255)
BACK = (140, 36, 40, 255)
BACK_TRIM = (200, 160, 120, 255)
ACCENT = (253, 249, 0, 255)

FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def font(px):
    return ImageFont.truetype(FONT_BOLD, px)


def draw_card(d, x, y, w, h, label=None, face=True, edge_w=None):
    """One card. Rounded corners scale with size; label centered."""
    x, y, w, h = int(x), int(y), int(w), int(h)
    r = max(2, w // 8)
    if edge_w is None:
        edge_w = max(1, w // 28)
    if face:
        d.rounded_rectangle([x, y, x + w, y + h], radius=r, fill=FACE,
                            outline=EDGE, width=edge_w)
        if label:
            f = font(int(h * 0.52))
            bb = d.textbbox((0, 0), label, font=f)
            tw, th = bb[2] - bb[0], bb[3] - bb[1]
            d.text((x + (w - tw) / 2 - bb[0], y + (h - th) / 2 - bb[1]),
                   label, font=f, fill=INK)
    else:
        d.rounded_rectangle([x, y, x + w, y + h], radius=r, fill=BACK,
                            outline=EDGE, width=edge_w)
        inset = max(2, int(w) // 7)
        d.rounded_rectangle([x + inset, y + inset, x + w - inset, y + h - inset],
                            radius=max(1, r // 2), outline=BACK_TRIM,
                            width=max(1, edge_w))


def cards_motif(size, transparent=False, scale=1.0):
    """The 1-2-3 stepped cards on a `size` square canvas."""
    im = Image.new("RGBA", (size, size),
                   (0, 0, 0, 0) if transparent else FELT)
    d = ImageDraw.Draw(im)
    s = size * scale
    cw = s * 0.30            # card width
    ch = cw * 1.42           # card height
    gap = s * 0.035
    total_w = 3 * cw + 2 * gap
    step = s * 0.14          # how far each card rises above the last
    x0 = (size - total_w) / 2
    base_y = size / 2 + (ch + 2 * step) / 2 - ch   # center the whole motif
    for i, label in enumerate(("1", "2", "3")):
        x = x0 + i * (cw + gap)
        y = base_y - i * step
        draw_card(d, x, y, cw, ch, label)
    return im


def save(im, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)
    print(f"wrote {path} ({im.size[0]}x{im.size[1]})")


def main():
    # Square launcher icons (legacy Android + Play icon + iOS marketing).
    for size, path in [
        (48,  "android/res/mipmap-mdpi/ic_launcher.png"),
        (72,  "android/res/mipmap-hdpi/ic_launcher.png"),
        (96,  "android/res/mipmap-xhdpi/ic_launcher.png"),
        (144, "android/res/mipmap-xxhdpi/ic_launcher.png"),
        (192, "android/res/mipmap-xxxhdpi/ic_launcher.png"),
        (512, "android/play-assets/icon-512.png"),
    ]:
        save(cards_motif(size, scale=0.92), path)

    # iOS marketing icon: opaque RGB (the App Store rejects alpha).
    save(cards_motif(1024, scale=0.92).convert("RGB"),
         "ios/Assets.xcassets/AppIcon.appiconset/icon-1024.png")

    # Adaptive-icon foregrounds: transparent, motif inside the central safe
    # zone (66 of 108 dp) so launcher masks never clip it.
    for size, path in [
        (108, "android/res/mipmap-mdpi/ic_launcher_foreground.png"),
        (162, "android/res/mipmap-hdpi/ic_launcher_foreground.png"),
        (216, "android/res/mipmap-xhdpi/ic_launcher_foreground.png"),
        (324, "android/res/mipmap-xxhdpi/ic_launcher_foreground.png"),
        (432, "android/res/mipmap-xxxhdpi/ic_launcher_foreground.png"),
    ]:
        save(cards_motif(size, transparent=True, scale=0.56), path)

    # Feature graphic: motif left, wordmark + tagline right.
    W, H = 1024, 500
    im = Image.new("RGB", (W, H), FELT[:3])
    d = ImageDraw.Draw(im)
    motif = cards_motif(440, transparent=True, scale=0.95)
    im.paste(motif, (40, (H - 440) // 2), motif)
    title = "OPENRACKEM"
    avail = W - 490 - 36
    fs = 92
    while fs > 24:
        bb = d.textbbox((0, 0), title, font=font(fs))
        if bb[2] - bb[0] <= avail:
            break
        fs -= 2
    f_big = font(fs)
    f_sub = font(30)
    bb = d.textbbox((0, 0), title, font=f_big)
    d.text((490, 190 - bb[1] - (bb[3] - bb[1]) // 2), title, font=f_big,
           fill=(255, 255, 255))
    d.text((494, 280), "Race your rack into order.", font=f_sub,
           fill=(150, 156, 170))
    d.text((494, 326), "Free. Open source. No ads.", font=f_sub,
           fill=ACCENT[:3])
    save(im, "android/play-assets/feature-graphic-1024x500.png")


if __name__ == "__main__":
    main()
