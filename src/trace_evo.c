/* trace_evo.c -- single-step the evolution_check loop so the operand order in
 * 0x0596..0x05AB is read from the machine, not guessed from the listing.
 *
 * gcc -O2 -o trace_evo.exe trace_evo.c digimon_core.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "digimon_core.h"

static uint8_t rom[65536];
static const uint32_t SEC = 32768;

static void run_ticks(uint32_t n)
{
    uint32_t t = dg_osc1_ticks() + n;
    while ((int32_t)(dg_osc1_ticks() - t) < 0) dg_step();
}
static void press(uint8_t m, uint32_t hold)
{
    dg_set_button(m, 1); run_ticks(hold);
    dg_set_button(m, 0); run_ticks(hold);
}
static void write2(int lo, int hi, int v)
{
    dg_ram_write(lo, (uint8_t)(v % 10));
    dg_ram_write(hi, (uint8_t)((v / 10) % 10));
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    dg_init(rom, (uint32_t)fread(rom, 1, sizeof rom, f));
    fclose(f);

    run_ticks(SEC * 2);
    press(1, 8192); press(4, 4096); press(1, 4096); press(2, 8192);
    if (dg_ram(0x009) == 1) { press(4, 8192); press(1, 8192); press(2, 8192); }
    for (int i = 0; i < 200 && dg_ram(0x00B) == 0; i++) run_ticks(SEC * 30);

    int nh = dg_ram(0x4E) | (dg_ram(0x4F) << 4);
    int nm = dg_ram(0x4D) * 10 + dg_ram(0x4C);
    int target = nh * 60 + nm;
    printf("next=%d:%02d -> parking stage at %d:%02d\n", nh, nm,
           (target - 2) / 60, (target - 2) % 60);
    write2(0x48, 0x49, (target - 2) % 60);
    dg_ram_write(0x4A, (uint8_t)(((target - 2) / 60) & 0xF));
    dg_ram_write(0x4B, (uint8_t)(((target - 2) / 60) >> 4));

    uint16_t r[8];
    int lines = 0;
    long guard = 0;
    while (lines < 60 && guard < 400000000L) {
        uint16_t pc = dg_pc();
        if (pc >= 0x0596 && pc <= 0x05AC) {
            dg_dbg_state(r);
            printf("%04X A=%X B=%X IX=%03X IY=%03X | 48=%X 49=%X 4A=%X 4B=%X "
                   "4C=%X 4D=%X 4E=%X 4F=%X slot=%X mode=%X\n",
                   pc, r[0], r[1], r[2], r[3],
                   dg_ram(0x48), dg_ram(0x49), dg_ram(0x4A), dg_ram(0x4B),
                   dg_ram(0x4C), dg_ram(0x4D), dg_ram(0x4E), dg_ram(0x4F),
                   dg_ram(0x00B), dg_ram(0x009));
            lines++;
        }
        dg_step();
        guard++;
    }
    printf("guard=%ld lines=%d slot=%X mode=%X\n", guard, lines,
           dg_ram(0x00B), dg_ram(0x009));
    return 0;
}
