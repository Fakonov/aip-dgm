/* Probe 5: which key actually advances the function-icon selection?
 * After hatching, from home (mode=0), press each mask 8 times with a longer
 * hold (0.6 s) and gap, logging mode + the 8 icon segment bits.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];
static const int ICON_B[8]   = {0, 18, 20, 22, 137, 155, 157, 159};
static const int ICON_BIT[8] = {0,  0,  0,  0,   3,   3,   3,   3};

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    long g = 0;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) { dg_step(); if (++g > 400000000L) return; }
}
static void press(uint8_t m, uint32_t hold, uint32_t gap)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(gap);
}
static void show(const char *tag)
{
    const uint8_t *v = dg_vram();
    uint32_t len = dg_vram_len();
    int on = 0;
    char bits[9];
    for (int i = 0; i < 8; i++) {
        int b = ((uint32_t)ICON_B[i] < len) ? (v[ICON_B[i]] >> ICON_BIT[i]) & 1 : 0;
        bits[i] = b ? '1' : '0';
        on += b;
    }
    bits[8] = 0;
    printf("%-16s mode=%X sub=%X idx=%X bits=%s on=%d\n",
           tag, dg_ram(0x009), dg_ram(0x00A), dg_ram(0x015), bits, on);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    size_t n = fread(rom, 1, sizeof rom, f); fclose(f);
    dg_init(rom, (uint32_t)n);

    const uint32_t SEC = 32768;
    run_ticks(SEC * 2);
    press(1, 8192, 8192); press(4, 4096, 4096); press(1, 4096, 4096); press(2, 8192, 8192);
    press(4, 8192, 8192); press(1, 8192, 8192); press(2, 8192, 8192);
    run_ticks(SEC * 6 * 60);
    show("hatched");

    const char *names[3] = {"mask4 (UP/A)", "mask2 (DOWN/B)", "mask1 (OK/C)"};
    const uint8_t masks[3] = {4, 2, 1};
    for (int k = 0; k < 3; k++) {
        printf("\n=== %s: 8 presses, 0.6 s hold / 0.6 s gap ===\n", names[k]);
        show("start");
        for (int i = 0; i < 8; i++) {
            press(masks[k], 20000, 20000);
            char t[24]; snprintf(t, sizeof t, "press %d", i + 1);
            show(t);
        }
        /* back home for the next key: C clears the selection (mask 1 = C) */
        press(1, 20000, 20000);
        show("after C");
    }
    printf("\nlast: pc=%u instr=%u\n", dg_pc(), dg_instr_count());
    return 0;
}
