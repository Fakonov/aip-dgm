/* speed_probe.c -- OSC1 加速能不能用
 *
 * 思路：跳过时间时把 osc1_div 放大 N 倍，每条指令消耗 N 倍的 OSC1 时间，
 * 于是走完同样的游戏分钟数只要 1/N 的步数。代价是 ROM 的定时器每 tick 只有
 * 原来的 1/N 指令可用，状态机可能来不及处理 —— 必须实测它还能不能正常孵化。
 *
 *   gcc -O2 -o speed_probe.exe speed_probe.c digimon_core.c
 *   ./speed_probe.exe digimon.bin
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
    long guard = 0;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) {
        dg_step();
        if (++guard > 2000000000L) return;
    }
}

static void press(uint8_t m, uint32_t hold)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(hold);
}

/* 用固件里同样的循环跑 n 分钟游戏内，返回步数和耗时 */
static void measure(const char *tag, int minutes, int mult)
{
    dg_set_osc1_speed(mult);
    uint32_t start = dg_osc1_ticks();
    uint32_t target = start + (uint32_t)minutes * 60u * OSC1_PER_SEC;
    long long steps = 0;
    double t0 = now_ms();
    while ((int32_t)(dg_osc1_ticks() - target) < 0) {
        dg_step();
        steps++;
    }
    double dt = now_ms() - t0;
    dg_set_osc1_speed(1);
    printf("%-10s mult=%-2d  %6lld steps  %7.0f ms  slot=%X stage=%X:%X%X:%X%X mode=%X\n",
           tag, mult, steps, dt, dg_ram(0x00B),
           dg_ram(0x4A), dg_ram(0x48), dg_ram(0x49), dg_ram(0x46), dg_ram(0x47),
           dg_ram(0x009));
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: speed_probe <rom.bin> [minutes]\n"); return 1; }
    int minutes = argc > 2 ? atoi(argv[2]) : 6;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no rom\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);

    printf("=== 蛋阶段：%d 分钟游戏内，不同加速倍数 ===\n", minutes);
    int mults[] = { 1, 2, 4, 8, 16 };
    for (int i = 0; i < 5; i++) {
        dg_init(rom, (uint32_t)n);
        run_ticks(OSC1_PER_SEC * 2);
        press(1, 8192); press(4, 4096); press(1, 4096); press(2, 8192);
        if (dg_ram(0x009) == 1) { press(4, 8192); press(1, 8192); press(2, 8192); }
        char tag[16];
        snprintf(tag, sizeof tag, "egg/%dmin", minutes);
        measure(tag, minutes, mults[i]);
        printf("           hatched: %s\n", dg_ram(0x00B) != 0 ? "YES" : "NO  <-- unusable");
    }

    printf("\n=== 更长的段：30 分钟 ===\n");
    for (int i = 0; i < 5; i++) {
        dg_init(rom, (uint32_t)n);
        run_ticks(OSC1_PER_SEC * 2);
        press(1, 8192); press(4, 4096); press(1, 4096); press(2, 8192);
        if (dg_ram(0x009) == 1) { press(4, 8192); press(1, 8192); press(2, 8192); }
        measure("egg/30min", 30, mults[i]);
        printf("           hatched: %s\n", dg_ram(0x00B) != 0 ? "YES" : "NO  <-- unusable");
    }
    return 0;
}
