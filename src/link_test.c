/* link_test.c -- M1 acceptance: the waveform layer talks to a real V1 ROM.
 *
 * One core instance + src/link_wave.c + a scripted "peer" that answers with
 * made-up cards.  This is battle_host.c rebuilt on top of the layer that the
 * firmware will actually use, so if this passes, M1's waveform layer is good.
 *
 * Expected output:
 *   [rom] frame #1 FC03   (slot 3 effort 0)   -> peer answers with 0xDC23
 *   [rom] frame #2 FE01   ROM accepted the peer frame and decided the match
 * and RAM 0x33 +1 afterwards.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "digimon_core.h"
#include "link_wave.h"

static uint8_t rom[65536];
static const uint32_t SEC = 32768;

static dglink_wave g_w;

/* ------------------------------------------------------------------ peer -- */
static int  g_peer_slot = 3;
static int  g_peer_effort = 0;
static int  g_peer_version = 0;
static int  g_frames_rx;
static int  g_frames_tx;
static int      g_pend;          /* peer frame waiting for the ROM to arm RX */
static uint32_t g_pend_due;
static uint16_t g_pend_word;
static int      g_pend_delay_ms = 0;

static uint8_t bitrev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

/* CON_DGM hands bytes out bit-reversed (MSB-first shift register), so a frame
 * that carries data byte d leaves tx_word = bitrev8(d) in the low half. */
static uint16_t wire_frame(uint8_t d0, uint8_t d1)
{
    return (uint16_t)((uint16_t)bitrev8(d0) | ((uint16_t)bitrev8(d1) << 8));
}

static void peer_on_rom_frame(uint16_t frame)
{
    uint8_t d0 = bitrev8((uint8_t)(frame & 0xFF));
    uint8_t d1 = bitrev8((uint8_t)(frame >> 8));
    int b;

    g_frames_rx++;
    printf("  [rom] frame #%d %04X   (data %02X %02X)\n", g_frames_rx, frame, d0, d1);

    if (g_frames_rx == 1) {
        b = ((g_peer_effort << 4) | g_peer_slot) & 0xFF;
        printf("  [peer] answer #1 with card slot=%d effort=%d (%04X)\n",
               g_peer_slot, g_peer_effort, wire_frame((uint8_t)b, (uint8_t)(~b & 0xFF)));
        if (dglink_start_rx(&g_w, wire_frame((uint8_t)b, (uint8_t)(~b & 0xFF)))) {
            g_frames_tx++;
        }
    } else if (g_frames_rx == 2) {
        /* frame 2 = [version 4b][verdict 4b], low byte first.
         * The ROM needs a moment to swing P2 from output back to input; replying
         * inside that window makes it drop the frame and resend its own A2. */
        int res = d0 & 0x0F;
        int mine = (res == 1) ? 2 : 1;
        b = ((g_peer_version << 4) | mine) & 0xFF;
        printf("  [peer] ROM verdict=%d, answering %d (%04X) after %d ms\n",
               res, mine, wire_frame((uint8_t)b, (uint8_t)(~b & 0xFF)), g_pend_delay_ms);
        g_pend_word = wire_frame((uint8_t)b, (uint8_t)(~b & 0xFF));
        g_pend_due = dg_osc1_ticks() + (uint32_t)(g_pend_delay_ms * 32.768);
        g_pend = 1;
    }
}

static uint8_t  g_rx_trace[512];
static uint32_t g_rx_trace_t[512];
static int      g_rx_trace_n;
static int g_rx_reads;
static int g_rx_read_min = 0xF;
static int g_rx_read_max;
static void on_p2_read(int value)
{
    if (dglink_rx_busy(&g_w)) {
        g_rx_reads++;
        if (value < g_rx_read_min) g_rx_read_min = value;
        if (value > g_rx_read_max) g_rx_read_max = value;
        if (g_frames_tx >= 2 && g_rx_trace_n < 512) {
            g_rx_trace_t[g_rx_trace_n] = dg_osc1_ticks();
            g_rx_trace[g_rx_trace_n++] = (uint8_t)(value & 1);
        }
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

/* one core step + one waveform tick + collect whatever the ROM just sent */
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
    if (dglink_take_tx(&g_w, &frame)) {
        peer_on_rom_frame(frame);
    }
}

/* ---------------------------------------------------------------- boot --- */
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
    for (int i = 0; i < 200 && dg_ram(0x00B) == 0; i++) {
        run_ticks(SEC * 30);
    }
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
    for (int s = 0; s < 200 && dg_ram(0x00B) == from; s++) {
        run_ticks(SEC);
    }
    for (int s = 0; s < 120 && dg_ram(0x009) != 0; s++) {
        run_ticks(SEC);
    }
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

    if (!f) {
        printf("cannot open ROM %s\n", path);
        return 1;
    }
    n = fread(rom, 1, sizeof rom, f);
    fclose(f);

    dg_init(rom, (uint32_t)n);
    dglink_reset(&g_w);
    dg_link_set_hook(on_p2_write);

    printf("M1 waveform-layer test (link_wave.c)\n");
    boot_to_hatch();
    evolve_once(0);
    evolve_once(0);
    printf("  grown: slot=%X mode=%X\n", dg_ram(0x00B), dg_ram(0x009));

    for (int i = 0; i < 4; i++) {
        run_ticks(SEC);
        press(4, 20000, 30000);
    }
    printf("  icon page: mode=%X sub=%X\n", dg_ram(0x009), dg_ram(0x00A));

    t0 = dg_osc1_ticks();
    while ((uint32_t)(dg_osc1_ticks() - t0) < SEC * 40) {
        serve();

        /* B pressed twice, like a human: enter battle screen, then start it */
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
            printf("  [host] second B: ROM is starting the battle\n");
        }
        if (press_active && (int32_t)(dg_osc1_ticks() - press_end) >= 0) {
            press_active = 0;
            dg_set_button(2, 0);
        }
    }

    printf("\n== result ==\n");
    printf("  frames from ROM : %d\n", g_frames_rx);
    printf("  frames from peer: %d\n", g_frames_tx);
    printf("  edges seen      : %u\n", g_w.edges);
    printf("  RX collisions   : %u\n", g_w.collisions);
    printf("  P2 reads while replaying: %d (values %X..%X)\n",
           g_rx_reads, g_rx_read_min, g_rx_read_max);
    if (g_rx_trace_n > 1) {
        printf("  B2 sample trace (%d points, ms from start):\n   ", g_rx_trace_n);
        for (int i = 0; i < g_rx_trace_n && i < 200; i++)
            printf("%s%.1f", (i % 16 == 0 && i) ? " | " : " ",
                   (double)(g_rx_trace_t[i] - g_rx_trace_t[0]) / 32.768);
        printf("\n   bits: ");
        for (int i = 0; i < g_rx_trace_n && i < 200; i++)
            printf("%d", g_rx_trace[i]);
        printf("\n");
    }
    printf("  mode=%X sub=%X slot=%X  RAM 30..35 = %X %X %X %X %X %X\n",
           dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00B),
           dg_ram(0x30), dg_ram(0x31), dg_ram(0x32),
           dg_ram(0x33), dg_ram(0x34), dg_ram(0x35));

    if (g_frames_rx >= 2 && g_frames_tx >= 2 && dg_ram(0x33) != 0) {
        printf("\nM1 LINK WAVE: PASS (ROM completed A1-->B1-->A2-->B2)\n");
        return 0;
    }
    printf("\nM1 LINK WAVE: FAIL (ROM never answered the peer frame)\n");
    return 2;
}
