/* dg_theme.h -- style C accent colours (trimmed 2026-11-08).
 *
 * This header used to carry a 592-entry per-block backdrop table (8 icon slots
 * + 2x32 gap cells + 512 matrix cells = 1.2 KB of flash).  Style C now memcpy()s
 * the full-resolution dg_backdrop.h instead, so those tables had no readers left
 * and were removed (todo #11: 1.2 KB of flash back, zero visual change).
 *
 * Only these three constants remain.  Keep them in sync with
 * tools/gen_theme_header.py, which regenerates this file. */
#ifndef DG_THEME_H
#define DG_THEME_H

#include <stdint.h>

/* The tone the icon sprites were baked against (GLASS_TONE[1][0]); blit_slot()
 * treats exactly this colour as transparent under style C. */
#define DG_GLASS_KEY565 0x950D

/* Grid line over the backdrop: light grey, not a darkened copy of the cell. */
#define DG_GRID565 0xE71C        /* #E7E7E7 — a hint of mesh, not graph paper */

/* Icons under style C: the baked olive outline is recoloured to this ink.
 * #2E382C is the softened grey-green replacement for the old near-black
 * #1B2417 (todo #19): the retail panel's "lit" dots read grey-green behind the
 * glass, not pure black.  Same value as GLASS_TONE[*][2] so dots and icons
 * stay one colour family. */
#define DG_ICON_INK565 0x29C5    /* #2E382C */

#endif /* DG_THEME_H */
