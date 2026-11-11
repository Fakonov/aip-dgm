# -*- coding: utf-8 -*-
"""把当前工作区导出成"公开仓库树"（可重复执行）。

公开仓库与本地交付包的区别只有一条：**不带版权素材**。具体是

  * Bandai 的 ROM 镜像（`main/roms/*.bin`）—— 绝不进
  * 说明书扫描件（PC 工具里的 57 张 base64 图、整幅官方路线图）—— 生成无图版
  * 本机的绝对路径 —— 脱敏成 `<home>`
  * 内部交接单（NEXT-SESSION / 会话链 / TODO）—— 不放

用法：

    python tools/build_public_tree.py "D:\\path\\to\\aip-dgm-v2.0-github"

脚本只负责铺结构 + 复制 + 脱敏 + 生成无图 html + 重算 SHA256SUMS；
README / CHANGELOG / THIRD-PARTY / .gitignore / LICENSE / VERSION 由人写，
脚本不会创建它们（重跑也不会覆盖）。
"""
import hashlib
import os
import re
import shutil
import sys

import strip_studio_images

FW = os.path.join('firmware', 'digimon-passport')
FW_TOP = ['.clangd', 'CMakeLists.txt', 'partitions.csv', 'sdkconfig.defaults', 'dependencies.lock',
          'version.txt']
FW_TREES = [('components/bsp', ('*.bin', '*.o', '*.exe')),
            ('main', ('*.bin', '*.o', '*.exe', '*.log', 'sdkconfig.old'))]

DOCS = ['HANDOFF.md', 'RETROSPECTIVE.md', 'studio-tool.md', 'OPTIMIZATION-STUDY.md',
        'BOUTS-WIDTH-STUDY.md', 'LINK-BATTLE-FEASIBILITY.md']

TOOLS = ['check_save_blob.mjs', 'check_evo_tables.mjs', 'core_body.c', 'cut_evo_cards.py',
         'dg_card_selftest.py', 'dg_console.py', 'dg_link_battle.py', 'dg_link_pair.py',
         'dg_link_pc_battle.py', 'dg_link_pc_responder.py', 'dg_link_raw_probe.py',
         'dg_reboot_watch.py', 'dg_sim.mjs', 'dg_sim_card.mjs', 'dg_sim_keys_probe.mjs',
         'dg_sim_menu_probe.mjs', 'dg_sim_menu_test.mjs', 'dg_sim_menu2.mjs',
         'dg_sim_menu3.mjs', 'dg_sim_menu4.mjs', 'dg_sim_menu5.mjs', 'dg_sim_rom.mjs',
         'dg_sim_rom_multi.mjs', 'dg_sim_skip.mjs', 'dg_sim_state.mjs', 'dg_sim_test.mjs',
         'dg_sim_wake.mjs', 'dg_snd_probe.py', 'disasm_v1.py', 'gen_backdrop.py',
         'gen_brick_tile.py', 'gen_digimon_icons.py', 'gen_evo_rules.py',
         'gen_icons_landscape.py', 'gen_op_tbl.py', 'gen_studio_tables.py',
         'gen_theme_header.py', 'icon_preview.py', 'icon_solid.py', 'icon_states.py',
         'icon_style.py', 'make_preview.py', 'mk_full_bin.py', 'mkc.py', 'mock_layout.py',
         'mock_v2.py', 'mock_v3.py', 'mock_v4.py', 'preview_darkgrey.py', 'py_ref.py',
         'render_icons_hi.py', 'strip_studio_images.py', 'build_public_tree.py']

DESENSITIZE = [(r'<home>', '<home>'),   # C string literals use doubled backslashes
               (r'<home>', '<home>'),
               (r'<home>', '<home>')]

TEXT_EXT = ('.md', '.txt', '.c', '.h', '.py', '.mjs', '.csv', '.json', '.defaults', '')


def clean_text(text):
    for a, b in DESENSITIZE:
        text = text.replace(a, b)
    out = []
    for line in text.split('\n'):
> 上一轮会话日志（内部文件，不随公开仓库分发）
            out.append('> 上一轮会话日志（内部文件，不随公开仓库分发）')
        else:
            out.append(line)
    return '\n'.join(out)


def copy_file(src, dst, sanitize=True):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if sanitize and os.path.splitext(src)[1].lower() in TEXT_EXT:
        with open(src, encoding='utf-8', errors='replace') as fh:
            text = fh.read()
        new = clean_text(text)
        with open(dst, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(new)
        return new != text
    shutil.copy2(src, dst)
    return False


def main(dest):
    dest = os.path.abspath(dest)
    os.makedirs(dest, exist_ok=True)
    changed = []
    count = 0

    for name in FW_TOP:
        src = os.path.join(FW, name)
        if os.path.exists(src):
            if copy_file(src, os.path.join(dest, 'firmware', 'digimon-passport', name)):
                changed.append(name)
            count += 1

    for sub, skip in FW_TREES:
        base = os.path.join(FW, sub)
        for root, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in ('build', 'build-sim', 'managed_components', 'roms')]
            for f in files:
                if any(f.endswith(s[1:]) for s in skip) or f == 'sdkconfig.old':
                    continue
                src = os.path.join(root, f)
                rel = os.path.relpath(src, FW)
                if copy_file(src, os.path.join(dest, 'firmware', 'digimon-passport', rel)):
                    changed.append(rel)
                count += 1

    for f in sorted(os.listdir('src')):
        if f.endswith(('.c', '.h')):
            if copy_file(os.path.join('src', f), os.path.join(dest, 'src', f)):
                changed.append('src/' + f)
            count += 1

    for f in DOCS:
        src = os.path.join('docs', f)
        if os.path.exists(src):
            if copy_file(src, os.path.join(dest, 'docs', f)):
                changed.append('docs/' + f)
            count += 1

    for f in TOOLS:
        src = os.path.join('tools', f)
        if os.path.exists(src):
            if copy_file(src, os.path.join(dest, 'tools', f)):
                changed.append('tools/' + f)
            count += 1
        else:
            print('  skip missing tool:', f)

    # images: the v1.0 README screenshots are not regenerated here (they are
    # still accurate), so they are taken from the previous public tree; the two
    # new ones come from this round's simulator shots.
    OLD_IMAGES = r'<home>\Desktop\dsh sth\aip-dgm\release\aip-dgm-v1.0-github\docs\images'
    if os.path.isdir(OLD_IMAGES):
        os.makedirs(os.path.join(dest, 'docs', 'images'), exist_ok=True)
        for f in sorted(os.listdir(OLD_IMAGES)):
            if f.endswith('.png'):
                shutil.copy2(os.path.join(OLD_IMAGES, f), os.path.join(dest, 'docs', 'images', f))
                count += 1
    extra = [('docs/ui-brick-line/sim/retail_main.png', 'docs/images/retail-main.png'),
             ('docs/ui-brick-line/sim/zoom_before_after.png', 'docs/images/brick-seam.png')]
    for src, rel in extra:
        if os.path.exists(src):
            copy_file(src, os.path.join(dest, rel), sanitize=False)
            count += 1

    # image-less PC tool
    html_src = os.path.join('tools', 'digimon_studio.html')
    html_dst = os.path.join(dest, 'tools', 'digimon_studio.html')
    os.makedirs(os.path.dirname(html_dst), exist_ok=True)
    strip_studio_images.strip(html_src, html_dst)
    count += 1

    print('\ncopied %d files to %s' % (count, dest))
    if changed:
        print('desensitized %d files: %s' % (len(changed), ', '.join(changed[:8])))
    sha256sums(dest)


def sha256sums(root):
    rows = []
    for cur, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ('.git',)]
        for f in sorted(files):
            if f in ('SHA256SUMS.txt',):
                continue
            p = os.path.join(cur, f)
            rel = os.path.relpath(p, root).replace('\\', '/')
            h = hashlib.sha256(open(p, 'rb').read()).hexdigest()
            rows.append((rel, h))
    rows.sort()
    with open(os.path.join(root, 'SHA256SUMS.txt'), 'w', encoding='utf-8', newline='\n') as fh:
        for rel, h in rows:
            fh.write('%s  %s\n' % (h, rel))
    print('SHA256SUMS.txt: %d entries' % len(rows))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(2)
    main(sys.argv[1])
