# -*- coding: utf-8 -*-
"""把带图版 PC 工具压成"无图精简版"，给公开仓库用。

公开仓库不能带版权素材：`tools/digimon_studio.html` 里有 57 张 base64 图，
分两类 ——

  1. `EVO_ART_V1..V4`：每张卡片 56×48 的小图，裁自《V-Pet Version 1-5》说明书扫描件
  2. 一张整幅"官方路线图"（JPEG，约 133 KB），同样是扫描件

渲染代码本来就写了空值分支（`src ? '<img...' : ""`），所以把这两个来源清空后，
页面其余部分（物种名、阶段色、数值、判据、实测 14,112 行规则）照常工作，只是没有配图。
整幅路线图连着标题一起删，换成一句说明。

用法：

    python tools/strip_studio_images.py tools/digimon_studio.html out.html

输出与 v1.0 公开版同一套做法，体积约从 400 KB 降到 120 KB。
"""
import re
import sys

BANNER = """/*
 * 公开仓库版：下面四张表的配图（彩色卡片、物种图标）裁自说明书扫描件，
 * 版权归 Bandai / 原出版社，因此这里只保留形状数据，配图置空。
 * 卡片名字、阶段色、数值、判据、实测规则全部照原样保留。
 * 要用完整版（带图）请取本地交付包里的 digimon_studio.html。
 */"""

ROADMAP_NOTE = """    <!-- 公开版不含"官方路线图"整幅扫描件（版权原因）：本地交付包提供完整版。 -->"""


def strip(src_path, dst_path):
    with open(src_path, encoding='utf-8') as fh:
        h = fh.read()
    before = len(h.encode('utf-8'))
    n_img = h.count('data:image/')

    # 1) the four per-card art tables -> {}
    h, n_tables = re.subn(r'(const EVO_ART_V\d\s*=\s*)\{.*?\};', r'\1{};', h, flags=re.S)

    # 2) the full-page official roadmap: drop the <img> with its heading
    n_road = 0
    m = re.search(r'<img[^>]*?src="data:image/[^"]*"[^>]*>', h)
    while m:
        start = h.rfind('\n', 0, h.rfind('<h3', 0, m.start()))
        end = h.find('</div>', m.end())
        if start < 0 or end < 0:
            break
        end += len('</div>')
        h = h[:start] + '\n' + ROADMAP_NOTE + h[end:]
        n_road += 1
        m = re.search(r'<img[^>]*?src="data:image/[^"]*"[^>]*>', h)

    # 3) any stray data URI left behind
    h, n_extra = re.subn(r'data:image/[a-z+]+;base64,[A-Za-z0-9+/=]+', '', h)

    # 4) banner right before the first emptied table
    anchor = h.find('const EVO_ART_V1')
    if anchor > 0:
        line = h.rfind('\n', 0, anchor) + 1
        h = h[:line] + BANNER + '\n' + h[line:]

    with open(dst_path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(h)
    after = len(h.encode('utf-8'))
    print('src %s: %d B, %d data-uri' % (src_path, before, n_img))
    print('dst %s: %d B (-%d)' % (dst_path, after, before - after))
    print('art tables emptied: %d, roadmap blocks removed: %d, stray URIs: %d'
          % (n_tables, n_road, n_extra))
    print('remaining data:image: %d, remaining <img src="data:' % h.count('data:image/'), h.count('<img src="data:'))
    return after


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        raise SystemExit(2)
    strip(sys.argv[1], sys.argv[2])
