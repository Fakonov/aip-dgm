/* probe_pages.c -- 在主机上跑到成长期，然后逐页翻开 ROM 的 8 个图标页，
 * 每页打印点阵 + 关键 RAM，用来确认"哪个页面显示什么数值、取自哪里"。
 *
 *   probe_pages <rom.bin> [speed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"

static uint8_t rom[65536];
static uint8_t pic[DG_LCD_H][DG_LCD_W];

static void grab(void)
{
    const uint8_t *v = dg_vram();
    uint32_t len = dg_vram_len();
    for (int r = 0; r < DG_LCD_H; r++)
        for (int c = 0; c < DG_LCD_W; c++) {
            uint16_t cell = dg_segmap[r][c];
            uint8_t b = cell & 0xFF, bit = cell >> 8;
            pic[r][c] = (b < len && ((v[b] >> bit) & 1)) ? 1 : 0;
        }
}

static void step_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) dg_step();
}

static void press(uint8_t m)
{
    dg_set_button(m, 1); step_ticks(8192);
    dg_set_button(m, 0); step_ticks(8192);
}

static void show(const char *tag)
{
    grab();
    printf("\n=== %-14s mode=%X sub=%X species=%X  t=%.0fmin ===\n",
           tag, dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00B),
           dg_osc1_ticks() / 32768.0 / 60.0);
    for (int r = 0; r < DG_LCD_H; r++) {
        printf("  ");
        for (int c = 0; c < DG_LCD_W; c++) putchar(pic[r][c] ? '#' : '.');
        putchar('\n');
    }
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    int speed = argc > 2 ? atoi(argv[2]) : 8;
    if (speed < 1) speed = 1;

    dg_init(rom, (uint32_t)n);
    const uint32_t SEC = 32768;
    step_ticks(SEC * 2);
    /* ROM 先把时钟设定完才会走时间：DOWN / UP / DOWN / OK / UP / OK / DOWN */
    press(2); press(4); press(2); press(1); press(4); press(1); press(2);
    step_ticks(SEC * 60 * 12);   /* 孵化 */
    show("hatched");

    /* 加速跑到成长期（species >= 3），同时保住 2Hz 定时器不丢 */
    dg_set_osc1_speed(speed);
    uint32_t guard = 0;
    while (dg_ram(0x00B) < 3 && guard < 4000000000u) { dg_step(); guard++; }
    dg_set_osc1_speed(1);
    show("child stage");

    /* 逐页：每次重来一份同样的状态太麻烦，直接原地按 A 遍历 */
    for (int i = 1; i <= 8; i++) {
        char tag[32];
        snprintf(tag, sizeof tag, "A x%d", i);
        press(4);
        step_ticks(SEC * 2);
        show(tag);
        printf("  RAM: 55=%X 56=%X 57=%X 58=%X 59=%X | 68=%X 69=%X 6A=%X | 31=%X 34=%X 35=%X\n",
               dg_ram(0x55), dg_ram(0x56), dg_ram(0x57), dg_ram(0x58), dg_ram(0x59),
               dg_ram(0x68), dg_ram(0x69), dg_ram(0x6A),
               dg_ram(0x31), dg_ram(0x34), dg_ram(0x35));
    }
    return 0;
}
