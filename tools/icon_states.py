"""Build the "lit" (selected) variants that keep the artwork detail.

Silhouette filling destroyed the gauge (a thin-frame drawing turns into a blob),
so the selected state is now the artwork itself, slightly bolder, on a plate:

  A  yellow plate  + black artwork      (matches the settings cursor colour)
  B  dark plate    + white artwork      (looks like a backlit LCD segment)
  C  silhouette only                    (what we have today, for reference)

Also rebuilds a comparison sheet.
"""
import os
from PIL import Image, ImageDraw, ImageFilter

ICONS = r"<home>\digimon-passport\docs\icons"
NAMES = ["stat", "virus", "medic", "battle", "poop", "light", "patch", "call"]
S = 128


def load(name, kind="off"):
    p = os.path.join(ICONS, "icon_%s%s_128.png" % (name, "" if kind == "off" else "_on"))
    return Image.open(p).convert("RGBA")


def bold(alpha, radius=2):
    """dilate the alpha channel: MaxFilter grows the ink, which also closes the
   1-2 px gaps that the low-res source left in thin strokes"""
    a = alpha.filter(ImageFilter.MaxFilter(radius * 2 + 1))
    return a


def plate(color, radius=18):
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([2, 2, S - 3, S - 3], radius=radius, fill=color)
    return im


def compose(name, style):
    if style == "C":
        # reuse the silhouette that render_icons_hi.py already produced
        return Image.open(os.path.join(ICONS, "icon_%s_solid_128.png" % name)).convert("RGBA")

    art = bold(load(name).getchannel("A"), 1)

    if style == "A":
        bg = plate((255, 217, 40, 255))
        fg = Image.new("RGBA", (S, S), (24, 24, 24, 0))
    else:
        bg = plate((44, 58, 71, 255))
        fg = Image.new("RGBA", (S, S), (245, 245, 235, 0))
    fg.putalpha(art)
    # keep a margin inside the plate
    art_small = fg.resize((int(S * 0.74), int(S * 0.74)), Image.LANCZOS)
    off = (S - art_small.width) // 2
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.alpha_composite(bg)
    out.alpha_composite(art_small, (off, off))
    return out


def sheet():
    cols = len(NAMES)
    cell = 140
    rows = [("off", "off"), ("A yellow", "A"), ("B dark", "B"), ("C solid", "C")]
    out = Image.new("RGB", (cols * cell + 20, len(rows) * cell + 20), (232, 232, 232))
    d = ImageDraw.Draw(out)
    for r, (label, style) in enumerate(rows):
        d.text((6, 20 + r * cell + cell // 2), label, fill=(40, 40, 40))
        for c, name in enumerate(NAMES):
            x = 20 + c * cell
            y = 20 + r * cell
            if r == 0:
                d.text((x, y - 12), name, fill=(40, 40, 40))
            img = load(name, "off") if style == "off" else compose(name, style)
            out.paste(img, (x, y), img)
    p = os.path.join(ICONS, "icons_states.png")
    out.save(p)
    return p


if __name__ == "__main__":
    print(sheet())
    for name in NAMES:
        for style in ("A", "B"):
            compose(name, style).save(os.path.join(ICONS, "icon_%s_on%s_128.png" % (name, style)))
    print("wrote per-icon A/B variants")
