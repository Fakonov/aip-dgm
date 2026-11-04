/* evo_probe.c -- does the ROM evolve when the stage clock reaches
 * next_evolution_at, and what happens if the clock steps over it?
 *
 * A: park stage 2 minutes before next, run, watch species_slot.
 * B: park stage 5 minutes PAST next, run, watch species_slot.
 *
 * RAM layout (page-0 nibbles, from the 1997 disassembly appendix B):
 *   stage 46/47 sec (BCD)  48/49 min (BCD)  4A/4B hours (8-bit, low/high)
 *   next  4C/4D min (BCD)  4E/4F hours (8-bit, low/high)
 *
 * gcc -O2 -o evo_probe.exe evo_probe.c digimon_core.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];
static const uint32_t SEC = 32768;

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) dg_step();
}

static void press(uint8_t m, uint32_t hold)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(hold);
}

/* hours are 8-bit binary split across two nibbles; minutes are BCD */
static int stage_min(void) { return dg_ram(0x49) * 10 + dg_ram(0x48); }
static int stage_hour(void) { return dg_ram(0x4A) | (dg_ram(0x4B) << 4); }
static int next_min(void) { return dg_ram(0x4D) * 10 + dg_ram(0x4C); }
static int next_hour(void) { return dg_ram(0x4E) | (dg_ram(0x4F) << 4); }

static void write2(int lo, int hi, int value)   /* BCD minutes */
{
    dg_ram_write(lo, (uint8_t)(value % 10));
    dg_ram_write(hi, (uint8_t)((value / 10) % 10));
}

static void write_hours(int lo, int hi, int hours)  /* 8-bit hours */
{
    dg_ram_write(lo, (uint8_t)(hours & 0xF));
    dg_ram_write(hi, (uint8_t)((hours >> 4) & 0xF));
}

static void set_next(int minutes)
{
    int h = minutes / 60, m = minutes % 60;
    write2(0x4C, 0x4D, m);
    write_hours(0x4E, 0x4F, h);
}

static void set_stage(int minutes)
{
    int h = minutes / 60, m = minutes % 60;
    write2(0x48, 0x49, m);
    write_hours(0x4A, 0x4B, h);
}

static void dump(const char *tag, long secs)
{
    printf("%-14s t=%4lds slot=%X mode=%X stage=%d:%02d next=%d:%02d "
           "care=%X train=%X%X over=%X sleep=%X\n",
           tag, secs, dg_ram(0x00B), dg_ram(0x009),
           stage_hour(), stage_min(), next_hour(), next_min(),
           dg_ram(0x031), dg_ram(0x035), dg_ram(0x034),
           dg_ram(0x032), dg_ram(0x033));
    fflush(stdout);
}

static void boot_to_hatch(void)
{
    dg_reset();
    run_ticks(SEC * 2);
    press(1, 8192); press(4, 4096); press(1, 4096); press(2, 8192);
    if (dg_ram(0x009) == 1) { press(4, 8192); press(1, 8192); press(2, 8192); }
    for (int i = 0; i < 200 && dg_ram(0x00B) == 0; i++) run_ticks(SEC * 30);
}

/* run `secs` game seconds, report the first second the slot changes */
static int watch(long secs, int from_slot)
{
    int changed_at = -1;
    for (long s = 0; s < secs; s++) {
        run_ticks(SEC);
        if (dg_ram(0x00B) != from_slot && changed_at < 0) {
            changed_at = (int)s;
            dump("CHANGED", s);
        }
        if (s % 300 == 299) dump("  tick", s);
    }
    return changed_at;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no rom\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);

    boot_to_hatch();
    printf("hatched: slot=%X stage=%d:%02d next=%d:%02d\n\n",
           dg_ram(0x00B), stage_hour(), stage_min(), next_hour(), next_min());

    /* ---------- A: 2 minutes before next ---------- */
    int stage0 = stage_hour() * 60 + stage_min();
    int next0 = next_hour() * 60 + next_min();
    printf("== A: natural next is %d min, stage is %d min ==\n", next0, stage0);
    set_stage(next0 - 2);
    putchar('\n');
    dump("parked -2", 0);
    int a = watch(400, dg_ram(0x00B));
    dump("A result", 400);
    printf("A: slot change at %d s -> %s\n\n", a,
           a >= 0 ? "EVOLVED" : "NO EVOLUTION");

    /* ---------- B: 5 minutes past next ---------- */
    boot_to_hatch();
    int n2 = next_hour() * 60 + next_min();
    set_stage(n2 + 5);
    putchar('\n');
    dump("parked +5", 0);
    int b = watch(400, dg_ram(0x00B));
    dump("B result", 400);
    printf("B: slot change at %d s -> %s\n", b,
           b >= 0 ? "EVOLVED" : "NO EVOLUTION");
    return 0;
}
