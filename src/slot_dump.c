/* slot_dump.c -- render the home-screen sprite for each species slot.
 *
 *   gcc -O2 -o slot_dump.exe slot_dump.c digimon_core.c
 *   ./slot_dump.exe <rom.bin> [start] [end]
 *
 * Used to map V4's 0..14 slots to the cards on the printed growth chart when
 * the ROM has no text labels.  The segment map is the shared V1/V2/V3 one.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"

static uint8_t rom[65536];

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) dg_step();
}

static void press(uint8_t m, uint32_t hold)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(hold);
}

static void boot(void)
{
    dg_reset();
    run_ticks(32768 * 2);
    press(1, 8192); press(4, 4096); press(1, 4096); press(2, 8192);
    if (dg_ram(0x009) == 1) { press(4, 8192); press(1, 8192); press(2, 8192); }
    for (int i = 0; i < 200 && dg_ram(0x00B) == 0; i++) run_ticks(32768 * 30);
}

static void show(int slot)
{
    const uint8_t *v = dg_vram();
    uint32_t len = dg_vram_len();
    printf("\n--- slot %X (mode=%X) ---\n", slot, dg_ram(0x009));
    for (int r = 0; r < DG_LCD_H; r++) {
        putchar(' ');
        for (int c = 0; c < DG_LCD_W; c++) {
            uint16_t cell = dg_segmap[r][c];
            uint8_t b = cell & 0xFF, bit = cell >> 8;
            putchar((b < len && ((v[b] >> bit) & 1)) ? '#' : '.');
        }
        putchar('\n');
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: slot_dump <rom.bin> [start] [end]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "no rom\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);
    boot();

    int start = argc > 2 ? (int)strtol(argv[2], NULL, 0) : 1;
    int end = argc > 3 ? (int)strtol(argv[3], NULL, 0) : 14;
    for (int slot = start; slot <= end; slot++) {
        /* park on the home screen, force the slot, let the animation task
         * draw one full frame; keep the rest of the RAM as-is */
        dg_ram_write(0x009, 0);
        dg_ram_write(0x00A, 0);
        dg_ram_write(0x00B, (uint8_t)slot);
        dg_ram_write(0x00C, 0);
        run_ticks(32768 * 2);
        show(slot);
    }
    return 0;
}
