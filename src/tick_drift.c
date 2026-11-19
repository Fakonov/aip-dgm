/* tick_drift: does the firmware's "one frame = TICKS_PER_FRAME OSC1 ticks" loop
 * actually advance the emulated clock by 655 ticks per frame, or does the
 * "while (ticks < target) dg_step()" overshoot accumulate into a fast clock?
 *
 * Reproduces the dg_task loop exactly (digimon_app.c:3495-3501):
 *     uint32_t tick_target = dg_osc1_ticks() + TICKS_PER_FRAME;
 *     while ((int32_t)(dg_osc1_ticks() - tick_target) < 0 && guard++ < 40000) dg_step();
 *
 * Reports ticks advanced per frame (expected 655.0), the distribution of the
 * per-step tick cost (which is what creates the overshoot), and how much the
 * ROM's own game clock RAM (0x46..0x4B, BCD) advances over an emulated hour.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "digimon_core.h"

static uint8_t rom[65536];

/* BCD lo/hi pair -> int */
static int bcd_pair(uint8_t lo, uint8_t hi)
{
    int l = (lo & 0x0f) + 10 * ((lo >> 4) & 0x0f);
    int h = (hi & 0x0f) + 10 * ((hi >> 4) & 0x0f);
    return l + 100 * h;
}

static int stage_sec(void)  { return bcd_pair(dg_ram(0x046), dg_ram(0x047)); }
static int stage_min(void)  { return bcd_pair(dg_ram(0x048), dg_ram(0x049)); }
static int stage_hr(void)   { return dg_ram(0x04a) + 256 * dg_ram(0x04b); }
static int stage_total_s(void) { return stage_hr() * 3600 + stage_min() * 60 + stage_sec(); }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: tick_drift <rom> [frames]\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 1; }
    size_t n = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    printf("rom %s: %zu bytes\n", argv[1], n);

    dg_init(rom, (uint32_t)n);

    /* Warm up: let the ROM finish its power-on path (this probe does not press
     * keys, so the ROM may sit on the clock-set page; that is the same state a
     * fresh toy is in and the clock registers still advance there). */
    for (int i = 0; i < 400000; i++) dg_step();

    const uint32_t TICKS = 655;           /* OSC1_HZ * FRAME_MS / 1000 */
    const int frames = (argc >= 3) ? atoi(argv[2]) : 5000;

    /* --- 1. overshoot per frame ------------------------------------------- */
    uint32_t t_start = dg_osc1_ticks();
    uint64_t steps = 0;
    int fast_frames = 0;
    /* histogram of tick cost per step (how big one dg_step() burst is) */
    long cost1 = 0, cost_small = 0, cost_big = 0;
    uint32_t prev = dg_osc1_ticks();
    for (int fr = 0; fr < frames; fr++) {
        uint32_t target = dg_osc1_ticks() + TICKS;
        int guard = 0;
        if (dg_cpu_fast_mode()) fast_frames++;
        while ((int32_t)(dg_osc1_ticks() - target) < 0 && guard++ < 40000) {
            dg_step();
            steps++;
            uint32_t now = dg_osc1_ticks();
            uint32_t d = now - prev;
            prev = now;
            if (d == 1) cost1++;
            else if (d <= 8) cost_small++;
            else cost_big++;
        }
    }
    uint32_t advanced = dg_osc1_ticks() - t_start;
    printf("\n--- frame pacing (dg_task loop, %d frames) ---\n", frames);
    printf("ticks advanced : %u  (%.3f per frame, expected %u)\n",
           advanced, (double)advanced / frames, TICKS);
    printf("clock error    : %+.4f %%  (%+.1f s per real hour)\n",
           100.0 * ((double)advanced / frames - TICKS) / TICKS,
           ((double)advanced / frames - TICKS) / TICKS * 3600.0);
    printf("steps          : %llu total, %.2f per frame\n",
           (unsigned long long)steps, (double)steps / frames);
    printf("step tick cost : d==1 %ld, 1<d<=8 %ld, d>8 %ld\n", cost1, cost_small, cost_big);
    printf("fast-mode frames: %d / %d (%.1f %%)\n",
           fast_frames, frames, 100.0 * fast_frames / frames);

    /* --- 2. the ROM's own game clock over an emulated hour ---------------- */
    printf("\n--- ROM game clock (RAM 0x46..0x4B) over an emulated hour ---\n");
    printf("start: %d:%02d:%02d\n", stage_hr(), stage_min(), stage_sec());
    const int total_frames = 50 * 3600;   /* 3600 s of emulated time at 50 fps */
    int last_print = -1;
    for (int fr = 0; fr < total_frames; fr++) {
        uint32_t target = dg_osc1_ticks() + TICKS;
        int guard = 0;
        while ((int32_t)(dg_osc1_ticks() - target) < 0 && guard++ < 40000) dg_step();
        int el = (fr + 1) / 50;                 /* emulated seconds elapsed */
        if (el != last_print && el % 600 == 0) {  /* every 10 emulated minutes */
            last_print = el;
            printf("  %5d s -> ROM %d:%02d:%02d\n", el, stage_hr(), stage_min(), stage_sec());
            fflush(stdout);
        }
    }
    printf("end  : %d:%02d:%02d  (emulated 3600 s)\n", stage_hr(), stage_min(), stage_sec());
    printf("ratio: %.4f  (ROM seconds per emulated second, want 1.0000)\n",
           (double)stage_total_s() / 3600.0);
    return 0;
}
