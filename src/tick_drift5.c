/* tick_drift5: A/B the ROM's own wall clock (0x01A..0x01F) against emulated
 * time, old pacing vs the fixed pacing.  Also tries to get the ROM past the
 * clock-set page by clearing M(0x02) (see HANDOFF:1014/1027, HALNOTE:428).
 *
 *   mode 0 = old: target = now + 655 every frame (what 2.1.4 shipped)
 *   mode 1 = new: fractional carry + overshoot carry-back
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "digimon_core.h"

#define OSC1_HZ    32768
#define FRAME_MS   20
#define STEP_GUARD 40000

static uint8_t rom[65536];
static uint32_t s_frame_frac;
static int32_t  s_frame_over;

static int dig(uint8_t v) { return (v & 0x0f) + 10 * ((v >> 4) & 0x0f); }
static int c_sec(void)  { return dig(dg_ram(0x01a)) + 10 * dig(dg_ram(0x01b)); }
static int c_min(void)  { return dig(dg_ram(0x01c)) + 10 * dig(dg_ram(0x01d)); }
static int c_hour(void) { return dig(dg_ram(0x01e)); }
static int c_pm(void)   { return dg_ram(0x01f) & 0x0f; }
static long clock_s(void) { return (long)(c_hour() % 12 + (c_pm() ? 12 : 0)) * 3600 + c_min() * 60 + c_sec(); }

static uint32_t budget_new(void)
{
    uint32_t whole = (uint32_t)(OSC1_HZ * FRAME_MS / 1000);
    s_frame_frac += (uint32_t)((OSC1_HZ * FRAME_MS) % 1000);
    if (s_frame_frac >= 1000) { s_frame_frac -= 1000; whole++; }
    if (s_frame_over > (int32_t)whole) s_frame_over = (int32_t)whole;
    if (s_frame_over < -(int32_t)whole) s_frame_over = -(int32_t)whole;
    int32_t owed = (int32_t)whole - s_frame_over;
    if (owed < 1) owed = 1;
    return (uint32_t)owed;
}

static void run_seconds(int seconds, int mode, long *rom_out, double *ticks_out)
{
    long prev = clock_s();
    long total = 0;
    uint32_t t0 = dg_osc1_ticks();
    for (int s = 0; s < seconds; s++) {
        for (int f = 0; f < 50; f++) {
            uint32_t b = (mode == 0) ? 655u : budget_new();
            uint32_t target = dg_osc1_ticks() + b;
            int guard = 0;
            while ((int32_t)(dg_osc1_ticks() - target) < 0 && guard++ < STEP_GUARD) dg_step();
            s_frame_over = (int32_t)(dg_osc1_ticks() - target);
        }
        long now = clock_s();
        if (now < prev) now += 12 * 3600;
        total += now - prev;
        prev = now;
    }
    *rom_out = total;
    *ticks_out = (double)(dg_osc1_ticks() - t0) / seconds;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: tick_drift5 <rom> [seconds]\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 1; }
    size_t n = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    int seconds = (argc >= 3) ? atoi(argv[2]) : 900;

    dg_init(rom, (uint32_t)n);
    for (int i = 0; i < 400000; i++) dg_step();

    printf("state @boot (no keys): M(02)=%02X M(03)=%02X  clock %02d:%02d:%02d %s\n",
           dg_ram(0x002), dg_ram(0x003), c_hour(), c_min(), c_sec(), c_pm() ? "PM" : "AM");

    /* try to leave the clock-set page */
    dg_ram_write(0x002, 0x00);
    for (int i = 0; i < 200000; i++) dg_step();
    printf("after M(02)=0        : M(02)=%02X M(03)=%02X  clock %02d:%02d:%02d %s\n",
           dg_ram(0x002), dg_ram(0x003), c_hour(), c_min(), c_sec(), c_pm() ? "PM" : "AM");

    for (int mode = 0; mode <= 1; mode++) {
        long rom_s = 0; double tps = 0;
        run_seconds(120, mode, &rom_s, &tps);          /* settle in this mode */
        run_seconds(seconds, mode, &rom_s, &tps);
        printf("mode %d (%s): ticks/s=%.3f  ROM clock +%ld s over %d s -> ratio %.4f  (%+.1f s/day)\n",
               mode, mode ? "carry" : "old  ", tps, rom_s, seconds,
               (double)rom_s / seconds, ((double)rom_s / seconds - 1.0) * 86400.0);
        fflush(stdout);
    }
    return 0;
}
