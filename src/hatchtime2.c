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
static uint32_t ph(void){ grab(); uint32_t h=2166136261u;
    for(int r=0;r<DG_LCD_H;r++) for(int c=0;c<DG_LCD_W;c++) h=(h^pic[r][c])*16777619u; return h; }
static void show(const char *tag){
    grab(); printf("--- %-30s pc=%-5u t=%.1f min\n", tag, dg_pc(), dg_osc1_ticks()/32768.0/60.0);
    for(int r=0;r<DG_LCD_H;r++){ printf("   "); for(int c=0;c<DG_LCD_W;c++) putchar(pic[r][c]?'#':'.'); putchar('\n'); }
}
static void ticks(uint32_t n){ uint32_t t=dg_osc1_ticks()+n; while((int32_t)(dg_osc1_ticks()-t)<0) dg_step(); }
static void press(uint8_t m){ dg_set_button(m,1); ticks(8192); dg_set_button(m,0); ticks(8192); }

int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    const uint32_t SEC=32768;

    ticks(SEC*2); show("boot");
    press(2);            /* DOWN: enter clock setup */
    show("after DOWN");
    press(4);            /* UP: hour */
    press(1);            /* OK: confirm */
    show("after setup done");

    uint32_t last = ph();
    printf("\n=== watching up to 90 min of game time ===\n");
    for(int s=1;s<=180;s++){
        ticks(SEC*30);
        uint32_t h=ph();
        if(h!=last){ last=h;
            char tag[48]; snprintf(tag,sizeof tag,"change at %.1f min", s*0.5);
            show(tag);
            if(s*0.5 > 70) break;
        }
    }
    return 0;
}
