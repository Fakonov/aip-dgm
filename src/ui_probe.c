#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"

static uint8_t rom[65536];
static void run(int n){ for(int i=0;i<n;i++) dg_step(); }

static void grab(uint8_t pic[DG_LCD_H][DG_LCD_W])
{
    const uint8_t *v = dg_vram(); uint32_t len = dg_vram_len();
    for(int r=0;r<DG_LCD_H;r++) for(int c=0;c<DG_LCD_W;c++){
        uint16_t cell = dg_segmap[r][c];
        uint8_t b = cell & 0xFF, bit = cell >> 8;
        pic[r][c] = (b < len && ((v[b]>>bit)&1)) ? 1 : 0;
    }
}
static void show(const char *tag, uint8_t pic[DG_LCD_H][DG_LCD_W])
{
    printf("--- %s (pc=%u)\n", tag, dg_pc());
    for(int r=0;r<DG_LCD_H;r++){
        printf("   ");
        for(int c=0;c<DG_LCD_W;c++) putchar(pic[r][c]?'#':'.');
        putchar('\n');
    }
}
static int diff(uint8_t a[DG_LCD_H][DG_LCD_W], uint8_t b[DG_LCD_H][DG_LCD_W])
{
    int n=0; for(int r=0;r<DG_LCD_H;r++) for(int c=0;c<DG_LCD_W;c++) if(a[r][c]!=b[r][c]) n++;
    return n;
}

int main(int argc, char **argv)
{
    FILE *f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    run(300000);

    uint8_t cur[DG_LCD_H][DG_LCD_W], prev[DG_LCD_H][DG_LCD_W];
    grab(cur); show("idle", cur);

    for(int b=0;b<3;b++){
        uint8_t mask=(uint8_t)(4>>b);
        memcpy(prev,cur,sizeof cur);
        dg_set_button(mask,1);
        run(4000);
        dg_set_button(mask,0);
        run(4000);
        grab(cur);
        char tag[32]; snprintf(tag,sizeof tag,"after pressing %c","ABC"[b]);
        printf("\n%s : %d pixels changed\n", tag, diff(prev,cur));
        show(tag, cur);
    }
    return 0;
}
