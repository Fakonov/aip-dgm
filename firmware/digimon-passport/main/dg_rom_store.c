/*
 * dg_rom_store.c -- see dg_rom_store.h for the slot layout.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "dg_rom_store.h"

static const char *TAG = "dgrom";

#define DG_ROM_MAGIC 0x31524744u   /* "DGR1" little endian */

static const esp_partition_t *s_part;
static dg_rom_entry_t s_ent[DG_ROM_SLOTS];

/* one mapping at a time: the core keeps the pointer, so the map must outlive
 * dg_init() until the next slot is selected */
static esp_partition_mmap_handle_t s_map_handle;
static const uint8_t *s_map_ptr;
static int s_map_idx = -1;
static uint8_t *s_map_copy;             /* fallback when mmap is unavailable */

/* streaming writer */
static int      s_wr_idx = -1;
static uint32_t s_wr_len;
static uint32_t s_wr_off;
static uint32_t s_wr_expect;
static uint32_t s_wr_crc;

uint32_t dg_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t dg_crc32(const uint8_t *p, uint32_t n)
{
    return ~dg_crc32_update(0xFFFFFFFFu, p, n);
}

static uint32_t hdr_crc(const dg_rom_hdr_t *h)
{
    return dg_crc32((const uint8_t *)h, sizeof(dg_rom_hdr_t) - sizeof(uint32_t));
}

static uint32_t slot_base(int idx)
{
    return (uint32_t)idx * DG_ROM_SLOT_STRIDE;
}

static bool hdr_ok(const dg_rom_hdr_t *h)
{
    if (h->magic != DG_ROM_MAGIC) return false;
    if (h->hdr_crc != hdr_crc(h)) return false;
    if (h->len == 0 || h->len > DG_ROM_MAX_LEN) return false;
    if (h->name[DG_ROM_NAME_LEN - 1] != 0) return false;
    for (int i = 0; i < DG_ROM_NAME_LEN; i++) {
        char c = h->name[i];
        if (c == 0) break;
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return false;
    }
    return true;
}

static void read_hdr(int idx, dg_rom_hdr_t *h)
{
    memset(h, 0, sizeof *h);
    esp_partition_read(s_part, slot_base(idx), h, sizeof *h);
}

/* true when the payload still matches the CRC in the header */
static bool payload_ok(int idx, const dg_rom_hdr_t *h)
{
    uint8_t chunk[256];
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t off = 0; off < h->len; ) {
        uint32_t n = h->len - off;
        if (n > sizeof chunk) n = sizeof chunk;
        if (esp_partition_read(s_part, slot_base(idx) + DG_ROM_DATA_OFF + off, chunk, n) != ESP_OK) {
            return false;
        }
        crc = dg_crc32_update(crc, chunk, n);
        off += n;
    }
    return ~crc == h->crc;
}

esp_err_t dg_rom_store_init(void)
{
    memset(s_ent, 0, sizeof s_ent);
    if (s_part) return ESP_OK;

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)0x40, "roms");
    if (!s_part) {
        ESP_LOGE(TAG, "no 'roms' data partition in the table");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_part->size < (uint32_t)DG_ROM_SLOTS * DG_ROM_SLOT_STRIDE) {
        ESP_LOGW(TAG, "roms partition is %u B, slot table needs %u B",
                 (unsigned)s_part->size,
                 (unsigned)((uint32_t)DG_ROM_SLOTS * DG_ROM_SLOT_STRIDE));
    }

    int found = 0;
    for (int i = 0; i < DG_ROM_SLOTS; i++) {
        dg_rom_hdr_t h;
        read_hdr(i, &h);
        if (!hdr_ok(&h)) continue;
        if (!payload_ok(i, &h)) {
            ESP_LOGW(TAG, "slot %d (%s) crc mismatch, ignored", i, h.name);
            continue;
        }
        s_ent[i].valid = true;
        s_ent[i].len = h.len;
        s_ent[i].crc = h.crc;
        memcpy(s_ent[i].name, h.name, DG_ROM_NAME_LEN);
        found++;
        ESP_LOGI(TAG, "slot %d: %s, %u B, crc %08x",
                 i, s_ent[i].name, (unsigned)h.len, (unsigned)h.crc);
    }
    ESP_LOGI(TAG, "rom store ready: %d/%d slots (%u B partition)",
             found, DG_ROM_SLOTS, (unsigned)s_part->size);
    return ESP_OK;
}

int dg_rom_store_slots(void) { return DG_ROM_SLOTS; }

bool dg_rom_store_get(int idx, dg_rom_entry_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (idx < 0 || idx >= DG_ROM_SLOTS) return false;
    *out = s_ent[idx];
    return s_ent[idx].valid;
}

int dg_rom_store_first(void)
{
    for (int i = 0; i < DG_ROM_SLOTS; i++) if (s_ent[i].valid) return i;
    return -1;
}

int dg_rom_store_next(int idx)
{
    if (dg_rom_store_first() < 0) return -1;
    for (int k = 1; k <= DG_ROM_SLOTS; k++) {
        int i = (idx + k) % DG_ROM_SLOTS;
        if (s_ent[i].valid) return i;
    }
    return -1;
}

static void unmap(void)
{
    if (s_map_handle) {
        esp_partition_munmap(s_map_handle);
        s_map_handle = 0;
    }
    s_map_ptr = NULL;
    s_map_idx = -1;
}

const uint8_t *dg_rom_store_map(int idx, uint32_t *len)
{
    if (!s_part || idx < 0 || idx >= DG_ROM_SLOTS || !s_ent[idx].valid) {
        if (len) *len = 0;
        return NULL;
    }
    if (s_map_idx == idx && s_map_ptr) {
        if (len) *len = s_ent[idx].len;
        return s_map_ptr;
    }
    unmap();

    /* Copy the image into RAM rather than mmap'ing it out of flash.
     *
     * Measured on the C3: a SKIP TIME fast-forward executes ~500k instructions
     * per second, and every one of them fetches from the ROM.  With the image in
     * flash those fetches fight the instruction stream for the same tiny cache,
     * which is what made a 30-minute skip take 38 s no matter how the core was
     * compiled.  16 KiB of RAM buys a straight ~2x here. */
    if (!s_map_copy) {
        s_map_copy = (uint8_t *)heap_caps_malloc(DG_ROM_MAX_LEN, MALLOC_CAP_8BIT);
    }
    if (s_map_copy) {
        if (esp_partition_read(s_part, slot_base(idx) + DG_ROM_DATA_OFF,
                               s_map_copy, s_ent[idx].len) == ESP_OK) {
            s_map_ptr = s_map_copy;
            s_map_idx = idx;
            if (len) *len = s_ent[idx].len;
            return s_map_ptr;
        }
    }

    /* no RAM for a copy: fall back to the flash mapping */
    const void *ptr = NULL;
    esp_err_t e = esp_partition_mmap(s_part, slot_base(idx) + DG_ROM_DATA_OFF,
                                     DG_ROM_MAX_LEN, ESP_PARTITION_MMAP_DATA,
                                     &ptr, &s_map_handle);
    if (e == ESP_OK && ptr) {
        s_map_ptr = (const uint8_t *)ptr;
        s_map_idx = idx;
        if (len) *len = s_ent[idx].len;
        return s_map_ptr;
    }
    if (len) *len = 0;
    return NULL;
}

#if 0
/* original mmap-first implementation, kept for reference */
static const uint8_t *dg_rom_store_map_old(int idx, uint32_t *len)
{
    if (!s_part || idx < 0 || idx >= DG_ROM_SLOTS || !s_ent[idx].valid) {
        if (len) *len = 0;
        return NULL;
    }
    unmap();

    const void *ptr = NULL;
    esp_err_t e = esp_partition_mmap(s_part, slot_base(idx) + DG_ROM_DATA_OFF,
                                     DG_ROM_MAX_LEN, ESP_PARTITION_MMAP_DATA,
                                     &ptr, &s_map_handle);
    if (e == ESP_OK && ptr) {
        s_map_ptr = (const uint8_t *)ptr;
        s_map_idx = idx;
        if (len) *len = s_ent[idx].len;
        return s_map_ptr;
    }
    if (len) *len = 0;
    return NULL;
}
#endif

esp_err_t dg_rom_store_read(int idx, uint32_t off, void *dst, uint32_t n)
{
    if (!s_part) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= DG_ROM_SLOTS || !s_ent[idx].valid) return ESP_ERR_INVALID_ARG;
    if (!dst || off + n > s_ent[idx].len) return ESP_ERR_INVALID_SIZE;
    return esp_partition_read(s_part, slot_base(idx) + DG_ROM_DATA_OFF + off, dst, n);
}

esp_err_t dg_rom_store_begin_write(int idx, uint32_t len)
{
    if (!s_part) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= DG_ROM_SLOTS) return ESP_ERR_INVALID_ARG;
    if (len == 0 || len > DG_ROM_MAX_LEN) return ESP_ERR_INVALID_SIZE;

    unmap();
    s_wr_idx = -1;
    esp_err_t e = esp_partition_erase_range(s_part,
                                            slot_base(idx) + DG_ROM_DATA_OFF,
                                            DG_ROM_MAX_LEN);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "erase slot %d data: %s", idx, esp_err_to_name(e));
        return e;
    }
    s_wr_idx = idx;
    s_wr_len = len;
    s_wr_off = 0;
    s_wr_crc = 0xFFFFFFFFu;
    s_wr_expect = 0;
    return ESP_OK;
}

esp_err_t dg_rom_store_write_chunk(const uint8_t *data, uint32_t len)
{
    if (!s_part || s_wr_idx < 0) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_OK;
    if (s_wr_off + len > s_wr_len) return ESP_ERR_INVALID_SIZE;

    esp_err_t e = esp_partition_write(s_part,
                                      slot_base(s_wr_idx) + DG_ROM_DATA_OFF + s_wr_off,
                                      data, len);
    if (e != ESP_OK) return e;
    s_wr_crc = dg_crc32_update(s_wr_crc, data, len);
    s_wr_off += len;
    return ESP_OK;
}

esp_err_t dg_rom_store_commit_write(const char *name, uint32_t expect_crc, uint32_t *out_crc)
{
    if (!s_part || s_wr_idx < 0) return ESP_ERR_INVALID_STATE;
    int idx = s_wr_idx;
    uint32_t crc = ~s_wr_crc;
    if (out_crc) *out_crc = crc;
    if (s_wr_off != s_wr_len) {
        dg_rom_store_abort_write();
        return ESP_ERR_INVALID_SIZE;
    }
    if (expect_crc && expect_crc != crc) {
        dg_rom_store_abort_write();
        ESP_LOGW(TAG, "slot %d: crc %08x != expected %08x", idx, (unsigned)crc, (unsigned)expect_crc);
        return ESP_ERR_INVALID_CRC;
    }

    dg_rom_hdr_t h;
    memset(&h, 0, sizeof h);
    h.magic = DG_ROM_MAGIC;
    h.len = s_wr_len;
    h.crc = crc;
    h.seq = 0;
    if (name && name[0]) {
        strncpy(h.name, name, DG_ROM_NAME_LEN - 1);
    } else {
        snprintf(h.name, sizeof h.name, "S%d", idx);
    }
    h.hdr_crc = hdr_crc(&h);

    esp_err_t e = esp_partition_erase_range(s_part, slot_base(idx), DG_ROM_DATA_OFF);
    if (e == ESP_OK) {
        e = esp_partition_write(s_part, slot_base(idx), &h, sizeof h);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "slot %d: publish header: %s", idx, esp_err_to_name(e));
        dg_rom_store_abort_write();
        return e;
    }
    s_ent[idx].valid = true;
    s_ent[idx].len = h.len;
    s_ent[idx].crc = h.crc;
    memcpy(s_ent[idx].name, h.name, DG_ROM_NAME_LEN);
    s_wr_idx = -1;
    s_map_idx = -1;
    ESP_LOGI(TAG, "slot %d committed: %s, %u B, crc %08x",
             idx, h.name, (unsigned)h.len, (unsigned)h.crc);
    return ESP_OK;
}

void dg_rom_store_abort_write(void)
{
    s_wr_idx = -1;
    s_wr_off = 0;
    s_wr_len = 0;
}

esp_err_t dg_rom_store_erase(int idx)
{
    if (!s_part) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= DG_ROM_SLOTS) return ESP_ERR_INVALID_ARG;
    if (s_map_idx == idx) unmap();
    esp_err_t e = esp_partition_erase_range(s_part, slot_base(idx), DG_ROM_DATA_OFF);
    if (e != ESP_OK) return e;
    memset(&s_ent[idx], 0, sizeof s_ent[idx]);
    return ESP_OK;
}
