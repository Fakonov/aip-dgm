"""One-off preview: dark-grey shell, no dot grid, no battery glyph,
version + percentage right-aligned at the bottom-right of the panel.

    python tools/preview_darkgrey.py

Geometry is the measured glass layout (pitch 7 -> 224 x 203) from
ref/vpet_v1v2v3/屏幕版式说明.md, lifted 4 px so the HUD gets a clean band
under the glass instead of colliding with it.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_v4 as M  # noqa: E402

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

ROOT = r"<home>\digimon-passport"
OUT = os.path.join(ROOT, "docs", "preview_darkgrey.png")

# glass 224x203; 14 px top margin so the HUD row under it stays clear of the
# panel's 30 px rounded corner (the bottom-right corner is the constraint)
GLASS_Y = 12


def cjk(px):
    for name in ("msyh.ttc", "simhei.ttf", "simsun.ttc", "seguisb.ttf"):
        p = os.path.join(r"C:\Windows\Fonts", name)
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, px)
            except OSError:
                pass
    return ImageFont.load_default()


def main():
    im, g = M.compose(os.path.join(ROOT, "docs", "icons", "_preview_dark.png"),
                      shell_name="darkgrey", p=7, hud="br", lit=(0, 7),
                      pad=6, grid=True, gy=GLASS_Y)
    Z = 2
    big = im.resize((M.W * Z, M.H * Z), Image.NEAREST)
    bar = 34
    sheet = Image.new("RGB", (M.W * Z, M.H * Z + bar), (22, 24, 25))
    sheet.paste(big, (0, 0))
    d = ImageDraw.Draw(sheet)
    d.text((12, M.H * Z + 8),
           "深灰外壳 · 保留网格 · 右下角 V1 82%  ·  玻璃 224×203 (pitch 7)",
           font=cjk(15), fill=(226, 222, 210))
    sheet.save(OUT)
    print("wrote", OUT, sheet.size, "glass at", (g["gx"], g["gy"]))


if __name__ == "__main__":
    main()
