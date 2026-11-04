#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
static uint8_t rom[65536];
static const int ICONS[7] = {0,20,22,137,155,157,159};
static int last[7];
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    const uint32_t SEC=32768;
    /* boot, setup, hatch */
    dg_set_button(2,1); { uint32_t t=dg_osc1_ticks()+8192; while((int32_t)(dg_osc1_ticks()-t)<0) dg_step(); } dg_set_button(2,0);
    { uint32_t t=dg_osc1_ticks()+8192; while((int32_t)(dg_osc1_ticks()-t)<0) dg_step(); }
    dg_set_button(1,1); { uint32_t t=dg_osc1_ticks()+8192; while((int32_t)(dg_osc1_ticks()-t)<0) dg_step(); } dg_set_button(1,0);

    for(int i=0;i<7;i++) last[i]=-1;
    int changes = 0;
    /* 90 minutes of game time, sample every second */
    for(int s=0; s<90*60; s++){
        uint32_t t=dg_osc1_ticks()+SEC;
        while((int32_t)(dg_osc1_ticks()-t)<0) dg_step();
        const uint8_t *v=dg_vram();
        for(int i=0;i<7;i++){
            int b = v[ICONS[i]];
            if(b != last[i]){
                printf("t=%3d:%02d  byte[%3d] = 0x%02X  (was 0x%02X)\n", s/60, s%60, ICONS[i], b, last[i]<0?0:last[i]);
                last[i]=b; changes++;
            }
        }
        if(changes > 40){ printf("... (stopping after 40 changes)\n"); break; }
    }
    if(changes==0) printf("NO CHANGE: none of bytes 0,20,22,137,155,157,159 ever became non-zero in 90 min\n");
    return 0;
}
