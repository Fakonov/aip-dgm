/* timeskip_probe2.c -- ????????????????RAM ??????"
 *
 * ??????RAM ??nibble ?????CD ???/??nibble ?????????? *   clock : 1A=??? 1B=??? 1C=??? 1D=??? 1E=??1F=PM
 *   stage : 46=??? 47=??? 48=??? 49=??? 4A=???? 4B=????
 *   next  : 4C=??? 4D=??? 4E=?? *
 * ???????? *   1) ??stage ?????? next_evolution ?????OM ??????????? *   2) ??clock ?????? ROM ??????????????/????? *
 *   gcc -O2 -o timeskip_probe2.exe timeskip_probe2.c digimon_core.c
 *   ./timeskip_probe2.exe digimon.bin
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
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

static int stage_min(void)  { return dg_ram(0x49) * 10 + dg_ram(0x48); }
static int stage_hour(void) { return dg_ram(0x4B) * 10 + dg_ram(0x4A); }
static int next_min(void)   { return dg_ram(0x4D) * 10 + dg_ram(0x4C); }
static int next_hour(void)  { return dg_ram(0x4E); }
static int clock_min(void)  { return dg_ram(0x1D) * 10 + dg_ram(0x1C); }
static int clock_hour(void) { return dg_ram(0x1E); }

static void dump(const char *tag)
{
    printf("%-18s slot=%X mode=%X stage=%d:%02d:%X%X next=%d:%02d clock=%d:%02d%s "
           "call=%X%X%X poop=%X%X%X care=%X\n",
           tag, dg_ram(0x00B), dg_ram(0x009),
           stage_hour(), stage_min(), dg_ram(0x47), dg_ram(0x46),
           next_hour(), next_min(),
           clock_hour(), clock_min(), dg_ram(0x1F) ? "PM" : "AM",
           dg_ram(0x42), dg_ram(0x41), dg_ram(0x40),
           dg_ram(0x82), dg_ram(0x81), dg_ram(0x80),
           dg_ram(0x031));
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: timeskip_probe2 <rom.bin>\n"); return 1; }
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

    /* ?????*/
    while (dg_ram(0x00B) == 0) run_ticks(SEC * 30);
    dump("hatched");

    run_ticks(SEC * 60);
    dump("+1min");

    /* ---- 实验 1：把 stage 设到 next 前 1 分钟，看 ROM 是否在"跨过"那一刻进化 ---- */
    int nh = next_hour(), nm = next_min();
    int pm2 = (nm + 59) % 60;
    int ph2 = (pm2 > nm) ? (nh + 11) % 12 : nh;
    printf("\n== setting stage %d:%02d -> %d:%02d (1 min before next) ==\n",
           stage_hour(), stage_min(), ph2, pm2);
    dg_ram_write(0x4A, (uint8_t)(ph2 % 10));
    dg_ram_write(0x4B, (uint8_t)(ph2 / 10));
    dg_ram_write(0x48, (uint8_t)(pm2 % 10));
    dg_ram_write(0x49, (uint8_t)(pm2 / 10));
    dump("injected");
    for (int s = 0; s < 180; s++) {
        run_ticks(SEC);
        if (s % 20 == 0) dump("  tick");
        if (dg_ram(0x00B) != 1) { printf(">>> slot changed to %X at %d s\n", dg_ram(0x00B), s); break; }
    }
    dump("after force");

    /* ---- ??? 2?????clock??????/?????? ---- */
    int h0 = clock_hour();
    int pm0 = dg_ram(0x1F);
    printf("\n== pushing clock from %d:%02d %s by +3h ==\n", h0, clock_min(), pm0 ? "PM" : "AM");
    for (int i = 0; i < 180; i++) {           /* +180 ??????????????????????*/
        int m = clock_min() + 1;
        int h = clock_hour();
        if (m >= 60) { m = 0; h++; if (h > 12) { h = 1; dg_ram_write(0x1F, dg_ram(0x1F) ? 0 : 1); } }
        dg_ram_write(0x1C, (uint8_t)(m % 10));
        dg_ram_write(0x1D, (uint8_t)(m / 10));
        dg_ram_write(0x1E, (uint8_t)h);
        run_ticks(SEC);                       /* ??ROM ??? 1 ??*/
    }
    dump("after clock jump");
    printf("sleep flag(0x54)=%X  status(0x05)=%X\n", dg_ram(0x054), dg_ram(0x005));
    return 0;
}
