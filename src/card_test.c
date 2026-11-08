/* card_test.c -- host acceptance for the offline battle card (G2 / M3).
 *
 * Same skeleton as src/link_test.c, but the peer is *not* the PC: it is the
 * card logic that main/dg_cards.c runs inside the firmware.  Those two copies
 * must stay in sync -- this file proves the answer frames the card builds are
 * the ones the ROM accepts, with no USB, no relay and no second machine.
 *
 *   ROM frame #1  [effort][slot]   -> card answers with ITS slot/effort
 *   ROM frame #2  [ver][verdict]   -> card answers the opposite verdict
 *
 * The card here is deliberately "unusual" (slot 0xA, effort 3, V2) so a
 * hard-coded answer would still pass with slot 3 / effort 0 by accident.
 *
 *   gcc -O2 -I.. -o card_test.exe card_test.c digimon_core.c link_wave.c
 *   ./card_test.exe [rom.bin]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "digimon_core.h"
#include "link_wave.h"

static uint8_t rom[16384];
static dglink_wave g_w;

static const uint32_t SEC = 32768;

/* ---- the card under test ------------------------------------------------ */
static int g_card_slot = 0xA;
static int g_card_effort = 3;
static int g_card_version = 1;

/* Anything above ~20 ms lands after the ROM swings P2 back to input; below that
 * it drops the frame and resends its own A2 (see dg_cards.c). */
static const uint32_t VERDICT_DELAY = SEC * 50 / 1000;

static int g_frames_rx;
static int g_frames_tx;
static int g_pend;
static uint32_t g_pend_due;
static uint16_t g_pend_word;

static uint16_t g_answer1;
static uint16_t g_answer2;

static uint8_t bitrev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

/* same packing as dg_cards.c / link_test.c */
static uint16_t wire_frame(uint8_t d0, uint8_t d1)
{
    return (uint16_t)((uint16_t)bitrev8(d0) | ((uint16_t)bitrev8(d1) << 8));
}

static void card_on_rom_frame(uint16_t frame)
{
    uint8_t d0 = bitrev8((uint8_t)(frame & 0xFF));
    int b;

    g_frames_rx++;
    printf("  [rom] frame #%d %04X  (data %02X)\n", g_frames_rx, frame, d0);

    if (g_frames_rx == 1) {
        b = ((g_card_effort & 0xF) << 4) | (g_card_slot & 0xF);
        g_answer1 = wire_frame((uint8_t)b, (uint8_t)(~b));
        printf("  [card] own card slot=%X effort=%d -> %04X (data %02X %02X)\n",
               g_card_slot, g_card_effort, g_answer1, b, (~b) & 0xFF);
        g_pend_word = g_answer1;
        g_pend_due = dg_osc1_ticks();       /* card answers land at once */
        g_pend = 1;
    } else if (g_frames_rx == 2) {
        int res = d0 & 0x0F;
        int mine = (res == 1) ? 2 : 1;
        b = ((g_card_version & 0xF) << 4) | (mine & 0xF);
        g_answer2 = wire_frame((uint8_t)b, (uint8_t)(~b));
        printf("  [card] ROM verdict=%d -> answer %d  %04X (data %02X %02X) after %u ms\n",
               res, mine, g_answer2, b, (~b) & 0xFF,
               (unsigned)(VERDICT_DELAY * 1000 / 32768));
        g_pend_word = g_answer2;
        g_pend_due = dg_osc1_ticks() + VERDICT_DELAY;
        g_pend = 1;
    }
}

static void on_p2_write(int value)
{
    dglink_p2_write(&g_w, value);
}

/* ------------------------------------------------------------- plumbing -- */
static void run_ticks(uint32_t n)
{
    uint32_t target = dg_osc1_ticks() + n;
    while ((int32_t)(dg_osc1_ticks() - target) < 0) {
        uint32_t t0 = dg_osc1_ticks();
        dg_step();
        dglink_tick(&g_w, dg_osc1_ticks() - t0);
    }
}

static void press(uint8_t mask, uint32_t hold, uint32_t gap)
{
    dg_set_button(mask, 1);
    run_ticks(hold);
    dg_set_button(mask, 0);
    run_ticks(gap);
}

static void serve(void)
{
    uint32_t t0 = dg_osc1_ticks();
    uint16_t frame;
    dg_step();
    dglink_tick(&g_w, dg_osc1_ticks() - t0);
    if (g_pend && (int32_t)(dg_osc1_ticks() - g_pend_due) >= 0) {
        g_pend = 0;
        if (dglink_start_rx(&g_w, g_pend_word)) g_frames_tx++;
    }
    if (dglink_take_tx(&g_w, &frame)) card_on_rom_frame(frame);
}

static void boot_to_hatch(void)
{
    dg_reset();
    run_ticks(SEC * 2);
    press(1, 8192, 8192);
    press(4, 4096, 4096);
    press(1, 4096, 4096);
    press(2, 8192, 8192);
    if (dg_ram(0x009) == 1) {
        press(4, 8192, 8192);
        press(1, 8192, 8192);
        press(2, 8192, 8192);
    }
    for (int i = 0; i < 200 && dg_ram(0x00B) == 0; i++) run_ticks(SEC * 30);
}

static int next_min_total(void)
{
    return (dg_ram(0x4E) | (dg_ram(0x4F) << 4)) * 60 + dg_ram(0x4D) * 10 + dg_ram(0x4C);
}

static void set_stage(int minutes)
{
    int h = minutes / 60;
    int m = minutes % 60;
    dg_ram_write(0x48, (uint8_t)(m % 10));
    dg_ram_write(0x49, (uint8_t)((m / 10) % 10));
    dg_ram_write(0x4A, (uint8_t)(h & 0xF));
    dg_ram_write(0x4B, (uint8_t)((h >> 4) & 0xF));
}

static void evolve_once(int care)
{
    int from = dg_ram(0x00B);
    dg_ram_write(0x031, (uint8_t)care);
    dg_ram_write(0x032, 0);
    dg_ram_write(0x033, 0);
    dg_ram_write(0x034, 0);
    dg_ram_write(0x035, 0);
    set_stage(next_min_total() - 2);
    for (int s = 0; s < 200 && dg_ram(0x00B) == from; s++) run_ticks(SEC);
    for (int s = 0; s < 120 && dg_ram(0x009) != 0; s++) run_ticks(SEC);
    printf("  evolve: slot %X -> %X (care=%d)\n", from, dg_ram(0x00B), care);
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
        : "<home>\\digimon-passport\\firmware\\digimon-passport\\main\\roms\\digimon.bin";
    FILE *f = fopen(path, "rb");
    size_t n;
    uint32_t t0;
    int b1 = 0, b2 = 0, press_active = 0;
    uint32_t press_end = 0;
    int bouts_before, win_before;

    if (!f) {
        printf("cannot open ROM %s\n", path);
        return 1;
    }
    n = fread(rom, 1, sizeof rom, f);
    fclose(f);

    dg_init(rom, (uint32_t)n);
    dglink_reset(&g_w);
    dg_link_set_hook(on_p2_write);

    printf("M3 offline card test (dg_cards.c logic)\n");
    printf("  card: slot=%X effort=%d version=%d\n", g_card_slot, g_card_effort, g_card_version);

    boot_to_hatch();
    evolve_once(0);
    evolve_once(0);
    printf("  grown: slot=%X mode=%X\n", dg_ram(0x00B), dg_ram(0x009));

    for (int i = 0; i < 4; i++) {
        run_ticks(SEC);
        press(4, 20000, 30000);
    }
    printf("  icon page: mode=%X sub=%X\n", dg_ram(0x009), dg_ram(0x00A));

    bouts_before = dg_ram(0x099);
    win_before = dg_ram(0x098);

    t0 = dg_osc1_ticks();
    while ((uint32_t)(dg_osc1_ticks() - t0) < SEC * 40) {
        serve();
        if (!b1 && (uint32_t)(dg_osc1_ticks() - t0) > SEC / 5) {
            b1 = 1;
            dg_set_button(2, 1);
            press_active = 1;
            press_end = dg_osc1_ticks() + SEC / 3;
        } else if (b1 && !b2 && !press_active && dg_ram(0x00A) == 2 &&
                   (uint32_t)(dg_osc1_ticks() - t0) > SEC * 2) {
            b2 = 1;
            dg_set_button(2, 1);
            press_active = 1;
            press_end = dg_osc1_ticks() + SEC / 3;
            printf("  [host] second B: card battle starting\n");
        }
        if (press_active && (int32_t)(dg_osc1_ticks() - press_end) >= 0) {
            press_active = 0;
            dg_set_button(2, 0);
        }
    }

    printf("\n== result ==\n");
    printf("  frames from ROM : %d\n", g_frames_rx);
    printf("  frames from card: %d\n", g_frames_tx);
    printf("  card answer #1  : %04X\n", g_answer1);
    printf("  card answer #2  : %04X\n", g_answer2);
    printf("  bouts %X -> %X   win %X -> %X\n",
           bouts_before, dg_ram(0x099), win_before, dg_ram(0x098));
    printf("  mode=%X sub=%X  RAM 30..35 = %X %X %X %X %X %X\n",
           dg_ram(0x009), dg_ram(0x00A),
           dg_ram(0x30), dg_ram(0x31), dg_ram(0x32),
           dg_ram(0x33), dg_ram(0x34), dg_ram(0x35));

    /* What the ROM has to have accepted: two frames each way, the card frame
     * carrying our slot/effort, and the battle counter actually moving.  On a
     * 4-bit defeats counter 15 is "full", so accept "already 15" as well. */
    int card_ok = g_answer1 == wire_frame((uint8_t)((g_card_effort << 4) | g_card_slot),
                                          (uint8_t)~((g_card_effort << 4) | g_card_slot));
    int counter_moved = (dg_ram(0x099) != bouts_before) || (bouts_before == 0xF);

    if (g_frames_rx >= 2 && g_frames_tx >= 2 && card_ok && counter_moved) {
        printf("\nM3 OFFLINE CARD: PASS (ROM played a full battle against the card)\n");
        return 0;
    }
    printf("\nM3 OFFLINE CARD: FAIL (rx=%d tx=%d card_ok=%d counter=%d)\n",
           g_frames_rx, g_frames_tx, card_ok, counter_moved);
    return 2;
}
