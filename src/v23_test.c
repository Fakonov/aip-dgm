#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"

static uint8_t rom[65536];

static int run_rom(const char *path, const char *name, int steps)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s: cannot open\n", name); return 1; }
    size_t n = fread(rom, 1, sizeof(rom), f);
    fclose(f);

    dg_init(rom, (uint32_t)n);
    for (int i = 0; i < steps; i++) dg_step();

    const uint8_t *v = dg_vram();
    uint32_t vlen = dg_vram_len();
    int lit = 0;
    for (int r = 0; r < DG_LCD_H; r++)
        for (int c = 0; c < DG_LCD_W; c++) {
            uint16_t cell = dg_segmap[r][c];
            uint8_t b = cell & 0xFF, bit = cell >> 8;
            if (b < vlen && ((v[b] >> bit) & 1)) lit++;
        }

    printf("== %s  (%zu bytes)\n", name, n);
    printf("   pc=%u instr=%u vram_len=%u vram_nonzero=%d lit=%d\n",
           dg_pc(), dg_instr_count(), vlen,
           ({int c2=0; for(uint32_t i=0;i<vlen;i++) if(v[i]) c2++; c2;}), lit);
    for (int r = 0; r < DG_LCD_H; r++) {
        printf("   ");
        for (int c = 0; c < DG_LCD_W; c++) {
            uint16_t cell = dg_segmap[r][c];
            uint8_t b = cell & 0xFF, bit = cell >> 8;
            printf("%c", (b < vlen && ((v[b] >> bit) & 1)) ? '#' : '.');
        }
        printf("\n");
    }
    return lit > 0 ? 0 : 2;
}

int main(int argc, char **argv)
{
    if (argc < 4) { printf("usage: v23_test v1 v2 v3\n"); return 1; }
    run_rom(argv[1], "V1", 300000);
    run_rom(argv[2], "V2", 300000);
    run_rom(argv[3], "V3", 300000);
    return 0;
}
