/* battle_trace4: at mode 5 sub 1, pulse K0.3 and see if the ROM reacts
 * (sub mode change, P0-P3 writes, different screen).  Sweeps pulse widths. */
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
    printf("%-26s mode=%X sub=%X 0B=%X 0C=%X |", tag, dg_ram(0x009), dg_ram(0x00A),
           dg_ram(0x00B), dg_ram(0x00C));
    for (int i = 0; i < 0x80; i++) if (g_io_hits[i]) printf(" %03X:%lu", 0xF00 + i, g_io_hits[i]);
    printf("\n");
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
    dg_set_button(2, 1); run_ticks(6554); dg_set_button(2, 0);
    run_ticks(32768);
    printf("after B: mode=%X sub=%X 0C=%X\n\n", dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00C));

    /* sweep K0.3 low pulses */
    const uint32_t holds[] = {33, 66, 130, 260, 650, 1300, 3300, 6600};
    for (int i = 0; i < 8; i++) {
        clear();
        dg_set_button(8, 1); run_ticks(holds[i]);
        dg_set_button(8, 0); run_ticks(32768 * 2);
        char t[64]; snprintf(t, sizeof t, "K0.3 low %u ms", (unsigned)(holds[i] * 1000 / 32768));
        report(t);
        if (dg_ram(0x009) != 5) { printf("  !! mode changed, stop sweep\n"); break; }
    }

    /* also try a pause-and-pulse while holding the middle button (initiator?) */
    printf("\n### middle held + K0.3 pulse train\n");
    dg_set_button(2, 1);
    for (int i = 0; i < 20; i++) { dg_set_button(8, 1); run_ticks(650); dg_set_button(8, 0); run_ticks(650); }
    dg_set_button(2, 0);
    run_ticks(32768);
    report("after pulse train");

    printf("\ninstr=%u\n", dg_instr_count());
    return 0;
}
