/* probe_poke.c -- 加载真机存档，按 addr=val 改若干 nibble，按键进页，把点阵写成 PGM。
 *
 *   probe_poke <rom.bin> <save.txt> <keys> <out.pgm> [addr=val ...]
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
static uint32_t g_shift = 4, g_bodyLen = 1304, g_romLen = 0;

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
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

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: probe_poke <rom> <save> <keys> <out.pgm> [a=v...]\n"); return 2; }
    const char *keys = argv[3], *outp = argv[4];

    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    g_romLen = (uint32_t)fread(rom, 1, sizeof rom, f);
    fclose(f);
    f = fopen(argv[2], "rb");
    if (!f) return 1;
    static char text[128 * 1024];
    size_t tn = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[tn] = 0;
    const char *p = strstr(text, "SAVEBEGIN");
    if (p) p = strchr(p, '\n');
    if (!p) return 1;
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
    for (uint32_t s = 0; s <= 12; s++) {
        g_shift = s;
        memset(state, 0, sizeof state);
        memcpy(state + 8 + s, blob + 20 + 8, g_bodyLen - 8);
        dg_init(rom, g_romLen);
        dg_state_load(state, dg_state_size());
        if (dg_ram(0x0B) == blob[20 + 8 + 0x0B] && dg_ram(0x56) == blob[20 + 8 + 0x56]) break;
    }

    memset(state, 0, sizeof state);
    memcpy(state + 8 + g_shift, blob + 20 + 8, g_bodyLen - 8);
    dg_init(rom, g_romLen);
    dg_state_load(state, dg_state_size());

    for (int i = 5; i < argc; i++) {
        int a, v;
        if (sscanf(argv[i], "%x=%x", &a, &v) == 2) {
            dg_ram_write(a & 0xFF, (uint8_t)(v & 0xF));
            printf("poke M(%02X)=%X\n", a & 0xFF, v & 0xF);
        }
    }
    ticks(32768);
    for (const char *k = keys; *k; k++) { press(*k == 'A' ? 4 : *k == 'B' ? 2 : 1); ticks(32768); }
    grab();

    FILE *o = fopen(outp, "wb");
    if (!o) { fprintf(stderr, "out\n"); return 1; }
    fprintf(o, "P5\n%d %d\n255\n", DG_LCD_W, DG_LCD_H);
    for (int r = 0; r < DG_LCD_H; r++)
        for (int c = 0; c < DG_LCD_W; c++)
            fputc(pic[r][c] ? 0 : 255, o);
    fclose(o);
    printf("mode=%X sub=%X  -> %s\n", dg_ram(0x09), dg_ram(0x0A), outp);
    return 0;
}
