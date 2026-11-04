/*
 * dg_rom_store.h -- Digimon ROM images live in their own flash partition.
 *
 * The firmware no longer embeds V1/V2/V3 (EMBED_FILES).  A ROM is a 16 KiB
 * image with no header, exactly the file MAME wants, and it is uploaded from
 * the PC tool over the USB console.  Until something has been uploaded the
 * partition is empty and the app has nothing to run -- the glass says NO ROM.
 *
 * Partition ("roms", data/0x40, 0x210000, 6.1 MB) layout:
 *
 *   slot i base   = 0x210000 + i * 0x8000          (32 KiB stride)
 *   + 0x0000      dg_rom_hdr_t, alone in the first 4 KiB sector
 *   + 0x1000      16 KiB of ROM data
 *
 * The header is the commit record: a transfer erases and writes the data
 * first, checks the CRC, and only then rewrites the header sector.  Losing
 * power halfway therefore leaves the old header pointing at a half-written
 * image, which the CRC check at scan time rejects -- the slot reads as empty
 * instead of booting garbage.
 */
#ifndef DG_ROM_STORE_H
#define DG_ROM_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DG_ROM_SLOTS      8          /* 8 * 32 KiB = 256 KiB of a 6.1 MB partition */
#define DG_ROM_SLOT_STRIDE 0x8000u
#define DG_ROM_DATA_OFF   0x1000u
#define DG_ROM_MAX_LEN    16384u
#define DG_ROM_NAME_LEN   12

typedef struct {
    uint32_t magic;
    uint32_t len;          /* bytes of ROM data, 16384 today */
    uint32_t crc;          /* crc32 of the data (IEEE, as zlib/crc32()) */
    uint32_t seq;          /* write counter, informational */
    char     name[DG_ROM_NAME_LEN];   /* "V1", "V2", ... NUL padded */
    uint32_t hdr_crc;      /* crc32 of the 28 bytes above */
} dg_rom_hdr_t;

typedef struct {
    bool     valid;
    uint32_t len;
    uint32_t crc;
    char     name[DG_ROM_NAME_LEN];
} dg_rom_entry_t;

/* Scans the partition and verifies every committed slot.  Safe to call twice. */
esp_err_t dg_rom_store_init(void);

/* Total number of slots the partition could hold (not "used"). */
int  dg_rom_store_slots(void);

/* Metadata for slot idx; *out is zeroed when the slot is empty or out of range. */
bool dg_rom_store_get(int idx, dg_rom_entry_t *out);

/* First committed slot, or -1 when nothing has been uploaded yet. */
int  dg_rom_store_first(void);

/* Next committed slot after idx (wraps), or -1 when nothing is committed. */
int  dg_rom_store_next(int idx);

/* Maps slot idx read-only.  The pointer stays valid until the next call. */
const uint8_t *dg_rom_store_map(int idx, uint32_t *len);

/* Reads arbitrary bytes of a slot without disturbing the active mapping --
 * used by ROMOUT while the pet is running. */
esp_err_t dg_rom_store_read(int idx, uint32_t off, void *dst, uint32_t n);

/* Streaming write: begin -> N chunks -> commit (or abort).  The data area is
 * erased by begin; commit verifies the running CRC and publishes the header.
 * expect_crc may be 0 to accept whatever arrives (the header stores the real
 * value either way). */
esp_err_t dg_rom_store_begin_write(int idx, uint32_t len);
esp_err_t dg_rom_store_write_chunk(const uint8_t *data, uint32_t len);
esp_err_t dg_rom_store_commit_write(const char *name, uint32_t expect_crc, uint32_t *out_crc);
void      dg_rom_store_abort_write(void);

/* Erases the header sector of slot idx (the data sectors are left alone). */
esp_err_t dg_rom_store_erase(int idx);

/* crc32 helper shared with the console layer (same polynomial as zlib). */
uint32_t dg_crc32(const uint8_t *p, uint32_t n);
uint32_t dg_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* DG_ROM_STORE_H */
