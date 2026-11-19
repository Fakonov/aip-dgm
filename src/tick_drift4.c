/* tick_drift4: verify the fixed frame budget (fractional carry + overshoot
 * carry-back) holds OSC1 at exactly 32768 ticks per emulated second.
 * Mirrors digimon_app.c: osc1_frame_budget() + the dg_task loop.
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

static uint32_t budget(void)
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

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: tick_drift4 <rom> [seconds]\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 1; }
    size_t n = fread(rom, 1, sizeof rom, fp);
    fclose(fp);

    dg_init(rom, (uint32_t)n);
    for (int i = 0; i < 400000; i++) dg_step();

    int seconds = (argc >= 3) ? atoi(argv[2]) : 3600;
    uint32_t t0 = dg_osc1_ticks();
    long long over_sum = 0, over_max = 0, over_min = 0;

    for (int s = 0; s < seconds; s++) {
        for (int f = 0; f < 50; f++) {
            uint32_t target = dg_osc1_ticks() + budget();
            int guard = 0;
            while ((int32_t)(dg_osc1_ticks() - target) < 0 && guard++ < STEP_GUARD) dg_step();
            s_frame_over = (int32_t)(dg_osc1_ticks() - target);
            over_sum += s_frame_over;
            if (s_frame_over > over_max) over_max = s_frame_over;
            if (s_frame_over < over_min) over_min = s_frame_over;
        }
        if ((s + 1) % 600 == 0) {
            double el = s + 1;
            printf("s=%5.0f  ticks/s=%.3f  (drift %+.4f %%)\n",
                   el, (double)(dg_osc1_ticks() - t0) / el,
                   ((double)(dg_osc1_ticks() - t0) / el - 32768.0) / 32768.0 * 100.0);
            fflush(stdout);
        }
    }
    double el = seconds;
    double rate = (double)(dg_osc1_ticks() - t0) / el;
    printf("\nfinal: %.3f ticks/s vs 32768 -> %+.4f %%  (%+.1f s per real day)\n",
           rate, (rate - 32768.0) / 32768.0 * 100.0,
           (rate / 32768.0 - 1.0) * 86400.0);
    printf("overshoot per frame: avg %+.3f, min %lld, max %lld ticks\n",
           (double)over_sum / ((long long)seconds * 50), over_min, over_max);
    return 0;
}
