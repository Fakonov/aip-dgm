#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"
static uint8_t rom[65536];
static void run(int n){ for(int i=0;i<n;i++) dg_step(); }
static void watch(const char *tag)
{
    const uint8_t *v = dg_vram();
    printf("%-22s pc=%-5u | b0=%x b20=%x b22=%x b137=%x b155=%x b157=%x b159=%x\n",
        tag, dg_pc(), v[0], v[20], v[22], v[137], v[155], v[157], v[159]);
}
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); if(!f) return 1;
    size_t n=fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);
    run(50000);  watch("boot 50k");
    /* press each key and look at those bytes */
    for(int b=0;b<3;b++){
        uint8_t m=(uint8_t)(4>>b);
        dg_set_button(m,1); run(2000); dg_set_button(m,0); run(2000);
        char t[32]; snprintf(t,sizeof t,"after %c","ABC"[b]); watch(t);
    }
    run(400000); watch("+400k");
    /* walk the whole vram looking for any non-zero byte outside 2..153 */
    const uint8_t *v = dg_vram();
    printf("\nbytes outside 2..153 that are non-zero at this moment:\n");
    for(int i=0;i<169;i++){ if((i<2||i>153) && v[i]) printf("  byte[%d] = 0x%x\n", i, v[i]); }
    return 0;
}
