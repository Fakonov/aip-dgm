/* battle_trace: which I/O locations does the ROM actually touch, and when?
 * Links against an instrumented copy of the core (trace_core_link.c) that
 * records every 0xF00..0xF7F access with PC + instruction count. */
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
static void hist(const char *tag, int only_ports)
{
    printf("---- %s (mode=%X sub=%X 0B=%X idx=%X) ----\n", tag,
           dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00B), dg_ram(0x015));
    for (int i = 0; i < 0x80; i++) {
        if (!g_io_hits[i]) continue;
        int a = 0xF00 + i;
        if (only_ports && !(a >= 0xF50 && a <= 0xF63)) continue;
        printf("  %03X x%lu\n", a, g_io_hits[i]);
    }
    fflush(stdout);
}
static void last_events(int n)
{
    unsigned start = g_io_ev_n > (unsigned)n ? g_io_ev_n - n : 0;
    for (unsigned i = start; i < g_io_ev_n; i++)
        printf("  #%u addr=%03X %s val=%X pc=%03X instr=%u\n", i,
               g_io_ev[i].addr, g_io_ev[i].rw ? "W" : "R",
               g_io_ev[i].val, g_io_ev[i].pc, g_io_ev[i].instr);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open ROM\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f); fclose(f);
    dg_init(rom, (uint32_t)n);
    boot_and_hatch();

    clear();
    run_ticks(32768 * 3);
    hist("idle 3 s (home)", 0);

    clear();
    press(4, 20000, 40000);          /* A: step to mode 2 */
    run_ticks(32768);
    hist("after A (mode 2 icon page)", 0);

    clear();
    dg_set_button(2, 1); run_ticks(6554); dg_set_button(2, 0);
    run_ticks(32768 * 3);
    hist("3 s after middle press", 0);
    printf("  -- last 40 I/O events --\n");
    last_events(40);

    printf("\ninstr=%u ev=%u\n", dg_instr_count(), g_io_ev_n);
    return 0;
}
