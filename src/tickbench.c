/* tickbench: how many dg_step() calls does one 20 ms OSC1 frame cost,
 * and what does N-times speedup cost in wall clock on this host. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include "digimon_core.h"

static double g_freq;
static double now_us(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / g_freq;
}

static uint8_t rom[65536];

int main(int argc, char **argv) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = (double)f.QuadPart;
    if (argc < 2) { printf("usage: tickbench <rom>\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 1; }
    size_t n = fread(rom, 1, sizeof(rom), fp);
    fclose(fp);
    printf("rom %s: %zu bytes\n", argv[1], n);

    dg_init(rom, (uint32_t)n);
    for (int i = 0; i < 300000; i++) dg_step();   /* warm up caches + settle */

    const uint32_t TICKS = 655;
    for (int mult = 1; mult <= 32; mult *= 2) {
        const int frames = 200;
        uint32_t steps_total = 0;
        double t0 = now_us();
        for (int fr = 0; fr < frames; fr++) {
            uint32_t target = dg_osc1_ticks() + TICKS * (uint32_t)mult;
            int guard = 0;
            while ((int32_t)(dg_osc1_ticks() - target) < 0 && guard++ < 400000) {
                dg_step();
                steps_total++;
            }
        }
        double per_frame_us = (now_us() - t0) / frames;
        printf("x%-2d  ticks/frame=%-6u steps/frame=%-8.1f  host=%8.1f us/frame  (=%.2f ms per emulated second)\n",
               mult, TICKS * mult, (double)steps_total / frames, per_frame_us,
               per_frame_us * (1000.0 / 20.0) / mult);
    }
    return 0;
}
