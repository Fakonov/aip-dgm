"""Landscape mockups v2: no title, battery HUD, and the A-style lit icon."""
import os
from PIL import Image, ImageDraw

ICONS = r".\docs\icons"
FRAME = r"<home>"
NAMES = ["stat", "virus", "medic", "battle", "poop", "light", "patch", "call"]


def frame_rows():
    rows = [l.strip() for l in open(FRAME, encoding="utf-8") if len(l.strip()) == 32]
    return rows[:16]


def battery(d, x, y, soc, ink, paper):
    """small battery glyph + percentage"""
    w, h = 22, 11
    d.rounded_rectangle([x, y, x + w, y + h], radius=2, outline=ink, width=2)
    d.rectangle([x + w + 1, y + 4, x + w + 3, y + 7], fill=ink)
    fill_w = int((w - 4) * max(0, min(100, soc)) / 100.0)
    if fill_w > 0:
        d.rectangle([x + 2, y + 2, x + 1 + fill_w, y + h - 2], fill=ink)


def landscape(path, scheme, lit_icon=0, soc=82, title=False):
    W, H = 320, 240
    im = Image.new("RGB", (W, H), scheme["paper"])
    d = ImageDraw.Draw(im)

    scale, gap = 9, 1
    gw, gh = 32 * scale, 16 * scale
    x0, y0 = (W - gw) // 2, (H - gh) // 2 + 4
    d.rectangle([x0 - 3, y0 - 3, x0 + gw + 2, y0 + gh + 2], fill=scheme["bezel"])
    rows = frame_rows()
    for r in range(16):
        for c in range(32):
            on = rows[r][c] == "#"
            color = scheme["ink"] if on else scheme["dot"]
            cx, cy = x0 + c * scale, y0 + r * scale
            d.rectangle([cx, cy, cx + scale - 1 - gap, cy + scale - 1 - gap], fill=color)

    size, step = 30, 46
    total = 4 * size + 3 * (step - size)
    ix = (W - total) // 2
    top_y, bot_y = y0 - size - 8, y0 + gh + 8
    for i, name in enumerate(NAMES):
        col = i % 4
        y = top_y if i < 4 else bot_y
        on = (i == lit_icon)
        f = "icon_%s_onA_128.png" % name if on else "icon_%s_128.png" % name
        img = Image.open(os.path.join(ICONS, f)).convert("RGBA").resize((size, size), Image.LANCZOS)
        im.paste(img, (ix + col * step, y), img)

    battery(d, W - 30, 6, soc, scheme["icon"], scheme["paper"])
    d.text((W - 78, H - 13), "V1  %d%%" % soc, fill=scheme["icon"])
    if title:
        d.text((6, 8), "DIGITAL MONSTER", fill=scheme["icon"])
    im.save(path)
    return path


if __name__ == "__main__":
    a = {"paper": (232, 236, 222), "dot": (214, 220, 200), "ink": (26, 30, 24),
         "bezel": (60, 66, 54), "icon": (90, 96, 80)}
    c = {"paper": (228, 232, 218), "dot": (221, 226, 210), "ink": (30, 34, 28),
         "bezel": (150, 155, 140), "icon": (78, 84, 70)}
    print(landscape(os.path.join(ICONS, "land_v2_a.png"), a, lit_icon=0, soc=82))
    print(landscape(os.path.join(ICONS, "land_v2_c.png"), c, lit_icon=4, soc=41))
