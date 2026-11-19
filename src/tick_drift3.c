/* tick_drift3: measure the ROM's OWN wall clock (page-0 nibbles 0x01A..0x01F,
 * the HH:MM:SS the owner sees on the clock screen) against emulated time, using
 * the firmware's exact per-frame tick budget.
 *
 * digimon_app.c:417-451 clock map:
 *   0x01A sec-lo  0x01B sec-hi  0x01C min-lo  0x01D min-hi
 *   0x01E hour(1-12)  0x01F PM flag
 * Each nibble byte holds one decimal digit.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "digimon_core.h"

static uint8_t rom[65536];

static int dig(uint8_t v) { return (v & 0x0f) + 10 * ((v >> 4) & 0x0f); }

static int c_sec(void)  { return dig(dg_ram(0x01a)) + 10 * dig(dg_ram(0x01b)); }
static int c_min(void)  { return dig(dg_ram(0x01c)) + 10 * dig(dg_ram(0x01d)); }
static int c_hour(void) { return dig(dg_ram(0x01e)); }
static int c_pm(void)   { return dg_ram(0x01f) & 0x0f; }

/* 12-hour clock -> seconds since 12:00 am, monotonic (handles 11:59 -> 12:00) */
static long clock_s(int hour, int min, int sec, int pm)
{
    int h = hour % 12;
    if (pm) h += 12;
    return (long)h * 3600 + min * 60 + sec;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: tick_drift3 <rom> [seconds]\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 1; }
    size_t n = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    printf("rom %s: %zu bytes\n", argv[1], n);

    dg_init(rom, (uint32_t)n);
    for (int i = 0; i < 400000; i++) dg_step();

    printf("clock @start: %02d:%02d:%02d %s  raw %02X %02X %02X %02X %02X %02X\n",
           c_hour(), c_min(), c_sec(), c_pm() ? "PM" : "AM",
           dg_ram(0x01a), dg_ram(0x01b), dg_ram(0x01c), dg_ram(0x01d),
           dg_ram(0x01e), dg_ram(0x01f));

    int seconds = (argc >= 3) ? atoi(argv[2]) : 3600;
    long base = clock_s(c_hour(), c_min(), c_sec(), c_pm());
    long prev = base;
    long total_rom = 0;
    uint32_t t0 = dg_osc1_ticks();

    for (int s = 1; s <= seconds; s++) {
        for (int f = 0; f < 50; f++) {
            uint32_t target = dg_osc1_ticks() + 655;
            int guard = 0;
            while ((int32_t)(dg_osc1_ticks() - target) < 0 && guard++ < 40000) dg_step();
        }
        long now = clock_s(c_hour(), c_min(), c_sec(), c_pm());
        if (now < prev) now += 12 * 3600;     /* 12-hour wrap */
        total_rom += now - prev;
        prev = now;

        if (s % 300 == 0) {
            printf("s=%5d  clock %02d:%02d:%02d %s  rom_delta=%+6ld  ratio=%+.4f  ticks/s=%.2f\n",
                   s, c_hour(), c_min(), c_sec(), c_pm() ? "PM" : "AM",
                   now - base > 0 ? now - base - (total_rom - (now - base)) : 0,
                   (double)total_rom / s, (double)(dg_osc1_ticks() - t0) / s);
            printf("        cumulative rom=%ld s over %d s -> ratio %.4f\n", total_rom, s, (double)total_rom / s);
            fflush(stdout);
        }
    }
    printf("end: %02d:%02d:%02d %s\n", c_hour(), c_min(), c_sec(), c_pm() ? "PM" : "AM");
    printf("ROM clock advanced %ld s over %d s emulated -> ratio %.4f  (%+.1f s per real hour)\n",
           total_rom, seconds, (double)total_rom / seconds,
           ((double)total_rom / seconds - 1.0) * 3600.0);
    printf("osc1 ticks advanced %u over %d s -> %.2f ticks/s (nominal 32768)\n",
           dg_osc1_ticks() - t0, seconds, (double)(dg_osc1_ticks() - t0) / seconds);
    return 0;
}
