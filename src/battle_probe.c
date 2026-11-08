/* battle_probe: can the ROM reach battle mode (mode 5) on its own?
 * Boots, hatches, presses the middle button (mask 2), then samples mode/RAM and
 * dumps the LCD dot matrix every half second of emulated time for 20 s.
 * Also tries the "responder" angle: fake an incoming start pulse is NOT
 * possible yet (no port injection API), so this only tests the local path. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"

static uint8_t rom[65536];

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n; long g = 0;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) { dg_step(); if (++g > 400000000L) return; }
}
static void press(uint8_t m, uint32_t hold, uint32_t gap)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(gap);
}
static void screen(const char *tag)
{
    const uint8_t *v = dg_vram(); uint32_t len = dg_vram_len();
    printf("---- %s  mode=%X sub=%X idx=%X 0B=%X 0C=%X ----\n", tag,
           dg_ram(0x009), dg_ram(0x00A), dg_ram(0x015), dg_ram(0x00B), dg_ram(0x00C));
    for (int r = 0; r < DG_LCD_H; r++) {
        for (int c = 0; c < DG_LCD_W; c++) {
            uint16_t cell = dg_segmap[r][c]; uint8_t b = cell & 0xFF, bit = cell >> 8;
            putchar((b < len && ((v[b] >> bit) & 1)) ? '#' : '.');
        }
        putchar('\n');
    }
    fflush(stdout);
}
static void boot_and_hatch(void)
{
    const uint32_t SEC = 32768;
    run_ticks(SEC * 2);
    press(1, 8192, 8192); press(4, 4096, 4096); press(1, 4096, 4096); press(2, 8192, 8192);
    press(4, 8192, 8192); press(1, 8192, 8192); press(2, 8192, 8192);
    run_ticks(SEC * 6 * 60);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open ROM\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f); fclose(f);
    dg_init(rom, (uint32_t)n);
    boot_and_hatch();
    screen("hatched / home");

    const uint32_t SEC = 32768;
    printf("\n### press middle button (mask 2), hold 200 ms\n");
    dg_set_button(2, 1); run_ticks(6554);
    dg_set_button(2, 0);
    for (int i = 0; i < 10; i++) {
        run_ticks(SEC / 2);
        char t[64]; snprintf(t, sizeof t, "after middle +%d ms", (i + 1) * 500);
        screen(t);
    }
    printf("\ninstr=%u\n", dg_instr_count());
    return 0;
}
