#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include "digimon_core.h"

static double g_freq;

static double now_us(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / g_freq;
}

int main(int argc, char **argv) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = (double)f.QuadPart;

    if (argc < 2) { printf("usage: bench <rom>\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 1; }
    static uint8_t rom[65536];
    size_t n = fread(rom, 1, sizeof(rom), fp);
    fclose(fp);
    printf("rom      : %s (%zu bytes)\n", argv[1], n);

    dg_init(rom, (uint32_t)n);
    for (int i = 0; i < 200000; i++) dg_step();

    const int steps = 8300, frames = 200;
    double t0 = now_us();
    for (int f2 = 0; f2 < frames; f2++)
        for (int i = 0; i < steps; i++) dg_step();
    double t1 = now_us();

    double per_frame = (t1 - t0) / frames;
    double per_step = per_frame / steps;
    printf("steps/frame : %d\n", steps);
    printf("per frame   : %.1f us (%.3f ms)\n", per_frame, per_frame / 1000.0);
    printf("per step    : %.3f us\n", per_step);
    printf("budget      : 20000 us (20 ms @ 50 fps)\n");
    printf("x86 load    : %.1f%% of budget\n", per_frame / 20000.0 * 100.0);
    printf("instr count : %u\n", dg_instr_count());
    return 0;
}

