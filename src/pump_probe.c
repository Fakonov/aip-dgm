/* pump_probe.c -- 验证固件 rom_skip_minutes 的循环语义
 *
 * 固件现在是"真跑"：把 OSC1 推到 target，期间不停 dg_step()。这个探针用
 * 完全相同的循环，跑 6 分钟游戏内（蛋应该孵出 Baby I），并记录 PC 上的耗时，
 * 用来推算 C3 上的等待时间。
 *
 *   gcc -O2 -o pump_probe.exe pump_probe.c digimon_core.c
 *   ./pump_probe.exe digimon.bin 6
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "digimon_core.h"

static uint8_t rom[65536];
#define OSC1_PER_SEC 32768u

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

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

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: pump_probe <rom.bin> [minutes]\n"); return 1; }
    int minutes = argc > 2 ? atoi(argv[2]) : 6;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no rom\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);

    /* 对时 */
    run_ticks(OSC1_PER_SEC * 2);
    press(1, 8192); press(4, 4096); press(1, 4096); press(2, 8192);
    if (dg_ram(0x009) == 1) { press(4, 8192); press(1, 8192); press(2, 8192); }

    printf("before: slot=%X mode=%X  (running %d min of game time)\n",
           dg_ram(0x00B), dg_ram(0x009), minutes);

    /* ==== 与固件 rom_skip_minutes() 完全相同的循环 ==== */
    double t0 = now_ms();
    uint32_t target = dg_osc1_ticks() + (uint32_t)minutes * 60u * OSC1_PER_SEC;
    long long steps = 0;
    while ((int32_t)(dg_osc1_ticks() - target) < 0) {
        dg_step();
        steps++;
    }
    double dt = now_ms() - t0;

    printf("after : slot=%X mode=%X  (%lld steps, %.0f ms on x86)\n",
           dg_ram(0x00B), dg_ram(0x009), steps, dt);
    printf("C3 estimate: x86 runs ~%0.1fx faster than the C3's 1.64 M ticks/s\n",
           (double)((uint32_t)minutes * 60u * OSC1_PER_SEC) / (dt / 1000.0) / 1.64e6);
    printf("=> on the toy this skip would take about %.1f s\n",
           dt / 1000.0 * ((double)((uint32_t)minutes * 60u * OSC1_PER_SEC) / (dt / 1000.0)) / 1.64e6);

    printf("slot %X = %s\n", dg_ram(0x00B),
           dg_ram(0x00B) == 0 ? "still egg (BAD)" :
           dg_ram(0x00B) == 1 ? "Baby I (hatched, good)" : "further stage");
    return 0;
}
