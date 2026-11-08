/* dg_cards.c -- see dg_cards.h. */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include "core/digimon_core.h"
#include "dg_cards.h"
#include "dg_link.h"

static const char *TAG = "dgcards";

#define DG_CARD_NVS_NS    "digicard"
#define DG_CARD_BLOB_LEN  18u
#define DG_CARD_MAGIC0    'D'
#define DG_CARD_MAGIC1    'G'
#define DG_CARD_MAGIC2    'C'
#define DG_CARD_MAGIC3    '1'

/* The ROM needs to swing P2 from output back to input before it listens again.
 * link_test.c reports that answering inside that window makes the ROM throw the
 * frame away and resend its own A2, so the verdict answer waits a moment. */
#define DG_CARD_VERDICT_DELAY_TICKS  (50u * 32768u / 1000u)   /* ~50 ms */

static bool     s_loaded;
static dg_card_t s_slots[DG_CARD_SLOTS];
static uint8_t  s_valid;                     /* bitmask: which slots hold a card */

/* ---- storage ------------------------------------------------------------ */

static void card_key(int idx, char *out, size_t n)
{
    snprintf(out, n, "c%d", idx);
}

static bool card_sane(const dg_card_t *c)
{
    if (c->slot < 3 || c->slot > 14) return false;
    if (c->effort > 4) return false;
    if (c->version > 3) return false;
    return true;
}

void dg_cards_init(void)
{
    memset(s_slots, 0, sizeof s_slots);
    s_valid = 0;
    s_loaded = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(DG_CARD_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no card namespace yet (%s)", esp_err_to_name(err));
        return;
    }
    for (int i = 0; i < DG_CARD_SLOTS; i++) {
        char key[8];
        uint8_t raw[DG_CARD_BLOB_LEN];
        size_t len = sizeof raw;
        card_key(i, key, sizeof key);
        if (nvs_get_blob(h, key, raw, &len) != ESP_OK || len != sizeof raw) continue;
        if (raw[0] != DG_CARD_MAGIC0 || raw[1] != DG_CARD_MAGIC1 ||
            raw[2] != DG_CARD_MAGIC2 || raw[3] != DG_CARD_MAGIC3) continue;
        dg_card_t c;
        memset(&c, 0, sizeof c);
        c.slot = raw[4];
        c.effort = raw[5];
        c.version = raw[6];
        memcpy(c.name, raw + 7, DG_CARD_NAME_LEN);
        c.name[DG_CARD_NAME_LEN] = 0;
        if (!card_sane(&c)) continue;
        s_slots[i] = c;
        s_valid |= (uint8_t)(1u << i);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "cards loaded: %d of %d slot(s) populated",
             dg_card_count(), DG_CARD_SLOTS);
}

bool dg_card_load(int idx, dg_card_t *out)
{
    if (!s_loaded) dg_cards_init();
    if (idx < 0 || idx >= DG_CARD_SLOTS) return false;
    if (!(s_valid & (1u << idx))) return false;
    if (out) *out = s_slots[idx];
    return true;
}

bool dg_card_save(int idx, const dg_card_t *card)
{
    if (!s_loaded) dg_cards_init();
    if (idx < 0 || idx >= DG_CARD_SLOTS || !card) return false;
    if (!card_sane(card)) return false;

    dg_card_t c = *card;
    c.name[DG_CARD_NAME_LEN] = 0;

    uint8_t raw[DG_CARD_BLOB_LEN];
    memset(raw, 0, sizeof raw);
    raw[0] = DG_CARD_MAGIC0;
    raw[1] = DG_CARD_MAGIC1;
    raw[2] = DG_CARD_MAGIC2;
    raw[3] = DG_CARD_MAGIC3;
    raw[4] = c.slot;
    raw[5] = c.effort;
    raw[6] = c.version;
    memcpy(raw + 7, c.name, DG_CARD_NAME_LEN);

    nvs_handle_t h;
    esp_err_t err = nvs_open(DG_CARD_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "card write: nvs_open failed (%s)", esp_err_to_name(err));
        return false;
    }
    char key[8];
    card_key(idx, key, sizeof key);
    err = nvs_set_blob(h, key, raw, sizeof raw);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "card write %d failed (%s)", idx, esp_err_to_name(err));
        return false;
    }
    s_slots[idx] = c;
    s_valid |= (uint8_t)(1u << idx);
    ESP_LOGI(TAG, "card %d saved: slot=%u effort=%u ver=%u name=%s", idx,
             (unsigned)c.slot, (unsigned)c.effort, (unsigned)c.version, c.name);
    return true;
}

bool dg_card_erase(int idx)
{
    if (!s_loaded) dg_cards_init();
    if (idx < 0 || idx >= DG_CARD_SLOTS) return false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(DG_CARD_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    char key[8];
    card_key(idx, key, sizeof key);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);

    memset(&s_slots[idx], 0, sizeof s_slots[idx]);
    s_valid &= (uint8_t)~(1u << idx);
    ESP_LOGI(TAG, "card %d erased", idx);
    return true;
}

int dg_card_first(void)
{
    if (!s_loaded) dg_cards_init();
    for (int i = 0; i < DG_CARD_SLOTS; i++)
        if (s_valid & (1u << i)) return i;
    return -1;
}

int dg_card_count(void)
{
    int n = 0;
    for (int i = 0; i < DG_CARD_SLOTS; i++)
        if (s_valid & (1u << i)) n++;
    return n;
}

void dg_card_label(const dg_card_t *card, char *buf, size_t n)
{
    if (!card || !buf || n == 0) return;
    if (card->name[0]) {
        snprintf(buf, n, "%s", card->name);
    } else {
        snprintf(buf, n, "SLOT %X", (unsigned)card->slot);
    }
}

/* ---- bit-level packing (copy of src/link_test.c) ------------------------- */

static uint8_t bitrev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

uint16_t dg_card_wire_frame(uint8_t d0, uint8_t d1)
{
    return (uint16_t)((uint16_t)bitrev8(d0) | ((uint16_t)bitrev8(d1) << 8));
}

/* ---- the answering peer ------------------------------------------------- */

static bool     s_armed;
static dg_card_t s_card;
static int      s_frames;
static int      s_replies;
static const char *s_state = "IDLE";

static uint16_t s_pend_word;
static int      s_pend_valid;
static int      s_pend_verdict;     /* 1 = this queued frame is the verdict */
static uint32_t s_pend_due;          /* OSC1 tick stamp */

void dg_card_battle_arm(const dg_card_t *card)
{
    if (!card) return;
    s_card = *card;
    s_armed = true;
    s_frames = 0;
    s_replies = 0;
    s_pend_valid = 0;
    s_state = "ARMED";
    ESP_LOGI(TAG, "card battle armed: slot=%u effort=%u ver=%u",
             (unsigned)s_card.slot, (unsigned)s_card.effort,
             (unsigned)s_card.version);
}

void dg_card_battle_disarm(void)
{
    if (s_armed) ESP_LOGI(TAG, "card battle disarmed (%d frame(s), %d answer(s))",
                          s_frames, s_replies);
    s_armed = false;
    s_pend_valid = 0;
    s_state = "IDLE";
}

bool dg_card_battle_armed(void)
{
    return s_armed;
}

static void card_queue(uint16_t word, uint32_t delay_ticks, int verdict)
{
    s_pend_word = word;
    s_pend_due = dg_osc1_ticks() + delay_ticks;
    s_pend_verdict = verdict;
    s_pend_valid = 1;
}

void dg_card_battle_feed(uint16_t rom_frame)
{
    if (!s_armed) return;

    uint8_t d0 = bitrev8((uint8_t)(rom_frame & 0xFF));
    s_frames++;
    ESP_LOGI(TAG, "card: ROM frame #%d %04X data %02X", s_frames,
             (unsigned)rom_frame, (unsigned)d0);

    if (s_frames == 1) {
        /* frame 1 is the opponent's card: answer with ours, no delay -- card
         * answers are already proven to land at once */
        uint8_t b = (uint8_t)(((s_card.effort & 0xF) << 4) | (s_card.slot & 0xF));
        card_queue(dg_card_wire_frame(b, (uint8_t)(~b)), 0, 0);
        s_state = "CARD";
    } else if (s_frames == 2) {
        /* frame 2 carries the ROM's verdict in the low nibble; the peer replies
         * with the opposite one, plus its own version in the high nibble. */
        int res = d0 & 0x0F;
        int mine = (res == 1) ? 2 : 1;
        uint8_t b = (uint8_t)(((s_card.version & 0xF) << 4) | (mine & 0xF));
        card_queue(dg_card_wire_frame(b, (uint8_t)(~b)),
                   DG_CARD_VERDICT_DELAY_TICKS, 1);
        s_state = "VERDICT";
    } else {
        s_state = "DONE";
    }
}

void dg_card_battle_tick(void)
{
    if (!s_armed || !s_pend_valid) return;
    if ((int32_t)(dg_osc1_ticks() - s_pend_due) < 0) return;

    if (dglink_start_rx(&g_dglink, s_pend_word)) {
        s_pend_valid = 0;
        s_replies++;
        ESP_LOGI(TAG, "card: answered #%d (%04X)", s_replies, (unsigned)s_pend_word);
        if (s_pend_verdict) s_state = "DONE";
    }
    /* line busy -> keep the answer queued, the next frame retries */
}

int dg_card_battle_frames(void)  { return s_frames; }
int dg_card_battle_replies(void) { return s_replies; }
const char *dg_card_battle_state(void) { return s_state; }
