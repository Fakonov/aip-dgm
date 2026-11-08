/* battle_trace2: is K0.3 (mask 8) the link data pin, and can the ROM be pushed
 * into battle mode (mode 5) by driving it?  Link core copy is instrumented. */
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
static void report(const char *tag, int show_events)
{
    printf("---- %s  mode=%X sub=%X 0B=%X 0C=%X idx=%X ----\n", tag,
           dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00B), dg_ram(0x00C), dg_ram(0x015));
    for (int i = 0; i < 0x80; i++) {
        if (!g_io_hits[i]) continue;
        printf("   %03X x%lu\n", 0xF00 + i, g_io_hits[i]);
    }
    if (show_events) {
        for (unsigned i = 0; i < g_io_ev_n; i++)
            printf("   #%u %03X %s val=%X pc=%03X instr=%u\n", i, g_io_ev[i].addr,
                   g_io_ev[i].rw ? "W" : "R", g_io_ev[i].val, g_io_ev[i].pc, g_io_ev[i].instr);
    }
    fflush(stdout);
}
static void screen15(void)
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
    printf("after hatch: mode=%X sub=%X\n\n", dg_ram(0x009), dg_ram(0x00A));

    /* control: A press, read values of K0 while pressed */
    clear();
    dg_set_button(4, 1); run_ticks(6554); dg_set_button(4, 0); run_ticks(6554);
    report("A press (mask 4) - control", 0);

    /* back home and try K0.3 as an incoming line */
    run_ticks(32768 * 2);
    clear();
    dg_set_button(8, 1); run_ticks(20000); dg_set_button(8, 0); run_ticks(32768 * 2);
    report("K0.3 held low 600 ms at home", 0);

    run_ticks(32768 * 2);
    clear();
    printf("\n### middle press with K0.3 held low\n");
    dg_set_button(8, 1); run_ticks(3277);
    dg_set_button(2, 1); run_ticks(6554); dg_set_button(2, 0);
    run_ticks(32768 * 3); dg_set_button(8, 0); run_ticks(3277);
    report("middle + K0.3 low", 1);

    printf("\n=== screen now ===\n");
    screen15();
    printf("\ninstr=%u\n", dg_instr_count());
    return 0;
}
