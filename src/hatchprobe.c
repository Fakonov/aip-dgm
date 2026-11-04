#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"

static uint8_t rom[65536];
static uint8_t pic[DG_LCD_H][DG_LCD_W];
static void grab(void)
{
    const uint8_t *v = dg_vram(); uint32_t len = dg_vram_len();
    for(int r=0;r<DG_LCD_H;r++) for(int c=0;c<DG_LCD_W;c++){
        uint16_t cell = dg_segmap[r][c];
        uint8_t b = cell & 0xFF, bit = cell >> 8;
        pic[r][c] = (b < len && ((v[b]>>bit)&1)) ? 1 : 0;
    }
}
static void show(const char *tag)
{
    grab();
    printf("--- %s  pc=%u  (game time %u min)\n", tag, dg_pc(), dg_osc1_ticks()/32768/60);
    for(int r=0;r<DG_LCD_H;r++){
        printf("   ");
        for(int c=0;c<DG_LCD_W;c++) putchar(pic[r][c]?'#':'.');
        putchar('\n');
    }
}
int main(int argc, char **argv)
{
    FILE *f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    const uint32_t per_min = 32768u*60u;
    int marks[] = {10, 30, 55, 60, 61, 65};
    int last = 0;
    for (unsigned k = 0; k < sizeof marks/sizeof marks[0]; k++) {
        uint32_t target = dg_osc1_ticks() + per_min * (uint32_t)(marks[k]-last);
        while ((int32_t)(dg_osc1_ticks() - target) < 0) dg_step();
        last = marks[k];
        char tag[48]; snprintf(tag,sizeof tag,"%d min", marks[k]);
        show(tag);
    }
    return 0;
}
