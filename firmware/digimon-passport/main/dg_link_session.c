/* dg_link_session.c -- see dg_link_session.h. */

#include "core/digimon_core.h"
#include "dg_link.h"
#include "dg_link_session.h"

/* The ROM gives up on its own after about 10 s; stay comfortably inside that
 * so the UI can show LINK FAIL before the glass snaps back to the main screen. */
#define DGLINK_SESS_TIMEOUT_TICKS   (8u * 32768u)

static dglink_sess_state s_state;
static dglink_frame_cb   s_cb;
static uint8_t           s_slot;
static uint8_t           s_effort;
static uint32_t          s_last_activity;
static uint32_t          s_tx_count;
static uint32_t          s_rx_count;
static uint16_t          s_pending;      /* peer frame waiting for a free line */
static int               s_has_pending;

void dg_link_session_reset(void)
{
    s_state = DGLINK_SESS_OFF;
    s_slot = 0;
    s_effort = 0;
    s_last_activity = 0;
    s_tx_count = 0;
    s_rx_count = 0;
    s_pending = 0;
    s_has_pending = 0;
}

void dg_link_session_set_cb(dglink_frame_cb cb)
{
    s_cb = cb;
}

void dg_link_session_arm(uint8_t slot, uint8_t effort)
{
    s_state = DGLINK_SESS_ARMED;
    s_slot = slot;
    s_effort = effort;
    s_last_activity = dg_osc1_ticks();
    s_tx_count = 0;
    s_rx_count = 0;
    s_has_pending = 0;
}

void dg_link_session_disarm(void)
{
    s_state = DGLINK_SESS_OFF;
    s_has_pending = 0;
}

int dg_link_session_reply(uint16_t frame)
{
    if (s_state != DGLINK_SESS_ARMED && s_state != DGLINK_SESS_TALKING) {
        return 0;
    }
    s_pending = frame;
    s_has_pending = 1;
    s_last_activity = dg_osc1_ticks();
    return 1;
}

void dg_link_session_tick(void)
{
    uint16_t frame;
    uint32_t now;

    if (s_state != DGLINK_SESS_ARMED && s_state != DGLINK_SESS_TALKING) {
        return;
    }

    /* a queued peer frame goes out as soon as the line is free */
    if (s_has_pending && !dglink_rx_busy(&g_dglink)) {
        if (dglink_start_rx(&g_dglink, s_pending)) {
            s_has_pending = 0;
            s_rx_count++;
            s_last_activity = dg_osc1_ticks();
            if (s_state == DGLINK_SESS_ARMED) {
                s_state = DGLINK_SESS_TALKING;
            }
        }
    }

    /* whatever the ROM just finished sending */
    while (dglink_take_tx(&g_dglink, &frame)) {
        s_tx_count++;
        s_last_activity = dg_osc1_ticks();
        if (s_state == DGLINK_SESS_ARMED) {
            s_state = DGLINK_SESS_TALKING;
        }
        if (s_cb) {
            s_cb(frame);
        }
    }

    now = dg_osc1_ticks();
    if ((uint32_t)(now - s_last_activity) > DGLINK_SESS_TIMEOUT_TICKS) {
        s_state = DGLINK_SESS_FAIL;
        s_has_pending = 0;
    } else if (s_state == DGLINK_SESS_TALKING && s_tx_count >= 2 && s_rx_count >= 2 &&
               !dglink_rx_busy(&g_dglink)) {
        /* both frames exchanged and the line is quiet: the ROM is playing the
         * animation now, nothing left for us to do */
        s_state = DGLINK_SESS_DONE;
    }
}

dglink_sess_state dg_link_session_state(void)
{
    return s_state;
}

const char *dg_link_session_state_name(void)
{
    switch (s_state) {
    case DGLINK_SESS_OFF:     return "OFF";
    case DGLINK_SESS_ARMED:   return "ARMED";
    case DGLINK_SESS_TALKING: return "TALK";
    case DGLINK_SESS_DONE:    return "DONE";
    case DGLINK_SESS_FAIL:    return "FAIL";
    default:                  return "?";
    }
}

uint32_t dg_link_session_tx_count(void) { return s_tx_count; }
uint32_t dg_link_session_rx_count(void) { return s_rx_count; }
