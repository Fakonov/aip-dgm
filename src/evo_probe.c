/* evo_probe.c -- verify the ROM's "evolution window" semantics on V1/V2/V3.
 *
 * Round 61 established, from the V1 disassembly, that 0x0596 compares the
 * four stage nibbles M(48..4B) with the four next-evolution nibbles
 * M(4C..4F) one by one and only evolves when they are *exactly* equal:
 *
 *   stage 48/49 min (BCD)  4A/4B hours (8-bit, low/high)
 *   next  4C/4D min (BCD)  4E/4F hours (8-bit, low/high)
 *
 * Crossing that minute without landing on it loses the evolution forever,
 * and the picker writes 0xFF into the next-evolution hour as a sentinel.
 *
 * This probe runs the same two experiments at two different transitions:
 *
 *   A1  baby I  -> baby II   (slot 1 -> 2, no care input)
 *   A2  baby II -> child     (slot 2 -> 3/4, the 0x0D10 care table)
 *   B1/B2 the same two transitions parked 5 minutes PAST next, which must
 *         never evolve.
 *
 * A2 uses two care values (0 and 6) so the care-based picker is exercised on
 * both sides of its first comparison.
 *
 *   gcc -O2 -o evo_probe.exe evo_probe.c digimon_core.c
 *   ./evo_probe.exe <rom.bin> [label]
 *
 * Output is intentionally line-oriented so logs from three ROMs can be
 * concatenated and diffed.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];
static uint8_t snap[4096];
static const uint32_t SEC = 32768;
static const char *g_label = "ROM";

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
static int next_none(void) { return next_hour() == 0xFF; }

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

static void set_stage(int minutes)
{
    int h = minutes / 60, m = minutes % 60;
    write2(0x48, 0x49, m);
    write_hours(0x4A, 0x4B, h);
}

static int left_minutes(void)
{
    if (next_none()) return -1;
    return next_hour() * 60 + next_min() - (stage_hour() * 60 + stage_min());
}

static void dump(const char *tag, long secs)
{
    printf("%-4s %-10s t=%4lds slot=%X mode=%X stage=%d:%02d next=",
           g_label, tag, secs, dg_ram(0x00B), dg_ram(0x009),
           stage_hour(), stage_min());
    if (next_none()) printf("none");
    else             printf("%d:%02d", next_hour(), next_min());
    printf(" left=%dmin care=%X train=%X%X over=%X sleep=%X raw4C..4F=%X%X%X%X\n",
           left_minutes(), dg_ram(0x031), dg_ram(0x035), dg_ram(0x034),
           dg_ram(0x032), dg_ram(0x033),
           dg_ram(0x4C), dg_ram(0x4D), dg_ram(0x4E), dg_ram(0x4F));
    fflush(stdout);
}

static void boot_to_hatch(void)
{
    dg_reset();
    run_ticks(SEC * 2);
    press(1, 8192); press(4, 4096); press(1, 4096); press(2, 8192);
    if (dg_ram(0x009) == 1) { press(4, 8192); press(1, 8192); press(2, 8192); }
    if (dg_ram(0x00B) == 0) {
        printf("%s egg defaults: M66=%X M67=%X M68=%X M69=%X M77=%X\n",
               g_label, dg_ram(0x66), dg_ram(0x67), dg_ram(0x68),
               dg_ram(0x69), dg_ram(0x77));
        dump("egg", 0);
    }
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
        if (s % 120 == 119) dump("tick", s);
    }
    return changed_at;
}

/* after an evolution the ROM sits in mode 9 (animation); wait for mode 0 */
static void wait_mode0(long max_secs)
{
    for (long s = 0; s < max_secs && dg_ram(0x009) != 0; s++) run_ticks(SEC);
}

static int a_leg(int from_slot, int care)
{
    int next0 = next_hour() * 60 + next_min();
    dg_ram_write(0x031, (uint8_t)care);
    dg_ram_write(0x032, 0);
    dg_ram_write(0x033, 0);
    dg_ram_write(0x034, 0);
    dg_ram_write(0x035, 0);
    set_stage(next0 - 2);
    dump("parked-2", 0);
    int at = watch(400, dg_ram(0x00B));
    dump("A-result", 400);
    printf("%s A slot=%X care=%d : slot change at %d s -> %s\n\n",
           g_label, from_slot, care, at, at >= 0 ? "EVOLVED" : "NO EVOLUTION");
    return at;
}

static int b_leg(int from_slot, int care)
{
    int next0 = next_hour() * 60 + next_min();
    dg_ram_write(0x031, (uint8_t)care);
    dg_ram_write(0x032, 0);
    dg_ram_write(0x033, 0);
    dg_ram_write(0x034, 0);
    dg_ram_write(0x035, 0);
    set_stage(next0 + 5);
    dump("parked+5", 0);
    int at = watch(400, dg_ram(0x00B));
    dump("B-result", 400);
    printf("%s B slot=%X care=%d : slot change at %d s -> %s\n\n",
           g_label, from_slot, care, at, at >= 0 ? "EVOLVED" : "NO EVOLUTION");
    return at;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: evo_probe <rom.bin> [label]\n"); return 1; }
    if (argc > 2) g_label = argv[2];
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no rom\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);

    printf("### %s : %s (%u bytes)\n", g_label, argv[1], (unsigned)n);
    boot_to_hatch();
    printf("%s hatched: slot=%X stage=%d:%02d next=%d:%02d (%d min)\n\n",
           g_label, dg_ram(0x00B), stage_hour(), stage_min(),
           next_hour(), next_min(), next_hour() * 60 + next_min());

    /* ---- A1: baby I -> baby II ---- */
    a_leg(1, 0);
    wait_mode0(120);
    if (dg_ram(0x00B) != 2) {
        printf("%s A1 ended in slot %X, expected 2 -- stopping\n", g_label, dg_ram(0x00B));
        return 2;
    }
    dump("post-A1", 0);

    /* snapshot the healthy slot-2 state so B2 can start from the same place */
    uint32_t ss = dg_state_size();
    if (ss > sizeof snap) ss = sizeof snap;
    dg_state_save(snap);

    /* ---- A2: slot 2 -> child, care 0 and care 6 ---- */
    for (int care = 0; care <= 6; care += 6) {
        dg_state_load(snap, ss);
        run_ticks(SEC * 2);
        a_leg(2, care);
        wait_mode0(120);
        dump("post-A2", 0);
    }

    /* ---- B1: fresh boot, park 5 past the first next ---- */
    boot_to_hatch();
    dump("hatched", 0);
    b_leg(1, 0);

    /* ---- B2: force a healthy slot-2 state, then park past its next ---- */
    dg_state_load(snap, ss);
    run_ticks(SEC * 2);
    for (int care = 0; care <= 6; care += 6) {
        dg_state_load(snap, ss);
        run_ticks(SEC * 2);
        b_leg(2, care);
    }

    printf("%s DONE\n", g_label);
    return 0;
}
