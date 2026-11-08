/* link_test_responder.c -- M2 debugging: run the ROM as the *answering* side.
 *
 * The real device is in exactly this situation when the PC pushes frame 1 at
 * it: the ROM only pressed B once (sub=2), it never initiated.  Two exchanges
 * happen but the ROM stays in sub=2 instead of starting the battle animation,
 * so reproduce it here where iterations cost 2 s instead of a reflash.
 *
 *   gcc -O2 -I.. -o link_test_responder.exe link_test_responder.c digimon_core.c link_wave.c
 *   ./link_test_responder.exe [--a2 02] [--delay-ms 60]
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "digimon_core.h"
#include "link_wave.h"

static uint8_t rom[65536];
static const uint32_t SEC = 32768;
static dglink_wave g_w;

static int g_a2 = 0x02;
static int g_delay_ms = 60;
static int g_frames_rx, g_frames_tx;
static int g_sent_a2;
static uint32_t g_t_a2_due;
static int g_a2_pending;
static int g_peer_slot = 3, g_peer_effort = 0, g_peer_version = 0;

static uint8_t bitrev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

static uint16_t wire_frame(uint8_t d0, uint8_t d1)
{
    return (uint16_t)((uint16_t)bitrev8(d0) | ((uint16_t)bitrev8(d1) << 8));
}

static void on_p2(int value) { dglink_p2_write(&g_w, value); }

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
    int h = minutes / 60, m = minutes % 60;
    dg_ram_write(0x48, (uint8_t)(m % 10));
    dg_ram_write(0x49, (uint8_t)((m / 10) % 10));
    dg_ram_write(0x4A, (uint8_t)(h & 0xF));
    dg_ram_write(0x4B, (uint8_t)((h >> 4) & 0xF));
}

static void evolve_once(int care)
{
    int from = dg_ram(0x00B);
    dg_ram_write(0x031, (uint8_t)care);
    dg_ram_write(0x032, 0); dg_ram_write(0x033, 0);
    dg_ram_write(0x034, 0); dg_ram_write(0x035, 0);
    set_stage(next_min_total() - 2);
    for (int s = 0; s < 200 && dg_ram(0x00B) == from; s++) run_ticks(SEC);
    for (int s = 0; s < 120 && dg_ram(0x009) != 0; s++) run_ticks(SEC);
}

int main(int argc, char **argv)
{
    const char *path = "<home>\\digimon-passport\\firmware\\digimon-passport\\main\\roms\\digimon.bin";
    FILE *f;
    size_t n;
    uint32_t t0;
    int entered = 0;
    int last_sub = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--a2") == 0 && i + 1 < argc) g_a2 = (int)strtol(argv[++i], NULL, 16);
        else if (strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) g_delay_ms = atoi(argv[++i]);
        else path = argv[i];
    }

    f = fopen(path, "rb");
    if (!f) { printf("cannot open ROM %s\n", path); return 1; }
    n = fread(rom, 1, sizeof rom, f);
    fclose(f);

    dg_init(rom, (uint32_t)n);
    dglink_reset(&g_w);
    dg_link_set_hook(on_p2);

    boot_to_hatch();
    evolve_once(0);
    evolve_once(0);
    printf("grown: slot=%X mode=%X\n", dg_ram(0x00B), dg_ram(0x009));

    for (int i = 0; i < 4; i++) { run_ticks(SEC); press(4, 20000, 30000); }
    printf("icon page: mode=%X sub=%X\n", dg_ram(0x009), dg_ram(0x00A));

    /* ONE B press: enter the battle screen and wait (this is the answering side) */
    press(2, 20000, 20000);
    printf("after one B: mode=%X sub=%X  (want mode=5 sub=2)\n",
           dg_ram(0x009), dg_ram(0x00A));

    {
        int card = ((g_peer_effort << 4) | g_peer_slot) & 0xFF;
        printf("PC card slot=%d effort=%d -> frame1 %04X\n",
               g_peer_slot, g_peer_effort, wire_frame((uint8_t)card, (uint8_t)(~card & 0xFF)));
        dglink_start_rx(&g_w, wire_frame((uint8_t)card, (uint8_t)(~card & 0xFF)));
        g_frames_tx++;
    }

    t0 = dg_osc1_ticks();
    while ((uint32_t)(dg_osc1_ticks() - t0) < SEC * 12) {
        uint32_t a = dg_osc1_ticks();
        dg_step();
        dglink_tick(&g_w, dg_osc1_ticks() - a);

        {
            uint16_t f2;
            if (dglink_take_tx(&g_w, &f2)) {
                g_frames_rx++;
                printf("  t=%.2fs [rom] frame #%d %04X (data %02X %02X) mode=%X sub=%X\n",
                       (double)(dg_osc1_ticks() - t0) / SEC, g_frames_rx, f2,
                       bitrev8((uint8_t)(f2 & 0xFF)), bitrev8((uint8_t)(f2 >> 8)),
                       dg_ram(0x009), dg_ram(0x00A));
                if (g_frames_rx == 1 && !g_sent_a2) {
                    g_sent_a2 = 1;
                    g_a2_pending = 1;
                    g_t_a2_due = dg_osc1_ticks() + (uint32_t)(g_delay_ms * 32.768);
                }
            }
        }
        if (g_a2_pending && (int32_t)(dg_osc1_ticks() - g_t_a2_due) >= 0) {
            g_a2_pending = 0;
            printf("  t=%.2fs [pc ] frame2 %04X (verdict %02X)\n",
                   (double)(dg_osc1_ticks() - t0) / SEC,
                   wire_frame((uint8_t)g_a2, (uint8_t)(~g_a2 & 0xFF)), g_a2);
            dglink_start_rx(&g_w, wire_frame((uint8_t)g_a2, (uint8_t)(~g_a2 & 0xFF)));
        }

        if (dg_ram(0x009) == 5 && dg_ram(0x00A) >= 3 && !entered) {
            entered = 1;
            printf("  t=%.2fs >>> BATTLE ANIMATION started (sub=%X)\n",
                   (double)(dg_osc1_ticks() - t0) / SEC, dg_ram(0x00A));
        }
        if (dg_ram(0x00A) != last_sub) {
            if (entered)
                printf("  t=%.2fs     sub=%X mode=%X\n",
                       (double)(dg_osc1_ticks() - t0) / SEC, dg_ram(0x00A), dg_ram(0x009));
            last_sub = dg_ram(0x00A);
        }
    }

    printf("\n== result ==\n  rom frames=%d  pc frames=%d  mode=%X sub=%X  RAM30..35=",
           g_frames_rx, g_frames_tx + g_sent_a2, dg_ram(0x009), dg_ram(0x00A));
    for (int i = 0x30; i <= 0x35; i++) printf("%X", dg_ram(i));
    printf("\n%s\n", entered ? "RESPONDER ANIMATION: PASS" : "RESPONDER ANIMATION: NO");
    return entered ? 0 : 2;
}
