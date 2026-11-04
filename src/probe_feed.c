/* probe_feed.c -- 从真机存档出发，进"食物"页并喂一次，看哪些 RAM nibble 变化。
 * 目的：确认 0x56/0x57 是不是体重（吃一次应该 +1 或 +2）。
 *
 *   probe_feed <rom.bin> <save.txt> <keysBefore> <keysFeed>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];
static uint8_t state[4096];
static uint8_t blob[4096];
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

static void keys(const char *k)
{
    for (; *k; k++) { press(*k == 'A' ? 4 : *k == 'B' ? 2 : 1); ticks(32768); }
}

static void load(void)
{
    memset(state, 0, sizeof state);
    memcpy(state + 8 + g_shift, blob + 20 + 8, g_bodyLen - 8);
    dg_init(rom, g_romLen);
    dg_state_load(state, dg_state_size());
}

static void snap(uint8_t *out)
{
    for (int a = 0; a < 0x100; a++) out[a] = dg_ram(a);
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: probe_feed <rom> <save> <pre> <feed>\n"); return 2; }

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
        load();
        if (dg_ram(0x0B) == blob[20 + 8 + 0x0B] && dg_ram(0x56) == blob[20 + 8 + 0x56]) break;
    }
    printf("shift=%u  mode=%X species=%X\n", g_shift, dg_ram(0x09), dg_ram(0x0B));

    static uint8_t before[256], after[256];
    load();
    for (int i = 5; i < argc; i++) {
        int a, v;
        if (sscanf(argv[i], "%x=%x", &a, &v) == 2) {
            dg_ram_write(a & 0xFF, (uint8_t)(v & 0xF));
            printf("poke M(%02X)=%X\n", a & 0xFF, v & 0xF);
        }
    }
    ticks(32768); keys(argv[3]); ticks(32768);
    snap(before);
    printf("pre  : mode=%X sub=%X  56=%X 57=%X 5E=%X 5F=%X 3D=%X 3E=%X 3C=%X\n",
           dg_ram(0x09), dg_ram(0x0A), before[0x56], before[0x57], before[0x5E], before[0x5F],
           before[0x3D], before[0x3E], before[0x3C]);

    keys(argv[4]); ticks(32768);
    snap(after);
    printf("post : mode=%X sub=%X  56=%X 57=%X 5E=%X 5F=%X 3D=%X 3E=%X 3C=%X\n",
           dg_ram(0x09), dg_ram(0x0A), after[0x56], after[0x57], after[0x5E], after[0x5F],
           after[0x3D], after[0x3E], after[0x3C]);

    printf("\n-- 变化的 nibble --\n");
    for (int a = 0; a < 0x100; a++)
        if (before[a] != after[a]) printf("  M(%02X): %X -> %X\n", a, before[a], after[a]);
    return 0;
}
