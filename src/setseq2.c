#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"
static uint8_t rom[65536];
static uint8_t pic[DG_LCD_H][DG_LCD_W];
static void grab(void){
    const uint8_t *v=dg_vram(); uint32_t len=dg_vram_len();
    for(int r=0;r<DG_LCD_H;r++) for(int c=0;c<DG_LCD_W;c++){
        uint16_t cell=dg_segmap[r][c]; uint8_t b=cell&0xFF, bit=cell>>8;
        pic[r][c]=(b<len && ((v[b]>>bit)&1))?1:0;
    }
}
static int lit(void){ int n=0; for(int r=0;r<DG_LCD_H;r++) for(int c=0;c<DG_LCD_W;c++) n+=pic[r][c]; return n; }
static void ticks(uint32_t n){ uint32_t t=dg_osc1_ticks()+n; while((int32_t)(dg_osc1_ticks()-t)<0) dg_step(); }
static void press(uint8_t m){ dg_set_button(m,1); ticks(6000); dg_set_button(m,0); ticks(6000); }
static void show(const char *tag){
    grab(); printf("  %-14s PC=%-5u lit=%-3d\n", tag, dg_pc(), lit());
    for(int r=0;r<DG_LCD_H;r++){ printf("     "); for(int c=0;c<DG_LCD_W;c++) putchar(pic[r][c]?'#':'.'); putchar('\n'); }
}
/* try one setup sequence, then wait 12 min and report whether the picture changed */
static int try_seq(const char *name, const uint8_t *seq, int n){
    dg_init(rom,(uint32_t)((FILE*)0, 0));   /* placeholder */
    return 0;
}
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    const uint32_t SEC=32768;

    /* candidate sequences to finish the clock setup */
    struct { const char *name; uint8_t seq[6]; int n; } cases[] = {
        {"DOWN OK",           {2,1},           2},
        {"DOWN OK OK",        {2,1,1},         3},
        {"DOWN OK OK OK",     {2,1,1,1},       4},
        {"DOWN UP OK",        {4,2,1},         3},
        {"DOWN A A OK",       {2,4,4,1},       4},
        {"DOWN B B OK",       {2,2,2,1},       4},
        {"DOWN OK A OK",      {2,1,4,1},       4},
    };
    int ncases = sizeof(cases)/sizeof(cases[0]);
    for(int k=0;k<ncases;k++){
        dg_init(rom,(uint32_t)n);
        ticks(SEC*2);
        for(int i=0;i<cases[k].n;i++) press(cases[k].seq[i]);
        grab(); int before = lit();
        uint32_t h0=0; { const uint8_t *v=dg_vram(); for(int i=0;i<160;i++) h0=h0*31+v[i]; }
        ticks(SEC*60*12);
        grab(); int after = lit();
        uint32_t h1=0; { const uint8_t *v=dg_vram(); for(int i=0;i<160;i++) h1=h1*31+v[i]; }
        printf("[%-16s] lit %d -> %d   %s\n", cases[k].name, before, after,
               (h0!=h1) ? "CHANGED (hatched?)" : "no change");
    }
    printf("\n=== showing the best candidate in detail ===\n");
    dg_init(rom,(uint32_t)n);
    ticks(SEC*2);
    press(2); press(1); press(1); press(1);
    show("after DOWN+OKx3");
    ticks(SEC*60*12);
    show("+12 min");
    return 0;
}
