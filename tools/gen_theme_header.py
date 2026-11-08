# -*- coding: utf-8 -*-
"""Regenerate main/dg_theme.h — the three style-C accent colours.

History: this script used to sample background_322x321_0028.png into a 592-entry
per-block colour table (8 icon slots + 2 gap rows + 512 matrix cells, 1.2 KB).
Style C then switched to the full-resolution dg_backdrop.h (70 KB memcpy), which
left the table with no readers.  The table was removed on 2026-11-08 (todo #11),
so the script now only owns the three constants and no longer needs the artwork.

Run from the repository root:
    python tools/gen_theme_header.py
"""
import os

OUT = os.path.join('firmware', 'digimon-passport', 'main', 'dg_theme.h')

L = []
L.append('/* dg_theme.h -- style C accent colours (trimmed 2026-11-08).')
L.append(' *')
L.append(' * This header used to carry a 592-entry per-block backdrop table (8 icon slots')
L.append(' * + 2x32 gap cells + 512 matrix cells = 1.2 KB of flash).  Style C now memcpy()s')
L.append(' * the full-resolution dg_backdrop.h instead, so those tables had no readers left')
L.append(' * and were removed (todo #11: 1.2 KB of flash back, zero visual change).')
L.append(' *')
L.append(' * Only these three constants remain.  Keep them in sync with')
L.append(' * tools/gen_theme_header.py, which regenerates this file. */')
L.append('#ifndef DG_THEME_H')
L.append('#define DG_THEME_H')
L.append('')
L.append('#include <stdint.h>')
L.append('')
L.append('/* The tone the icon sprites were baked against (GLASS_TONE[1][0]); blit_slot()')
L.append(' * treats exactly this colour as transparent under style C. */')
L.append('#define DG_GLASS_KEY565 0x950D')
L.append('')
L.append('/* Grid line over the backdrop: light grey, not a darkened copy of the cell. */')
L.append('#define DG_GRID565 0xE71C        /* #E7E7E7 — a hint of mesh, not graph paper */')
L.append('')
L.append('/* Icons under style C: the baked olive outline is recoloured to this ink.')
L.append(' * #2E382C is the softened grey-green replacement for the old near-black')
L.append(' * #1B2417 (todo #19): the retail panel\'s "lit" dots read grey-green behind the')
L.append(' * glass, not pure black.  Same value as GLASS_TONE[*][2] so dots and icons')
L.append(' * stay one colour family. */')
L.append('#define DG_ICON_INK565 0x29C5    /* #2E382C */')
L.append('')
L.append('#endif /* DG_THEME_H */')

open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(L) + '\n')
print('wrote', OUT)
