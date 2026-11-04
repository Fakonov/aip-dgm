#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
static uint8_t rom[65536];
static const int ICONS[7] = {0,20,22,137,155,157,159};
static int last[7];
static int run_ticks(uint32_t n){
    uint32_t target = dg_osc1_ticks() + n;
    int guard = 0;
    while ((int32_t)(dg_osc1_ticks() - target) < 0) {
        dg_step();
        if (++guard > 2000000) return 0;   /* stalled */
    }
    return 1;
}
static void press(uint8_t m){ dg_set_button(m,1); run_ticks(8192); dg_set_button(m,0); run_ticks(8192); }
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    const uint32_t SEC=32768;
    run_ticks(SEC*2); press(2); press(1);
    for(int i=0;i<7;i++) last[i]=-1;
    int changes=0, stalls=0;
    for(int s=0; s<60*60; s++){
        if(!run_ticks(SEC)){ stalls++; break; }
        const uint8_t *v=dg_vram();
        for(int i=0;i<7;i++){
            int b=v[ICONS[i]];
            if(b!=last[i]){ printf("t=%d:%02d byte[%d]=0x%02X\n", s/60, s%60, ICONS[i], b); last[i]=b; changes++; fflush(stdout); }
        }
        if(changes>30) break;
    }
    printf("done: changes=%d stalls=%d\n", changes, stalls); fflush(stdout);
    return 0;
}
