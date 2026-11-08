/* dg_cards.h -- offline "battle cards" (G2 transport, milestone M3).
 *
 * A card is the 12 bits a D-Link peer would put on the wire: which species slot
 * it is, how much effort it carries, and which ROM generation it came from.  The
 * battle itself is still played by the ROM -- the card only has to answer the two
 * frames the ROM sends, exactly like the PC relays in tools/dg_link_pair.py.
 *
 *   ROM frame #1  [effort 4b][slot 4b]        -> answer with OUR card
 *   ROM frame #2  [version 4b][verdict 4b]    -> answer with the opposite verdict
 *
 * The bit-level plumbing is a straight copy of src/link_test.c (M1 acceptance),
 * including the bit-reversed wire packing and the ~50 ms settle delay before the
 * verdict answer: replying inside that window makes the ROM drop the frame and
 * resend its own A2.  See docs/ROADMAP-BATTLE.md §5 for the design.
 *
 * Cards live in NVS (namespace "digicard", keys c0..c7), 18 bytes each, so they
 * survive reboots and ROM switches and need no partition changes.
 */
#ifndef DG_CARDS_H
#define DG_CARDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DG_CARD_SLOTS     8
#define DG_CARD_NAME_LEN  10

typedef struct {
    uint8_t slot;                    /* ROM species slot, 3..14 */
    uint8_t effort;                  /* 0..4 (same range the ROM sends) */
    uint8_t version;                 /* 0 = V1, 1 = V2, 2 = V3, 3 = V4 */
    char    name[DG_CARD_NAME_LEN + 1];
} dg_card_t;

/* Reads every slot once.  Safe to call before NVS is initialised: slots simply
 * come back empty. */
void dg_cards_init(void);

bool dg_card_load(int idx, dg_card_t *out);
bool dg_card_save(int idx, const dg_card_t *card);
bool dg_card_erase(int idx);

/* First populated slot, or -1.  Used by the menu to skip empty rows. */
int  dg_card_first(void);
int  dg_card_count(void);

/* Human-readable label for the menu / console: the stored name, or "SLOT n". */
void dg_card_label(const dg_card_t *card, char *buf, size_t n);

/* ---- the card as the answering peer ------------------------------------
 * Arm before the ROM starts talking (i.e. when the battle screen is entered),
 * feed it every frame the ROM puts on the pin, and tick it once per main-loop
 * frame so a delayed answer goes out.  Nothing here touches the line while
 * disarmed, so the PC relay path keeps working unchanged. */
void dg_card_battle_arm(const dg_card_t *card);
void dg_card_battle_disarm(void);
bool dg_card_battle_armed(void);
void dg_card_battle_feed(uint16_t rom_frame);
void dg_card_battle_tick(void);

int         dg_card_battle_frames(void);    /* ROM frames seen */
int         dg_card_battle_replies(void);   /* frames we queued back */
const char *dg_card_battle_state(void);     /* IDLE / CARD / VERDICT / DONE */

/* Bit-reversed wire packing (CON_DGM shifts MSB-first).  Exposed for the host
 * test and for tools that want to pre-compute a frame. */
uint16_t dg_card_wire_frame(uint8_t d0, uint8_t d1);

#endif /* DG_CARDS_H */
