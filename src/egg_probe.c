/* egg_probe.c -- 蛋阶段的孵化到底看什么
 *
 * 之前只在"孵化之后"验证过 stage 注入能触发进化；跳 2 小时却连蛋都没孵，
 * 说明蛋 -> Baby I 用的是另一套计时。这个探针把蛋阶段的 RAM 全 dump 出来，
 * 再试着推 stage 时间，看能不能孵。
 *
 *   gcc -O2 -o egg_probe.exe egg_probe.c digimon_core.c
 *   ./egg_probe.exe digimon.bin
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "digimon_core.h"

static uint8_t rom[65536];

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    long guard = 0;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) {
        dg_step();
        if (++guard > 2000000000L) { printf("guard\n"); return; }
    }
}

static void press(uint8_t m, uint32_t hold)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(hold);
}

static void dump(const char *tag)
{
    printf("%-16s slot=%X mode=%X sub=%X | 40: %X%X%X%X %X%X%X%X %X%X%X%X | 1A: %X%X %X%X %X %X\n",
           tag, dg_ram(0x00B), dg_ram(0x009), dg_ram(0x00A),
           dg_ram(0x4F), dg_ram(0x4E), dg_ram(0x4D), dg_ram(0x4C),
           dg_ram(0x4B), dg_ram(0x4A), dg_ram(0x49), dg_ram(0x48),
           dg_ram(0x47), dg_ram(0x46), dg_ram(0x45), dg_ram(0x44),
           dg_ram(0x1B), dg_ram(0x1A), dg_ram(0x1D), dg_ram(0x1C),
           dg_ram(0x1E), dg_ram(0x1F));
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: egg_probe <rom.bin>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no rom\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);

    const uint32_t SEC = 32768;
    run_ticks(SEC * 2);
    press(1, 8192); press(4, 4096); press(1, 4096); press(2, 8192);
    if (dg_ram(0x009) == 1) { press(4, 8192); press(1, 8192); press(2, 8192); }

    dump("clock done");
    for (int s = 0; s < 8; s++) { run_ticks(SEC * 30); dump("  +30s"); }
    printf("\n-- 蛋阶段自然走 4 分钟，slot=%X --\n", dg_ram(0x00B));

    /* 把 stage 分钟推到 next 之后，看能不能孵 */
    printf("\n-- 注入 stage：把 0x4C..0x4E 各自扫一遍 --\n");
    for (int pass = 0; pass < 3; pass++) {
        dg_ram_write(0x4C, 4);
        dg_ram_write(0x4D, 0);
        dg_ram_write(0x4E, (uint8_t)pass);
        dg_ram_write(0x48, 5);
        dg_ram_write(0x49, 0);
        dg_ram_write(0x4A, (uint8_t)pass);
        dg_ram_write(0x4B, 0);
        printf("pass %d: next.h=%d stage=%d:05\n", pass, pass, pass);
        for (int s = 0; s < 120; s++) {
            run_ticks(SEC);
            if (dg_ram(0x00B) != 0) { printf("  >>> hatched at %d s, slot=%X\n", s, dg_ram(0x00B)); break; }
        }
        dump("after pass");
        if (dg_ram(0x00B) != 0) break;
    }

    printf("\nfinal slot=%X\n", dg_ram(0x00B));
    return 0;
}
