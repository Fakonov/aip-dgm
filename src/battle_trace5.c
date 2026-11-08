/* battle_trace5: the link line is P2.0 (BrickEmuPy CON_DGM).  Find the ROM
 * state that actually touches P2: walk mode 5 and press B up to 4 times. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "trace_core_link.c"
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
static void boot_and_hatch(void)
{
    const uint32_t SEC = 32768;
    run_ticks(SEC * 2);
    press(1, 8192, 8192); press(4, 4096, 4096); press(1, 4096, 4096); press(2, 8192, 8192);
    press(4, 8192, 8192); press(1, 8192, 8192); press(2, 8192, 8192);
    run_ticks(SEC * 6 * 60);
}
static void clear(void) { g_io_ev_n = 0; memset(g_io_hits, 0, sizeof g_io_hits); }
static void report(const char *tag)
{
    printf("%-22s mode=%X sub=%X 0B=%X 0C=%X |", tag, dg_ram(0x009), dg_ram(0x00A),
           dg_ram(0x00B), dg_ram(0x00C));
    for (int i = 0; i < 0x80; i++) if (g_io_hits[i]) printf(" %03X:%lu", 0xF00 + i, g_io_hits[i]);
    printf("\n");
    fflush(stdout);
}
static void screen(void)
{
    const uint8_t *v = dg_vram(); uint32_t len = dg_vram_len();
    for (int r = 0; r < DG_LCD_H; r++) {
        for (int c = 0; c < DG_LCD_W; c++) {
            uint16_t cell = dg_segmap[r][c]; uint8_t b = cell & 0xFF, bit = cell >> 8;
            putchar((b < len && ((v[b] >> bit) & 1)) ? '#' : '.');
        }
        putchar('\n');
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open ROM\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f); fclose(f);
    dg_init(rom, (uint32_t)n);
    boot_and_hatch();
    for (int i = 0; i < 4; i++) press(4, 20000, 30000);
    printf("icon page mode=%X\n", dg_ram(0x009));

    for (int step = 0; step < 5; step++) {
        clear();
        press(2, 20000, 40000);
        run_ticks(32768 * 2);
        char t[48]; snprintf(t, sizeof t, "B press #%d", step + 1);
        report(t);
        if (step == 0) { printf("--- screen after first B ---\n"); screen(); }
        if (dg_ram(0x009) == 0) { printf("(back home)\n"); break; }
    }
    printf("\ninstr=%u\n", dg_instr_count());
    return 0;
}
