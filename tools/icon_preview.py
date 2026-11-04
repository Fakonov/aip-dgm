"""Side-by-side of the icon styles, rendered at 3x so the strokes are judgeable.

    python tools/icon_preview.py

Writes docs/icons/icons_v2_sheet.png:
    row 1  v1 outline (pure black)          <- what shipped in round 55
    row 2  v1 lit     (slate plate)
    row 3  v2 outline (olive ink)
    row 4  v2 lit     (olive plate + rim)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import icon_style as S  # noqa: E402

OLD = dict(paper=(232, 232, 232), ink=(24, 24, 24), plate=(44, 58, 71),
           edge=(44, 58, 71), glyph=(245, 245, 235), inner_ratio=0.72, radius=6)
NEW = dict(paper=S.PAPER, ink=S.INK, plate=S.PLATE, edge=S.PLATE_EDGE,
           glyph=S.GLYPH, inner_ratio=S.INNER_RATIO, radius=S.RADIUS)

if __name__ == "__main__":
    img = S.sheet(scale=3, styles=[("v1", OLD), ("v2", NEW)])
    out = os.path.join(S.ICON_DIR, "icons_v2_sheet.png")
    img.save(out)
    print("wrote", out, img.size)
