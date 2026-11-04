"""320x240 mock-up of the *measured* Glass layout, straight from

    ref/vpet_v1v2v3/屏幕版式说明.md

Facts it enforces (all from the SVG measurement, see the doc for the numbers):

  * one piece of glass, THREE bands, all 32 columns wide;
  * top band 4 dot-rows, bottom band 4 dot-rows, matrix 32x16 in the middle;
  * 2.4 / 2.5 dot-rows of blank glass between a band and the matrix -- the
    bands are NOT row 0 / row 17 of the matrix;
  * glass aspect 32 : 28.7, integer scaling only;
  * each icon owns an 8-column slot, ink centred at ~60-65 % of the slot.

    python tools/mock_v4.py

writes docs/icons/land_v4_*.png
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import icon_style as S  # noqa: E402

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

ICONS = S.ICON_DIR
FRAME_TXT = os.path.join(os.environ.get("TEMP", "."), "frame.txt")
NAMES = S.NAMES
FONT_DIR = r"C:\Windows\Fonts"

W, H = 320, 240

# ---- measured glass, in dot-rows -----------------------------------------
BAND_ROWS = 4.0          # 上/下功能区，各 4 格
GAP_TOP = 2.4            # 玻璃留白
MATRIX_ROWS = 16
GAP_BOT = 2.5
GLASS_ROWS = 28.7        # 4 + 2.4 + 16 + 2.5 ≈ 28.9，实测 28.7

# ---- palettes (shell colours are the only thing a colourway changes) ------
PAPER = (0xE9, 0xE6, 0xD8)
GLASS = (0x93, 0xA0, 0x6B)      # base tone of the LCD
GAPC = (0xA2, 0xAF, 0x7B)       # 1 px inter-dot gap
LIT = (0x1B, 0x24, 0x17)
INK = (0x46, 0x50, 0x36)
PLATE = (0x2A, 0x32, 0x26)
EDGE = (0x5C, 0x68, 0x47)
GLYPH = (0xF7, 0xF5, 0xE9)

SHELLS = {
    "cream": dict(body=PAPER, rim=(0x6D, 0x7A, 0x52), hi=(0x8A, 0x96, 0x6C),
                  well=(0x33, 0x3A, 0x28), hud=(0x4A, 0x52, 0x3A)),
    "brown": dict(body=(0x8A, 0x5A, 0x3C), rim=(0x6B, 0x42, 0x2A),
                  hi=(0xA8, 0x76, 0x52), well=(0x38, 0x24, 0x18),
                  hud=(0xF2, 0xE6, 0xD6)),
    "darkgrey": dict(body=(0x3C, 0x41, 0x43), rim=(0x2A, 0x2E, 0x30),
                     hi=(0x5E, 0x66, 0x69), well=(0x17, 0x1A, 0x1B),
                     hud=(0xD9, 0xD4, 0xC6)),
    "red":   dict(body=(0xB4, 0x33, 0x2C), rim=(0x86, 0x23, 0x1E),
                  hi=(0xD6, 0x62, 0x55), well=(0x3A, 0x14, 0x12),
                  hud=(0xFB, 0xEC, 0xE6)),
    "green": dict(body=(0x3E, 0x7A, 0x46), rim=(0x2B, 0x59, 0x32),
                  hi=(0x63, 0xA0, 0x6B), well=(0x14, 0x2A, 0x18),
                  hud=(0xEF, 0xF7, 0xEC)),
}


def font(px, bold=True):
    for name in (("seguisb.ttf", "segoeuib.ttf", "arialbd.ttf") if bold
                 else ("segoeui.ttf", "arial.ttf")):
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


def geometry(p, gy=None):
    """p = integer dot pitch.  Returns every box in panel pixels."""
    band = round(BAND_ROWS * p)
    gtop = round(GAP_TOP * p)
    mat_h = MATRIX_ROWS * p
    gbot = round(GAP_BOT * p)
    gh = band + gtop + mat_h + gbot + band
    gw = 32 * p
    gx = (W - gw) // 2
    gy = (H - gh) // 2 if gy is None else gy
    return dict(p=p, gw=gw, gh=gh, gx=gx, gy=gy, band=band, gtop=gtop,
                gbot=gbot, mat_h=mat_h, mat_y=gy + band + gtop, mat_x=gx, mat_w=gw)


def draw_glass(im, d, g, shell, rows, lit=(), grid=True):
    gx, gy, p = g["gx"], g["gy"], g["p"]
    # glass base
    d.rectangle([gx, gy, gx + g["gw"] - 1, gy + g["gh"] - 1], fill=GLASS)

    # dot matrix.  grid=True: the 1 px inter-dot gap is its own (lighter) tone and
    # each cell is painted base-or-lit on top, which is what draws the mesh.
    # grid=False: cells are painted full size straight on the glass.
    my = g["mat_y"]
    d.rectangle([gx, my, gx + g["gw"] - 1, my + g["mat_h"] - 1], fill=GAPC if grid else GLASS)
    if grid:
        # closing line above row 0: the block is exactly 32x16 pitches, so the
        # top row would otherwise sit open against the blank glass
        d.rectangle([gx, my - 1, gx + g["gw"] - 1, my - 1], fill=GAPC)
    cell = p - 1 if grid else p
    for r in range(16):
        for c in range(32):
            x0, y0 = gx + c * p, my + r * p
            if rows[r][c] == "#":
                d.rectangle([x0, y0, x0 + cell - 1, y0 + cell - 1], fill=LIT)
            elif grid:
                d.rectangle([x0, y0, x0 + cell - 1, y0 + cell - 1], fill=GLASS)

    # icon bands: 4 slots of 8 columns each, no grid (they are segments, not dots)
    slot_w = g["gw"] // 4
    for i, name in enumerate(NAMES):
        row, col = divmod(i, 4)
        sx = gx + col * slot_w
        sy = gy if row == 0 else gy + g["gh"] - g["band"]
        kind = "filled" if i in lit else "outline"
        img = S.render_slot(name, kind, slot_w, g["band"], GLASS,
                            box_w=int(slot_w * 0.60), box_h=g["band"] - 6,
                            ink=INK, plate=PLATE, edge=EDGE, glyph=GLYPH,
                            plate_pad=3, radius=5)
        im.paste(img, (sx, sy))


def bezel(d, g, shell, pad=6, radius=8):
    x0, y0 = g["gx"] - pad, g["gy"] - pad
    x1, y1 = g["gx"] + g["gw"] + pad - 1, g["gy"] + g["gh"] + pad - 1
    d.rounded_rectangle([x0, y0, x1, y1], radius=radius, fill=shell["rim"])
    d.rounded_rectangle([x0, y0, x1, y1], radius=radius, outline=shell["hi"], width=1)
    q = max(2, pad - 3)
    d.rounded_rectangle([g["gx"] - q, g["gy"] - q, g["gx"] + g["gw"] + q - 1,
                         g["gy"] + g["gh"] + q - 1], radius=4, fill=shell["well"])


def battery(d, x, y, soc, ink, w=24, h=12):
    d.rounded_rectangle([x, y, x + w, y + h], radius=3, outline=ink, width=2)
    d.rectangle([x + w + 2, y + 4, x + w + 3, y + h - 4], fill=ink)
    fw = int((w - 4) * max(0, min(100, soc)) / 100)
    if fw > 0:
        d.rectangle([x + 2, y + 2, x + 1 + fw, y + h - 2], fill=ink)


def hud_right(d, shell, soc, g):
    """stacked in the strip right of the glass (which is all we have at p=8)"""
    x = g["gx"] + g["gw"] + 5
    battery(d, x, 10, soc, shell["hud"], w=20, h=10)
    d.text((x, 26), "V1", fill=shell["hud"], font=font(13))
    d.text((x - 1, 42), "%d%%" % soc, fill=shell["hud"], font=font(14))


def hud_left_right(d, shell, soc, g):
    """V1 in the left strip, battery + % in the right strip"""
    d.text((g["gx"] - 26, 10), "V1", fill=shell["hud"], font=font(13))
    x = g["gx"] + g["gw"] + 5
    battery(d, x, 10, soc, shell["hud"], w=20, h=10)
    d.text((x - 1, 26), "%d%%" % soc, fill=shell["hud"], font=font(14))


def hud_on_glass(d, g, shell, soc):
    """inside the blank glass margin the ROM never addresses"""
    y = g["gy"] + g["gh"] - g["band"] - 4
    d.text((g["gx"] + g["gw"] - 30, y - 13), "%d%%" % soc, fill=INK, font=font(12))
    d.text((g["gx"] + g["gw"] - 55, y - 13), "V1", fill=INK, font=font(12))


def hud_stack(d, shell, soc, g):
    """what the firmware does: one stack in the right shell strip --
    battery bar, then V1, then the percentage (all at x = glass right + 6)"""
    x = g["gx"] + g["gw"] + 6
    battery(d, x, 8, soc, shell["hud"], w=20, h=12)
    d.text((x, 23), "V1", fill=shell["hud"], font=font(13))
    d.text((x, 38), "%d%%" % soc, fill=shell["hud"], font=font(13))


def hud_bottom_right(d, shell, soc, g, y=None, right=304):
    """version + percentage only, right-aligned on the shell below the glass.
    Kept inside the panel's 30 px rounded-corner mask: at y = 217..231 the
    right edge may go out to x = 310 before the corner shaves it."""
    f = font(14)
    text = "V1  %d%%" % soc
    tw = d.textlength(text, font=f)
    x = right - tw
    d.text((x, 222 if y is None else y), text, fill=shell["hud"], font=f)


def compose(path, shell_name="cream", p=8, hud="stack", lit=(), soc=82, pad=2,
            grid=True, gy=None):
    shell = SHELLS[shell_name]
    im = Image.new("RGB", (W, H), shell["body"])
    d = ImageDraw.Draw(im)
    g = geometry(p, gy)
    bezel(d, g, shell, pad=pad, radius=6 if pad <= 3 else 9)
    rows = frame_rows()
    draw_glass(im, d, g, shell, rows, lit, grid=grid)
    if hud == "right":
        hud_right(d, shell, soc, g)
    elif hud == "sides":
        hud_left_right(d, shell, soc, g)
    elif hud == "glass":
        hud_on_glass(d, g, shell, soc)
    elif hud == "br":
        hud_bottom_right(d, shell, soc, g)
    else:
        hud_stack(d, shell, soc, g)
    im.save(path)
    return im, g


VARIANTS = [
    ("v4_p7_sides", dict(p=7, hud="sides", shell_name="cream", lit=(0, 7), pad=6)),
    ("v4_p7_bezel", dict(p=7, hud="right", shell_name="cream", lit=(0, 7), pad=8)),
    ("v4_p7_brown", dict(p=7, hud="sides", shell_name="brown", lit=(0, 7), pad=6)),
    ("v4_p7_red", dict(p=7, hud="sides", shell_name="red", lit=(0, 7), pad=6)),
    ("v4_p7_green", dict(p=7, hud="sides", shell_name="green", lit=(0, 7), pad=6)),
    ("v4_p8_rejected", dict(p=8, hud="sides", shell_name="cream", lit=(0, 7), pad=2)),
]

if __name__ == "__main__":
    tiles = []
    for name, kw in VARIANTS:
        im, g = compose(os.path.join(ICONS, name + ".png"), **kw)
        tiles.append(im)
        print("wrote %-24s p=%d glass %dx%d at (%d,%d)" %
              (name, g["p"], g["gw"], g["gh"], g["gx"], g["gy"]))
    sheet = Image.new("RGB", (W * 2 + 12, H * 3 + 16), (255, 255, 255))
    for i, im in enumerate(tiles):
        sheet.paste(im, (6 + (i % 2) * (W + 4), 6 + (i // 2) * (H + 4)))
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.LANCZOS)
    sheet.save(os.path.join(ICONS, "land_v4_sheet.png"))
    print("wrote contact sheet")
