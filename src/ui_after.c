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
static void show(const char *tag){
    grab();
    printf("--- %-24s pc=%-5u t=%u min\n", tag, dg_pc(), dg_osc1_ticks()/32768/60);
    for(int r=0;r<DG_LCD_H;r++){ printf("   "); for(int c=0;c<DG_LCD_W;c++) putchar(pic[r][c]?'#':'.'); putchar('\n'); }
}
static void ticks(uint32_t n){ uint32_t target=dg_osc1_ticks()+n; while((int32_t)(dg_osc1_ticks()-target)<0) dg_step(); }
static void press(uint8_t mask){ dg_set_button(mask,1); ticks(16384); dg_set_button(mask,0); ticks(16384); }

int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    const uint32_t SEC=32768;

    ticks(SEC*2); press(2); press(1);      /* enter time-set, confirm */
    ticks(SEC*600); show("hatched +10 min");

    for(int b=0;b<3;b++){
        press((uint8_t)(4>>b));
        char tag[40]; snprintf(tag,sizeof tag,"after %c (short)","ABC"[b]);
        show(tag);
    }
    /* try holding each button */
    for(int b=0;b<3;b++){
        uint8_t mask=(uint8_t)(4>>b);
        dg_set_button(mask,1); ticks(SEC); dg_set_button(mask,0); ticks(SEC/2);
        char tag[40]; snprintf(tag,sizeof tag,"after %c (1s hold)","ABC"[b]);
        show(tag);
    }
    return 0;
}
