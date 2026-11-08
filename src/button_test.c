#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint32_t fnv(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void run(int steps) { for (int i = 0; i < steps; i++) dg_step(); }

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: button_test <rom>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open rom\n"); return 1; }
    static uint8_t rom[65536];
    size_t n = fread(rom, 1, sizeof(rom), f);
    fclose(f);

    dg_init(rom, (uint32_t)n);
    run(200000);
    uint32_t base = fnv(dg_vram(), (int)dg_vram_len());
    printf("baseline      pc=%5u vram=%08x\n", dg_pc(), base);

    /* button A (mask 4) held for a while */
    dg_set_button(4, 1);
    run(3000);
    uint32_t aDown = fnv(dg_vram(), (int)dg_vram_len());
    dg_set_button(4, 0);
    run(3000);
    uint32_t aUp = fnv(dg_vram(), (int)dg_vram_len());
    printf("after A press pc=%5u vram=%08x (%s)\n", dg_pc(), aDown, aDown != base ? "CHANGED" : "same");

    /* button B (mask 2) */
    dg_set_button(2, 1);
    run(3000);
    uint32_t bDown = fnv(dg_vram(), (int)dg_vram_len());
    dg_set_button(2, 0);
    run(3000);
    uint32_t bUp = fnv(dg_vram(), (int)dg_vram_len());
    printf("after B press pc=%5u vram=%08x (%s)\n", dg_pc(), bDown, bDown != aUp ? "CHANGED" : "same");

    /* button C (mask 1) */
    dg_set_button(1, 1);
    run(3000);
    uint32_t cDown = fnv(dg_vram(), (int)dg_vram_len());
    dg_set_button(1, 0);
    run(3000);
    printf("after C press pc=%5u vram=%08x (%s)\n", dg_pc(), cDown, cDown != bUp ? "CHANGED" : "same");

    /* let it run and see whether the picture keeps evolving on its own */
    run(200000);
    uint32_t later = fnv(dg_vram(), (int)dg_vram_len());
    printf("after 200k    pc=%5u vram=%08x (%s)\n", dg_pc(), later, later != cDown ? "CHANGED" : "same");
    printf("instr total   %u\n", dg_instr_count());
    return 0;
}
