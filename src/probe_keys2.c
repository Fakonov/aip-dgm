/* Probe 6: (a) does B enter the highlighted function?  (b) minimum hold time
 * for a key press to be seen by the ROM?
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];
static const int ICON_B[8]   = {137, 155, 157, 159, 22, 20, 18, 0};
static const int ICON_BIT[8] = {3,   3,   3,   3,   0,  0,  0,  0};

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n; long g = 0;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) { dg_step(); if (++g > 400000000L) return; }
}
static void press(uint8_t m, uint32_t hold, uint32_t gap)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(gap);
}
static void show(const char *tag)
{
    const uint8_t *v = dg_vram(); uint32_t len = dg_vram_len();
    char b[9]; int on = 0;
    for (int i = 0; i < 8; i++) {
        int x = ((uint32_t)ICON_B[i] < len) ? (v[ICON_B[i]] >> ICON_BIT[i]) & 1 : 0;
        b[i] = x ? '1' : '0'; on += x;
    }
    b[8] = 0;
    printf("%-22s mode=%X sub=%X idx=%X bits=%s on=%d\n", tag,
           dg_ram(0x009), dg_ram(0x00A), dg_ram(0x015), b, on);
    fflush(stdout);
}
static void boot_and_hatch(void)
{
    const uint32_t SEC = 32768;
    run_ticks(SEC * 2);
    press(1, 8192, 8192); press(4, 4096, 4096); press(1, 4096, 4096); press(2, 8192, 8192);
    press(4, 8192, 8192); press(1, 8192, 8192); press(2, 8192, 8192);
    run_ticks(SEC * 6 * 60);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    size_t n = fread(rom, 1, sizeof rom, f); fclose(f);
    dg_init(rom, (uint32_t)n);
    boot_and_hatch();

    printf("=== (a) A to mode 2, then B, then C ===\n");
    show("hatched");
    press(4, 20000, 20000); show("A -> icon 1");
    press(2, 20000, 20000); show("B (enter?)");
    press(1, 20000, 20000); show("C (cancel?)");
    press(1, 20000, 20000); show("C again");

    printf("\n=== (b) minimum hold: A press while at home ===\n");
    /* back to home: press A 7 more times to wrap */
    for (int i = 0; i < 7; i++) press(4, 20000, 20000);
    show("home?");
    const uint32_t holds[] = {2000, 4000, 6000, 8192, 12000, 16000, 20000};
    for (int i = 0; i < 7; i++) {
        press(4, holds[i], 20000);
        char t[64]; snprintf(t, sizeof t, "hold %u ticks (%u ms)",
                             (unsigned)holds[i], (unsigned)(holds[i] * 1000 / 32768));
        show(t);
        /* reset to home if we left it: C then 7xA wrap */
        if (dg_ram(0x009) != 0) {
            press(1, 20000, 20000);
            for (int k = 0; k < 7; k++) press(4, 20000, 20000);
        }
    }
    printf("\ndone instr=%u\n", dg_instr_count());
    return 0;
}
