#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
#include "digimon_segmap.h"
static uint8_t rom[65536];
static void ticks(uint32_t n){ uint32_t t=dg_osc1_ticks()+n; while((int32_t)(dg_osc1_ticks()-t)<0) dg_step(); }
static void press(uint8_t m){ dg_set_button(m,1); ticks(8192); dg_set_button(m,0); ticks(8192); }
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    const uint32_t SEC=32768;
    ticks(SEC*2); press(2); press(1); ticks(SEC*60*12);
    const uint8_t *v=dg_vram(); uint32_t len=dg_vram_len();
    for(int r=0;r<DG_LCD_H;r++){
        for(int c=0;c<DG_LCD_W;c++){
            uint16_t cell=dg_segmap[r][c]; uint8_t b=cell&0xFF, bit=cell>>8;
            putchar((b<len && ((v[b]>>bit)&1))?'#':'.');
        }
        putchar('\n');
    }
    return 0;
}
