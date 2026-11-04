/* timeskip_probe.c -- 跳过时间的可行性实验
 *
 * 目的：搞清楚 ROM 的时间是"自增式"还是"读绝对时钟式"。
 *
 *   - 如果 stage_hours/minutes 是 ROM 每 tick 自己累加的，直接写 RAM 会被下一个
 *     tick 覆盖 → 跳过时间只能靠真跑指令（约 50 倍速，2 小时要 2 分半）。
 *   - 如果 ROM 是读某个基准再算差值，写 RAM 就能瞬跳。
 *
 * 另外顺带量一下：跑 1 分钟游戏内时间在 x86 上要多久，用来估算 C3 上的可行性。
 *
 *   gcc -O2 -o timeskip_probe.exe timeskip_probe.c digimon_core.c
 *   ./timeskip_probe.exe digimon.bin
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "digimon_core.h"

static uint8_t rom[65536];

#define ADDR_CARE        0x031
#define ADDR_TRAIN_LO    0x034
#define ADDR_TRAIN_HI    0x035
#define ADDR_POOP        0x007
#define ADDR_CALL_MIN_LO 0x040
#define ADDR_CALL_MIN_HI 0x041
#define ADDR_CALL_HOUR   0x042
#define ADDR_STAGE_SEC   0x046
#define ADDR_STAGE_MIN   0x048
#define ADDR_STAGE_HOUR  0x04A
#define ADDR_NEXT_EVOL   0x04C
#define ADDR_POOP_MIN    0x080
#define ADDR_POOP_HOUR   0x082

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* 跑 n 个 OSC1 tick */
static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    long guard = 0;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) {
        dg_step();
        if (++guard > 2000000000L) { printf("!! guard hit\n"); return; }
    }
}

static int bcd2(int v) { return ((v >> 4) & 0xF) * 10 + (v & 0xF); }
static int bin2bcd(int v) { return ((v / 10) << 4) | (v % 10); }

static void dump(const char *tag)
{
    printf("%-22s slot=%X mode=%X stage=%02X:%02X:%02X (raw %X %X %X) "
           "next=%X%X%X call=%X%X%X poop=%X%X%X care=%X train=%X%X poopN=%X\n",
           tag,
           dg_ram(0x00B), dg_ram(0x009),
           bcd2(dg_ram(ADDR_STAGE_HOUR)), bcd2(dg_ram(ADDR_STAGE_MIN)),
           bcd2(dg_ram(ADDR_STAGE_SEC)),
           dg_ram(ADDR_STAGE_HOUR), dg_ram(ADDR_STAGE_MIN), dg_ram(ADDR_STAGE_SEC),
           dg_ram(ADDR_NEXT_EVOL + 2), dg_ram(ADDR_NEXT_EVOL + 1), dg_ram(ADDR_NEXT_EVOL),
           dg_ram(ADDR_CALL_HOUR), dg_ram(ADDR_CALL_MIN_HI), dg_ram(ADDR_CALL_MIN_LO),
           dg_ram(ADDR_POOP_HOUR), dg_ram(0x081), dg_ram(ADDR_POOP_MIN),
           dg_ram(ADDR_CARE), dg_ram(ADDR_TRAIN_HI), dg_ram(ADDR_TRAIN_LO),
           dg_ram(ADDR_POOP));
    fflush(stdout);
}

static void press(uint8_t m, uint32_t hold)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(hold);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: timeskip_probe <rom.bin>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no rom\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);

    const uint32_t SEC = 32768;
    const uint8_t A_TOP = 4, B_OK = 2, C_BOT = 1;

    /* ---- 对时（同 probe_ui_state 的实测流程）---- */
    run_ticks(SEC * 2);
    press(C_BOT, 8192); press(A_TOP, 4096); press(C_BOT, 4096); press(B_OK, 8192);
    if (dg_ram(0x009) == 1) { press(A_TOP, 8192); press(C_BOT, 8192); press(B_OK, 8192); }
    dump("clock done");

    /* ---- 等孵化：先量 x86 的吞吐 ---- */
    double t0 = now_ms();
    uint32_t ticks0 = dg_osc1_ticks();
    int waited = 0;
    while (dg_ram(0x00B) == 0 && waited < 90 * 60) {   /* 最多 90 分钟游戏内 */
        run_ticks(SEC * 60);
        waited++;
    }
    double dt = now_ms() - t0;
    uint32_t ticks_run = dg_osc1_ticks() - ticks0;
    printf("\nhatched after %d min game time, wall clock %.1f s\n", waited, dt / 1000.0);
    printf("throughput: %.2f M ticks/s, %.2f M instr/s (instr=%u)\n",
           ticks_run / (dt / 1000.0) / 1e6,
           dg_instr_count() / (dt / 1000.0) / 1e6, dg_instr_count());
    printf("=> C3 is ~0.4 ms / 655 ticks, so it can run roughly %.0fx real time\n",
           20.0 / 0.4);
    dump("hatched");

    /* ---- 让阶段时间走一会儿，建立基线 ---- */
    run_ticks(SEC * 120);          /* 2 分钟游戏内 */
    dump("after 2 min");

    /* ---- 实验 1：直接写 stage_hours，看是否被 ROM 覆盖 ---- */
    uint8_t before = dg_ram(ADDR_STAGE_HOUR);
    dg_ram_write(ADDR_STAGE_HOUR, (uint8_t)(before + 5));
    printf("\n-- wrote stage_hour %X -> %X, running 10 s game time --\n",
           before, dg_ram(ADDR_STAGE_HOUR));
    run_ticks(SEC * 10);
    dump("after inject 10s");

    /* ---- 实验 2：把 stage 时间推到 next_evolution 附近 ---- */
    int nh = bcd2(dg_ram(ADDR_NEXT_EVOL + 2));
    int nm = bcd2(dg_ram(ADDR_NEXT_EVOL + 1));
    printf("\nnext_evolution raw: h=%X m=%X  (dec %d:%02d)\n",
           dg_ram(ADDR_NEXT_EVOL + 2), dg_ram(ADDR_NEXT_EVOL + 1), nh, nm);
    if (nh > 0 && nh < 100) {
        dg_ram_write(ADDR_STAGE_HOUR, (uint8_t)bin2bcd(nh));
        dg_ram_write(ADDR_STAGE_MIN, (uint8_t)bin2bcd(nm > 2 ? nm - 2 : 0));
        printf("-- forced stage to %02d:%02d, running 30 s game time --\n", nh, nm);
        for (int s = 0; s < 30; s++) {
            run_ticks(SEC);
            if (dg_ram(0x00B) != 1) { printf("slot changed at %d s\n", s); break; }
        }
    }
    dump("after forced evolution");
    printf("\nslot=%X (1=still baby, >1 = evolved, 0 = egg)\n", dg_ram(0x00B));
    return 0;
}
