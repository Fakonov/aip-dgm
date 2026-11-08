#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];
static void run(int n){ for (int i=0;i<n;i++) dg_step(); }

static void dump_extra(const char *tag)
{
    const uint8_t *v = dg_vram();
    printf("%-14s pc=%-5u  b0=%x b1=%x | b154..159 = %x %x %x %x %x %x\n",
           tag, dg_pc(), v[0], v[1], v[154], v[155], v[156], v[157], v[158], v[159]);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if(!f){ printf("no rom\n"); return 1; }
    size_t n = fread(rom,1,sizeof rom,f); fclose(f);
    dg_init(rom,(uint32_t)n);

    for (int k = 0; k < 12; k++) {
        run(200000);
        char tag[32]; snprintf(tag,sizeof tag,"t=%d.%ds", (k+1)*200000/4700, 0);
        dump_extra(tag);
    }

    printf("\n--- pressing A / B / C in turn, watching the extra bytes ---\n");
    for (int b = 0; b < 3; b++) {
        uint8_t mask = (uint8_t)(4 >> b);
        dg_set_button(mask, 1);
        run(3000);
        char tag[32]; snprintf(tag,sizeof tag,"press %c", "ABC"[b]);
        dump_extra(tag);
        dg_set_button(mask, 0);
        run(3000);
        dump_extra("  release");
    }
    return 0;
}
