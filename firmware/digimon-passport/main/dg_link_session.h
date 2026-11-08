/* dg_link_session.h -- D-Link battle session bookkeeping (M1 skeleton).
 *
 * What M0a changed: the ROM drives the whole exchange itself.  Once the user
 * walks into the BATTLE icon and presses B, the ROM puts frame 1 on the pin;
 * the other machine (or the PC) only has to answer.  So this layer is not a
 * "send A1, wait B1, send A2" sequencer any more -- it is a frame router plus
 * a watchdog:
 *
 *   ROM finishes a frame -> dg_link_session_tick() hands it to the callback
 *   transport has an answer -> dg_link_session_reply() queues it for replay
 *
 * The transport (serial for G1, card for G2, radio for G3) is whatever owns
 * the callback.  Nothing here knows about bytes-on-a-wire.
 */
#ifndef DG_LINK_SESSION_H
#define DG_LINK_SESSION_H

#include <stdint.h>

typedef enum {
    DGLINK_SESS_OFF = 0,     /* not in a battle */
    DGLINK_SESS_ARMED,       /* armed, waiting for the ROM's first frame */
    DGLINK_SESS_TALKING,     /* at least one frame went out */
    DGLINK_SESS_DONE,        /* ROM finished the exchange */
    DGLINK_SESS_FAIL         /* timed out */
} dglink_sess_state;

typedef void (*dglink_frame_cb)(uint16_t frame);

void dg_link_session_reset(void);
void dg_link_session_set_cb(dglink_frame_cb cb);

/* Call when the user enters the battle icon and the ROM is about to talk.
 * slot/effort describe *our* side; they are only used for logging and for
 * transports that need to advertise a card (G3). */
void dg_link_session_arm(uint8_t slot, uint8_t effort);
void dg_link_session_disarm(void);

/* Call once per emulation step.  Picks up finished ROM frames and feeds the
 * callback; also runs the watchdog. */
void dg_link_session_tick(void);

/* Queue one peer frame for replay onto the ROM's pin.  0 = line busy. */
int dg_link_session_reply(uint16_t frame);

dglink_sess_state dg_link_session_state(void);
const char        *dg_link_session_state_name(void);
uint32_t           dg_link_session_tx_count(void);
uint32_t           dg_link_session_rx_count(void);

#endif /* DG_LINK_SESSION_H */
