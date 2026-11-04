/* dumpframe.c -- run the ROM to a hatched state and print the 32x16 frame as
 * ASCII, so mockups can use a real screen instead of a hand-made sprite. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"

static uint8_t rom[65536];
static uint8_t pic[DG_LCD_H][DG_LCD_W];

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) dg_step();
}

static void grab(void)
{
    const uint8_t *v = dg_vram();
    uint32_t len = dg_vram_len();
    for (int r = 0; r < DG_LCD_H; r++)
        for (int c = 0; c < DG_LCD_W; c++) {
            uint16_t cell = dg_segmap[r][c];
            uint8_t b = (uint8_t)(cell & 0xFF), bit = (uint8_t)(cell >> 8);
            pic[r][c] = (b < len && ((v[b] >> bit) & 1u)) ? 1 : 0;
        }
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);

    const uint32_t SEC = 32768;
    run_ticks(SEC * 2);
    run_ticks(SEC * 400);          /* let it hatch and grow a bit */

    /* skip a dozen frames so the animation lands mid-cycle */
    for (int i = 0; i < 12; i++) run_ticks(SEC / 4);
    grab();
    for (int r = 0; r < DG_LCD_H; r++) {
        for (int c = 0; c < DG_LCD_W; c++) putchar(pic[r][c] ? '#' : '.');
        putchar('\n');
    }
    return 0;
}
