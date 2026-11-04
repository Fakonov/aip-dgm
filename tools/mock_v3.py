"""Offline mock-ups of the 320x240 landscape screen, at 1:1 device pixels.

    python tools/mock_v3.py

Renders the same numbers the firmware uses (canvas 288x144 at (16,48), scale 9,
1 px gap, 32 px icons at x = 84/124/164/204) so the palette decision can be made
on a monitor instead of on the toy.  Writes docs/icons/land_v3_*.png plus a 3x
zoom of the dot matrix for judging the grid contrast.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import icon_style as S  # noqa: E402

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

ICONS = S.ICON_DIR
FRAME_TXT = os.path.join(os.environ.get("TEMP", "."), "frame.txt")
NAMES = S.NAMES

W, H = 320, 240
SCALE, GAP = 9, 1
GW, GH = 32 * SCALE, 16 * SCALE          # 288 x 144
X0, Y0 = (W - GW) // 2, (H - GH) // 2     # (16, 48)
ICON = 32
STEP = ICON + 8                          # 40 -> x = 84/124/164/204
BAR_X = (W - (4 * ICON + 3 * (STEP - ICON))) // 2
TOP_Y = Y0 - ICON - 8                    # 8
BOT_Y = Y0 + GH + 8                      # 200

FONT_DIR = r"C:\Windows\Fonts"


def font(px, bold=True):
    for name in (("seguisb.ttf", "segoeuib.ttf", "arialbd.ttf", "segoeui.ttf")
                 if bold else ("segoeui.ttf", "arial.ttf")):
        p = os.path.join(FONT_DIR, name)
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, px)
            except OSError:
                pass
    return ImageFont.load_default()


def frame_rows():
    rows = [l.strip() for l in open(FRAME_TXT, encoding="utf-8") if len(l.strip()) == 32]
    return rows[:16]


def draw_lcd(d, x0, y0, scale_x, rows, tone):
    """tone = (unlit, gap, lit) as sRGB tuples, already glass-corrected."""
    gap = 1 if scale_x >= 5 else 0
    # the gaps are their own colour, not "whatever is behind the canvas"
    d.rectangle([x0, y0, x0 + 32 * scale_x - 1, y0 + 16 * scale_x - 1], fill=tone[1])
    for r in range(16):
        for c in range(32):
            on = rows[r][c] == "#"
            color = tone[2] if on else tone[0]
            cx, cy = x0 + c * scale_x, y0 + r * scale_x
            d.rectangle([cx, cy, cx + scale_x - 1 - gap, cy + scale_x - 1 - gap], fill=color)


def bezel_flat(d, tone):
    d.rectangle([X0 - 3, Y0 - 3, X0 + GW + 2, Y0 + GH + 2], fill=tone["bezel"])


def bezel_layered(d, tone):
    """plastic rim -> dark recess -> glass: three rings, 3 px each."""
    outer = [X0 - 7, Y0 - 7, X0 + GW + 6, Y0 + GH + 6]
    d.rounded_rectangle(outer, radius=9, fill=tone["bezel"])
    d.rounded_rectangle(outer, radius=9, outline=tone["bezel_hi"], width=1)
    inner = [X0 - 4, Y0 - 4, X0 + GW + 3, Y0 + GH + 3]
    d.rounded_rectangle(inner, radius=6, fill=tone["recess"])


def battery(d, x, y, soc, ink, paper, w=22, h=11):
    d.rounded_rectangle([x, y, x + w, y + h], radius=3, outline=ink, width=2)
    d.rounded_rectangle([x + w + 2, y + 3, x + w + 4, y + h - 3], radius=1, fill=ink)
    fill_w = int((w - 4) * max(0, min(100, soc)) / 100.0)
    if fill_w > 0:
        d.rectangle([x + 2, y + 2, x + 1 + fill_w, y + h - 2], fill=ink)


def hud_single(d, tone, soc):
    d.text((W - 88, H - 14), "V1  %d%%" % soc, fill=tone["hud"], font=font(14))


def hud_split(d, tone, soc):
    """small 'V1' + big percentage, right-aligned on the bottom margin.
    Mirrors digimon_app.c: HUD_MARGIN_X/Y = 16/11, fonts 14 and 20."""
    small = font(14)
    big = font(20)
    pct = "%d%%" % soc
    pw = d.textlength(pct, font=big)
    d.text((W - 16 - pw, H - 11 - 20), pct, fill=tone["hud_strong"], font=big)
    vw = d.textlength("V1", font=small)
    d.text((W - 16 - pw - 7 - vw, H - 11 - 15), "V1", fill=tone["hud"], font=small)


def screen(path, tone, style="layer", hud="split", lit=0, soc=82):
    im = Image.new("RGB", (W, H), tone["paper"])
    d = ImageDraw.Draw(im)
    rows = frame_rows()

    (bezel_layered if style == "layer" else bezel_flat)(d, tone)
    draw_lcd(d, X0, Y0, SCALE, rows, (tone["unlit"], tone["gap"], tone["lit"]))

    for i, name in enumerate(NAMES):
        col, row = i % 4, i // 4
        kind = "filled" if i == lit else "outline"
        img = S.render(name, kind, paper=tone["paper"], ink=tone["icon_ink"],
                       plate=tone["plate"], edge=tone["plate_edge"],
                       normalize=True)
        im.paste(img, (BAR_X + col * STEP, TOP_Y if row == 0 else BOT_Y))

    battery(d, W - 20 - 26 - 3, 7, soc, tone["hud_strong"], tone["paper"])
    (hud_split if hud == "split" else hud_single)(d, tone, soc)
    im.save(path)
    return im


# ---- palettes ------------------------------------------------------------
def glass(c):
    """constant -> what the RGB565 panel really shows"""
    return S.on_glass(c)


# A: shipped in round 55 (flat bezel, black icons, one-line HUD)
A = dict(
    paper=glass((232, 232, 232)),
    unlit=glass((0x9C, 0xAA, 0x73)),
    gap=glass((0xB5, 0xC2, 0x8C)),
    lit=glass((0x18, 0x20, 0x10)),
    bezel=glass((0x7C, 0x8A, 0x5E)),
    bezel_hi=glass((0x5E, 0x6B, 0x45)),
    recess=glass((0x7C, 0x8A, 0x5E)),
    hud=glass((0x30, 0x30, 0x30)),
    hud_strong=glass((0x30, 0x30, 0x30)),
    icon_ink=(24, 24, 24),
    plate=(44, 58, 71),
    plate_edge=(44, 58, 71),
)

# B: sage paper + soft grid + layered bezel  (== firmware DG_LCD_TONE 1)
B = dict(
    paper=glass((0xE9, 0xE6, 0xD8)),
    unlit=glass((0x93, 0xA0, 0x6B)),
    gap=glass((0xA2, 0xAF, 0x7B)),
    lit=glass((0x1B, 0x24, 0x17)),
    bezel=glass((0x6D, 0x7A, 0x52)),
    bezel_hi=glass((0x8A, 0x96, 0x6C)),
    recess=glass((0x33, 0x3A, 0x28)),
    hud=glass((0x6B, 0x75, 0x55)),
    hud_strong=glass((0x2E, 0x36, 0x25)),
    icon_ink=S.INK,
    plate=S.PLATE,
    plate_edge=S.PLATE_EDGE,
)

# C: deeper green, stronger mesh (closer to a real V-pet LCD)
C = dict(
    paper=glass((0xE9, 0xE6, 0xD8)),
    unlit=glass((0x8C, 0x9A, 0x66)),
    gap=glass((0x96, 0xA4, 0x73)),
    lit=glass((0x20, 0x2A, 0x1B)),
    bezel=glass((0x6D, 0x7A, 0x52)),
    bezel_hi=glass((0x8A, 0x96, 0x6C)),
    recess=glass((0x33, 0x3A, 0x28)),
    hud=glass((0x6B, 0x75, 0x55)),
    hud_strong=glass((0x2E, 0x36, 0x25)),
    icon_ink=S.INK,
    plate=S.PLATE,
    plate_edge=S.PLATE_EDGE,
)

# D: neutral stone, almost no mesh -- the "C" of DESIGN.md
D = dict(
    paper=glass((0xED, 0xEA, 0xE3)),
    unlit=glass((0xA9, 0xAE, 0x9C)),
    gap=glass((0xB4, 0xB9, 0xA7)),
    lit=glass((0x23, 0x26, 0x1F)),
    bezel=glass((0x8E, 0x91, 0x86)),
    bezel_hi=glass((0xAA, 0xAD, 0xA2)),
    recess=glass((0x50, 0x53, 0x4A)),
    hud=glass((0x74, 0x78, 0x70)),
    hud_strong=glass((0x2B, 0x2E, 0x28)),
    icon_ink=S.INK,
    plate=S.PLATE,
    plate_edge=S.PLATE_EDGE,
)

VARIANTS = [
    ("a_shipped", A, "flat", "single", "shipped round 55"),
    ("b_soft", B, "layer", "split", "cream paper, soft grid, layered bezel"),
    ("c_deep", C, "layer", "split", "deeper green, denser mesh"),
    ("d_stone", D, "layer", "split", "neutral stone, faint mesh"),
]


def zoom(src, path, factor=3, box=None):
    box = box or (X0 - 8, Y0 - 8, X0 + 120, Y0 + 80)
    crop = src.crop(box)
    crop = crop.resize((crop.width * factor, crop.height * factor), Image.NEAREST)
    crop.save(path)
    return crop


if __name__ == "__main__":
    tiles = []
    for name, tone, style, hud, label in VARIANTS:
        p = os.path.join(ICONS, "land_v3_%s.png" % name)
        im = screen(p, tone, style=style, hud=hud)
        tiles.append(im)
        print("wrote", p, label)
    # 2x2 contact sheet
    sheet = Image.new("RGB", (W * 2 + 12, H * 2 + 12), (255, 255, 255))
    for i, im in enumerate(tiles):
        sheet.paste(im, (6 + (i % 2) * (W + 4), 6 + (i // 2) * (H + 4)))
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.LANCZOS)
    sheet.save(os.path.join(ICONS, "land_v3_sheet.png"))
    zoom(tiles[1], os.path.join(ICONS, "land_v3_zoom_b.png"))
    zoom(tiles[2], os.path.join(ICONS, "land_v3_zoom_c.png"))
    print("wrote contact sheet + zooms")
