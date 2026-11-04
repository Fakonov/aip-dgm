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
static void show(const char *tag){
    grab(); printf("--- %-20s pc=%-5u lit=%d\n", tag, dg_pc(), lit());
    for(int r=0;r<DG_LCD_H;r++){ printf("   "); for(int c=0;c<DG_LCD_W;c++) putchar(pic[r][c]?'#':'.'); putchar('\n'); }
}
static void ticks(uint32_t n){ uint32_t t=dg_osc1_ticks()+n; while((int32_t)(dg_osc1_ticks()-t)<0) dg_step(); }
static void press(uint8_t m, uint32_t hold){ dg_set_button(m,1); ticks(hold); dg_set_button(m,0); ticks(hold); }
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    const uint32_t SEC=32768;
    ticks(SEC*2); press(2,SEC/4); press(1,SEC/4);
    ticks(SEC*60*12);
    show("pet");
    for(int k=1;k<=8;k++){
        press(2, SEC/8);
        char tag[40]; snprintf(tag,sizeof tag,"B x%d", k); show(tag);
    }
    return 0;
}
