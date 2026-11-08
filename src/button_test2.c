#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];
static void run(int n){ for (int i=0;i<n;i++) dg_step(); }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no rom\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f);
    fclose(f);
    dg_init(rom, (uint32_t)n);
    run(200000);

    printf("baseline   pc=%u ik0=%d\n", dg_pc(), dg_ik0_pending());

    /* press TOP (mask 4) and look for the K0 interrupt latch + vector jump */
    dg_set_button(4, 1);
    int seen_irq = 0, seen_vector = 0;
    for (int i = 0; i < 400; i++) {
        dg_step();
        if (dg_ik0_pending()) seen_irq = 1;
        uint16_t pc = dg_pc();
        if (pc >= 0x100 && pc <= 0x120) seen_vector = 1;
    }
    printf("A pressed  pc=%u ik0=%d  irq_seen=%d vector_seen=%d\n",
           dg_pc(), dg_ik0_pending(), seen_irq, seen_vector);

    dg_set_button(4, 0);
    run(400);
    printf("A released pc=%u ik0=%d\n", dg_pc(), dg_ik0_pending());

    /* same for CENTRE (mask 2) */
    dg_set_button(2, 1);
    seen_irq = 0; seen_vector = 0;
    for (int i = 0; i < 400; i++) {
        dg_step();
        if (dg_ik0_pending()) seen_irq = 1;
        uint16_t pc = dg_pc();
        if (pc >= 0x100 && pc <= 0x120) seen_vector = 1;
    }
    printf("B pressed  pc=%u ik0=%d  irq_seen=%d vector_seen=%d\n",
           dg_pc(), dg_ik0_pending(), seen_irq, seen_vector);
    dg_set_button(2, 0);
    return 0;
}
