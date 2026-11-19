/* glass_paint_test.c -- host test for the dirty-rect glass painter.
 *
 * The firmware used to repaint the whole 192x183 canvas on every VRAM change
 * and hand the entire canvas to LVGL.  Since R3 it repaints only the cells that
 * flipped and invalidates only their bounding box.  That is only safe if
 *
 *     paint a cell now  ==  the same cell in a from-scratch full repaint
 *
 * for every cell, both themes, and any order -- including painting the same
 * cell twice (the retail mesh darkens the backdrop by 6 %, so a cell that read
 * the canvas instead of the backdrop would get darker every time).
 *
 * This test drives the exact same inline code the firmware uses
 * (main/dg_glass_paint.h, no LVGL) and compares an incrementally updated canvas
 * against a full recompute after every step.
 *
 *   gcc -O2 -o glass_paint_test.exe src/glass_paint_test.c -I../main
 *   ./glass_paint_test.exe
 *
 * Geometry is duplicated from digimon_app.c's DG_* block on purpose: the test
 * asserts the algorithm, not the layout.  If the pitch changes, both sides
 * change and the test still holds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dg_backdrop.h"        /* dg_backdrop_pal[256] + dg_backdrop[35136] */
#include "dg_glass_paint.h"

/* W/H are written out instead of using DG_BACKDROP_W/H: that header uses
 * DG_BACKDROP_H for *both* its include guard and the height (a pre-existing
 * wart, harmless because the height wins), so a re-include would be noisy. */
#define W       192
#define H       183
#define PITCH   6
#define MAT_Y   (28 + (2 * PITCH) + 3)      /* DG_MAT_Y at pitch 6 */
#define LCD_W   32
#define LCD_H   16

/* GLASS_TONE[1] -- the shipped tone. */
#define TONE_BASE 0x950D
#define TONE_GAP  0xA56F
#define TONE_LIT  0x29C5

static uint16_t buf_inc[W * H];     /* evolved by single-cell paints */
static uint16_t buf_ref[W * H];     /* recomputed from scratch every step */
static uint8_t  cur[LCD_H][LCD_W];

static void full_repaint(dgp_glass_t *g, uint16_t *buf, int style)
{
    g->buf = buf;
    g->style = style;
    dgp_fill(g);
    dgp_top_line(g);
    for (int r = 0; r < LCD_H; r++) {
        for (int c = 0; c < LCD_W; c++) dgp_cell(g, r, c, cur[r][c]);
    }
}

static int failures;

static void check(const char *what, int style, int step)
{
    if (memcmp(buf_inc, buf_ref, sizeof buf_inc) == 0) return;
    /* point at the first differing pixel, it is always more useful than "diff" */
    for (int i = 0; i < W * H; i++) {
        if (buf_inc[i] != buf_ref[i]) {
            printf("  FAIL %s style=%d step=%d: pixel %d (x=%d y=%d) inc=%04X ref=%04X\n",
                   what, style, step, i, i % W, i / W, buf_inc[i], buf_ref[i]);
            break;
        }
    }
    failures++;
}

static unsigned rnd(unsigned *s)
{
    unsigned x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

int main(void)
{
    unsigned seed = 0x5EED1234u;
    printf("glass paint test: %dx%d, pitch %d, matrix at y=%d\n", W, H, PITCH, MAT_Y);

    for (int style = 0; style <= 1; style++) {
        dgp_glass_t g_inc = {
            .buf = buf_inc, .w = W, .h = H, .mat_y = MAT_Y, .pitch = PITCH,
            .style = style, .base = TONE_BASE, .gap = TONE_GAP,
            .lit = dim565(TONE_LIT, 15, 16),
            /* classic must never read the backdrop: pass NULL to prove it */
            .backdrop = style ? dg_backdrop : NULL,
            .pal = style ? dg_backdrop_pal : NULL,
        };
        dgp_glass_t g_ref = g_inc;
        g_ref.buf = buf_ref;

        memset(cur, 0, sizeof cur);
        full_repaint(&g_inc, buf_inc, style);       /* first pass: full repaint */
        full_repaint(&g_ref, buf_ref, style);
        check("initial full repaint", style, 0);

        /* 2000 random steps, 1..4 cells flipping each time, biased towards
         * small moves like the pet walking a dot at a time */
        for (int step = 1; step <= 2000; step++) {
            int n = 1 + (int)(rnd(&seed) & 3);
            for (int k = 0; k < n; k++) {
                int r = (int)(rnd(&seed) % LCD_H);
                int c = (int)(rnd(&seed) % LCD_W);
                cur[r][c] ^= 1u;
                dgp_cell(&g_inc, r, c, cur[r][c]);  /* dirty path, one cell */
            }
            full_repaint(&g_ref, buf_ref, style);   /* ground truth */
            check("dirty step", style, step);
            if (failures) break;
        }

        /* idempotence: painting the same cell twice must not change a byte
         * (this is the retail 6 %-mesh double-darkening trap) */
        if (!failures) {
            static uint16_t once[W * H];
            memcpy(once, buf_inc, sizeof once);
            for (int r = 0; r < LCD_H; r++) {
                for (int c = 0; c < LCD_W; c++) dgp_cell(&g_inc, r, c, cur[r][c]);
            }
            if (memcmp(once, buf_inc, sizeof once) != 0) {
                printf("  FAIL style=%d: repainting every cell changed pixels\n", style);
                failures++;
            }
        }

        /* The closing line above row 0 must be untouched by cell painting:
         * identical to the full recompute, and in retail still the *dimmed*
         * backdrop (proving the 6 % mesh really is applied there). */
        if (!failures) {
            const uint16_t *top_inc = &buf_inc[(MAT_Y - 1) * W];
            const uint16_t *top_ref = &buf_ref[(MAT_Y - 1) * W];
            int differs = 0, raw = 0;
            for (int x = 0; x < W; x++) {
                if (top_inc[x] != top_ref[x]) differs++;
                if (style && top_inc[x] == dg_backdrop_pal[dg_backdrop[(MAT_Y - 1) * W + x]]) raw++;
            }
            if (differs || raw) {
                printf("  FAIL style=%d: closing line differs in %d px (%d undimmed)\n",
                       style, differs, raw);
                failures++;
            }
        }
        printf("  style %d: %s\n", style, failures ? "FAILED" : "ok (2000 dirty steps)");
    }

    printf(failures ? "GLASS PAINT: FAIL\n" : "GLASS PAINT: PASS\n");
    return failures ? 1 : 0;
}
