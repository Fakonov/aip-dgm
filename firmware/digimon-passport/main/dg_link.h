/* dg_link.h -- D-Link (Digital Monster V1/V2 "CON_DGM") waveform layer.
 *
 * Faithful C port of BrickEmuPy's peripherals/CON_DGM.py, which is the only
 * implementation proven to make a real V1 ROM complete a battle
 * (docs/M0A-BRICKEMUPY-LINK.md).
 *
 * This file is the firmware copy of src/link_wave.{h,c}; keep the two in sync.
 * The host-side harness is src/link_test.c (run it after touching either).
 *
 * Two halves:
 *   TX  watch what the ROM drives onto P2.0 and turn the pulse widths back into
 *       16-bit frames (LSB first, high-vs-low width comparison).
 *   RX  take a peer's 16-bit frame and replay it onto the ROM's P2.0 pin as the
 *       original waveform (start-low, head, 16 data bits, idle high).
 *
 * >>> POLARITY <<<  The frame is strict alternating low/high/low/high...:
 *   start low (60 ms) -> HEAD HIGH (2 ms) -> head low (1 ms) -> bit0 high ...
 * Getting this wrong (i.e. replaying the frame upside down) leaves the ROM
 * silently polling for 10 s and then dropping out of battle.  That exact bug
 * cost two rounds of M0 debugging -- see HANDOFF "坑 37".
 *
 * All durations are in 32768 Hz OSC1 ticks, the same time base as
 * dg_osc1_ticks() and dg_link_latch().
 */
#ifndef DG_LINK_H
#define DG_LINK_H

#include <stdint.h>

#define DGLINK_T_START_LO  1966u   /* 60 ms   */
#define DGLINK_T_HEAD_HI     66u   /* 2.01 ms */
#define DGLINK_T_HEAD_LO     33u   /* 1.01 ms */
#define DGLINK_T_BIT0_HI     33u   /* 1.01 ms */
#define DGLINK_T_BIT0_LO     98u   /* 2.99 ms */
#define DGLINK_T_BIT1_HI     90u   /* 2.75 ms */
#define DGLINK_T_BIT1_LO     41u   /* 1.25 ms */
#define DGLINK_T_END_HI     216u   /* 6.6 ms of idle high ends the frame */

/* RX states */
#define DGLINK_RX_IDLE   0
#define DGLINK_RX_REPLAY 1
#define DGLINK_RX_TAIL   2

typedef struct {
    uint32_t now;              /* free-running tick stamp (mirrors dg_osc1_ticks) */

    /* --- TX: decoding what the ROM drives --------------------------------- */
    int      p2_prev;
    uint32_t p2_since;
    uint32_t tx_hi;            /* width of the high run that just ended */
    int      tx_period;
    uint32_t tx_word;
    uint8_t  tx_lo;
    uint8_t  tx_hi_byte;
    int      tx_nbytes;
    int      tx_ready;
    uint16_t tx_frame;
    uint32_t tx_frames;        /* diagnostic: frames decoded */

    /* --- RX: replaying a peer frame --------------------------------------- */
    int      rx_step;
    uint16_t rx_word;
    int      rx_period;
    int32_t  rx_hi_left;
    int32_t  rx_lo_left;
    uint32_t rx_frames;        /* diagnostic: frames replayed */

    /* --- half-duplex guard (see HANDOFF 坑 50/51) ------------------------- */
    uint16_t pend_word;        /* peer frame held back while we are mid-frame */
    int      pend_valid;
    uint32_t pend_age;         /* ticks spent waiting; 1 s failsafe */
    uint32_t tx_dropped;       /* ROM edges ignored because RX owned the pin */

    /* --- diagnostics ------------------------------------------------------ */
    uint32_t edges;
    uint32_t collisions;       /* frames dropped because the line was busy */
} dglink_wave;

/* Firmware entry point: reset the layer and hook it into the core. */
void dg_link_init(void);

void dglink_reset(dglink_wave *w);

/* Call once per core step with the number of OSC1 ticks dg_step() consumed.
 * Advances the RX replay state machine; harmless when idle. */
void dglink_tick(dglink_wave *w, uint32_t ticks);

/* Hook target: called whenever the ROM writes 0xF62 (value = what it wrote). */
void dglink_p2_write(dglink_wave *w, int value);

/* 1 while a frame is being replayed onto the pin (RX busy). */
int  dglink_rx_busy(const dglink_wave *w);

/* Queue one 16-bit peer frame for replay.  Returns 0 if the line is busy
 * (caller should retry after the next frame slot). */
int  dglink_start_rx(dglink_wave *w, uint16_t word);

/* 1 and *word set when the ROM has just finished sending a frame. */
int  dglink_take_tx(dglink_wave *w, uint16_t *word);

/* The firmware owns exactly one link; the session layer uses it directly. */
extern dglink_wave g_dglink;

#endif /* DG_LINK_H */
