"""从说明书 PDF 里抽出来的彩页裁出数码兽卡片，转成 base64 供单文件 HTML 内嵌。

    python tools/cut_evo_cards.py [--src 图片] [--out 目录] [--size 56] [--colors 48]

为什么要连通域：说明书的分解图里，卡片底色和连线是同一套颜色（Ver.1 的 MERAMON
是红卡 + 红连线），直接按颜色阈值切会把两只连成一个框。做法是先对高饱和掩码做
3 次 4 邻域腐蚀，把 3 px 宽的连线腐蚀掉、只留卡片实心块，再标连通域。

卡片坐标不是硬编码的：脚本自己找 14 个候选框并打印出来，方便用 --list 核对。
需要 numpy + Pillow（DSH 内置 python 自带）。
"""
import argparse
import base64
import io
import os
import sys
from collections import defaultdict

import numpy as np
from PIL import Image


def find_boxes(img, min_area=800, min_side=35, max_side=110):
    a = np.asarray(img.convert("RGB")).astype(np.int16)
    h, w = a.shape[:2]
    mx, mn = a.max(axis=2), a.min(axis=2)
    mask = ((mx - mn) > 55) & (mx > 80)
    mask[:46, :] = False                       # 顶部阶段色带，不是卡片

    core = mask.copy()
    for _ in range(3):                         # 腐蚀掉连线
        core = (core & np.roll(core, 1, 0) & np.roll(core, -1, 0)
                     & np.roll(core, 1, 1) & np.roll(core, -1, 1))
        core[:1, :] = core[-1:, :] = False
        core[:, :1] = core[:, -1:] = False

    parent = list(range(w * h))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a_, b_):
        ra, rb = find(a_), find(b_)
        if ra != rb:
            parent[rb] = ra

    flat = core.ravel()
    for y in range(h):
        row = y * w
        for x in range(w):
            i = row + x
            if not flat[i]:
                continue
            if x and flat[i - 1]:
                union(i, i - 1)
            if y and flat[i - w]:
                union(i, i - w)

    stats = defaultdict(lambda: [10 ** 9, 10 ** 9, -1, -1, 0])
    for y in range(h):
        row = y * w
        for x in range(w):
            i = row + x
            if not flat[i]:
                continue
            r = find(i)
            s = stats[r]
            s[0] = min(s[0], x); s[1] = min(s[1], y)
            s[2] = max(s[2], x); s[3] = max(s[3], y)
            s[4] += 1

    boxes = []
    for _, (x0, y0, x1, y1, area) in stats.items():
        bw, bh = x1 - x0 + 1, y1 - y0 + 1
        if area < min_area or not (min_side <= bw <= max_side and min_side <= bh <= max_side):
            continue
        boxes.append((x0 - 4, y0 - 4, bw + 8, bh + 8))
    boxes.sort(key=lambda b: (b[0] // 40, b[1]))
    return boxes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=r"docs/pdf_assets/page_img_01.jpg")
    ap.add_argument("--out", default=r"docs/evo_cards")
    ap.add_argument("--size", type=int, default=56)
    ap.add_argument("--colors", type=int, default=48)
    ap.add_argument("--list", action="store_true", help="只打印检测到的框，不写文件")
    ap.add_argument("--drop", default="", help="逗号分隔的误检框编号，保留原编号但不写出")
    args = ap.parse_args()

    drop = {int(x) for x in args.drop.split(",") if x.strip()}
    img = Image.open(args.src)
    boxes = find_boxes(img)
    print("%s: %d boxes" % (args.src, len(boxes)))
    for i, (x, y, bw, bh) in enumerate(boxes):
        print("  #%02d  x=%d y=%d w=%d h=%d%s" % (i, x, y, bw, bh, "  [drop]" if i in drop else ""))
    if args.list:
        return 0
    if len(boxes) - len(drop) != 14:
        print("!! 期望 14 个框，实际 %d 个 —— 先在 --list 下核对坐标" % (len(boxes) - len(drop)), file=sys.stderr)
        return 1

    os.makedirs(args.out, exist_ok=True)
    print("\n/* slot 顺序需按图上的位置人工对应，写进 HTML 前核对一遍 */")
    for i, (x, y, bw, bh) in enumerate(boxes):
        if i in drop:
            continue
        box = img.crop((x, y, x + bw, y + bh))
        side = min(box.size)
        l = (box.width - side) // 2
        t = (box.height - side) // 2
        sq = box.crop((l, t, l + side, t + side)).resize((args.size, args.size), Image.LANCZOS)
        buf = io.BytesIO()
        sq.quantize(colors=args.colors, method=Image.MEDIANCUT).save(buf, "PNG", optimize=True)
        raw = buf.getvalue()
        with open(os.path.join(args.out, "box%02d.png" % i), "wb") as f:
            f.write(raw)
        print("  #%02d  %5d B  b64=%d" % (i, len(raw), len(base64.b64encode(raw))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
