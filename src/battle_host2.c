/* battle_host2.c -- battle_host.c with the waveform constants taken from
 * BrickEmuPy's CON_DGM, which is *known* to make a V1 accept a peer frame
 * (docs/M0A-BRICKEMUPY-LINK.md).  Only the timing numbers differ:
 *
 *            battle_host.c    CON_DGM (sys cycles @1.06 MHz -> ticks @32768 Hz)
 *   start    60 ms            67869 / 32.348 = 2098  -> 64.0 ms
 *   head hi  66               2135 / 32.348 = 66.0
 *   head lo  33               1003 / 32.348 = 31.0
 *   bit0 hi  33               1067 / 32.348 = 33.0
 *   bit0 lo  98               3461 / 32.348 = 107.0
 *   bit1 hi  90               2911 / 32.348 = 90.0
 *   bit1 lo  41               1779 / 32.348 = 55.0
 *   end      6 ms             7000 / 32.348 = 216.4 -> 6.6 ms
 *
 * Time base: dg_osc1_ticks() (32768 Hz).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "trace_core_link.c"
#include "digimon_segmap.h"

static uint8_t rom[65536];
static const uint32_t SEC = 32768;

#define T_MS(n)     ((uint32_t)((n) * 32.768))
#define HI_HEAD     66u
#define LO_HEAD     33u
#define HI_BIT0     33u
#define LO_BIT0     98u
#define HI_BIT1     90u
#define LO_BIT1     41u
#define START_LOW   T_MS(60)
#define FRAME_GAP   T_MS(6)

static uint32_t g_now;
static int g_latch = 1, g_prev = 1;
static int g_peer_drive = -1;

static uint32_t g_hi[40];
static int g_nhi;
static uint32_t g_high_start;

static uint32_t g_tx_seq[64];
static int g_tx_len, g_tx_i, g_tx_active;
static uint32_t g_tx_next;

static int g_frames_rx, g_frames_tx, g_last = -1;
static int g_peer_slot = 3, g_peer_effort = 2, g_peer_version = 0;

static void set_peer_line(int level)
{
    if (g_peer_drive == level) return;
    g_peer_drive = level;
    dg_pin_handler(&g_cpu, PORT_P2, 1, level);
}

static void p2_hook(int value) { g_latch = value & 1; }
static int g_read_min = 0xF, g_read_max = 0;
static int g_read_pc_min = -1;
static int g_trace_reads, g_trace_n;
static uint32_t g_trace_start;
static void p2_read_hook(int value)
{
    if (value < g_read_min) { g_read_min = value; g_read_pc_min = dg_pc(); }
    if (value > g_read_max) g_read_max = value;
    if (g_trace_reads && g_trace_n < 4000) {
        printf("       read[%d] t=%u val=%X pc=%03X\n", g_trace_n,
               (unsigned)(dg_osc1_ticks() - g_trace_start), value, dg_pc());
        g_trace_n++;
    }
}
static int g_dir_out = 1;
static void dir_hook(int ioc)
{
    int out = (ioc & 4) ? 1 : 0;
    if (out != g_dir_out) {
        g_dir_out = out;
        printf("     [dir] P2 -> %s at %u\n", out ? "OUTPUT" : "input", g_now);
    }
}

static void build_bits(int word)
{
    int n = 0;
    g_tx_seq[n++] = START_LOW;
    g_tx_seq[n++] = HI_HEAD;
    g_tx_seq[n++] = LO_HEAD;
    for (int i = 0; i < 16; i++) {
        if (word & (1 << i)) { g_tx_seq[n++] = HI_BIT1; g_tx_seq[n++] = LO_BIT1; }
        else                 { g_tx_seq[n++] = HI_BIT0; g_tx_seq[n++] = LO_BIT0; }
    }
    g_tx_len = n;
}

static void start_tx(int word)
{
    build_bits(word);
    g_tx_i = 0;
    g_tx_active = 1;
    set_peer_line(0);
    g_tx_next = g_now + g_tx_seq[0];
    if (g_frames_tx == 0) { g_trace_reads = 1; g_trace_n = 0; g_trace_start = g_now; }
    printf("  [peer] TX %04X at %u\n", word, g_now);
}

/* hi[0] is the head pulse, hi[1..16] are the data pulses */
static int decode(void)
{
    if (g_nhi < 17) return -1;
    uint32_t thr = g_hi[0];
    int word = 0;
    for (int i = 0; i < 16; i++)
        if (g_hi[1 + i] > thr) word |= (1 << i);
    return word;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open ROM\n"); return 1; }
    size_t n = fread(rom, 1, sizeof rom, f); fclose(f);
    dg_init(rom, (uint32_t)n);
    g_link_p2_hook = p2_hook;
    g_link_dir_hook = dir_hook;
    g_link_p2_read_hook = p2_read_hook;
    set_peer_line(1);          /* the real cable idles high through the pull-up */

    #define RUN_TICKS(k) do { uint32_t t = dg_osc1_ticks() + (k); \
        while ((int32_t)(dg_osc1_ticks() - t) < 0) dg_step(); } while (0)
    #define PRESS(b) do { dg_set_button((b), 1); RUN_TICKS(8192); \
        dg_set_button((b), 0); RUN_TICKS(8192); } while (0)

    dg_reset();
    RUN_TICKS(SEC * 2);
    PRESS(1); PRESS(4); PRESS(1); PRESS(2);
    if (dg_ram(0x009) == 1) { PRESS(4); PRESS(1); PRESS(2); }
    for (int i = 0; i < 200 && dg_ram(0x00B) == 0; i++) RUN_TICKS(SEC * 30);

    for (int leg = 0; leg < 2; leg++) {
        int next = (dg_ram(0x4E) | (dg_ram(0x4F) << 4)) * 60 + dg_ram(0x4D) * 10 + dg_ram(0x4C);
        int target = next - 2;
        int from = dg_ram(0x00B);
        dg_ram_write(0x031, 0); dg_ram_write(0x032, 0); dg_ram_write(0x033, 0);
        dg_ram_write(0x034, 0); dg_ram_write(0x035, 0);
        dg_ram_write(0x48, (uint8_t)((target % 60) % 10));
        dg_ram_write(0x49, (uint8_t)(((target % 60) / 10) % 10));
        dg_ram_write(0x4A, (uint8_t)((target / 60) & 0xF));
        dg_ram_write(0x4B, (uint8_t)(((target / 60) >> 4) & 0xF));
        for (int s = 0; s < 200 && dg_ram(0x00B) == from; s++) RUN_TICKS(SEC);
        for (int s = 0; s < 120 && dg_ram(0x009) != 0; s++) RUN_TICKS(SEC);
    }
    printf("ready: slot=%X mode=%X\n", dg_ram(0x00B), dg_ram(0x009));
    for (int i = 0; i < 4; i++) PRESS(4);
    printf("icon page mode=%X sub=%X 0C=%X\n", dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00C));

    uint32_t t0 = dg_osc1_ticks();
    int press_active = 0, b1 = 0, b1r = 0, b2 = 0;
    uint32_t press_end = 0, next_report = 0;
    g_now = t0;

    while ((uint32_t)(g_now - t0) < SEC * 30) {
        g_now = dg_osc1_ticks();

        /* sample the link line */
        if (g_latch == 1 && g_prev == 0) g_high_start = g_now;
        if (g_latch == 0 && g_prev == 1) {
            uint32_t h = g_now - g_high_start;
            if (h < FRAME_GAP && g_nhi < 40) g_hi[g_nhi++] = h;
        }
        g_prev = g_latch;

        /* a long high ends a frame (or marks idle) */
        if (!g_tx_active && g_nhi > 0 && g_latch == 1 && (g_now - g_high_start) > FRAME_GAP) {
            int word = decode();
            if (word >= 0) {
                g_frames_rx++; g_last = word;
                printf("  [peer] RX #%d = %04X  slot=%X effort=%X ver=%X out=%X\n",
                       g_frames_rx, word, (word & 0xF), ((word >> 4) & 0xF),
                       ((word >> 12) & 0xF), ((word >> 8) & 0xF) ^ 0xF);
                if (g_frames_rx == 1) {
                    int b = ((g_peer_effort << 4) | g_peer_slot) & 0xFF;
                    start_tx(((~b & 0xFF) << 8) | b);
                } else if (g_frames_rx == 2) {
                    int res = ((word >> 8) & 0xF) ^ 0xF;
                    int mine = (res == 1) ? 2 : 1;
                    int b = ((g_peer_version << 4) | mine) & 0xFF;
                    printf("  [peer] rom outcome=%d -> reply %d\n", res, mine);
                    start_tx(((~b & 0xFF) << 8) | b);
                }
            }
            g_nhi = 0;
        }

        /* transmitter */
        if (g_tx_active && (int32_t)(g_now - g_tx_next) >= 0) {
            g_tx_i++;
            if (g_tx_i >= g_tx_len) {
                g_tx_active = 0;
                set_peer_line(1);
                g_frames_tx++;
                            printf("  [peer] TX done (%d)\n", g_frames_tx);
                if (g_frames_tx == 1) { /* trace already armed in start_tx() */ }
            } else {
                /* g_tx_seq[] alternates low(0), high(1), low(2), ... so the
                 * *level* of step i is (i & 1).  The original battle_host.c had
                 * this inverted -- it replayed the whole frame upside down. */
                set_peer_line((g_tx_i & 1) ? 1 : 0);
                g_tx_next += g_tx_seq[g_tx_i];
            }
        }

        /* buttons */
        if (!b1 && (uint32_t)(g_now - t0) > SEC / 10) {
            b1 = 1; dg_set_button(2, 1); press_active = 1; press_end = g_now + SEC / 3;
        } else if (b1 && !b1r && !press_active) {
            b1r = 1;
            printf("  [host] after B#1: mode=%X sub=%X\n", dg_ram(0x009), dg_ram(0x00A));
        } else if (b1r && !b2 && !press_active && dg_ram(0x00A) == 2 &&
                   (uint32_t)(g_now - t0) > SEC * 2) {
            b2 = 1; dg_set_button(2, 1); press_active = 1; press_end = g_now + SEC / 3;
            printf("  [host] B#2 down\n");
        }
        if (press_active && (int32_t)(g_now - press_end) >= 0) {
            press_active = 0; dg_set_button(2, 0);
        }

        if ((uint32_t)(g_now - t0) > next_report) {
            next_report += SEC * 2;
            int lastval = -1, lastpc = -1;
            for (int i = (int)g_io_ev_n - 1; i >= 0 && i > (int)g_io_ev_n - 4000; i--)
                if (g_io_ev[i].addr == 0xF62 && !g_io_ev[i].rw) {
                    lastval = g_io_ev[i].val; lastpc = g_io_ev[i].pc; break;
                }
            printf("  [t=%2us] mode=%X sub=%X 0C=%X slot=%X p2last=%X@%03X read=%X..%X@%03X p2acc=%lu rx=%d tx=%d\n",
                   (unsigned)((g_now - t0) / SEC), dg_ram(0x009), dg_ram(0x00A),
                   dg_ram(0x00C), dg_ram(0x00B), lastval, lastpc, g_read_min, g_read_max, g_read_pc_min,
                   g_io_hits[0x62], g_frames_rx, g_frames_tx);
            g_io_hits[0x62] = 0;
        }

        dg_step();
    }

    printf("\nfinal: mode=%X sub=%X slot=%X 0C=%X rx=%d tx=%d last=%04X\n",
           dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00B), dg_ram(0x00C),
           g_frames_rx, g_frames_tx, g_last);
    printf("RAM 30..35 = %X %X %X %X %X %X\n", dg_ram(0x30), dg_ram(0x31), dg_ram(0x32),
           dg_ram(0x33), dg_ram(0x34), dg_ram(0x35));
    printf("instr=%u\n", dg_instr_count());
    return 0;
}
