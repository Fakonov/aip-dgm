/* probe_ui_state.c -- 功能图标（8 个自定义段）与 ROM UI 状态探针
 *
 * 结论（本机实测，w64devkit gcc，ROM: digimon.bin 16384 B）：
 *
 *   1) 8 个自定义段不是"丝印"，是 ROM 真实驱动的 LCD 段。
 *      之前测到"恒为 0"是因为当时停在 mode=1（对时）或未孵化。
 *   2) 孵化那一刻 species_slot 0->1 同时 (0,0) 段点亮。
 *   3) 把 mode 强制成 2..8，(137,3)(155,3)(157,3)(159,3)(22,0)(20,0)(18,0)
 *      依次点亮 —— 7 个菜单模式对 7 个图标段，一一对应。
 *
 * 用法：
 *   gcc -O2 -std=c99 -o probe_ui_state.exe probe_ui_state.c digimon_core.c
 *   ./probe_ui_state.exe digimon.bin
 *
 * RAM 地址来自 1997 固件反汇编附录（article/appendix-ram.md）：
 *   0x009 mode  0 home, 1 clock, 2..8 menu icons, 9 evolving, A poop, B dying, C grave
 *   0x00A submode
 *   0x00B species_slot  0 egg, 1 baby I, ...
 *   0x00C overlay_icon
 *   0x015 menu_index
 *   0x040 call_timer
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];

/* 32x16 点阵之外的 8 个自定义段 */
static const int ICON_B[8]   = {0, 18, 20, 22, 137, 155, 157, 159};
static const int ICON_BIT[8] = {0,  0,  0,  0,   3,   3,   3,   3};
/* 实测得到的 mode -> 段 对应关系 */
static const char *MODE_ICON[9] = {
    "home", "clock", "(137,3)", "(155,3)", "(157,3)", "(159,3)",
    "(22,0)", "(20,0)", "(18,0)"
};

static int run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    long guard = 0;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) {
        dg_step();
        if (++guard > 400000000L) return 0;
    }
    return 1;
}

static void press(uint8_t m, uint32_t hold)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(hold);
}

static void state(const char *tag)
{
    const uint8_t *v = dg_vram();
    uint32_t len = dg_vram_len();
    printf("%-14s mode=%X sub=%X slot=%X idx=%X ovl=%X call=%X | icons=",
           tag, dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00B), dg_ram(0x015),
           dg_ram(0x00C), dg_ram(0x040));
    for (int i = 0; i < 8; i++) {
        int bit = ((uint32_t)ICON_B[i] < len) ? (v[ICON_B[i]] >> ICON_BIT[i]) & 1 : 0;
        printf("%d", bit);
    }
    printf("  pc=%u\n", dg_pc());
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: probe_ui_state <rom.bin>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no rom: %s\n", argv[1]); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);

    const uint32_t SEC = 32768;
    const uint8_t A_TOP = 4, B_OK = 2, C_BOT = 1;

    /* ---- 对时（真机流程：DOWN 进对时、UP 小时、DOWN 分钟、OK 确认；
     *      本 ROM 实测还需再 A/C/B 各一次才落到 mode=0） ---- */
    run_ticks(SEC * 2);
    state("boot");
    press(C_BOT, 8192); press(A_TOP, 4096); press(C_BOT, 4096); press(B_OK, 8192);
    state("after ok");
    if (dg_ram(0x009) == 1) { press(A_TOP, 8192); press(C_BOT, 8192); press(B_OK, 8192); }
    state("clock done");

    /* ---- 等孵化 ---- */
    printf("\n=== waiting for hatch ===\n");
    int lastslot = -1;
    for (int s = 1; s <= 15 * 60; s++) {
        run_ticks(SEC);
        int slot = dg_ram(0x00B);
        if (slot != lastslot) {
            char t[32]; snprintf(t, sizeof t, "t=%d:%02d", s / 60, s % 60);
            state(t); lastslot = slot;
        }
        if (s == 5 * 60) state("t=5:00");
    }

    /* ---- 按 A 看菜单是否被 ROM 接管 ---- */
    printf("\n=== pressing A 8x ===\n");
    for (int i = 0; i < 8; i++) {
        press(A_TOP, 8192);
        run_ticks(SEC / 2);
        char t[32]; snprintf(t, sizeof t, "A#%d", i + 1);
        state(t);
    }

    /* ---- 强制 mode=2..8，验证段映射 ---- */
    printf("\n=== forcing mode 2..8 (proves the segment mapping) ===\n");
    for (int m = 2; m <= 8; m++) {
        dg_ram_write(0x009, (uint8_t)m);
        dg_ram_write(0x00A, 0);
        run_ticks(SEC / 4);
        char t[48]; snprintf(t, sizeof t, "mode=%X -> %s", m, MODE_ICON[m]);
        state(t);
        dg_ram_write(0x009, 0);
        run_ticks(SEC / 4);
    }

    printf("\ndone instr=%u\n", dg_instr_count());
    return 0;
}
