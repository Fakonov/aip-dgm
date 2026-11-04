"""Re-render the eight function icons from the vector face (DigimonV1J.svg).

The 40x40 bitmaps in digimon_icons.h came from a low-resolution raster of that
SVG, which is why the outlines look ragged.  The artwork is vector, so we can
render it at any size: coarse pass to find the ink bbox, tight pass at 176 px,
then trim and save with alpha.
"""
import os, re, subprocess, sys
from PIL import Image

SVG = r"<home>"
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
OUT = r".\docs\icons"
TMP = os.path.join(OUT, "_tmp")

NAMES = {
    (137, 3): "stat", (155, 3): "virus", (157, 3): "medic", (159, 3): "battle",
    (22, 0): "poop", (20, 0): "light", (18, 0): "patch", (0, 0): "call",
}

os.makedirs(TMP, exist_ok=True)
svg_text = open(SVG, encoding="utf-8", errors="replace").read()


def group_block(gid):
    i = svg_text.find('<g id="%s"' % gid)
    if i < 0:
        m = re.search(r'<path id="%s"[^>]*/>' % gid, svg_text)
        if m:
            return m.group(0)
        raise SystemExit("missing group " + gid)
    depth = 0
    j = i
    while j < len(svg_text):
        m = re.compile(r"<g\b|</g>").search(svg_text, j)
        if not m:
            break
        if m.group(0) == "<g":
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                return svg_text[i:m.end()]
        j = m.end()
    raise SystemExit("unterminated group " + gid)


def render(inner, viewbox, w, h, path):
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s" width="%d" height="%d">'
           '%s</svg>' % (viewbox, w, h, inner))
    html = os.path.join(TMP, "page.html")
    with open(html, "w", encoding="utf-8") as f:
        f.write("<html><body style='margin:0;background:transparent'>" + svg + "</body></html>")
    for f in (path,):
        if os.path.exists(f):
            os.remove(f)
    subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                    "--default-background-color=00000000",
                    "--window-size=%d,%d" % (w, h),
                    "--screenshot=" + path, "file:///" + html.replace("\\", "/")],
                   check=False, capture_output=True)
    if not os.path.exists(path):
        raise SystemExit("render failed: " + path)
    return Image.open(path).convert("RGBA")


def ink_bbox(im):
    alpha = im.getchannel("A")
    if alpha.getbbox() is None:
        return None
    return alpha.getbbox()


report = []
for (byte, bit), name in NAMES.items():
    gid = "%d_%d" % (byte, bit)
    inner = group_block(gid)
    # coarse pass over the whole face
    coarse = render(inner, "0 0 4028 2692", 1007, 673, os.path.join(TMP, gid + "_coarse.png"))
    bb = ink_bbox(coarse)
    if not bb:
        print("no ink for", gid)
        continue
    sx = 4028 / 1007.0
    x0, y0, x1, y1 = bb[0] * sx, bb[1] * sx, bb[2] * sx, bb[3] * sx
    pad = max(x1 - x0, y1 - y0) * 0.06
    x0, y0, x1, y1 = x0 - pad, y0 - pad, x1 + pad, y1 + pad
    side = max(x1 - x0, y1 - y0)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    vb = "%.2f %.2f %.2f %.2f" % (cx - side / 2, cy - side / 2, side, side)
    # tight pass: 176 px for the source art, then downscale to taste
    tight = render(inner, vb, 176, 176, os.path.join(TMP, gid + "_tight.png"))
    bb2 = ink_bbox(tight)
    tight = tight.crop(bb2)
    # square canvas with a little margin, 128 px
    size = max(tight.size)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(tight, ((size - tight.width) // 2, (size - tight.height) // 2))
    canvas = canvas.resize((128, 128), Image.LANCZOS)
    out = os.path.join(OUT, "icon_%s_128.png" % name)
    canvas.save(out)
    # silhouette variant (what "lit" looks like on the real toy)
    alpha = canvas.getchannel("A").point(lambda v: 255 if v > 40 else 0)
    from PIL import ImageDraw
    solid = Image.new("L", alpha.size, 0)
    ImageDraw.floodfill(alpha.copy(), (0, 0), 128) if False else None
    # fill interior holes: flood the outside on a copy, keep everything else
    flood = alpha.copy()
    for seed in [(0, 0), (127, 0), (0, 127), (127, 127)]:
        if flood.getpixel(seed) == 0:
            ImageDraw.floodfill(flood, seed, 128)
            break
    # fill interior holes: BFS the outside from every border pixel
    from collections import deque
    outside = [[False] * 128 for _ in range(128)]
    dq = deque()
    for x in range(128):
        for y in (0, 127):
            if alpha.getpixel((x, y)) == 0:
                outside[y][x] = True; dq.append((x, y))
    for y in range(128):
        for x in (0, 127):
            if alpha.getpixel((x, y)) == 0 and not outside[y][x]:
                outside[y][x] = True; dq.append((x, y))
    while dq:
        x, y = dq.popleft()
        for dx, dy in ((1,0),(-1,0),(0,1),(0,-1)):
            nx, ny = x+dx, y+dy
            if 0 <= nx < 128 and 0 <= ny < 128 and not outside[ny][nx] and alpha.getpixel((nx, ny)) == 0:
                outside[ny][nx] = True; dq.append((nx, ny))
    for y in range(128):
        for x in range(128):
            if alpha.getpixel((x, y)) or not outside[y][x]:
                solid.putpixel((x, y), 255)
    on = Image.new("RGBA", (128, 128), (16, 16, 16, 0))
    on.putalpha(solid)
    on.save(os.path.join(OUT, "icon_%s_on_128.png" % name))
    report.append((name, gid, bb2))

print("rendered:")
for name, gid, bb in report:
    print("  %-7s %-7s ink %dx%d" % (name, gid, bb[2] - bb[0], bb[3] - bb[1]))

