/* link_wave.c -- see link_wave.h.  Port of BrickEmuPy peripherals/CON_DGM.py.
 *
 * CON_DGM does its work in two places and so do we:
 *   port_handler(port, mask)  -> dglink_p2_write()   (TX edge decoding)
 *   clock(cycles)             -> dglink_tick()       (RX waveform replay)
 */
#include <string.h>

#include "digimon_core.h"
#include "link_wave.h"

void dglink_reset(dglink_wave *w)
{
    memset(w, 0, sizeof *w);
    w->p2_prev = 1;
    w->now = dg_osc1_ticks();
    w->p2_since = w->now;
    /* idle line: the cable's pull-up holds it high */
    dg_link_drive(1);
}

/* ---------------------------------------------------------------- TX ---- */
/* Mirrors CON_DGM.port_handler(): every edge restarts a run-length timer, and
 * the *next* rising edge compares the high run against the low run that just
 * followed it.  Longer high than low means a "1" bit (2.75 ms vs 1.25 ms). */

void dglink_p2_write(dglink_wave *w, int value)
{
    int level = value & 1;
    uint32_t now;
    uint32_t width;

    /* Half-duplex guard, mirrors firmware dg_link.c (HANDOFF 坑 50): while a
     * peer frame owns the pin the ROM still writes 0xF62, and those edges are
     * not data on the wire -- decoding them shifted tx_word and turned the
     * next frame into garbage. */
    if (w->rx_step != DGLINK_RX_IDLE || w->pend_valid) {
        w->tx_dropped++;
        return;
    }

    now = dg_osc1_ticks();

    if (level == w->p2_prev) {
        return;
    }
    w->edges++;
    width = now - w->p2_since;
    w->p2_since = now;

    if (level) {                       /* rising: the low run just ended */
        w->tx_period++;
        if (w->tx_period > 2) {
            w->tx_word = (w->tx_word << 1) | (w->tx_hi > width ? 1u : 0u);
            if (w->tx_period == 10) {
                w->tx_lo = (uint8_t)(w->tx_word & 0xFF);
            } else if (w->tx_period == 18) {
                w->tx_hi_byte = (uint8_t)(w->tx_word & 0xFF);
                w->tx_frame = (uint16_t)(w->tx_lo | (w->tx_hi_byte << 8));
                w->tx_ready = 1;
                w->tx_frames++;
                w->tx_period = 0;
            }
        }
    } else {                           /* falling: remember the high run */
        w->tx_hi = width;
    }
    w->p2_prev = level;
}

int dglink_take_tx(dglink_wave *w, uint16_t *word)
{
    if (!w->tx_ready) {
        return 0;
    }
    w->tx_ready = 0;
    if (word) {
        *word = w->tx_frame;
    }
    return 1;
}

/* ---------------------------------------------------------------- RX ---- */
/* Mirrors CON_DGM.clock(): two nested countdowns.  `rx_hi_left` gates the
 * low pulse, `rx_lo_left` gates the high pulse -- the names follow CON_DGM,
 * not the electrical level. */

int dglink_rx_busy(const dglink_wave *w)
{
    return w->rx_step != DGLINK_RX_IDLE;
}

/* Take the pin and start replaying `word`; resets the TX decoder because the
 * ROM's in-flight frame no longer describes what reaches the wire. */
static void dglink_begin_rx(dglink_wave *w, uint16_t word)
{
    w->rx_word = word;
    w->rx_period = 0;
    w->rx_hi_left = 0;                        /* fire on the next tick */
    w->rx_lo_left = -(int32_t)DGLINK_T_START_LO;
    w->rx_step = DGLINK_RX_REPLAY;
    w->rx_frames++;

    w->tx_period = 0;
    w->tx_word = 0;
    w->tx_ready = 0;
    w->p2_prev = -1;
    w->p2_since = w->now;
}

int dglink_start_rx(dglink_wave *w, uint16_t word)
{
    /* Byte order, mirrors firmware dg_link.c (HANDOFF 坑 51): CON_DGM pops the
     * two bytes off the tail of its queue, so the replay register is the
     * byte-swap of the transmit register.  Replaying `word` as-is put every
     * frame one byte out of phase and verdict frames were filed as cards. */
    word = (uint16_t)((word << 8) | (word >> 8));

    if (w->rx_step != DGLINK_RX_IDLE || w->pend_valid) {
        w->collisions++;
        return 0;
    }
    if (w->tx_period != 0) {
        /* Half duplex: the ROM is mid-frame.  Hold the peer frame back. */
        w->pend_word = word;
        w->pend_valid = 1;
        w->pend_age = 0;
        return 1;
    }
    dglink_begin_rx(w, word);
    return 1;
}

void dglink_tick(dglink_wave *w, uint32_t ticks)
{
    w->now += ticks;

    if (w->pend_valid) {
        w->pend_age += ticks;
        if (w->tx_period == 0 || w->pend_age >= 32768u) {   /* 1 s failsafe */
            w->pend_valid = 0;
            dglink_begin_rx(w, w->pend_word);
        }
    }

    if (w->rx_step == DGLINK_RX_IDLE) {
        return;
    }

    w->rx_hi_left += (int32_t)ticks;
    if (w->rx_hi_left < 0) {
        return;
    }

    if (w->rx_step == DGLINK_RX_TAIL) {
        /* Frame over.  CON_DGM releases the pin here (emit_pin(-1)) and P2 has
         * no external pull-up in this model, so the ROM reads the line LOW for
         * the rest of the idle gap.  Driving it high instead makes the ROM
         * believe the peer is still talking and it answers its own frame again. */
        dg_link_drive(-1);
        w->rx_step = DGLINK_RX_IDLE;
        w->p2_prev = -1;
        w->p2_since = w->now;
        w->tx_period = 0;
        w->tx_word = 0;
        return;
    }

    dg_link_drive(0);                         /* low pulse starts */

    w->rx_lo_left += (int32_t)ticks;
    if (w->rx_lo_left < 0) {
        return;
    }

    dg_link_drive(1);                         /* high pulse starts */
    w->rx_period++;

    if (w->rx_period < 2) {
        w->rx_hi_left = -(int32_t)DGLINK_T_HEAD_HI;
        w->rx_lo_left = -(int32_t)DGLINK_T_HEAD_LO;
    } else if (w->rx_period < 18) {
        if (w->rx_word & 0x8000u) {
            w->rx_hi_left = -(int32_t)DGLINK_T_BIT1_HI;
            w->rx_lo_left = -(int32_t)DGLINK_T_BIT1_LO;
        } else {
            w->rx_hi_left = -(int32_t)DGLINK_T_BIT0_HI;
            w->rx_lo_left = -(int32_t)DGLINK_T_BIT0_LO;
        }
        w->rx_word = (uint16_t)(w->rx_word << 1);
    } else {
        w->rx_step = DGLINK_RX_TAIL;
        w->rx_hi_left = -(int32_t)DGLINK_T_END_HI;
    }
}
