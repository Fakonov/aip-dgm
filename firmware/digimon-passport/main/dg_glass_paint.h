/* dg_glass_paint.h -- the glass canvas painter, in one place.
 *
 * Pure functions over a caller-owned RGB565 buffer: no LVGL, no ESP-IDF, no
 * globals.  That is deliberate -- the firmware paints the glass from
 * digimon_app.c, while src/glass_paint_test.c (host, gcc) uses the same code to
 * prove that "repaint only the cells that changed" is pixel-identical to
 * "repaint the whole glass", which is the whole point of the dirty-rect change
 * (R3 audit §5.2).
 *
 * Layout, for orientation (see the long comment above DG_PITCH):
 *
 *   +----------------------------+  <- top icon band, 4 dot-rows
 *   +----------------------------+  <- the closing line, 1 px, at mat_y - 1
 *   |      32 x 16 dot matrix    |     cell = pitch x pitch, grid line on its
 *   +----------------------------+     right and bottom edge
 *   +----------------------------+  <- bottom icon band
 *
 * Every cell is self-contained: it reads the backdrop (retail) or the flat tone
 * (classic) and never the canvas, so painting one cell twice produces the same
 * pixels -- no accumulated 6 % darkening.
 */
#ifndef DG_GLASS_PAINT_H
#define DG_GLASS_PAINT_H

#include <stdint.h>
#include <string.h>

/* One RGB565 colour a little darker (integer only).  Two users: the 6 % mesh
 * that runs through every cell, and the dot ink itself -- on the photo backdrop
 * the dots read lighter than they do on the flat CLASSIC glass, so both themes
 * share an ink that is one step deeper than the tone table. */
static inline uint16_t dim565(uint16_t v, uint16_t num, uint16_t den)
{
    uint16_t r5 = (uint16_t)(((v >> 11) & 0x1F) * num / den);
    uint16_t g6 = (uint16_t)(((v >> 5) & 0x3F) * num / den);
    uint16_t b5 = (uint16_t)((v & 0x1F) * num / den);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* Everything a paint call needs.  `style` 0 = classic (flat glass tone),
 * 1 = retail (photo backdrop, mesh shaded 6 % into the picture). */
typedef struct {
    uint16_t       *buf;        /* canvas, w * h pixels, RGB565 */
    int             w, h;
    int             mat_y;      /* y of matrix row 0 inside the glass */
    int             pitch;      /* panel px per dot */
    int             style;
    uint16_t        base;       /* classic: unlit cell body */
    uint16_t        gap;        /* grid line inside a cell */
    uint16_t        lit;        /* lit dot body */
    const uint8_t  *backdrop;   /* I8 indices, w * h (retail only) */
    const uint16_t *pal;        /* 256-entry palette for `backdrop` */
} dgp_glass_t;

/* The whole glass: backdrop (retail) or flat tone (classic).  Wipes both icon
 * bands too -- callers must re-blit the icons afterwards. */
static inline void dgp_fill(const dgp_glass_t *g)
{
    if (g->style) {
        const int n = g->w * g->h;
        for (int i = 0; i < n; i++) g->buf[i] = g->pal[g->backdrop[i]];
    } else {
        const int n = g->w * g->h;
        for (int i = 0; i < n; i++) g->buf[i] = g->base;
    }
}

/* The closing line above matrix row 0.  Without it the mesh has no top edge;
 * retail shades the picture instead of painting the classic gap tone, so the
 * line sits in the photo rather than on it.  Steals one pixel from the top
 * margin (see DG_GAP_TOP). */
static inline void dgp_top_line(const dgp_glass_t *g)
{
    uint16_t *row = &g->buf[(g->mat_y - 1) * g->w];
    for (int x = 0; x < g->w; x++) {
        row[x] = g->style ? dim565(row[x], 15, 16) : g->gap;
    }
}

/* One dot cell at matrix (r, c).  A lit cell still carries the grid line on its
 * right/bottom edge -- filling it solid made a run of dots merge into one bar,
 * which is what the owner reported in round 121.  An unlit retail cell is the
 * backdrop with the same 6 % mesh on those edges. */
static inline void dgp_cell(const dgp_glass_t *g, int r, int c, int on)
{
    const int p = g->pitch;
    for (int dy = 0; dy < p; dy++) {
        const int y = g->mat_y + r * p + dy;
        uint16_t *dst = &g->buf[y * g->w + c * p];
        const int edge_y = (dy == p - 1);
        if (g->style) {
            const uint8_t *bp = &g->backdrop[y * g->w + c * p];
            for (int dx = 0; dx < p; dx++) {
                const uint16_t bg = g->pal[bp[dx]];
                if (on) {
                    dst[dx] = (edge_y || dx == p - 1) ? g->gap : g->lit;
                } else if (edge_y || dx == p - 1) {
                    dst[dx] = dim565(bg, 15, 16);
                } else {
                    dst[dx] = bg;
                }
            }
        } else {
            const uint16_t color = on ? g->lit : g->base;
            for (int dx = 0; dx < p; dx++) {
                dst[dx] = (edge_y || dx == p - 1) ? g->gap : color;
            }
        }
    }
}

#endif /* DG_GLASS_PAINT_H */
