/* probe_page.c -- 把真机存档灌进 ROM，逐页打印 ROM 自己画的信息页。
 *
 *   probe_page <rom.bin> <save.txt> <pages>
 *
 * pages: 逗号分隔的按键串，字符 A/B/C（映射到 4/2/1）。空串表示只看当前画面。
 *
 * 存档是设备（32-bit xtensa ABI）的 dg_cpu 原样 memcpy，主机是 x86_64，
 * 头部指针宽度和 double 对齐都会挪动偏移，这里暴力试出正确的搬移量。
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

static void show(const char *tag)
{
    grab();
    printf("\n=== %s   mode=%X sub=%X species=%X pc=%X ===\n",
           tag, dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00B), dg_pc());
    for (int r = 0; r < DG_LCD_H; r++) {
        printf("  ");
        for (int c = 0; c < DG_LCD_W; c++) putchar(pic[r][c] ? '#' : '.');
        putchar('\n');
    }
    printf("  M(54-5F):");
    for (int a = 0x54; a <= 0x5F; a++) printf(" %02X=%X", a, dg_ram(a));
    printf("\n  M(68-6B):");
    for (int a = 0x68; a <= 0x6B; a++) printf(" %02X=%X", a, dg_ram(a));
    printf("\n  care=%X over=%X sleep=%X train=%X%X prot=%X str=%X hung=%X\n",
           dg_ram(0x31), dg_ram(0x32), dg_ram(0x33), dg_ram(0x34), dg_ram(0x35),
           dg_ram(0x3C), dg_ram(0x3D), dg_ram(0x3E));
}

static void build_state(uint32_t shift, uint32_t bodyLen)
{
    memset(state, 0, sizeof state);
    memcpy(state + 8 + shift, blob + 20 + 8, bodyLen - 8);
}

static int signature_ok(void)
{
    static const int probe[] = { 0x0B, 0x05, 0x1A, 0x1B, 0x1C, 0x1D, 0x3D, 0x3E, 0x56, 0x57, 0x68, 0x69 };
    for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        int a = probe[i];
        if (dg_ram(a) != blob[20 + 8 + a]) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: probe_page <rom> <save> [pages]\n"); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "rom open failed\n"); return 1; }
    size_t rn = fread(rom, 1, sizeof rom, f);
    fclose(f);

    f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "save open failed\n"); return 1; }
    static char text[128 * 1024];
    size_t tn = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[tn] = 0;

    const char *p = strstr(text, "SAVEBEGIN");
    if (p) p = strchr(p, '\n');
    if (!p) { fprintf(stderr, "no SAVEBEGIN envelope\n"); return 1; }
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
    if (bn < 24) { fprintf(stderr, "save too short\n"); return 1; }
    uint32_t bodyLen = (uint32_t)(bn - 20);
    printf("save %d B, body %u B, core state %u B\n", bn, bodyLen, dg_state_size());

    int found = -1;
    for (uint32_t s = 0; s <= 12 && found < 0; s++) {
        build_state(s, bodyLen);
        dg_init(rom, (uint32_t)rn);
        if (dg_state_load(state, dg_state_size()) != 0) continue;
        if (signature_ok()) found = (int)s;
    }
    if (found < 0) { printf("!! no alignment fits, falling back to 4\n"); found = 4; }
    g_shift = (uint32_t)found;
    printf("alignment shift = %u   (0B=%X 56=%X 57=%X 68=%X 69=%X)\n",
           g_shift, dg_ram(0x0B), dg_ram(0x56), dg_ram(0x57), dg_ram(0x68), dg_ram(0x69));

    const char *pages = argc > 3 ? argv[3] : "";
    char seq[24];
    int si = 0;
    for (const char *q = pages; ; q++) {
        if (*q == ',' || *q == 0) {
            seq[si] = 0;
            build_state(g_shift, bodyLen);
            dg_init(rom, (uint32_t)rn);
            dg_state_load(state, dg_state_size());
            ticks(32768);
            for (int i = 0; i < si; i++) {
                char k = seq[i];
                press(k == 'A' ? 4 : k == 'B' ? 2 : 1);
                ticks(32768);
            }
            char tag[40];
            if (si == 0) snprintf(tag, sizeof tag, "(current)");
            else         snprintf(tag, sizeof tag, "keys %s", seq);
            show(tag);
            si = 0;
            if (*q == 0) break;
        } else if (si < (int)sizeof seq - 1) {
            seq[si++] = *q;
        }
    }
    return 0;
}
