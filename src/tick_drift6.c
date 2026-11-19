/* tick_drift6: drive the ROM past its clock-set page with real key edges, then
 * A/B the clock drift (old 655/frame vs carry-corrected pacing).
 *
 * Key map (digimon_app.c:847-853):  UP = A = mask 4, DOWN = B = mask 2, OK = C = mask 1
 * The ROM samples K0 at ~2 Hz and needs ~120 ms hold, so keys are held 250 ms.
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

static void frame(int mode)
{
    uint32_t b;
    if (mode == 0) {
        b = 655;
    } else {
        uint32_t whole = (uint32_t)(OSC1_HZ * FRAME_MS / 1000);
        s_frame_frac += (uint32_t)((OSC1_HZ * FRAME_MS) % 1000);
        if (s_frame_frac >= 1000) { s_frame_frac -= 1000; whole++; }
        if (s_frame_over > (int32_t)whole) s_frame_over = (int32_t)whole;
        if (s_frame_over < -(int32_t)whole) s_frame_over = -(int32_t)whole;
        int32_t owed = (int32_t)whole - s_frame_over;
        if (owed < 1) owed = 1;
        b = (uint32_t)owed;
    }
    uint32_t target = dg_osc1_ticks() + b;
    int guard = 0;
    while ((int32_t)(dg_osc1_ticks() - target) < 0 && guard++ < STEP_GUARD) dg_step();
    s_frame_over = (int32_t)(dg_osc1_ticks() - target);
}

static int dig(uint8_t v) { return (v & 0x0f) + 10 * ((v >> 4) & 0x0f); }
static int c_sec(void)  { return dig(dg_ram(0x01a)) + 10 * dig(dg_ram(0x01b)); }
static int c_min(void)  { return dig(dg_ram(0x01c)) + 10 * dig(dg_ram(0x01d)); }
static int c_hour(void) { return dig(dg_ram(0x01e)); }
static int c_pm(void)   { return dg_ram(0x01f) & 0x0f; }
static long clock_s(void) { return (long)(c_hour() % 12 + (c_pm() ? 12 : 0)) * 3600 + c_min() * 60 + c_sec(); }

static void key(uint8_t mask, int hold_frames, int tail_frames)
{
    dg_set_button(mask, 1);
    for (int i = 0; i < hold_frames; i++) frame(0);
    dg_set_button(mask, 0);
    for (int i = 0; i < tail_frames; i++) frame(0);
}

static void show(const char *tag)
{
    printf("%-22s M(02)=%02X M(03)=%02X  %02d:%02d:%02d %s  ticks=%u\n",
           tag, dg_ram(0x002), dg_ram(0x003),
           c_hour(), c_min(), c_sec(), c_pm() ? "PM" : "AM", dg_osc1_ticks());
    fflush(stdout);
}

static double run(int seconds, int mode, long *rom_s)
{
    long prev = clock_s();
    long total = 0;
    uint32_t t0 = dg_osc1_ticks();
    for (int s = 0; s < seconds; s++) {
        for (int f = 0; f < 50; f++) frame(mode);
        long now = clock_s();
        if (now < prev) now += 12 * 3600;
        total += now - prev;
        prev = now;
    }
    *rom_s = total;
    return (double)(dg_osc1_ticks() - t0) / seconds;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: tick_drift6 <rom> [seconds]\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 1; }
    size_t n = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    int seconds = (argc >= 3) ? atoi(argv[2]) : 600;

    dg_init(rom, (uint32_t)n);
    for (int i = 0; i < 400000; i++) dg_step();
    show("boot");

    /* get past the clock-set page: DOWN (enter), UP (hour +?), DOWN, OK */
    key(2, 14, 20); show("after DOWN");
    key(4, 14, 20); show("after UP");
    key(2, 14, 20); show("after DOWN");
    key(1, 14, 20); show("after OK");
    key(1, 14, 20); show("after OK again");
    for (int i = 0; i < 200; i++) frame(0);
    show("settled");

    if (c_sec() == 0 && c_min() == 0 && c_hour() == 0) {
        printf("\n!! clock never started (still 00:00:00) -- check the key sequence\n");
    }

    for (int mode = 0; mode <= 1; mode++) {
        long rom_s = 0;
        double tps;
        run(60, mode, &rom_s);                 /* settle */
        tps = run(seconds, mode, &rom_s);
        printf("mode %d (%s): ticks/s=%.3f  ROM clock +%ld s / %d s -> ratio %.4f (%+.1f s/day)\n",
               mode, mode ? "carry" : "old  ", tps, rom_s, seconds,
               (double)rom_s / seconds, ((double)rom_s / seconds - 1.0) * 86400.0);
        fflush(stdout);
    }
    show("end");
    return 0;
}
