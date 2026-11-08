"""One place for the eight function-icon look.

Both the header generator (tools/gen_icons_landscape.py) and the offline
mock-ups (tools/mock_v3.py) import this module, so a style change shows up in
the preview and in the firmware in the same commit.

Design rules learned the hard way (docs/HANDOFF.md, PROGRESS round 49-55):

  * LIT icons are a dark rounded plate with the ORIGINAL artwork in white.
    A solid silhouette (BFS-filled outline) turns the gauge and the band-aid
    into black blobs, so never go back to that.
  * The unlit state is the plain outline artwork.  Pure black reads as
    "printed label"; a dark olive ink reads as an unlit LCD segment and sits
    better on the sage paper.
  * The sprite background must be exactly the screen paper colour, because the
    images are RGB565 with no alpha: a mismatch draws a visible square.
"""
import os

from PIL import Image, ImageDraw, ImageFilter

ICON_DIR = r"<home>\digimon-passport\docs\icons"
NAMES = ["stat", "virus", "medic", "battle", "poop", "light", "patch", "call"]

# ---- palette -------------------------------------------------------------
# Written as sRGB; both the generator and the firmware quantise to RGB565, so
# the sprite background and the LVGL screen colour land on the same 16-bit
# value even though the panel only has 5-6 bits per channel.
PAPER        = (0xE9, 0xE6, 0xD8)   # default shell colour behind everything
GLASS        = (0x93, 0xA0, 0x6B)   # LCD glass base: the icon band sits on it
INK          = (0x46, 0x50, 0x36)   # unlit outline
PLATE        = (0x2A, 0x32, 0x26)   # lit icon plate
PLATE_EDGE   = (0x5C, 0x68, 0x47)   # 1 px rim that keeps the plate from
                                    # reading as a hole in the panel
GLYPH        = (0xF7, 0xF5, 0xE9)   # artwork on the plate

# ---- metrics -------------------------------------------------------------
SIZE         = 32
INNER_RATIO  = 0.76     # artwork size inside the plate (24 px at 32 px)
RADIUS       = 7        # plate corner radius
EDGE_PX      = 1        # plate rim width
ALPHA_BOOST  = 1.55     # un-premultiply the LANCZOS downscale so 1.5 px
                        # strokes stay solid instead of turning grey
ALPHA_FLOOR  = 40       # alpha below this is dropped


def rgb565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def expand565(v):
    """What LVGL actually puts on the glass for a 565 value."""
    r5 = (v >> 11) & 0x1F
    g6 = (v >> 5) & 0x3F
    b5 = v & 0x1F
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def on_glass(rgb):
    """Colour a constant really becomes after the round trip to RGB565."""
    return expand565(rgb565(rgb))


def _crisp(art, size):
    """Downscale the 128 px artwork and rebuild the alpha ramp.

    LANCZOS on a 1-2 px stroke yields a wide grey ramp; the panel has no
    alpha channel, so that grey is baked in as light grey.  Pushing the ramp
    up keeps the stroke solid and only the outermost pixel soft.
    """
    small = art.resize((size, size), Image.LANCZOS)
    px = small.load()
    for y in range(size):
        for x in range(size):
            r, g, b, a = px[x, y]
            if a <= ALPHA_FLOOR:
                px[x, y] = (r, g, b, 0)
            else:
                a2 = min(255, int((a - ALPHA_FLOOR) * ALPHA_BOOST))
                px[x, y] = (r, g, b, a2)
    return small


def load_source(name):
    return Image.open(os.path.join(ICON_DIR, "icon_%s_128.png" % name)).convert("RGBA")


def normalize(src, size, fill=0.78):
    """Crop to the artwork bbox and scale it so its longest side is `fill` of
    the cell.  Fixed-ratio scaling made the big two-figure 'battle' glyph beat
    the thin 'call' bolt by ~20 % optical weight; a bbox fit evens them out."""
    bbox = src.getchannel("A").getbbox()
    if not bbox:
        return src
    art = src.crop(bbox)
    longest = max(art.size)
    if longest <= 0:
        return src
    target = max(1, int(round(size * fill)))
    k = target / float(longest)
    art = art.resize((max(1, int(round(art.size[0] * k))),
                      max(1, int(round(art.size[1] * k)))), Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(art, ((size - art.size[0]) // 2, (size - art.size[1]) // 2))
    return canvas


def _glyph(name, size, inner_ratio, normalized, opt_scale):
    """The source artwork as an RGBA layer of `size` x `size`, alpha boosted."""
    src = load_source(name)
    if normalized:
        art = normalize(src, size, opt_scale)
    else:
        art = src.resize((int(round(size * inner_ratio)),) * 2, Image.LANCZOS)
        art = _recentre(art, size)
    return _boost(art)


def _recentre(art, size):
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(art, ((size - art.size[0]) // 2, (size - art.size[1]) // 2))
    return canvas


def _boost(art):
    px = art.load()
    w, h = art.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a <= ALPHA_FLOOR:
                px[x, y] = (r, g, b, 0)
            else:
                px[x, y] = (r, g, b, min(255, int((a - ALPHA_FLOOR) * ALPHA_BOOST)))
    return art


def _tint(art, color):
    return Image.merge("RGBA", (
        Image.new("L", art.size, color[0]),
        Image.new("L", art.size, color[1]),
        Image.new("L", art.size, color[2]),
        art.getchannel("A")))


def render(name, kind, size=SIZE, paper=PAPER, plate=PLATE, edge=PLATE_EDGE,
           glyph=GLYPH, ink=INK, inner_ratio=INNER_RATIO, radius=RADIUS,
           edge_px=EDGE_PX, normalize=False, opt_scale=0.78):
    """kind = 'outline' | 'filled'.  Returns an RGB image of `size` x `size`.

    normalize=True fits each glyph's bounding box to `opt_scale` of the cell
    instead of scaling every glyph by the same ratio."""
    base = Image.new("RGB", (size, size), paper)
    art = _tint(_glyph(name, size, inner_ratio, normalize, opt_scale),
                ink if kind == "outline" else glyph)

    if kind == "filled":
        d = ImageDraw.Draw(base)
        d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=plate)
        if edge_px:
            d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius,
                                outline=edge, width=edge_px)
    base.paste(art, (0, 0), art)
    return base


def render_slot(name, kind, w, h, bg, box_w=None, box_h=None, pad=2,
                plate=PLATE, edge=PLATE_EDGE, glyph=GLYPH, ink=INK,
                plate_pad=4, radius=6):
    """One icon inside a band slot of `w` x `h` pixels (2019-era layout).

    The band is part of the glass, not a separate widget strip, so:
      * the canvas is the slot size and is filled with the *glass* colour `bg`
        (the shell colour must not leak in here);
      * the artwork is fitted into a box (default = slot minus `pad`), keeping
        its aspect ratio, so the wide gauge fills the width and the narrow bolt
        does not get blown up to match it;
      * the lit state is a rounded plate just big enough for the ink.
    """
    base = Image.new("RGB", (w, h), bg)
    bw = box_w if box_w else w - 2 * pad
    bh = box_h if box_h else h - 2 * pad

    src = load_source(name)
    bbox = src.getchannel("A").getbbox()
    art = src.crop(bbox) if bbox else src
    k = min(bw / art.size[0], bh / art.size[1])
    art = art.resize((max(1, int(round(art.size[0] * k))),
                      max(1, int(round(art.size[1] * k)))), Image.LANCZOS)
    art = _boost(art)
    art = _tint(art, ink if kind == "outline" else glyph)

    d = ImageDraw.Draw(base)
    if kind == "filled":
        pw = min(w - 2, art.size[0] + 2 * plate_pad)
        ph = min(h - 2, art.size[1] + 2 * plate_pad)
        x0, y0 = (w - pw) // 2, (h - ph) // 2
        d.rounded_rectangle([x0, y0, x0 + pw - 1, y0 + ph - 1], radius=radius,
                            fill=plate)
        if edge:
            d.rounded_rectangle([x0, y0, x0 + pw - 1, y0 + ph - 1], radius=radius,
                                outline=edge, width=1)
    base.paste(art, ((w - art.size[0]) // 2, (h - art.size[1]) // 2), art)
    return base


def sheet(scale=3, gap=6, styles=None):
    """Return a comparison sheet: one row per style, offs over ons."""
    styles = styles or [("v2", {})]
    cell = SIZE * scale
    rows = len(styles) * 2
    w = len(NAMES) * (cell + gap) + gap
    h = rows * (cell + gap) + gap
    out = Image.new("RGB", (w, h), (250, 250, 250))
    for si, (label, kw) in enumerate(styles):
        for ri, kind in enumerate(("outline", "filled")):
            for ci, name in enumerate(NAMES):
                img = render(name, kind, **kw).resize((cell, cell), Image.NEAREST)
                out.paste(img, (gap + ci * (cell + gap), gap + (si * 2 + ri) * (cell + gap)))
    return out
