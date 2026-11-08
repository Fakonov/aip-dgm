"""Silhouettes for the "lit" state, done safely.

A 1 px transparent border is added first, so the outside region is always
connected and a single flood fill can mark it; anything that is not outside is
part of the solid shape.  (The earlier BFS version crashed the interpreter.)
"""
import os
from PIL import Image, ImageDraw

ICONS = r"<home>\digimon-passport\docs\icons"
NAMES = ["stat", "virus", "medic", "battle", "poop", "light", "patch", "call"]
S = 128


def silhouette(path_in, path_out):
    im = Image.open(path_in).convert("RGBA")
    alpha = im.getchannel("A").point(lambda v: 255 if v > 40 else 0)
    padded = Image.new("L", (S + 2, S + 2), 0)
    padded.paste(alpha, (1, 1))
    ImageDraw.floodfill(padded, (0, 0), 128)
    px = padded.load()
    solid = Image.new("L", (S, S), 0)
    sp = solid.load()
    for y in range(S):
        for x in range(S):
            v = px[x + 1, y + 1]
            if v != 128:            # ink or interior hole
                sp[x, y] = 255
    out = Image.new("RGBA", (S, S), (16, 16, 16, 0))
    out.putalpha(solid)
    out.save(path_out)


if __name__ == "__main__":
    for n in NAMES:
        src = os.path.join(ICONS, "icon_%s_128.png" % n)
        dst = os.path.join(ICONS, "icon_%s_solid_128.png" % n)
        silhouette(src, dst)
        print("solid:", n)
