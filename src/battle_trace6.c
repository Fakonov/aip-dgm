/* battle_trace6: grow to Child (slot 3) the same way evo_probe does, then try
 * the battle icon and watch P2 (the D-Link line, BrickEmuPy "CON_DGM": P2.0). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "trace_core_link.c"
#include "digimon_segmap.h"

static uint8_t rom[65536];
static const uint32_t SEC = 32768;

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n; long g = 0;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) { dg_step(); if (++g > 900000000L) return; }
}
static void press(uint8_t m, uint32_t hold, uint32_t gap)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(gap);
}
static void clear(void) { g_io_ev_n = 0; memset(g_io_hits, 0, sizeof g_io_hits); }
static void report(const char *tag)
{
    printf("%-24s mode=%X sub=%X slot=%X 0C=%X |", tag, dg_ram(0x009), dg_ram(0x00A),
           dg_ram(0x00B), dg_ram(0x00C));
    for (int i = 0; i < 0x80; i++) if (g_io_hits[i]) printf(" %03X:%lu", 0xF00 + i, g_io_hits[i]);
    printf("\n");
    fflush(stdout);
}
static void boot_to_hatch(void)
{
    dg_reset();
    run_ticks(SEC * 2);
    press(1, 8192, 8192); press(4, 4096, 4096); press(1, 4096, 4096); press(2, 8192, 8192);
    if (dg_ram(0x009) == 1) { press(4, 8192, 8192); press(1, 8192, 8192); press(2, 8192, 8192); }
    for (int i = 0; i < 200 && dg_ram(0x00B) == 0; i++) run_ticks(SEC * 30);
}
static int next_min_total(void) { return (dg_ram(0x4E) | (dg_ram(0x4F) << 4)) * 60 + dg_ram(0x4D) * 10 + dg_ram(0x4C); }
static void set_stage(int minutes)
{
    int h = minutes / 60, m = minutes % 60;
    dg_ram_write(0x48, (uint8_t)(m % 10));
    dg_ram_write(0x49, (uint8_t)((m / 10) % 10));
    dg_ram_write(0x4A, (uint8_t)(h & 0xF));
    dg_ram_write(0x4B, (uint8_t)((h >> 4) & 0xF));
}
static void evolve_once(int care)
{
    dg_ram_write(0x031, (uint8_t)care);
    dg_ram_write(0x032, 0); dg_ram_write(0x033, 0);
    dg_ram_write(0x034, 0); dg_ram_write(0x035, 0);
    int from = dg_ram(0x00B);
    set_stage(next_min_total() - 2);
    for (int s = 0; s < 200 && dg_ram(0x00B) == from; s++) run_ticks(SEC);
    for (int s = 0; s < 120 && dg_ram(0x009) != 0; s++) run_ticks(SEC);
    printf("evolve: slot %X -> %X (care=%d)\n", from, dg_ram(0x00B), care);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open ROM\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f); fclose(f);
    dg_init(rom, (uint32_t)n);
    boot_to_hatch();
    evolve_once(0);
    evolve_once(0);
    printf("now slot=%X mode=%X\n\n", dg_ram(0x00B), dg_ram(0x009));

    clear();
    for (int i = 0; i < 4; i++) press(4, 20000, 30000);
    report("icon page (A x4)");

    for (int step = 0; step < 2; step++) {
        clear();
        press(2, 20000, 40000);
        run_ticks(SEC * 2);
        char t[48]; snprintf(t, sizeof t, "B press #%d", step + 1);
        report(t);
        if (g_io_hits[0x62]) {
            printf("  >>> P2 touched! first events:\n");
            for (unsigned i = 0; i < g_io_ev_n && i < 24; i++)
                printf("   %03X %s val=%X pc=%03X instr=%u\n", g_io_ev[i].addr,
                       g_io_ev[i].rw ? "W" : "R", g_io_ev[i].val, g_io_ev[i].pc, g_io_ev[i].instr);
        }
    }
    printf("\ninstr=%u\n", dg_instr_count());
    return 0;
}
