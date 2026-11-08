"""Mockups for the next iteration:
   docs/icons/icons_compare.png  - current 40px bitmaps vs vector re-render
   docs/icons/landscape_a.png    - landscape layout, classic LCD dot matrix
   docs/icons/landscape_b.png    - landscape layout, neutral grey dots
"""
import os
from PIL import Image, ImageDraw

ICONS = r"<home>\digimon-passport\docs\icons"
HDR = r"<home>\digimon-passport\firmware\digimon-passport\main\core\digimon_icons.h"
FRAME = r"<home>\AppData\Local\Temp\frame.txt"

NAMES = ["stat", "virus", "medic", "battle", "poop", "light", "patch", "call"]


def current_icons():
    """pull the 40x40 outline bitmaps out of the header"""
    import re
    txt = open(HDR, encoding="utf-8").read()
    out = {}
    for name in NAMES:
        hdr = "toilet" if name == "poop" else name
        m = re.search(r"dg_icon_" + hdr + r"_outline\b[^{]*\{(.*?)\};", txt, re.S)
        if not m:
            raise SystemExit("no array for " + name)
        vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4})", m.group(1))]
        im = Image.new("RGB", (40, 40))
        px = im.load()
        for i, v in enumerate(vals):
            r = ((v >> 11) & 0x1F) * 255 // 31
            g = ((v >> 5) & 0x3F) * 255 // 63
            b = (v & 0x1F) * 255 // 31
            px[i % 40, i // 40] = (r, g, b)
        out[name] = im
    return out


def compare_sheet():
    cur = current_icons()
    W, H = 8 * 150 + 20, 3 * 150 + 20
    sheet = Image.new("RGB", (W, H), (232, 232, 232))
    d = ImageDraw.Draw(sheet)
    for i, name in enumerate(NAMES):
        x = 20 + i * 150
        sheet.paste(cur[name].resize((128, 128), Image.NEAREST), (x, 20))
        off = Image.open(os.path.join(ICONS, "icon_%s_128.png" % name)).convert("RGBA")
        sheet.paste(off, (x, 170), off)
        on = Image.open(os.path.join(ICONS, "icon_%s_on_128.png" % name)).convert("RGBA")
        sheet.paste(on, (x, 320), on)
        d.text((x, 2), name, fill=(40, 40, 40))
    out = os.path.join(ICONS, "icons_compare.png")
    sheet.save(out)
    return out


def frame_rows():
    rows = []
    for line in open(FRAME, encoding="utf-8"):
        line = line.strip()
        if len(line) == 32:
            rows.append(line)
    return rows[:16]


def landscape(scheme, path):
    W, H = 320, 240
    paper = scheme["paper"]
    im = Image.new("RGB", (W, H), paper)
    d = ImageDraw.Draw(im)

    # LCD: 32x16 cells, 9 px cell with a 1 px gap -> 288x144
    scale, gap = 9, 1
    gw, gh = 32 * scale, 16 * scale
    x0, y0 = (W - gw) // 2, (H - gh) // 2
    d.rectangle([x0 - 3, y0 - 3, x0 + gw + 2, y0 + gh + 2], fill=scheme["bezel"])
    lit = frame_rows()
    for r in range(16):
        for c in range(32):
            on = lit[r][c] == "#"
            color = scheme["ink"] if on else scheme["dot"]
            cx = x0 + c * scale
            cy = y0 + r * scale
            d.rectangle([cx, cy, cx + scale - 1 - gap, cy + scale - 1 - gap], fill=color)

    # function icons: 4 above, 4 below
    size = 30
    step = 46
    total = 4 * size + 3 * (step - size)
    ix = (W - total) // 2
    top_y = y0 - size - 6
    bot_y = y0 + gh + 6
    for i, name in enumerate(NAMES):
        col = i % 4
        y = top_y if i < 4 else bot_y
        img = Image.open(os.path.join(ICONS, "icon_%s_128.png" % name)).convert("RGBA")
        img = img.resize((size, size), Image.LANCZOS)
        # tint the outline towards the ink colour
        px = img.load()
        for yy in range(size):
            for xx in range(size):
                a = px[xx, yy][3]
                px[xx, yy] = (scheme["icon"][0], scheme["icon"][1], scheme["icon"][2], a)
        im.paste(img, (ix + col * step, y), img)

    d.text((6, 4), "DIGITAL MONSTER", fill=scheme["icon"])
    d.text((W - 22, H - 14), "V1", fill=scheme["icon"])
    im.save(path)
    return path


if __name__ == "__main__":
    print(compare_sheet())
    a = {"paper": (232, 236, 222), "dot": (214, 220, 200), "ink": (26, 30, 24),
         "bezel": (60, 66, 54), "icon": (90, 96, 80)}
    b = {"paper": (236, 236, 236), "dot": (222, 224, 222), "ink": (24, 24, 24),
         "bezel": (70, 70, 70), "icon": (80, 80, 80)}
    print(landscape(a, os.path.join(ICONS, "landscape_a.png")))
    print(landscape(b, os.path.join(ICONS, "landscape_b.png")))

