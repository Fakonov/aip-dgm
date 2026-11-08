"""One-page preview of the glass layout: old vs new, plus a colourway row.

    python tools/make_preview.py

Reads the two firmware framebuffer shots and the p=7 mocks, writes
docs/preview_layout.png
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_v4 as M  # noqa: E402

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

ROOT = r"<home>\digimon-passport"
OLD = os.path.join(ROOT, "docs", "ui-v2", "17_icon_lit2.png")
NEW = os.path.join(ROOT, "docs", "ui-v4", "17_icon_lit2.png")
OUT = os.path.join(ROOT, "docs", "preview_layout.png")

Z = 2            # zoom for the two big tiles
GAP = 18
PAD = 22


def cjk(px):
    for name in ("msyh.ttc", "simhei.ttf", "simsun.ttc", "seguisb.ttf"):
        p = os.path.join(r"C:\Windows\Fonts", name)
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, px)
            except OSError:
                pass
    return ImageFont.load_default()


def label(d, xy, text, px=16, fill=(28, 30, 24), bold=False):
    d.text(xy, text, font=cjk(px), fill=fill)


def main():
    old = Image.open(OLD).convert("RGB")
    new = Image.open(NEW).convert("RGB")

    shells = ["brown", "red", "green", "purple" if "purple" in M.SHELLS else "green"]
    tiles = []
    for s in ("brown", "red", "green"):
        im, _ = M.compose(os.path.join(ROOT, "docs", "icons", "_preview_%s.png" % s),
                          shell_name=s, p=7, hud="stack", lit=(0, 7), pad=6)
        tiles.append((s, im))

    big_w, big_h = 320 * Z, 240 * Z
    row2_w = 320 * 3 + GAP * 2
    W = max(big_w * 2 + GAP, row2_w) + PAD * 2
    H = PAD + 34 + big_h + 56 + 240 + 40
    sheet = Image.new("RGB", (W, H), (247, 246, 240))
    d = ImageDraw.Draw(sheet)

    label(d, (PAD, PAD - 4), "元祖版式移植 · AI Passport 320×240", 22)
    y = PAD + 34
    sheet.paste(old.resize((big_w, big_h), Image.NEAREST), (PAD, y))
    sheet.paste(new.resize((big_w, big_h), Image.NEAREST), (PAD + big_w + GAP, y))
    label(d, (PAD, y + big_h + 6), "round 56：点阵 288×144，图标浮在纸面", 15)
    label(d, (PAD + big_w + GAP, y + big_h + 6),
          "round 57：玻璃 224×203，三条带同宽、同一块玻璃", 15, (150, 40, 30))

    y2 = y + big_h + 56
    label(d, (PAD, y2 - 24), "外壳配色（SHELL 行切换，玻璃三色不变）", 15)
    for i, (name, im) in enumerate(tiles):
        x = PAD + i * (320 + GAP)
        sheet.paste(im, (x, y2))
        label(d, (x + 4, y2 + 240 + 6), name.upper(), 14)

    sheet.save(OUT)
    print("wrote", OUT, sheet.size)


if __name__ == "__main__":
    main()
