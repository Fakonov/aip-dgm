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
static uint32_t hashpic(void){ grab(); uint32_t h=2166136261u;
  for(int r=0;r<DG_LCD_H;r++) for(int c=0;c<DG_LCD_W;c++) h=(h^pic[r][c])*16777619u; return h; }
static int lit(void){ int n=0; for(int r=0;r<DG_LCD_H;r++) for(int c=0;c<DG_LCD_W;c++) n+=pic[r][c]; return n; }
static int rows(void){ int n=0; for(int r=0;r<DG_LCD_H;r++){ for(int c=0;c<DG_LCD_W;c++) if(pic[r][c]){n++;break;} } return n; }
static void ticks(uint32_t n){ uint32_t t=dg_osc1_ticks()+n; while((int32_t)(dg_osc1_ticks()-t)<0) dg_step(); }
static void press(uint8_t m){ dg_set_button(m,1); ticks(8192); dg_set_button(m,0); ticks(8192); }
static void show(const char *tag){
    grab();
    printf("--- %-18s pc=%-5u lit=%-3d rows=%-2d\n", tag, dg_pc(), lit(), rows());
    for(int r=0;r<DG_LCD_H;r++){ printf("   "); for(int c=0;c<DG_LCD_W;c++) putchar(pic[r][c]?'#':'.'); putchar('\n'); }
}
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    const uint32_t SEC=32768;
    ticks(SEC*2); press(2); press(1); ticks(SEC*60*12);   /* setup + hatch */

    show("main");
    uint32_t base = hashpic();
    /* try every 1- and 2-press sequence over A(4) B(2) C(1) */
    uint8_t keys[3] = {4,2,1};
    const char *kn = "ABC";
    for(int len=1; len<=2; len++){
        for(int i=0;i<3;i++){
            if(len==1){
                dg_init(rom,(uint32_t)n);
                ticks(SEC*2); press(2); press(1); ticks(SEC*60*12);
                press(keys[i]);
                char tag[24]; snprintf(tag,sizeof tag,"%c",kn[i]);
                if(hashpic()!=base){ show(tag); }
                else printf("--- %-10s (no change)\n", tag);
            } else {
                for(int j=0;j<3;j++){
                    dg_init(rom,(uint32_t)n);
                    ticks(SEC*2); press(2); press(1); ticks(SEC*60*12);
                    press(keys[i]); press(keys[j]);
                    char tag[24]; snprintf(tag,sizeof tag,"%c%c",kn[i],kn[j]);
                    if(hashpic()!=base){ show(tag); }
                    else printf("--- %-10s (no change)\n", tag);
                }
            }
        }
    }
    return 0;
}
