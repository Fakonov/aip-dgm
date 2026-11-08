/* snd_probe.c -- replay a real device save on the host core and log every
 * buzzer on/off edge with the PC that caused it.
 *
 *   snd_probe <rom.bin> <save.txt> [keys] [hold]
 *   snd_probe <rom.bin> <save.txt> brute <other-save.txt> [hold]
 *
 * save.txt is the raw console capture containing SAVEBEGIN ... SAVEEND.
 * brute: load <save>, replace one RAM nibble at a time with <other-save>'s
 * value, press A, and report every address that makes the buzzer sound again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];
static uint8_t state[4096];
static uint8_t blob[4096];
static uint8_t blob2[4096];
static uint32_t g_shift = 4, g_bodyLen = 0, g_romLen = 0;
static int g_blob2len = 0;

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int load_blob(const char *path, uint8_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    static char text[256 * 1024];
    size_t tn = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[tn] = 0;

    const char *p = strstr(text, "SAVEBEGIN");
    if (p) p = strchr(p, '\n');
    if (!p) { fprintf(stderr, "%s: no SAVEBEGIN\n", path); return -1; }
    p++;
    int bn = 0;
    while (*p && bn < 4096) {
        if (strncmp(p, "SAVEEND", 7) == 0) break;
        int h = hexv((unsigned char)*p);
        if (h < 0) { p++; continue; }
        const char *q = p + 1;
        while (*q && hexv((unsigned char)*q) < 0) q++;
        if (!*q) break;
        out[bn++] = (uint8_t)((h << 4) | hexv((unsigned char)*q));
        p = q + 1;
    }
    if (bn < 24) { fprintf(stderr, "%s: short blob %d\n", path, bn); return -1; }
    return bn;
}

static void run_ticks(uint32_t n, const char *label)
{
    uint32_t t = dg_osc1_ticks() + n;
    int last = dg_sound_active();
    while ((int32_t)(dg_osc1_ticks() - t) < 0) {
        dg_step();
        int a = dg_sound_active();
        if (a != last) {
            printf("  [%s] tick=%u pc=%04X active=%d freq=%.1f\n",
                   label, (unsigned)dg_osc1_ticks(), dg_pc(), a, dg_sound_freq());
            last = a;
        }
    }
}

static void load_state(const uint8_t *st)
{
    memset(state, 0, sizeof state);
    memcpy(state + 8 + g_shift, st + 20 + 8, g_bodyLen - 8);
    dg_init(rom, g_romLen);
    dg_state_load(state, dg_state_size());
}

/* settle, then hold A and report whether the buzzer ever came on. */
static int run_and_detect(uint32_t hold)
{
    run_ticks(32768, "settle");
    dg_set_button(4, 1);
    uint32_t t = dg_osc1_ticks() + hold;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) {
        dg_step();
        if (dg_sound_active()) { dg_set_button(4, 0); return 1; }
    }
    dg_set_button(4, 0);
    return 0;
}

static void press(uint8_t m, uint32_t hold)
{
    dg_set_button(m, 1); run_ticks(hold, "down");
    dg_set_button(m, 0); run_ticks(16384, "up");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: snd_probe <rom> <save> [keys] [hold]\n"
                        "       snd_probe <rom> <save> brute <other> [hold]\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("rom"); return 1; }
    g_romLen = (uint32_t)fread(rom, 1, sizeof rom, f);
    fclose(f);

    int bn = load_blob(argv[2], blob);
    if (bn < 0) return 1;
    g_bodyLen = (uint32_t)(bn - 20);

    for (uint32_t s = 0; s <= 12; s++) {
        g_shift = s;
        memset(state, 0, sizeof state);
        memcpy(state + 8 + s, blob + 20 + 8, g_bodyLen - 8);
        dg_init(rom, g_romLen);
        dg_state_load(state, dg_state_size());
        if (dg_ram(0x0B) == blob[20 + 8 + 0x0B] && dg_ram(0x56) == blob[20 + 8 + 0x56]) break;
    }

    if (argc > 3 && strcmp(argv[3], "brute") == 0) {
        uint32_t hold = (argc > 5) ? (uint32_t)atoi(argv[5]) : 30000;
        g_blob2len = load_blob(argv[4], blob2);
        if (g_blob2len < 0) return 1;
        printf("brute: shift=%u body=%u other=%d\n", (unsigned)g_shift,
               (unsigned)g_bodyLen, g_blob2len);

        load_state(blob);
        printf("baseline (save itself): beeps=%d\n", run_and_detect(hold));
        load_state(blob2);
        printf("baseline (other save):  beeps=%d\n", run_and_detect(hold));

        int hits = 0;
        for (int addr = 0; addr < 0x300; addr++) {
            uint8_t va = blob[28 + addr] & 0xF;
            uint8_t vb = blob2[28 + addr] & 0xF;
            if (va == vb) continue;
            load_state(blob);
            dg_ram_write(addr, vb);
            if (run_and_detect(hold)) {
                printf("HIT M(%03X): %X -> %X\n", addr, va, vb);
                hits++;
            }
        }
        printf("hits=%d\n", hits);
        return 0;
    }

    const char *keys = (argc > 3) ? argv[3] : "A";
    uint32_t hold = (argc > 4) ? (uint32_t)atoi(argv[4]) : 30000;

    if (strcmp(keys, "watch") == 0) {
        load_state(blob);
        uint32_t total = (argc > 4) ? (uint32_t)atoi(argv[4]) : 32768u * 20u;
        const char *wk = (argc > 5) ? argv[5] : "";
        if (wk[0]) dg_ram_write(0x17, 0);   /* reproduce the manual unmute */
        printf("watch m17 start=%X pc=%04X ticks=%u keys=%s\n",
               dg_ram(0x17), dg_pc(), (unsigned)total, wk);
        for (const char *k = wk; *k; k++) {
            uint8_t m = (*k == 'A') ? 4 : (*k == 'B') ? 2 : 1;
            press(m, hold);
            printf("  after %c: m17=%X mode=%X sub=%X pc=%04X\n",
                   *k, dg_ram(0x17), dg_ram(0x09), dg_ram(0x0A), dg_pc());
        }
        uint8_t last = dg_ram(0x17);
        uint32_t t = dg_osc1_ticks() + total;
        int n = 0;
        while ((int32_t)(dg_osc1_ticks() - t) < 0) {
            uint16_t pc_before = dg_pc();
            dg_step();
            uint8_t v = dg_ram(0x17);
            if (v != last) {
                printf("  m17 %X->%X  pc_before=%04X tick=%u\n",
                       last, v, pc_before, (unsigned)dg_osc1_ticks());
                last = v;
                if (++n >= 40) break;
            }
        }
        printf("watch done, changes=%d final=%X pc=%04X\n", n, dg_ram(0x17), dg_pc());
        return 0;
    }

    load_state(blob);
    printf("shift=%u mode=%X sub=%X sleep=%X species=%X injured=%d\n",
           (unsigned)g_shift, dg_ram(0x09), dg_ram(0x0A), dg_ram(0x33),
           dg_ram(0x0B), dg_ram(0x05) & 1);

    run_ticks(32768, "settle");
    printf("after settle: active=%d freq=%.1f pc=%04X\n",
           dg_sound_active(), dg_sound_freq(), dg_pc());

    for (const char *k = keys; *k; k++) {
        uint8_t m = (*k == 'A') ? 4 : (*k == 'B') ? 2 : 1;
        printf("-- press %c\n", *k);
        press(m, hold);
    }
    printf("final: active=%d freq=%.1f pc=%04X mode=%X sub=%X\n",
           dg_sound_active(), dg_sound_freq(), dg_pc(), dg_ram(0x09), dg_ram(0x0A));
    return 0;
}
