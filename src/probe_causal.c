/* probe_causal.c -- 找出"信息页到底读哪些 RAM"。
 *
 *   probe_causal <rom.bin> <save.txt> <键序列，如 AB 或 AAB 或空>
 *
 * 做法：把真机存档灌进 ROM，进到指定信息页，记下屏幕；然后对 0x00..0xFF
 * 每个 nibble 各 +1（重来一遍），再渲染同一页，比较点阵。
 * 变了的地址 = ROM 在这一页真的读了它。因果，不是猜。
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"

static uint8_t rom[65536];
static uint8_t state[4096];
static uint8_t blob[4096];
static uint8_t pic[DG_LCD_H][DG_LCD_W];
static uint32_t g_shift = 4;
static uint32_t g_bodyLen = 1304;
static uint32_t g_romLen = 0;

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

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

static void ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) dg_step();
}

static void press(uint8_t m)
{
    dg_set_button(m, 1); ticks(16384);
    dg_set_button(m, 0); ticks(16384);
}

static void build_state(void)
{
    memset(state, 0, sizeof state);
    memcpy(state + 8 + g_shift, blob + 20 + 8, g_bodyLen - 8);
}

/* 进页并渲染；pokeAddr<0 表示不改 RAM */
static void enter_and_grab(const char *keys, int pokeAddr)
{
    build_state();
    dg_init(rom, g_romLen);
    dg_state_load(state, dg_state_size());
    if (pokeAddr >= 0) dg_ram_write(pokeAddr, (dg_ram(pokeAddr) + 1) & 0xF);
    ticks(32768);
    for (const char *k = keys; *k; k++) {
        press(*k == 'A' ? 4 : *k == 'B' ? 2 : 1);
        ticks(32768);
    }
    grab();
}

static uint32_t hash(void)
{
    uint32_t h = 2166136261u;
    for (int r = 0; r < DG_LCD_H; r++)
        for (int c = 0; c < DG_LCD_W; c++) h = (h ^ pic[r][c]) * 16777619u;
    return h;
}

static void dump(void)
{
    for (int r = 0; r < DG_LCD_H; r++) {
        printf("  ");
        for (int c = 0; c < DG_LCD_W; c++) putchar(pic[r][c] ? '#' : '.');
        putchar('\n');
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: probe_causal <rom> <save> [keys]\n"); return 2; }
    const char *keys = argc > 3 ? argv[3] : "";

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "rom\n"); return 1; }
    size_t rn = fread(rom, 1, sizeof rom, f);
    g_romLen = (uint32_t)rn;
    fclose(f);

    f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "save\n"); return 1; }
    static char text[128 * 1024];
    size_t tn = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[tn] = 0;
    const char *p = strstr(text, "SAVEBEGIN");
    if (p) p = strchr(p, '\n');
    if (!p) { fprintf(stderr, "envelope\n"); return 1; }
    p++;
    int bn = 0;
    while (*p && bn < (int)sizeof blob) {
        if (strncmp(p, "SAVEEND", 7) == 0) break;
        int h = hexv((unsigned char)*p);
        if (h < 0) { p++; continue; }
        const char *q = p + 1;
        while (*q && hexv((unsigned char)*q) < 0) q++;
        if (!*q) break;
        blob[bn++] = (uint8_t)((h << 4) | hexv((unsigned char)*q));
        p = q + 1;
    }
    g_bodyLen = (uint32_t)(bn - 20);

    /* 对齐搜索：以存档 0B/1A/1C 等为指纹 */
    int found = -1;
    for (uint32_t s = 0; s <= 12 && found < 0; s++) {
        g_shift = s;
        build_state();
        dg_init(rom, (uint32_t)rn);
        if (dg_state_load(state, dg_state_size()) != 0) continue;
        if (dg_ram(0x0B) == blob[20 + 8 + 0x0B] && dg_ram(0x1A) == blob[20 + 8 + 0x1A] &&
            dg_ram(0x56) == blob[20 + 8 + 0x56] && dg_ram(0x68) == blob[20 + 8 + 0x68]) found = (int)s;
    }
    if (found < 0) { printf("!! alignment not found\n"); return 1; }
    g_shift = (uint32_t)found;

    /* dg_init 需要真实 rom 指针，上面临时传 0 只为搜索；这里统一用重载版 */
    printf("keys='%s' shift=%u\n", keys, g_shift);

    enter_and_grab(keys, -1);
    uint32_t base = hash();
    printf("baseline hash %08X\n", base);
    dump();

    printf("\n-- nibble +1 会让这一页变化的地址 --\n");
    int hits = 0;
    for (int a = 0; a < 0x100; a++) {
        /* build_state 每次重来，所以要重新 init + load 再 poke */
        build_state();
        dg_init(rom, (uint32_t)rn);
        dg_state_load(state, dg_state_size());
        dg_ram_write(a, (dg_ram(a) + 1) & 0xF);
        ticks(32768);
        for (const char *k = keys; *k; k++) { press(*k == 'A' ? 4 : *k == 'B' ? 2 : 1); ticks(32768); }
        grab();
        if (hash() != base) {
            printf("  M(%02X) = %X -> %X\n", a, blob[20 + 8 + a], (dg_ram(a)));
            hits++;
        }
    }
    printf("total %d addresses affect this page\n", hits);
    return 0;
}
