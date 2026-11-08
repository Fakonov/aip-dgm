/* ram_watch.c -- ?????? ROM ?????nibble????????????
 *
 * ?????ppendix-ram.md ???????????????????nibble ???????BCD ???? * ?????????????????????????????state????????????????????
 * ?????nibble ???????? *
 *   gcc -O2 -o ram_watch.exe ram_watch.c digimon_core.c
 *   ./ram_watch.exe digimon.bin [seconds]
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "digimon_core.h"

static uint8_t rom[65536];

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    long guard = 0;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) {
        dg_step();
        if (++guard > 2000000000L) { printf("guard\n"); return; }
    }
}

static void press(uint8_t m, uint32_t hold)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(hold);
}

/* ???????????????nibble */
static void row(const char *tag, int from, int to)
{
    printf("%-10s", tag);
    for (int a = from; a <= to; a++) printf(" %X", dg_ram(a));
    printf("   [%02X..%02X]\n", from, to);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: ram_watch <rom.bin> [seconds]\n"); return 1; }
    int total = argc > 2 ? atoi(argv[2]) : 180;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no rom\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);

    const uint32_t SEC = 32768;
    const uint8_t A_TOP = 4, B_OK = 2, C_BOT = 1;

    run_ticks(SEC * 2);
    press(C_BOT, 8192); press(A_TOP, 4096); press(C_BOT, 4096); press(B_OK, 8192);
    if (dg_ram(0x009) == 1) { press(A_TOP, 8192); press(C_BOT, 8192); press(B_OK, 8192); }

    printf("header          ");
    for (int a = 0x40; a <= 0x53; a++) printf(" %X", a & 0xF);
    printf("\n");

    for (int s = 0; s < total; s++) {
        char tag[16];
        snprintf(tag, sizeof tag, "t=%3ds", s);
        row(tag, 0x40, 0x4F);
        if (s % 10 == 0) {
            row("   stage2", 0x50, 0x53);
            row("   poop", 0x80, 0x83);
            row("   clock", 0x1A, 0x1F);
            printf("   slot=%X mode=%X\n", dg_ram(0x00B), dg_ram(0x009));
        }
        run_ticks(SEC);
    }
    return 0;
}
