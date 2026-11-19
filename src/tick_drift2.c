/* tick_drift2: dump the ROM's game-clock RAM raw, and measure how much ROM
 * time passes per 60 s of emulated time, to see whether the fast clock is a
 * constant offset or a growing one ("越来越快").
 *
 * Clock registers (digimon_app.c:417-451):
 *   0x046/0x047 sec lo/hi   0x048/0x049 min lo/hi   0x04A/0x04B hour (binary)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "digimon_core.h"

static uint8_t rom[65536];

static void frame(void)
{
    uint32_t target = dg_osc1_ticks() + 655;
    int guard = 0;
    while ((int32_t)(dg_osc1_ticks() - target) < 0 && guard++ < 40000) dg_step();
}

static void dump(const char *tag, int el)
{
    printf("%s el=%5d  raw %02X %02X | %02X %02X | %02X %02X   ticks=%u\n",
           tag, el,
           dg_ram(0x046), dg_ram(0x047), dg_ram(0x048), dg_ram(0x049),
           dg_ram(0x04a), dg_ram(0x04b), dg_osc1_ticks());
}

/* BCD packed as two nibbles */
static int bcd(uint8_t v) { return (v & 0x0f) + 10 * ((v >> 4) & 0x0f); }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: tick_drift2 <rom> [seconds]\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 1; }
    size_t n = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    printf("rom %s: %zu bytes\n", argv[1], n);

    dg_init(rom, (uint32_t)n);
    for (int i = 0; i < 400000; i++) dg_step();

    int seconds = (argc >= 3) ? atoi(argv[2]) : 1800;
    dump("t0  ", 0);

    int prev_sec_val = -1;
    double prev_rom = 0;
    for (int s = 1; s <= seconds; s++) {
        for (int f = 0; f < 50; f++) frame();

        int sec = bcd(dg_ram(0x046)) + 100 * bcd(dg_ram(0x047));
        int mn  = bcd(dg_ram(0x048)) + 100 * bcd(dg_ram(0x049));
        int hr  = dg_ram(0x04a) + 256 * dg_ram(0x04b);
        double rom_s = hr * 3600.0 + mn * 60.0 + sec;

        if (s % 60 == 0) {
            printf("s=%5d  raw %02X %02X | %02X %02X | %02X %02X  -> %d:%02d:%02d  rom=%8.1f  delta=%+7.1f  ratio=%+.4f\n",
                   s, dg_ram(0x046), dg_ram(0x047), dg_ram(0x048), dg_ram(0x049),
                   dg_ram(0x04a), dg_ram(0x04b), hr, mn, sec, rom_s,
                   rom_s - prev_rom, (rom_s - prev_rom) / 60.0);
            prev_rom = rom_s;
            fflush(stdout);
        }
        prev_sec_val = sec;
    }
    (void)prev_sec_val;
    dump("end ", seconds);
    return 0;
}
