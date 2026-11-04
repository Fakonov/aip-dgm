// main/digimon_app.c ???Digital Monster V1/V2/V3 on AI Passport (ESP32-C3)
//
// Real time needs ~151k instructions/s; measured 0.366 instructions per clock() call,
// so ~8300 clock() calls per 20 ms frame (0.027 us/call on x86 => ~0.2 ms/frame there).
// The 32x16 LCD is rendered into an LVGL canvas, scaled 10x (320x160).
// Long-press OK (>1.5 s) cycles V1 -> V2 -> V3; the choice is kept in NVS.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"

#include "digimon_core.h"
#include "digimon_segmap.h"
#include "digimon_icons.h"
#include "dg_rom_store.h"

#ifdef DG_SIM_UART0_INPUT
/* Simulator builds read the console from UART0: the wasm emulator models the
 * UART peripheral and nothing else, so the USB-Serial-JTAG console the device
 * uses is unreachable there (uart_input() lands on UART0 RX).  Real hardware
 * builds never define this -- UART0 TX is the backlight pin. */
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_rom_sys.h"
#include "esp_vfs_dev.h"
#endif

static const char *TAG = "digimon";

// ---- ROMs live in the 'roms' data partition ------------------------------
/* Nothing is embedded any more: V1/V2/V3 arrive over the USB console (the PC
 * tool's ROM upload) and sit in their own flash partition, so a firmware update
 * no longer has to carry -- or overwrite -- game images, and a brand new board
 * honestly reports that it has nothing to run.  See dg_rom_store.h for the
 * slot layout.  s_rom_idx < 0 means "no ROM": the glass shows NO ROM and the
 * emulator is never stepped. */
static dg_rom_entry_t s_roms[DG_ROM_SLOTS];
static int s_rom_idx = -1;
static bool s_have_rom;
static const uint8_t *s_rom_ptr;      /* mapped (or copied) image of s_rom_idx */
static uint32_t s_rom_len;


/* ---- glass geometry -------------------------------------------------------
 * ref/vpet_v1v2v3/屏幕版式说明.md measured the real glass off DigimonV1J.svg:
 * ONE piece of glass, three bands, all 32 columns wide, aspect 32 : 28.7.
 *
 *   +----------------------------+  <- top band, 4 dot-rows, 4 slots of 8 cols
 *   |  [1]     [2]     [3]    [4]|
 *   +----------------------------+  blank glass, 2.4 rows
 *   |                            |
 *   |        32 x 16 matrix      |  16 rows
 *   |                            |
 *   +----------------------------+  blank glass, 2.5 rows
 *   |  [5]     [6]     [7]    [8]|
 *   +----------------------------+  <- bottom band, 4 dot-rows
 *
 * Integer pitch only (the measured doc is explicit about that).  At 320x240 the
 * glass is 28.7 dot-rows tall, so the biggest pitch that fits is 7 -> 224x203:
 * pitch 8 would be 256x231 and its canvas needs 118 KB, but after LVGL init the
 * largest free block is ~112 KB and the malloc returns NULL (probed on the
 * simulator with an esp_rom_printf marker, screen came up blank).  Pitch 7 also
 * leaves a 48 px shell strip on each side for the HUD.
 * The bands are glass segments, not matrix rows, so the icon sprites are
 * slot-sized and blitted straight into the canvas instead of living as separate
 * LVGL widgets.
 */
#define DG_PITCH      7                          /* panel px per dot */
#define DG_BAND_H     (4 * DG_PITCH)             /* 28 */
#define DG_GAP_TOP    ((2 * DG_PITCH) + 3)       /* 2.4 rows -> 17 */
#define DG_GAP_BOT    ((2 * DG_PITCH) + 4)       /* 2.5 rows -> 18 */
#define DG_GLASS_W    (DG_LCD_W * DG_PITCH)      /* 224 */
#define DG_GLASS_H    (DG_BAND_H + DG_GAP_TOP + DG_LCD_H * DG_PITCH + DG_GAP_BOT + DG_BAND_H)
                                                 /* 28+17+112+18+28 = 203 */
#define DG_MAT_Y      DG_BAND_H + DG_GAP_TOP     /* matrix top inside the glass */
#define DG_SLOT_MAX   8                          /* 4 slots per band */

/* Sprites are 64x32 (an 8-column slot x a 4-row band) and carry the glass
 * colour as their background, so blitting them keeps the band seamless. */
#define DG_IMG_DSC(n,v) { \
    .header.magic = LV_IMAGE_HEADER_MAGIC, \
    .header.cf = LV_COLOR_FORMAT_RGB565, \
    .header.w = DG_SLOT_W, .header.h = DG_SLOT_H, \
    .data_size = sizeof(dg_slot_##n##_##v), .data = (const uint8_t *)dg_slot_##n##_##v }
static const lv_image_dsc_t dg_img_stat_o    = DG_IMG_DSC(stat,off);
static const lv_image_dsc_t dg_img_virus_o   = DG_IMG_DSC(virus,off);
static const lv_image_dsc_t dg_img_medic_o   = DG_IMG_DSC(medic,off);
static const lv_image_dsc_t dg_img_battle_o  = DG_IMG_DSC(battle,off);
static const lv_image_dsc_t dg_img_poop_o    = DG_IMG_DSC(poop,off);
static const lv_image_dsc_t dg_img_light_o   = DG_IMG_DSC(light,off);
static const lv_image_dsc_t dg_img_patch_o   = DG_IMG_DSC(patch,off);
static const lv_image_dsc_t dg_img_call_o    = DG_IMG_DSC(call,off);
static const lv_image_dsc_t dg_img_stat_f    = DG_IMG_DSC(stat,on);
static const lv_image_dsc_t dg_img_virus_f   = DG_IMG_DSC(virus,on);
static const lv_image_dsc_t dg_img_medic_f   = DG_IMG_DSC(medic,on);
static const lv_image_dsc_t dg_img_battle_f  = DG_IMG_DSC(battle,on);
static const lv_image_dsc_t dg_img_poop_f    = DG_IMG_DSC(poop,on);
static const lv_image_dsc_t dg_img_light_f   = DG_IMG_DSC(light,on);
static const lv_image_dsc_t dg_img_patch_f   = DG_IMG_DSC(patch,on);
static const lv_image_dsc_t dg_img_call_f    = DG_IMG_DSC(call,on);

static const lv_image_dsc_t *const ICON_OUTLINE[8] = {
    &dg_img_stat_o, &dg_img_virus_o, &dg_img_medic_o, &dg_img_battle_o,
    &dg_img_poop_o, &dg_img_light_o, &dg_img_patch_o, &dg_img_call_o,
};
static const lv_image_dsc_t *const ICON_FILLED[8] = {
    &dg_img_stat_f, &dg_img_virus_f, &dg_img_medic_f, &dg_img_battle_f,
    &dg_img_poop_f, &dg_img_light_f, &dg_img_patch_f, &dg_img_call_f,
};

/* name shown in the menu / HUD / log lines; "--" is the empty glass */
static const char *rom_name(int idx)
{
    if (idx < 0 || idx >= DG_ROM_SLOTS || !s_roms[idx].valid) return "--";
    return s_roms[idx].name;
}

/* The eight function icons are not decoration: each one is a real LCD segment
 * outside the 32x16 dot matrix, and the ROM itself drives it.  Measured on the
 * V1 ROM (probe_ui_state.c / probe_key_matrix.c):
 *   - mode 2..8 light the first seven in exactly this order (A cycles through
 *     them, B enters, C cancels), so we must NOT invent a cursor;
 *   - (0,0) lights when the egg hatches and stays on while the pet wants
 *     attention (the alert/call indicator).
 * Index order matches ICON_OUTLINE[]: status, virus, medic, battle, poop,
 * light, patch, call. */
static const uint8_t ICON_SEG[8][2] = {
    { 137, 3 }, { 155, 3 }, { 157, 3 }, { 159, 3 },
    { 22, 0 },  { 20, 0 },  { 18, 0 },  { 0, 0 },
};
static uint8_t s_icon_state = 0xFF;   /* last state logged; 0xFF = repaint */
static bool s_icon_stale = true;      /* the canvas was wiped, bands need a blit */

// ---- layout -------------------------------------------------------------
static int s_view_w = DG_GLASS_W;
static int s_view_h = DG_GLASS_H;
static lv_obj_t *s_canvas;
static int s_view_x, s_view_y;          /* canvas origin on the panel */
#define FRAME_MS        20
/* Pace by physical time, not by a fixed step count: OSC1 runs at 32768 Hz no
 * matter which clock the CPU is on, and dg_step() burns 7 OSC1 ticks in slow
 * mode but only ~0.22 in fast mode. 20 ms therefore equals 655 ticks exactly.
 * (The old fixed 8300 steps/frame ran the game at roughly 2.7x speed.) */
#define OSC1_HZ         32768
#define TICKS_PER_FRAME (OSC1_HZ * FRAME_MS / 1000)
#define STEP_GUARD      40000
#define ROM_HOLD_US     1500000                 /* long-press OK to switch ROM */

/* ---- glass palette + shell colourways -------------------------------------
 * The glass (bands + matrix) is one fixed piece of hardware, so its three tones
 * do not depend on the shell: base, the 1 px inter-dot gap, and a lit dot.
 * The gap is what actually draws the grid; round 55 used a gap 24 sRGB steps
 * brighter than the base, which at this size read as wire mesh.  Tone 1 (+15)
 * is the default, see docs/icons/lcd_tone_compare.png for the 4x comparison.
 *
 * The shell is what changes between the 14 retail colourways
 * (ref/vpet_v1v2v3/元祖暴龙???全配色总表.png): V1 brown/grey + red/green/blue/
 * yellow, V2 black/white + clear-black/clear-white, V3 orange/purple + clear.
 * Only the surround and the HUD ink move, because the icon sprites are baked
 * against the glass colour and must stay shell-independent.
 * NOTE: the values above are 24-bit RGB888 for lv_color_hex(); the canvas
 * buffer is RGB565.  Mixing the two paints the bezel bright blue. */
#define DG_LCD_TONE 1
static const uint16_t GLASS_TONE[3][3] = {
    /*   base      gap       lit        (hex in the comment) */
    { 0x9D4E, 0xB611, 0x1902 },   /* 0  #9CAA73 #B5C28C #182010  round 55, +24 */
    { 0x950D, 0xA56F, 0x1922 },   /* 1  #93A06B #A2AF7B #1B2417  default,   +15 */
    { 0x8CCC, 0x952E, 0x1922 },   /* 2  #8C9A66 #96A473 #1B2417  deep,      +10 */
};
#define GLASS_BASE_888 0x93A06B       /* must stay in sync with tone 1 */

typedef struct {
    const char *name;
    uint32_t body;      /* shell plastic */
    uint32_t rim;       /* bezel ring   */
    uint32_t hi;        /* 1 px highlight on the rim */
    uint32_t well;      /* recess around the glass */
    uint32_t hud;       /* HUD ink on the shell */
} shell_t;

/* Order = the retail order of the colourway sheet, so "SHELL" in the menu
 * walks the same sequence as the reference photo. */
static const shell_t SHELLS[] = {
    { "BROWN",  0x8A5A3C, 0x6B422A, 0xA87652, 0x382418, 0xF2E6D6 },  /* V1 初版???*/
    { "GREY",   0x9AA0A2, 0x6E7476, 0xBFC4C6, 0x2A2E30, 0x1E2426 },  /* V1 初版???*/
    { "RED",    0xB4332C, 0x86231E, 0xD66255, 0x3A1412, 0xFBECE6 },  /* V1 追加???*/
    { "GREEN",  0x3E7A46, 0x2B5932, 0x63A06B, 0x142A18, 0xEFF7EC },  /* V1 追加???*/
    { "BLUE",   0x2F5C8F, 0x1F3F66, 0x5A8CC0, 0x111E33, 0xE8F1FA },  /* V1 追加???*/
    { "YELLOW", 0xD8B33C, 0xA88A24, 0xEFD76E, 0x3A2F0C, 0x2A2306 },  /* V1 追加???*/
    { "BLACK",  0x2B2B2B, 0x141414, 0x555555, 0x0A0A0A, 0xD8D8D8 },  /* V2 初版???*/
    { "WHITE",  0xEDEDE6, 0xC2C2B8, 0xFFFFFF, 0x2E2E2A, 0x22221F },  /* V2 初版???*/
    { "CLEARB", 0x3A3A3E, 0x1E1E22, 0x6A6A70, 0x14141A, 0xE2E2E8 },  /* V2 透明???*/
    { "CLEARW", 0xDDDCD2, 0xB4B3A8, 0xF6F5EE, 0x2A2A26, 0x242420 },  /* V2 透明???*/
    { "ORANGE", 0xE0701C, 0xB0520F, 0xFF9A44, 0x3D1E06, 0x2B1604 },  /* V3 ???*/
    { "PURPLE", 0x6B4C9A, 0x4A3270, 0x9077C4, 0x231636, 0xF0E9FA },  /* V3 ???*/
    { "DARKGREY", 0x3C4143, 0x2A2E30, 0x5E6669, 0x171A1B, 0xD9D4C6 },/* 深灰（默认） */
    { "CREAM",  0xE9E6D8, 0x6D7A52, 0x8A966C, 0x333A28, 0x4A523A },  /* 工程米色 */
};
#define SHELL_COUNT ((int)(sizeof(SHELLS) / sizeof(SHELLS[0])))
#define SHELL_DEFAULT (SHELL_COUNT - 2)          /* DARKGREY */
static uint8_t s_shell = SHELL_DEFAULT;
#define PAPER_COLOR   SHELLS[s_shell].body
#define BEZEL_COLOR   SHELLS[s_shell].rim
#define BEZEL_HI      SHELLS[s_shell].hi
#define RECESS_COLOR  SHELLS[s_shell].well
#define HUD_COLOR     SHELLS[s_shell].hud
#define HUD_STRONG    SHELLS[s_shell].hud
/* ---- buzzer audio: 1-bit speaker on R4, rendered as a square wave ---- */
#define AUDIO_HZ     16000
#define AUDIO_SAMPLES (AUDIO_HZ * FRAME_MS / 1000)     /* 320 @ 20 ms */
typedef struct { int16_t pcm[AUDIO_SAMPLES]; } audio_block_t;
static QueueHandle_t s_audio_q;
static double s_phase;

static uint16_t *s_canvas_buf;
static uint8_t s_last_pic[DG_LCD_H][DG_LCD_W];   /* shadow, for dirty detection */
static bool s_pic_valid;
static lv_obj_t *s_canvas;
static QueueHandle_t s_key_q;
static uint32_t s_frames;

/* minimum-hold shim for the emulated keys (see the drain loop in dg_task) */
static uint8_t s_key_mask;        /* mask of the key currently held down */
static int64_t s_key_down_us;     /* esp_timer timestamp of that press */
static uint8_t s_key_pending;     /* release waiting for the minimum hold */
static int64_t s_key_release_at;

/* Long-hold OK opens the settings menu: 3 s, and it fires *while the key is
 * still down* - no need to let go first.  s_ok_held_consumed then swallows the
 * release, otherwise that release would land in the menu, where a short C press
 * means "close", and the panel would flash open and shut. */
#define MENU_HOLD_US 3000000
static bool s_ok_down;
static bool s_ok_armed;           /* a real OK press is being timed */
static bool s_ok_held_consumed;   /* the hold already opened the menu */
static int64_t s_ok_press_us;
static uint8_t s_wake_mask;       /* key whose press woke the screen */

typedef struct { uint8_t mask; uint8_t down; } dg_key_ev_t;

/* The ROM needs a key to be held for ~120 ms to see it (measured), so short
 * taps are stretched by the forwarding layer instead of by a timer here. */
#define KEY_MIN_HOLD_US 260000       /* >= 2 samples of the ROM's key scan */

// ---- NVS ----------------------------------------------------------------
static void load_saved_rom(void)
{
    nvs_handle_t h;
    uint8_t v = 0xFF;
    if (nvs_open("digimon", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "rom", &v) != ESP_OK) v = 0xFF;
        nvs_close(h);
    }
    if (v < DG_ROM_SLOTS && s_roms[v].valid) {
        s_rom_idx = (int)v;
        return;
    }
    /* the remembered slot is gone (erased over USB, or this is a fresh board) */
    s_rom_idx = dg_rom_store_first();
}

static void save_rom_index(int idx)
{
    nvs_handle_t h;
    if (nvs_open("digimon", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "rom", (uint8_t)idx);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- per-ROM save slots -------------------------------------------------
 * The toy itself has no storage, but the emulated machine is just registers
 * plus RAM, so a whole "pet" fits in ~1.3 KB. Each ROM version gets its own
 * slot, checked against a CRC of that ROM, so V1/V2/V3 never mix. Saves are
 * written every SAVE_INTERVAL_US and whenever something big happens (settings
 * menu, ROM switch), because the power key is hardware and cuts power without
 * telling the firmware. */
#define SAVE_MAGIC       0x31474744u          /* "DGG1" */
#define SAVE_INTERVAL_US (60 * 1000 * 1000)
#define SAVE_BUF_SIZE    (4096)

typedef struct {
    uint32_t magic;
    uint32_t rom_crc;
    uint32_t state_size;
    uint32_t state_crc;
    uint32_t seq;
} save_hdr_t;

static uint8_t  s_save_buf[SAVE_BUF_SIZE];
static uint32_t s_rom_crc[DG_ROM_SLOTS];
static uint32_t s_save_seq;
static int64_t  s_last_save_us;

static uint32_t crc32_calc(const uint8_t *p, uint32_t n)
{
    return dg_crc32(p, n);
}

static void rom_crc_init(void)
{
    for (int i = 0; i < DG_ROM_SLOTS; i++) s_rom_crc[i] = s_roms[i].crc;
}

/* Pulls the slot table out of the partition layer into the RAM copy used by
 * the menu / HUD / save-slot checks. */
static void rom_table_load(void)
{
    for (int i = 0; i < DG_ROM_SLOTS; i++) dg_rom_store_get(i, &s_roms[i]);
}

static void switch_rom(int idx);      /* defined with the settings menu */
static void reset_save(int idx);      /* defined with the ROM slot list */
static void ui_rom_state(void);       /* defined with the HUD */
static void ui_rom_state_locked(void);/* same, for callers that hold the lock */
static void rom_activate(int idx);    /* defined with the console commands */
static int  con_getc(void);           /* defined with the console transport */

/* ---- game clock RAM map (1997 firmware, page-0 nibbles) -----------------
 * Reverse-engineered addresses (see dg_ram() in digimon_core.h).  RAM is
 * nibble organised, so every BCD number is split low/high across two adjacent
 * addresses -- reading only the low nibble is what made the first attempt at
 * this feature look like "seconds never move".
 *
 *   clock : 1A sec-lo  1B sec-hi  1C min-lo  1D min-hi  1E hour(1-12)  1F PM
 *   stage : 46 sec-lo  47 sec-hi  48 min-lo  49 min-hi  4A hr-lo  4B hr-hi
 *   next  : 4C min-lo  4D min-hi  4E hr-lo  4F hr-hi
 *
 * Two decoding traps, both read off the disassembly and confirmed on the unit:
 *
 *   - MINUTES ARE BCD, HOURS ARE NOT.  M(48-49) and M(4C-4D) are two decimal
 *     nibbles (23 min = 0x03,0x02), but M(4A-4B) and M(4E-4F) are a plain
 *     8-bit binary hour count split low/high (26 h = 4A=0xA, 4B=0x1).  BCD
 *     decoding the hour is correct only while it is below 16 and then drifts:
 *     at 0x1A (26 h) it reads 20 h, which is what made the toy look overdue.
 *   - next_evolution_at is FOUR nibbles.  Reading only 4E throws away the top
 *     half of the hour, so a 45-hour target (0x2D) displayed as 13 hours.
 *
 * The ROM does not evolve on "past due".  evolution_check at 0x0596 walks the
 * four nibbles of M(48-4B) against M(4C-4F) and only starts mode 9 when all
 * four are EQUAL; step over that minute and the evolution is lost for good.
 * Host probe (evo_probe.c): parked at next-2 it evolved 94 s later, parked at
 * next+5 it never did.  An hour byte of 0xFF in M(4E-4F) means "no further
 * evolution" (that is also how the ROM retires a target it missed).
 */
#define R_CLOCK_MIN_LO 0x01C
#define R_CLOCK_MIN_HI 0x01D
#define R_CLOCK_HOUR   0x01E
#define R_CLOCK_PM     0x01F
#define R_STAGE_MIN_LO 0x048
#define R_STAGE_MIN_HI 0x049
#define R_STAGE_HR_LO  0x04A
#define R_STAGE_HR_HI  0x04B
#define R_NEXT_MIN_LO  0x04C
#define R_NEXT_MIN_HI  0x04D
#define R_NEXT_HR_LO   0x04E
#define R_NEXT_HR_HI   0x04F
#define R_SPECIES_SLOT 0x00B
#define OSC1_PER_SEC   32768u

/* 养育总时间：ROM 的 stage 计时会在进化时清零，所以"这只养了多久"只能由固件
 * 自己累计 —— 每帧把 OSC1 增量折成秒，孵化那一刻（species_slot 0 -> 1）归零。
 * 跟着 ROM 槽存 NVS（a0..a7），切走再切回来还能接上。 */
static uint32_t s_pet_sec;
static uint32_t s_age_frac;
static uint32_t s_age_last_tick;
static uint8_t  s_last_species_slot;
static void age_reset(void);              /* defined after rom_load() */
static bool save_slot(int idx);           /* defined with the NVS save slots */
static lv_obj_t *s_skip_lbl;              /* "SKIP 2H" card while fast-forwarding */

static int ram_bcd(int lo, int hi)
{
    return dg_ram(hi) * 10 + dg_ram(lo);
}

/* 8-bit counter split low nibble / high nibble.  Counter-intuitive but real:
 * the stage and next-evolution HOURS are binary, only the minutes are BCD. */
static int ram_u8(int lo, int hi)
{
    return dg_ram(lo) | (dg_ram(hi) << 4);
}


/* minutes since this growth stage started / at which the next evolution fires */
static int stage_minutes_now(void)
{
    return ram_bcd(R_STAGE_MIN_LO, R_STAGE_MIN_HI) +
           ram_u8(R_STAGE_HR_LO, R_STAGE_HR_HI) * 60;
}

/* false once the ROM has retired the target (hour byte 0xFF) */
static bool stage_next_pending(void)
{
    return ram_u8(R_NEXT_HR_LO, R_NEXT_HR_HI) != 0xFF;
}

static int stage_minutes_next(void)
{
    return ram_bcd(R_NEXT_MIN_LO, R_NEXT_MIN_HI) +
           ram_u8(R_NEXT_HR_LO, R_NEXT_HR_HI) * 60;
}




/* Fast-forwards the pet by `minutes` of game time.
 *
 * This really executes those minutes instead of poking the clock registers.
 * Writing the stage clock forward does not work: the ROM only notices an
 * evolution when its own tick counters walk over next_evolution_at, so a
 * 0 -> 120 minute poke leaves the egg unhatched forever (measured in the host
 * probe and in the simulator).  The C3 runs the core ~55x real time, so the
 * cost is ~1.8 s per game minute: 5M = 9 s, 30M = 55 s, 2H = 3.5 min.
 *
 * Three things matter for the owner:
 *   - the result is written to NVS before returning, otherwise a power cycle
 *     silently throws the skipped time away (that was the "restart and nothing
 *     happened" report);
 *   - the key queue is polled inside the loop so C cancels instead of the toy
 *     looking dead for three minutes;
 *   - the label is updated a few times a second, which is what convinces the
 *     user it is working rather than hung. */
static void rom_skip_minutes(int minutes, const char *label)
{
    if (!s_have_rom || minutes <= 0) return;
    const uint32_t total_ticks = (uint32_t)minutes * 60u * OSC1_PER_SEC;

    /* Both entry points -- the SKIP TIME row and the console's `SKIP n` -- have
     * to show the same card.  The console path passed label=NULL and only
     * rewrote the (still hidden) label, so a 2 h skip ran with a frozen screen
     * and looked exactly like a crash. */
    char tag[24];
    if (!label) {
        snprintf(tag, sizeof tag, "SKIP %dM", minutes);
        label = tag;
    }
    if (s_skip_lbl && bsp_lvgl_lock(200)) {
        lv_label_set_text_fmt(s_skip_lbl, "%s\nC=STOP", label);
        lv_obj_remove_flag(s_skip_lbl, LV_OBJ_FLAG_HIDDEN);
        bsp_lvgl_unlock();
    }

    uint32_t start = dg_osc1_ticks();
    uint32_t target = start + total_ticks;
    long guard = 0;
    bool cancelled = false;
    int last_pct = -1;

    /* let LVGL actually paint the SKIP card before the core hogs the CPU */
    vTaskDelay(pdMS_TO_TICKS(120));

    int batch = 0;
    while ((int32_t)(dg_osc1_ticks() - target) < 0) {
        for (int i = 0; i < 8192; i++) { dg_step(); guard++; }
        batch++;

        dg_key_ev_t e;
        while (xQueueReceive(s_key_q, &e, 0) == pdTRUE) {
            if (e.mask == 1 && e.down) cancelled = true;   /* C cancels */
        }
        if (cancelled) break;

        /* Yield 1 ms out of every ~65k steps (~11 % of the CPU): enough for the
         * progress number to visibly tick over without dragging the skip out.
         * An earlier version yielded every 4096 steps and threw 95 % of the CPU
         * away, which is why a 2 h skip took over three minutes. */
        if ((batch & 7) == 0) {
            int pct = (int)(((uint64_t)(dg_osc1_ticks() - start) * 100u) / total_ticks);
            if (pct != last_pct && s_skip_lbl && bsp_lvgl_lock(20)) {
                last_pct = pct;
                lv_label_set_text_fmt(s_skip_lbl, "%s  %d%%\nC=STOP", label, pct);
                bsp_lvgl_unlock();
            }
            vTaskDelay(1);
        }
    }

    uint32_t done_ticks = dg_osc1_ticks() - start;
    s_pet_sec += done_ticks / OSC1_PER_SEC;   /* skipped time counts as owned time */
    s_age_last_tick = dg_osc1_ticks();        /* do not count it twice in age_tick */
    s_age_frac = 0;
    s_pic_valid = false;
    s_icon_state = 0xFF;
    s_icon_stale = true;

    /* Commit right away: the autosave only runs every 60 s, and a toy that is
     * still busy skipping is exactly where the owner reaches for the power
     * switch.  Losing the skip here was the bug behind "rebooted, time did not
     * move". */
    save_slot(s_rom_idx);

    if (s_skip_lbl && bsp_lvgl_lock(200)) {
        lv_obj_add_flag(s_skip_lbl, LV_OBJ_FLAG_HIDDEN);
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "skip %s: %s at %ld steps, stage %d/%d, age %lu s",
             cancelled ? "cancelled" : "done", label ? label : "-", guard,
             stage_minutes_now(), stage_minutes_next(), (unsigned long)s_pet_sec);
}


/* Maps slot idx and hands it to the core.  The core keeps the pointer, so the
 * mapping stays alive until the next slot is selected (dg_rom_store_map()
 * drops the previous one).  A slot whose image cannot be mapped reads as empty
 * rather than booting whatever happens to be in RAM. */
static bool rom_load(int idx)
{
    uint32_t len = 0;
    const uint8_t *p = dg_rom_store_map(idx, &len);
    if (!p || len == 0) {
        s_rom_ptr = NULL;
        s_rom_len = 0;
        return false;
    }
    s_rom_ptr = p;
    s_rom_len = len;
    dg_init(p, len);
    age_reset();
    s_pic_valid = false;
    s_icon_state = 0xFF;
    s_icon_stale = true;
    return true;
}

static void age_nvs_key(int idx, char *out, size_t n)
{
    snprintf(out, n, "a%d", idx);
}

/* 新 ROM 刚挂上时调用：RAM 全 0，age 从头开始，等孵化那一刻再清零一次 */
static void age_reset(void)
{
    s_pet_sec = 0;
    s_age_frac = 0;
    s_age_last_tick = dg_osc1_ticks();
    s_last_species_slot = dg_ram(R_SPECIES_SLOT);
}

static void age_load(int idx)
{
    age_reset();
    if (idx < 0) return;
    nvs_handle_t h;
    if (nvs_open("digimon", NVS_READONLY, &h) != ESP_OK) return;
    char key[16];
    age_nvs_key(idx, key, sizeof key);
    uint32_t v = 0;
    if (nvs_get_u32(h, key, &v) == ESP_OK && v < 100000000ul) s_pet_sec = v;
    nvs_close(h);
}

static void age_save(int idx)
{
    if (idx < 0) return;
    nvs_handle_t h;
    if (nvs_open("digimon", NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    age_nvs_key(idx, key, sizeof key);
    nvs_set_u32(h, key, s_pet_sec);
    nvs_commit(h);
    nvs_close(h);
}

/* Play time of any slot without disturbing the live counter: the active ROM
 * reads from RAM (it is fresher than NVS by up to one autosave), the others
 * straight out of their NVS key.  Used by the ROM slot list. */
static uint32_t age_peek(int idx)
{
    if (idx < 0 || idx >= DG_ROM_SLOTS) return 0;
    if (idx == s_rom_idx) return s_pet_sec;
    nvs_handle_t h;
    if (nvs_open("digimon", NVS_READONLY, &h) != ESP_OK) return 0;
    char key[16];
    age_nvs_key(idx, key, sizeof key);
    uint32_t v = 0;
    if (nvs_get_u32(h, key, &v) != ESP_OK || v >= 100000000ul) v = 0;
    nvs_close(h);
    return v;
}

/* 12H34M / 47M12S / 100H+ -- the shape the old AGE row used */
static void fmt_play_time(char *buf, size_t n, uint32_t sec)
{
    if (sec >= 100u * 3600u)
        snprintf(buf, n, "%uH+", (unsigned)(sec / 3600u));
    else if (sec >= 3600u)
        snprintf(buf, n, "%uH%02uM", (unsigned)(sec / 3600u), (unsigned)((sec / 60u) % 60u));
    else
        snprintf(buf, n, "%uM%02uS", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

/* 每帧调用：孵化检测 + 按 OSC1 增量累加。跳过时间时由 rom_skip_minutes()
 * 直接补，所以这里只处理真实流逝。 */
static void age_tick(void)
{
    if (!s_have_rom) return;
    uint8_t slot = dg_ram(R_SPECIES_SLOT);
    if (slot >= 1 && s_last_species_slot == 0) {
        s_pet_sec = 0;
        s_age_frac = 0;
        ESP_LOGI(TAG, "hatched: age counter starts at 0");
    }
    s_last_species_slot = slot;

    uint32_t now = dg_osc1_ticks();
    s_age_frac += now - s_age_last_tick;
    s_age_last_tick = now;
    if (s_age_frac >= OSC1_PER_SEC) {
        s_pet_sec += s_age_frac / OSC1_PER_SEC;
        s_age_frac %= OSC1_PER_SEC;
    }
}

static void slot_key(int idx, char *out, size_t n)
{
    snprintf(out, n, "s%d", idx);
}

static bool save_slot(int idx)
{
    if (idx < 0 || idx >= DG_ROM_SLOTS || !s_roms[idx].valid) return false;
    save_hdr_t *hdr = (save_hdr_t *)s_save_buf;
    uint8_t *st = s_save_buf + sizeof(save_hdr_t);
    uint32_t size = dg_state_size();
    if (sizeof(save_hdr_t) + size > SAVE_BUF_SIZE) return false;

    dg_state_save(st);
    hdr->magic = SAVE_MAGIC;
    hdr->rom_crc = s_rom_crc[idx];
    hdr->state_size = size;
    hdr->state_crc = crc32_calc(st, size);
    hdr->seq = ++s_save_seq;

    nvs_handle_t h;
    if (nvs_open("digimon", NVS_READWRITE, &h) != ESP_OK) return false;
    char key[16];
    slot_key(idx, key, sizeof key);
    esp_err_t e = nvs_set_blob(h, key, s_save_buf, sizeof(save_hdr_t) + size);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) {
        s_last_save_us = esp_timer_get_time();
        age_save(idx);
        ESP_LOGI(TAG, "saved %s (%u B)", rom_name(idx), (unsigned)size);
    } else {
        ESP_LOGW(TAG, "save %s failed: %s", rom_name(idx), esp_err_to_name(e));
    }
    return e == ESP_OK;
}

static bool load_slot(int idx)
{
    if (idx < 0 || idx >= DG_ROM_SLOTS || !s_roms[idx].valid) return false;
    nvs_handle_t h;
    if (nvs_open("digimon", NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    slot_key(idx, key, sizeof key);
    size_t len = sizeof(s_save_buf);
    esp_err_t e = nvs_get_blob(h, key, s_save_buf, &len);
    nvs_close(h);
    if (e != ESP_OK || len < sizeof(save_hdr_t)) return false;

    const save_hdr_t *hdr = (const save_hdr_t *)s_save_buf;
    const uint8_t *st = s_save_buf + sizeof(save_hdr_t);
    if (hdr->magic != SAVE_MAGIC) return false;
    if (hdr->rom_crc != s_rom_crc[idx]) return false;      /* other ROM version */
    if (hdr->state_size != dg_state_size()) return false;  /* core changed */
    if (len != sizeof(save_hdr_t) + hdr->state_size) return false;
    if (hdr->state_crc != crc32_calc(st, hdr->state_size)) return false;
    if (dg_state_load(st, hdr->state_size) != 0) return false;

    if (hdr->seq > s_save_seq) s_save_seq = hdr->seq;
    s_last_save_us = esp_timer_get_time();
    age_load(idx);
    ESP_LOGI(TAG, "restored %s (seq %u)", rom_name(idx), (unsigned)hdr->seq);
    return true;
}

// ---- buttons ------------------------------------------------------------
static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    /* UP = A (cycles the function icons, and adjusts hours in the clock screen)
     * DOWN = B (enters the highlighted function)
     * OK = C (cancels / back)
     * Verified against the ROM: mask 4 turns mode 0 into 2..8 step by step,
     * mask 2 sets submode 1 (enter) and toggles the clock screen, mask 1 backs
     * out.  Every event is forwarded -- the ROM owns the whole interaction. */
    uint8_t mask = btn == BSP_BTN_UP ? 4 : (btn == BSP_BTN_DOWN ? 2 : 1);

    /* The BSP also reports CLICK / DOUBLE / LONG.  Treating those as releases
     * (as the old `ev == PRESS ? 1 : 0` did) cut every long press short at the
     * driver's 500 ms long-press timer, which is why holding OK could never
     * reach the 1.2 s menu threshold.  Only the raw edges matter here. */
    if (ev != BSP_BTN_PRESS && ev != BSP_BTN_RELEASE) return;

    dg_key_ev_t e = { mask, (uint8_t)(ev == BSP_BTN_PRESS ? 1 : 0) };
    xQueueSend(s_key_q, &e, 0);
}

// ---- rendering ----------------------------------------------------------
/* Returns 1 if anything was redrawn. Digimon's LCD is segment driven and only
 * changes a few times per second, so repainting the whole glass every frame
 * would eat the whole 20 ms budget on a single-core C3 (that showed up as a
 * task watchdog reset on the first hardware run). Compare against a 512-byte
 * shadow and touch the canvas only on a real change.
 *
 * The canvas is the *whole glass*: bands included, filled with the base tone,
 * with the 32x16 matrix stamped in the middle.  Putting the bands on the same
 * surface as the matrix is what the real glass looks like - they are segments
 * on one piece of glass, 2.4 / 2.5 dot-rows away from the matrix, not rows of
 * it (ref/vpet_v1v2v3/屏幕版式说明.md). */
static int render_lcd(void)
{
    const uint8_t *v = dg_vram();
    const uint32_t vlen = dg_vram_len();
    uint8_t cur[DG_LCD_H][DG_LCD_W];
    bool changed = !s_pic_valid;

    for (int r = 0; r < DG_LCD_H; r++) {
        for (int c = 0; c < DG_LCD_W; c++) {
            uint16_t cell = dg_segmap[r][c];
            uint8_t byte = (uint8_t)(cell & 0xFF);
            uint8_t bit  = (uint8_t)(cell >> 8);
            uint8_t on = (byte < vlen) && ((v[byte] >> bit) & 1u) ? 1u : 0u;
            cur[r][c] = on;
            if (on != s_last_pic[r][c]) changed = true;
        }
    }

    if (!changed) return 0;

    const uint16_t *tone = GLASS_TONE[DG_LCD_TONE];
    const uint16_t base = tone[0], gap = tone[1];

    /* 1. the whole glass, including both icon bands and the blank margins */
    for (int i = 0; i < s_view_w * s_view_h; i++) s_canvas_buf[i] = base;

    /* 2. the dot matrix: every cell is PITCH square and carries the grid line
     *    on its right and bottom, so the mesh closes at the bottom and on the
     *    right.  The block is exactly 32 x 16 pitches (224 x 112), which leaves
     *    no pixel for a line above row 0 - with only that rule the top row sat
     *    open against the blank glass.  Steal one pixel from the top margin and
     *    draw the closing line there.  (The left edge is flush with the glass
     *    itself and needs none: the bezel recess is the border.) */
    {
        uint16_t *top = &s_canvas_buf[(DG_MAT_Y - 1) * s_view_w];
        for (int x = 0; x < s_view_w; x++) top[x] = gap;
    }
    const int p = DG_PITCH;
    for (int r = 0; r < DG_LCD_H; r++) {
        uint16_t *row = &s_canvas_buf[(DG_MAT_Y + r * p) * s_view_w];
        for (int dy = 0; dy < p; dy++) {
            for (int dx = 0; dx < s_view_w; dx++) row[dy * s_view_w + dx] = gap;
        }
        for (int c = 0; c < DG_LCD_W; c++) {
            uint16_t color = tone[cur[r][c] ? 2 : 0];
            for (int dy = 0; dy < p - 1; dy++) {
                uint16_t *dst = &row[dy * s_view_w + c * p];
                for (int dx = 0; dx < p - 1; dx++) dst[dx] = color;
            }
        }
    }

    memcpy(s_last_pic, cur, sizeof(s_last_pic));
    s_pic_valid = true;
    s_icon_stale = true;         /* the bands were just wiped: re-blit them */
    lv_obj_invalidate(s_canvas);
    return 1;
}

/* ---- function-icon band --------------------------------------------------
 * Eight LCD segments sit on the same glass, outside the 32x16 matrix, and the
 * ROM drives them.  Painting them from VRAM is all the UI we need: the ROM
 * decides which icon is highlighted (mode 2..8) and when the alert icon lights,
 * so no firmware-side cursor exists and nothing has to be suppressed while the
 * egg has not hatched.  Only eight bit lookups run per frame, and the canvas is
 * touched only when one of them flips.
 *
 * The sprites are slot-sized (8 columns x 4 dot-rows) with the glass colour
 * baked in, so they are blitted straight into the glass canvas - the band is
 * part of the LCD, not a row of widgets sitting on the shell. */
static uint8_t icon_bits(void)
{
    const uint8_t *v = dg_vram();
    const uint32_t len = dg_vram_len();
    uint8_t state = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t byte = ICON_SEG[i][0];
        uint8_t bit = ICON_SEG[i][1];
        if (byte < len && ((v[byte] >> bit) & 1u)) state |= (uint8_t)(1u << i);
    }
    return state;
}

static void blit_slot(const lv_image_dsc_t *img, int x, int y)
{
    const uint16_t *src = (const uint16_t *)img->data;
    for (int r = 0; r < DG_SLOT_H; r++) {
        int dy = y + r;
        if (dy < 0 || dy >= s_view_h) continue;
        uint16_t *dst = &s_canvas_buf[dy * s_view_w + x];
        const uint16_t *s = &src[r * DG_SLOT_W];
        for (int c = 0; c < DG_SLOT_W; c++) {
            int dx = x + c;
            if (dx >= 0 && dx < s_view_w) dst[c] = s[c];
        }
    }
}

static void render_icons(void)
{
    uint8_t state = icon_bits();
    if (!s_icon_stale && state == s_icon_state) return;

    const int slot_w = DG_GLASS_W / 4;          /* 8 columns = 56 px at pitch 7 */
    for (int i = 0; i < 8; i++) {
        int col = i & 3, row = i >> 2;
        int x = col * slot_w;
        int y = row ? DG_GLASS_H - DG_BAND_H : 0;
        blit_slot((state >> i) & 1u ? ICON_FILLED[i] : ICON_OUTLINE[i], x, y);
    }
    s_icon_stale = false;
    lv_obj_invalidate(s_canvas);

    /* Text-mode acceptance check for hardware runs: bit i = icon i lit,
     * plus the ROM's own UI state so the log can be read without a camera.
     * Only on a real state change - the bands are re-blitted every time the
     * matrix repaints, and logging each of those buried the console. */
    if (state != s_icon_state) {
        s_icon_state = state;
        ESP_LOGI(TAG, "icons=%02X mode=%X sub=%X",
                 state, dg_ram(0x009), dg_ram(0x00A));
    }
}

/* ---- settings menu ------------------------------------------------------
 * Hold OK (MENU_HOLD_US) in the game to open it; inside, a short C press closes
 * it again, so there is no EXIT row any more.  The power key is a hardware
 * latch that is not wired to the MCU, so it cannot be used here at all.
 *   A (UP)     move the yellow cursor
 *   B (DOWN)   change the value / run the action / open the ROM sub-list
 *   C (OK)     close the menu (BACK inside the ROM sub-list)
 * While it is open the ROM receives no key events.
 *
 * Seven rows, time first: SKIP TIME, PLAY TIME, then the settings proper.
 * SAVE OUT / SAVE IN are gone on purpose -- saving belongs to the PC tool
 * (tools/digimon_studio.html drives SAVEOUT / SAVEIN), and duplicating a 25 s
 * blocking import on a 240 px panel was never a good idea.  The console
 * commands themselves are untouched. */
#define MENU_ROWS 7
enum { M_SKIP, M_PLAY, M_ROM, M_SHELL, M_BRIGHT, M_VOL, M_AUTO };

/* SKIP TIME: consumed when the panel closes, never persisted */
static const uint8_t SKIP_MINUTES[] = { 0, 5, 30, 60, 120 };
static const char *const SKIP_TEXT[] = { "OFF", "5M/9S", "30M/38S", "1H/75S", "2H/150S" };
#define SKIP_COUNT ((int)(sizeof SKIP_MINUTES / sizeof SKIP_MINUTES[0]))
static uint8_t s_skip_idx;

/* PLAY TIME reset is two-step: the first B only arms it, so a stray press
 * while scrolling down the list cannot wipe the counter. */
static bool s_play_reset_armed;

static lv_obj_t *s_menu;
static lv_obj_t *s_menu_row[MENU_ROWS];
static lv_obj_t *s_menu_val[MENU_ROWS];
static bool s_menu_open;
static int s_menu_sel;

/* second level: every ROM slot with the play time stored next to its save */
/* second level: every ROM slot with the play time stored next to its save,
 * plus one danger row at the bottom that wipes a save after two confirmations */
#define ROM_MENU_ROWS (DG_ROM_SLOTS + 1)
#define ROM_RESET_ROW DG_ROM_SLOTS
static lv_obj_t *s_rom_menu;
static lv_obj_t *s_rom_row[ROM_MENU_ROWS];
static lv_obj_t *s_rom_lbl[ROM_MENU_ROWS];
static lv_obj_t *s_rom_time[ROM_MENU_ROWS];
static bool s_rom_menu_open;
static int s_rom_sel;              /* 0..7 = slot, DG_ROM_SLOTS = reset row */
static int s_rom_confirm;          /* 0 idle, 1 = SURE?, 2 = REALLY? */
static uint8_t s_bright = 100;      /* 20..100 % */
static uint8_t s_vol = 60;          /* 0..100 % */
static uint16_t s_auto_sec;         /* screen-off timeout, 0 = never */
static bool s_screen_off;
static int64_t s_last_key_us;

static const uint8_t BRIGHT_STEPS[] = { 20, 30, 40, 50, 60, 70, 80, 90, 100 };
static const uint8_t VOL_STEPS[]    = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
static const uint16_t AUTO_STEPS[]  = { 0, 15, 30, 60, 120, 300 };

#define MENU_YELLOW 0xFFD928

static uint8_t next_u8(const uint8_t *steps, int n, uint8_t cur)
{
    for (int i = 0; i < n; i++) if (steps[i] == cur) return steps[(i + 1) % n];
    return steps[0];
}

static uint16_t next_u16(const uint16_t *steps, int n, uint16_t cur)
{
    for (int i = 0; i < n; i++) if (steps[i] == cur) return steps[(i + 1) % n];
    return steps[0];
}

static void save_ui_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("digimon", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bright", s_bright);
        nvs_set_u8(h, "vol", s_vol);
        nvs_set_u16(h, "auto", s_auto_sec);
        nvs_set_u8(h, "shell", s_shell);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_ui_settings(void)
{
    nvs_handle_t h;
    uint8_t v8;
    uint16_t v16;
    if (nvs_open("digimon", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u8(h, "bright", &v8) == ESP_OK && v8 >= 20 && v8 <= 100) s_bright = v8;
    if (nvs_get_u8(h, "vol", &v8) == ESP_OK && v8 <= 100) s_vol = v8;
    if (nvs_get_u16(h, "auto", &v16) == ESP_OK) s_auto_sec = v16;
    if (nvs_get_u8(h, "shell", &v8) == ESP_OK && v8 < SHELL_COUNT) s_shell = v8;
    nvs_close(h);
}

static void screen_set(bool off)
{
    s_screen_off = off;
    bsp_display_backlight(off ? 0 : s_bright);
    s_last_key_us = esp_timer_get_time();
    if (off) s_wake_mask = 0;
    ESP_LOGI(TAG, "screen %s", off ? "off" : "on");
}

/* ---- save slot export / import over the USB console ---------------------
 * Export prints the slot as hex between SAVEBEGIN/SAVEEND lines; import reads
 * the same thing back.  Both go through the same USB cable the logs use, so a
 * backup is a copy-paste on the PC side. */
static char s_hexbuf[80];

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Protocol replies go out through here instead of bare con_printf().  On hardware
 * that is just stdout (USB-Serial-JTAG).  In the simulator build it is
 * esp_rom_printf: the wasm emulator forwards UART0 TX only for polling writes,
 * not for the UART driver's interrupt path, so con_printf() would vanish there and
 * the verification script would have nothing to assert on. */
static void con_printf(const char *fmt, ...)
{
    /* 192 was enough for DUMP but silently truncated the longer STATE line
     * (~220 chars) in the middle of age=... -- keep headroom. */
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef DG_SIM_UART0_INPUT
    esp_rom_printf("%s", buf);
#else
    fputs(buf, stdout);
    fflush(stdout);
#endif
}

static void save_export(int idx)
{
    if (idx < 0 || idx >= DG_ROM_SLOTS || !s_roms[idx].valid) return;
    nvs_handle_t h;
    size_t len = sizeof(s_save_buf);
    if (nvs_open("digimon", NVS_READONLY, &h) != ESP_OK) return;
    char key[16];
    slot_key(idx, key, sizeof key);
    esp_err_t e = nvs_get_blob(h, key, s_save_buf, &len);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "export: no slot for %s", rom_name(idx));
        con_printf("SAVEEMPTY %s\n", rom_name(idx));
        return;
    }
    con_printf("SAVEBEGIN %s %u\n", rom_name(idx), (unsigned)len);
    for (size_t i = 0; i < len; i += 32) {
        size_t n = len - i;
        if (n > 32) n = 32;
        for (size_t k = 0; k < n; k++) {
            static const char hx[] = "0123456789abcdef";
            s_hexbuf[k * 2] = hx[(s_save_buf[i + k] >> 4) & 0xF];
            s_hexbuf[k * 2 + 1] = hx[s_save_buf[i + k] & 0xF];
        }
        s_hexbuf[n * 2] = 0;
        con_printf("%s\n", s_hexbuf);
    }
    con_printf("SAVEEND\n");
    fflush(stdout);
}

/* returns 1 on success; waits up to wait_ms for the whole blob */
static int save_import(int idx, int wait_ms)
{
    static char line[128];
    size_t got = 0;
    bool started = false;
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;

    if (idx < 0 || idx >= DG_ROM_SLOTS || !s_roms[idx].valid) {
        ESP_LOGW(TAG, "import: no ROM loaded, nothing to restore into");
        con_printf("IMPORTBAD\n");
        fflush(stdout);
        return 0;
    }

    ESP_LOGI(TAG, "import: send SAVEBEGIN/hash/SAVEEND for %s", rom_name(idx));
    con_printf("IMPORTREADY %s\n", rom_name(idx));
    fflush(stdout);

    while (esp_timer_get_time() < deadline) {
        int c = con_getc();
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\r') continue;
        if (c == '\n') {
            if (line[0] == 0) continue;
            if (strncmp(line, "SAVEBEGIN", 9) == 0) {
                started = true;
                got = 0;
            } else if (strncmp(line, "SAVEEND", 7) == 0) {
                if (!started || got < sizeof(save_hdr_t)) break;
                const save_hdr_t *hdr = (const save_hdr_t *)s_save_buf;
                const uint8_t *st = s_save_buf + sizeof(save_hdr_t);
                bool ok = hdr->magic == SAVE_MAGIC &&
                          hdr->rom_crc == s_rom_crc[idx] &&
                          hdr->state_size == dg_state_size() &&
                          got == sizeof(save_hdr_t) + hdr->state_size &&
                          hdr->state_crc == crc32_calc(st, hdr->state_size);
                if (!ok) {
                    ESP_LOGW(TAG, "import: blob rejected (%u bytes)", (unsigned)got);
                    con_printf("IMPORTBAD\n");
                    fflush(stdout);
                    return 0;
                }
                nvs_handle_t h;
                if (nvs_open("digimon", NVS_READWRITE, &h) != ESP_OK) return 0;
                char key[16];
                slot_key(idx, key, sizeof key);
                esp_err_t e = nvs_set_blob(h, key, s_save_buf, got);
                if (e == ESP_OK) e = nvs_commit(h);
                nvs_close(h);
                if (e != ESP_OK) {
                    con_printf("IMPORTFAIL\n");
                    fflush(stdout);
                    return 0;
                }
                rom_load(idx);
                s_pic_valid = false;
                s_icon_state = 0xFF;
                s_icon_stale = true;
                load_slot(idx);
                ESP_LOGI(TAG, "import: %s restored from console", rom_name(idx));
                con_printf("IMPORTOK %s\n", rom_name(idx));
                fflush(stdout);
                return 1;
            } else if (started) {
                /* A log line from another task can land in the middle of the
                 * blob (the 60 s autosave is the usual offender).  Skip lines
                 * that are not pure even-length hex instead of truncating the
                 * blob, so one stray line does not poison the whole transfer;
                 * the CRC check at SAVEEND still guards the result. */
                size_t llen = strlen(line);
                bool hexline = (llen > 0) && (llen % 2) == 0;
                for (size_t k = 0; hexline && k < llen; k++) {
                    if (hex_nibble(line[k]) < 0) hexline = false;
                }
                if (hexline) {
                    for (size_t k = 0; k < llen; k += 2) {
                        int hi = hex_nibble(line[k]), lo = hex_nibble(line[k + 1]);
                        if (got < sizeof(s_save_buf)) {
                            s_save_buf[got++] = (uint8_t)((hi << 4) | lo);
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "import: skipped non-hex line (%u chars)",
                             (unsigned)llen);
                }
            }
            line[0] = 0;
            continue;
        }
        size_t l = strlen(line);
        if (l + 1 < sizeof line) { line[l] = (char)c; line[l + 1] = 0; }
    }
    con_printf("IMPORTTIMEOUT\n");
    fflush(stdout);
    return 0;
}

/* ---- console transport ---------------------------------------------------
 * On hardware the console is the USB-Serial-JTAG peripheral, which is what
 * fgetc(stdin) reaches.  Simulator builds rebind stdio to the UART0 driver in
 * digimon_app_start() instead: the wasm emulator has no USB-Serial-JTAG model,
 * and uart_input() lands on UART0 RX.  That macro is only ever defined for
 * verification builds -- on the real board UART0 TX is the backlight pin. */
static int con_getc(void)
{
    return fgetc(stdin);
}

/* ---- ROM images over the USB console ------------------------------------
 * These are the commands the PC tool (tools/digimon_studio.html) speaks:
 *
 *   ROMINFO                          ask what is in the partition
 *   ROMIN <slot> <len> [crc] [name]  -> ROMREADY, hex lines, ROMEND
 *                                    -> ROMOK <slot> <len> <crc> | ROMBAD <why>
 *   ROMOUT <slot>                    -> ROMBEGIN, hex lines, ROMEND
 *   ROMDEL <slot>                    -> ROMDELETED <slot>
 *   ROMBOOT <slot>                   -> run that slot now
 *
 * The data is streamed into flash in 512 B chunks and the slot header is only
 * published after the CRC matches, so unplugging the cable mid-transfer leaves
 * the previous image bootable instead of bricking the slot. */
#define ROM_CHUNK_BYTES 512
static uint8_t s_rom_chunk[ROM_CHUNK_BYTES];

static void rom_info_print(void)
{
    con_printf("ROMSLOTS %d\n", DG_ROM_SLOTS);
    for (int i = 0; i < DG_ROM_SLOTS; i++) {
        dg_rom_entry_t e;
        if (dg_rom_store_get(i, &e)) {
            con_printf("ROMSLOT %d %s %u %08x %s\n", i, e.name, (unsigned)e.len,
                   (unsigned)e.crc, i == s_rom_idx ? "ACTIVE" : "-");
        } else {
            con_printf("ROMSLOT %d - 0 00000000 %s\n", i, i == s_rom_idx ? "ACTIVE" : "-");
        }
    }
    con_printf("ROMCUR %d %s\n", s_rom_idx, rom_name(s_rom_idx));
    fflush(stdout);
}

static void rom_export(int idx)
{
    dg_rom_entry_t e;
    if (!dg_rom_store_get(idx, &e)) {
        con_printf("ROMEMPTY %d\n", idx);
        fflush(stdout);
        return;
    }
    con_printf("ROMBEGIN %d %u %08x %s\n", idx, (unsigned)e.len, (unsigned)e.crc, e.name);
    uint8_t row[32];
    for (uint32_t off = 0; off < e.len; off += sizeof row) {
        uint32_t n = e.len - off;
        if (n > sizeof row) n = sizeof row;
        if (dg_rom_store_read(idx, off, row, n) != ESP_OK) break;
        static const char hx[] = "0123456789abcdef";
        for (uint32_t k = 0; k < n; k++) {
            s_hexbuf[k * 2] = hx[(row[k] >> 4) & 0xF];
            s_hexbuf[k * 2 + 1] = hx[row[k] & 0xF];
        }
        s_hexbuf[n * 2] = 0;
        con_printf("%s\n", s_hexbuf);
    }
    con_printf("ROMEND\n");
    fflush(stdout);
}

static void rom_activate(int idx)
{
    bool ok = (idx >= 0 && idx < DG_ROM_SLOTS && s_roms[idx].valid);
    if (ok) {
        /* park the pet of the slot we are leaving, unless we are overwriting it */
        if (s_have_rom && idx != s_rom_idx) save_slot(s_rom_idx);
        s_rom_idx = idx;
        s_have_rom = rom_load(idx);
        if (s_have_rom) {
            save_rom_index(idx);
            (void)load_slot(idx);          /* rejected when the image changed */
        } else {
            s_rom_idx = -1;
        }
    } else {
        s_have_rom = false;
        s_rom_idx = -1;
    }
    s_pic_valid = false;
    s_icon_state = 0xFF;
    s_icon_stale = true;
    ui_rom_state();
}

static void rom_import_cmd(char *args)
{
    int idx = -1;
    unsigned long len = 0;
    unsigned long crc = 0;
    char name[DG_ROM_NAME_LEN];
    memset(name, 0, sizeof name);

    int nf = sscanf(args, "%d %lu %lx %11s", &idx, &len, &crc, name);
    if (nf < 2 || idx < 0 || idx >= DG_ROM_SLOTS || len == 0 || len > DG_ROM_MAX_LEN) {
        con_printf("ROMBAD args\n");
        fflush(stdout);
        return;
    }
    esp_err_t e = dg_rom_store_begin_write(idx, (uint32_t)len);
    if (e != ESP_OK) {
        con_printf("ROMBAD begin %s\n", esp_err_to_name(e));
        fflush(stdout);
        return;
    }
    con_printf("ROMREADY %d %lu\n", idx, len);
    fflush(stdout);

    static char line[96];
    size_t ln = 0;
    uint32_t got = 0;
    size_t clen = 0;
    int64_t deadline = esp_timer_get_time() + 60000 * 1000;

    for (;;) {
        if (esp_timer_get_time() >= deadline) {
            dg_rom_store_abort_write();
            con_printf("ROMBAD timeout %u\n", (unsigned)got);
            fflush(stdout);
            return;
        }
        int c = con_getc();
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (c == '\r') continue;
        if (c != '\n') {
            if (ln + 1 < sizeof line) line[ln++] = (char)c;
            continue;
        }
        line[ln] = 0;
        ln = 0;
        if (line[0] == 0) continue;
        if (strncmp(line, "ROMEND", 6) == 0) break;

        size_t ll = strlen(line);
        bool hexline = (ll > 0) && (ll % 2) == 0;
        for (size_t k = 0; hexline && k < ll; k++) {
            if (hex_nibble(line[k]) < 0) hexline = false;
        }
        if (!hexline) continue;             /* a log line slipped in: ignore it */

        if (got + ll / 2 > len) {
            dg_rom_store_abort_write();
            con_printf("ROMBAD long %u\n", (unsigned)(got + ll / 2));
            fflush(stdout);
            return;
        }
        for (size_t k = 0; k < ll; k += 2) {
            s_rom_chunk[clen++] = (uint8_t)((hex_nibble(line[k]) << 4) | hex_nibble(line[k + 1]));
            got++;
            if (clen == sizeof s_rom_chunk) {
                if (dg_rom_store_write_chunk(s_rom_chunk, (uint32_t)clen) != ESP_OK) {
                    dg_rom_store_abort_write();
                    con_printf("ROMBAD write %u\n", (unsigned)got);
                    fflush(stdout);
                    return;
                }
                clen = 0;
            }
        }
    }
    if (clen && dg_rom_store_write_chunk(s_rom_chunk, (uint32_t)clen) != ESP_OK) {
        dg_rom_store_abort_write();
        con_printf("ROMBAD write %u\n", (unsigned)got);
        fflush(stdout);
        return;
    }
    if (got != len) {
        dg_rom_store_abort_write();
        con_printf("ROMBAD size %u/%lu\n", (unsigned)got, len);
        fflush(stdout);
        return;
    }

    uint32_t real = 0;
    e = dg_rom_store_commit_write(name, (uint32_t)crc, &real);
    if (e != ESP_OK) {
        con_printf("ROMBAD %s\n", esp_err_to_name(e));
        fflush(stdout);
        return;
    }
    rom_table_load();
    rom_crc_init();
    rom_activate(idx);
    ESP_LOGI(TAG, "ROMIN slot %d: %s, %lu B, crc %08x", idx, rom_name(idx), len, (unsigned)real);
    con_printf("ROMOK %d %lu %08x\n", idx, len, (unsigned)real);
    fflush(stdout);
}

static void rom_delete_cmd(int idx)
{
    if (idx < 0 || idx >= DG_ROM_SLOTS) {
        con_printf("ROMBAD args\n");
        fflush(stdout);
        return;
    }
    esp_err_t e = dg_rom_store_erase(idx);
    if (e != ESP_OK) {
        con_printf("ROMBAD %s\n", esp_err_to_name(e));
        fflush(stdout);
        return;
    }
    rom_table_load();
    rom_crc_init();
    if (idx == s_rom_idx) rom_activate(dg_rom_store_first());
    ESP_LOGI(TAG, "slot %d erased", idx);
    con_printf("ROMDELETED %d\n", idx);
    fflush(stdout);
}

static void rom_boot_cmd(int idx)
{
    if (idx < 0 || idx >= DG_ROM_SLOTS || !s_roms[idx].valid) {
        con_printf("ROMBOOT NONE\n");
        fflush(stdout);
        return;
    }
    if (idx != s_rom_idx) switch_rom(idx);
    con_printf("ROMBOOT %d %s\n", idx, rom_name(idx));
    fflush(stdout);
}

/* ---- console commands ---------------------------------------------------
 * SAVEOUT / SAVEIN / PET typed into the USB console do the same thing as the
 * SAVE OUT / SAVE IN menu rows.  Handy for scripted backups, and it is how the
 * save round trip gets tested without touching the buttons. */
static void console_poll(void)
{
    static char line[64];
    static size_t n;
    for (int i = 0; i < 32; i++) {
        int c = con_getc();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            if (strncmp(line, "ROMINFO", 7) == 0) {
                rom_info_print();
            } else if (strncmp(line, "ROMIN ", 6) == 0) {
                rom_import_cmd(line + 6);
            } else if (strncmp(line, "ROMOUT ", 7) == 0) {
                rom_export(atoi(line + 7));
            } else if (strncmp(line, "ROMDEL ", 7) == 0) {
                rom_delete_cmd(atoi(line + 7));
            } else if (strncmp(line, "ROMBOOT ", 8) == 0) {
                rom_boot_cmd(atoi(line + 8));
            } else if (strncmp(line, "SAVEOUT", 7) == 0) {
                save_slot(s_rom_idx);
                save_export(s_rom_idx);
            } else if (strncmp(line, "SAVEIN", 6) == 0) {
                save_import(s_rom_idx, 30000);
            } else if (strncmp(line, "SKIP ", 5) == 0) {
                /* same jump as the SKIP TIME menu row; handy for scripted checks
                 * now that a skip really executes the game time */
                rom_skip_minutes(atoi(line + 5), NULL);
            } else if (strncmp(line, "STATE", 5) == 0) {
                /* One line with every evolution input, for the PC tool's care
                 * page.  Kept separate from DUMP so old scripts keep working.
                 *
                 * weight vs basew (round 65): M(68-69) is the *species base*
                 * weight that 0C17 writes at evolution (Betamon = 20 g), NOT
                 * what the toy shows.  The number on the status page is the
                 * current weight, two BCD digits at M(56) units / M(57) tens
                 * -- proved on the unit by feeding once (+1 g) and by a
                 * nibble-by-nibble screen-causality sweep (probe_causal.c).
                 * Keeping both: basew is the useful "this species weighs 20"
                 * anchor, weight is what the user sees. */
                int sn = stage_minutes_next();
                bool pend = stage_next_pending();
                char nxt2[16];
                if (pend) snprintf(nxt2, sizeof nxt2, "%d:%02d", sn / 60, sn % 60);
                else      snprintf(nxt2, sizeof nxt2, "none");
                con_printf("STATE slot=%X mode=%X sub=%X care=%d train=%d over=%d sleep=%d "
                           "hunger=%d strength=%d weight=%d basew=%d protein=%d doses=%d poop=%d "
                           "injured=%d injc=%d injh=%d win=%d bouts=%d energy=%d "
                           "stage=%d:%02d next=%s left=%d age=%lu\n",
                           dg_ram(0x00B), dg_ram(0x009), dg_ram(0x00A),
                           dg_ram(0x031), ram_u8(0x034, 0x035), dg_ram(0x032), dg_ram(0x033),
                           dg_ram(0x03E), dg_ram(0x03D),
                           ram_bcd(0x056, 0x057), ram_bcd(0x068, 0x069),
                           dg_ram(0x03C), dg_ram(0x062), dg_ram(0x007),
                           dg_ram(0x005) & 1, ram_bcd(0x036, 0x037), ram_bcd(0x03A, 0x03B),
                           dg_ram(0x098), dg_ram(0x099), ram_bcd(0x08C, 0x08D),
                           stage_minutes_now() / 60, stage_minutes_now() % 60, nxt2,
                           pend ? sn - stage_minutes_now() : -1,
                           (unsigned long)s_pet_sec);
                fflush(stdout);
            } else if (strncmp(line, "RAM", 3) == 0) {
                /* page-0 nibble dump: 0x00-0xFF, 16 per line, one hex digit each */
                for (int base = 0; base < 0x100; base += 16) {
                    char row[20];
                    for (int j = 0; j < 16; j++)
                        row[j] = "0123456789ABCDEF"[dg_ram(base + j) & 0xF];
                    row[16] = 0;
                    con_printf("RAM %02X %s\n", base, row);
                }
                con_printf("RAMEND\n");
                fflush(stdout);
            } else if (strncmp(line, "AGES", 4) == 0) {
                /* play time of every slot, for the PC tool's ROM page */
                con_printf("AGES");
                for (int i = 0; i < DG_ROM_SLOTS; i++)
                    con_printf(" %d=%lu", i, (unsigned long)age_peek(i));
                con_printf("\n");
                fflush(stdout);
            } else if (strncmp(line, "SAVEDEL ", 8) == 0) {
                int idx = atoi(line + 8);
                if (idx < 0 || idx >= DG_ROM_SLOTS) {
                    con_printf("SAVEDELBAD\n");
                } else {
                    reset_save(idx);
                    con_printf("SAVEDEL ok %d\n", idx);
                }
                fflush(stdout);
            } else if (strncmp(line, "DUMP", 4) == 0) {
                /* evolution inputs, straight out of page 0 (see appendix-ram).
                 * stage/next are printed as h:mm decoded the ROM's way (BCD
                 * minutes, binary hours) -- the old version showed 26:23 as
                 * 20:23 and dropped the top half of the next-evolution hour,
                 * which is what made a perfectly healthy pet look overdue. */
                char nxt[16];
                if (stage_next_pending())
                    snprintf(nxt, sizeof nxt, "%d:%02d",
                             stage_minutes_next() / 60, stage_minutes_next() % 60);
                else
                    snprintf(nxt, sizeof nxt, "none");
                con_printf("DUMP slot=%X mode=%X sub=%X care=%X train=%X%X over=%X sleep=%X win=%X bouts=%X hurt=%X stage=%d:%02d next=%s left=%dmin age=%lu\n",
                           dg_ram(0x00B), dg_ram(0x009), dg_ram(0x00A),
                           dg_ram(0x031), dg_ram(0x035), dg_ram(0x034),
                           dg_ram(0x032), dg_ram(0x033), dg_ram(0x098), dg_ram(0x099),
                           dg_ram(0x005),
                           stage_minutes_now() / 60, stage_minutes_now() % 60, nxt,
                           stage_next_pending() ? stage_minutes_next() - stage_minutes_now() : -1,
                           (unsigned long)s_pet_sec);
                con_printf("RAW 46..4F = %X %X %X %X %X %X %X %X %X %X   76=%X 7F=%X\n",
                           dg_ram(0x46), dg_ram(0x47), dg_ram(0x48), dg_ram(0x49),
                           dg_ram(0x4A), dg_ram(0x4B), dg_ram(0x4C), dg_ram(0x4D),
                           dg_ram(0x4E), dg_ram(0x4F), dg_ram(0x76), dg_ram(0x7F));
            } else if (strncmp(line, "PET", 3) == 0) {
                ESP_LOGI(TAG, "pet mode=%X sub=%X slot=%X seq=%u",
                         dg_ram(0x009), dg_ram(0x00A), dg_ram(0x00B), (unsigned)s_save_seq);
            }
            n = 0;
            line[0] = 0;
            continue;
        }
        if (n + 1 < sizeof line) { line[n++] = (char)c; line[n] = 0; }
    }
}

/* caller must hold the LVGL lock */
static void menu_refresh(void)
{
    if (!s_menu) return;
    char buf[20];

    lv_label_set_text(s_menu_val[M_SKIP], SKIP_TEXT[s_skip_idx]);

    if (s_play_reset_armed) {
        lv_label_set_text(s_menu_val[M_PLAY], "RESET? B");   /* second B wipes */
    } else {
        fmt_play_time(buf, sizeof buf, s_pet_sec);
        lv_label_set_text(s_menu_val[M_PLAY], buf);
    }

    /* the trailing '>' is the hint that B opens a sub-list */
    snprintf(buf, sizeof buf, "%s >", rom_name(s_rom_idx));
    lv_label_set_text(s_menu_val[M_ROM], buf);

    lv_label_set_text(s_menu_val[M_SHELL], SHELLS[s_shell].name);
    snprintf(buf, sizeof buf, "%u%%", (unsigned)s_bright);
    lv_label_set_text(s_menu_val[M_BRIGHT], buf);
    snprintf(buf, sizeof buf, "%u%%", (unsigned)s_vol);
    lv_label_set_text(s_menu_val[M_VOL], buf);
    if (s_auto_sec == 0) {
        lv_label_set_text(s_menu_val[M_AUTO], "NEVER");
    } else if (s_auto_sec < 60) {
        snprintf(buf, sizeof buf, "%uS", (unsigned)s_auto_sec);
        lv_label_set_text(s_menu_val[M_AUTO], buf);
    } else {
        snprintf(buf, sizeof buf, "%uM", (unsigned)(s_auto_sec / 60));
        lv_label_set_text(s_menu_val[M_AUTO], buf);
    }

    for (int i = 0; i < MENU_ROWS; i++) {
        /* yellow bar + dark text for the selected row: at 240 px wide a subtle
         * shade was impossible to read */
        bool sel = (i == s_menu_sel);
        lv_obj_set_style_bg_color(s_menu_row[i], lv_color_hex(MENU_YELLOW), 0);
        lv_obj_set_style_bg_opa(s_menu_row[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_menu_row[i], lv_color_hex(0x17202A), 0);
    }
}

/* Wipes one slot's save blob and its play-time key.  If that slot is the one
 * currently running, the core is reset too, so the toy drops back to a fresh
 * egg: the ROM clears its own page-0 RAM during init (0x0113), which is the
 * same path a cold boot takes.  Nothing here touches the ROM image itself --
 * the slot stays available, only the save is gone. */
static void reset_save(int idx)
{
    if (idx < 0 || idx >= DG_ROM_SLOTS) return;

    nvs_handle_t h;
    if (nvs_open("digimon", NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        slot_key(idx, key, sizeof key);
        nvs_erase_key(h, key);
        age_nvs_key(idx, key, sizeof key);
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "save reset: slot %d (%s)", idx, rom_name(idx));

    if (idx == s_rom_idx) {
        dg_reset();
        age_reset();
        s_last_species_slot = 0;
        s_pic_valid = false;
        s_icon_state = 0xFF;
        s_icon_stale = true;
        ESP_LOGI(TAG, "live core reset, back to the egg");
    }
}

/* Second level: one row per ROM slot.  Left column = index + name (the active
 * slot gets a '*'), right column = the play time stored beside that slot's
 * save.  Empty slots are dimmed and inert.  The settings list underneath is
 * hidden while this is up, otherwise its text shows through the 90 % panel. */
static void menu_rom_refresh(void)
{
    if (!s_rom_menu) return;
    char left[28], right[12];

    for (int i = 0; i < DG_ROM_SLOTS; i++) {
        bool valid = s_roms[i].valid;
        if (valid) {
            snprintf(left, sizeof left, "%d %s%s", i, (i == s_rom_idx) ? "*" : " ", rom_name(i));
            fmt_play_time(right, sizeof right, age_peek(i));
        } else {
            snprintf(left, sizeof left, "%d  --", i);
            snprintf(right, sizeof right, "--");
        }
        lv_label_set_text(s_rom_lbl[i], left);
        lv_label_set_text(s_rom_time[i], right);
        lv_obj_set_style_text_color(s_rom_row[i], lv_color_hex(0x17202A), 0);
        lv_obj_set_style_text_opa(s_rom_row[i], valid ? LV_OPA_COVER : LV_OPA_40, 0);
    }

    /* Danger row.  It always targets the ACTIVE slot (s_rom_idx), never the
     * row the cursor happens to pass on its way down: with only A to move, a
     * cursor-following target would be whatever slot sits just above this row.
     * "Reset the current save" therefore means exactly what it says -- to wipe
     * another slot, boot it first, then reset. */
    int tgt = s_rom_idx;
    bool can = (tgt >= 0 && tgt < DG_ROM_SLOTS && s_roms[tgt].valid);
    if (!can)
        snprintf(left, sizeof left, "RESET SAVE");
    else if (s_rom_confirm == 0)
        snprintf(left, sizeof left, "RESET SAVE %s", rom_name(tgt));
    else if (s_rom_confirm == 1)
        snprintf(left, sizeof left, "SURE?  %s", rom_name(tgt));
    else
        snprintf(left, sizeof left, "REALLY? %s", rom_name(tgt));
    lv_label_set_text(s_rom_lbl[ROM_RESET_ROW], left);
    lv_label_set_text(s_rom_time[ROM_RESET_ROW], can ? "B x3" : "--");
    lv_obj_set_style_text_opa(s_rom_row[ROM_RESET_ROW], can ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_text_color(s_rom_row[ROM_RESET_ROW],
                                lv_color_hex(s_rom_confirm ? 0x8A1F14 : 0xB23A2E), 0);

    for (int i = 0; i < ROM_MENU_ROWS; i++) {
        bool sel = (i == s_rom_sel);
        lv_obj_set_style_bg_color(s_rom_row[i], lv_color_hex(MENU_YELLOW), 0);
        lv_obj_set_style_bg_opa(s_rom_row[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (sel)                                /* dark text on the yellow bar */
            lv_obj_set_style_text_color(s_rom_row[i], lv_color_hex(0x17202A), 0);
    }
}

static void menu_rom_build(lv_obj_t *parent)
{
    s_rom_menu = lv_obj_create(parent);
    lv_obj_set_size(s_rom_menu, 300, 200);
    lv_obj_align(s_rom_menu, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_rom_menu, lv_color_hex(0xFBF9F0), 0);
    lv_obj_set_style_bg_opa(s_rom_menu, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_rom_menu, lv_color_hex(0x2C3324), 0);
    lv_obj_set_style_border_width(s_rom_menu, 2, 0);
    lv_obj_set_style_radius(s_rom_menu, 8, 0);
    lv_obj_set_style_pad_all(s_rom_menu, 3, 0);
    lv_obj_set_style_shadow_width(s_rom_menu, 12, 0);
    lv_obj_set_style_shadow_opa(s_rom_menu, LV_OPA_40, 0);
    lv_obj_remove_flag(s_rom_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_rom_menu, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_rom_menu);
    lv_label_set_text(title, "ROM SLOTS");
    lv_obj_set_style_text_color(title, lv_color_hex(0x252C1F), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, -1);

    for (int i = 0; i < ROM_MENU_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_rom_menu);
        /* 18 px rows at 19 px pitch: eight slots plus the danger row fit the
         * 200 px panel with 10 px to spare at the bottom. */
        lv_obj_set_size(row, 278, 18);
        lv_obj_set_pos(row, 8, 20 + i * 19);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_set_style_pad_all(row, 1, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_rom_row[i] = row;

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x252C1F), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 3, 0);
        s_rom_lbl[i] = lbl;

        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, "");
        lv_obj_set_style_text_color(t, lv_color_hex(0x252C1F), 0);
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, -3, 0);
        s_rom_time[i] = t;
    }
}

static void menu_build(lv_obj_t *parent)
{
    s_menu = lv_obj_create(parent);
    lv_obj_set_size(s_menu, 300, 200);
    lv_obj_align(s_menu, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(0xFBF9F0), 0);
    /* Semi-transparent: LVGL blends this against the glass canvas below, so the
     * pet stays faintly visible while the panel is open.  90 % keeps the dark
     * text readable over the green glass; go lower and the labels wash out. */
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_menu, lv_color_hex(0x2C3324), 0);
    lv_obj_set_style_border_width(s_menu, 2, 0);
    lv_obj_set_style_radius(s_menu, 8, 0);
    lv_obj_set_style_pad_all(s_menu, 3, 0);
    lv_obj_set_style_shadow_width(s_menu, 12, 0);
    lv_obj_set_style_shadow_opa(s_menu, LV_OPA_40, 0);
    lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_menu);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, lv_color_hex(0x252C1F), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, -1);

    static const char *names[MENU_ROWS] = {
        "SKIP TIME", "PLAY TIME", "ROM", "SHELL", "BRIGHT", "VOLUME", "AUTO OFF"
    };
    for (int i = 0; i < MENU_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_menu);
        /* 20 px rows at 22 px pitch: seven rows now, so each one gets a little
         * more air than the cramped 18/19 the nine-row panel needed. */
        lv_obj_set_size(row, 278, 20);
        lv_obj_set_pos(row, 8, 22 + i * 22);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_menu_row[i] = row;

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, names[i]);
        lv_obj_set_style_text_color(name, lv_color_hex(0x252C1F), 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 2, 0);

        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, "-");
        lv_obj_set_style_text_color(val, lv_color_hex(0x252C1F), 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -2, 0);
        s_menu_val[i] = val;
    }

    /* second level lives on top of the list; it stays hidden until B lands on
     * the ROM row */
    menu_rom_build(parent);
}

static void menu_set_open(bool open)
{
    s_menu_open = open;
    if (!open) {
        s_play_reset_armed = false;     /* an armed wipe never survives closing */
        s_rom_menu_open = false;
        s_rom_confirm = 0;
    }
    if (bsp_lvgl_lock(100)) {
        if (open) {
            menu_refresh();
            lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
            if (s_rom_menu) lv_obj_add_flag(s_rom_menu, LV_OBJ_FLAG_HIDDEN);
        }
        bsp_lvgl_unlock();
    }
    s_pic_valid = false;                 /* repaint the pet when it closes */
    ESP_LOGI(TAG, "menu %s", open ? "open" : "closed");

    /* SKIP TIME is a one-shot action: the amount is picked with B, and the jump
     * runs the moment the panel closes.  Doing it here (not in menu_key) means
     * the amount is never applied twice and an abandoned menu changes nothing. */
    if (!open && s_skip_idx > 0) {
        int idx = s_skip_idx;
        int minutes = SKIP_MINUTES[idx];
        char label[16];
        snprintf(label, sizeof label, "SKIP %s", SKIP_TEXT[idx]);
        s_skip_idx = 0;
        ESP_LOGI(TAG, "skip %d min on menu close", minutes);
        /* rom_skip_minutes() now owns the card: it shows before the busy loop
         * and hides after, so the menu path and the console path look identical */
        rom_skip_minutes(minutes, label);
    }

    /* Persist once per menu session instead of on every keypress: cycling the
     * colourway used to commit NVS a dozen times in a row, which stalls the
     * game loop for ~100 ms each time and wears the flash for nothing.
     * Deferred until after the skip: the commit stalls flash for ~100 ms, and
     * doing it between hiding the panel and showing the SKIP card made the
     * panel look like it closed in slow motion. */
    if (!open) save_ui_settings();
}

static void switch_rom(int idx);   /* defined below */
static void apply_shell(void);     /* defined below */

/* Keys inside the menu:
 *   A (UP,   mask 4)  next row
 *   B (DOWN, mask 2)  change the value / run the action / open the ROM list
 *   C (OK,   mask 1)  close the menu; inside the ROM list it is BACK
 *
 * Every LVGL call here has to be under bsp_lvgl_lock(): apply_shell() used to
 * be called straight from the key path, racing the esp_lvgl_port task.  That
 * corrupted the heap after a handful of SHELL changes and froze the toy --
 * which is why it only died after cycling the colourway for a while. */
static void menu_key(uint8_t mask)
{
    /* ---- second level: the ROM slot list -------------------------------
     * A walks all eight slots, B boots the highlighted one, C goes back to the
     * settings list instead of closing everything.  Empty slots are listed
     * (the owner asked for the full picture) but are inert. */
    if (s_rom_menu_open) {
        /* C: first press only cancels a pending wipe; a second one leaves */
        if (mask == 1) {
            if (s_rom_confirm > 0) {
                s_rom_confirm = 0;
                if (bsp_lvgl_lock(100)) { menu_rom_refresh(); bsp_lvgl_unlock(); }
                return;
            }
            s_rom_menu_open = false;
            if (bsp_lvgl_lock(100)) {
                lv_obj_add_flag(s_rom_menu, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_HIDDEN);   /* list back */
                menu_refresh();
                bsp_lvgl_unlock();
            }
            return;
        }

        if (mask == 4) {                      /* A: move, cancels a pending wipe */
            s_rom_sel = (s_rom_sel + 1) % ROM_MENU_ROWS;
            s_rom_confirm = 0;
            if (bsp_lvgl_lock(100)) { menu_rom_refresh(); bsp_lvgl_unlock(); }
            return;
        }

        if (mask == 2) {
            /* danger row: B once -> SURE?, twice -> REALLY?, third press wipes */
            if (s_rom_sel == ROM_RESET_ROW) {
                int tgt = s_rom_idx;
                bool can = (tgt >= 0 && tgt < DG_ROM_SLOTS && s_roms[tgt].valid);
                if (can) {
                    if (s_rom_confirm < 2) {
                        s_rom_confirm++;
                    } else {
                        s_rom_confirm = 0;
                        reset_save(tgt);
                    }
                }
                if (bsp_lvgl_lock(100)) { menu_rom_refresh(); bsp_lvgl_unlock(); }
                return;
            }
            /* a slot row: boot it */
            if (s_rom_sel >= 0 && s_roms[s_rom_sel].valid && s_rom_sel != s_rom_idx) {
                switch_rom(s_rom_sel);
                s_rom_menu_open = false;
                s_rom_confirm = 0;
                if (bsp_lvgl_lock(100)) {
                    lv_obj_add_flag(s_rom_menu, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
                    menu_refresh();
                    bsp_lvgl_unlock();
                }
            }
        }
        return;
    }

    if (mask == 1) {                      /* C: close */
        menu_set_open(false);
        return;
    }

    bool shell_changed = false;
    if (mask == 4) {                      /* A: next row */
        s_menu_sel = (s_menu_sel + 1) % MENU_ROWS;
        s_play_reset_armed = false;       /* walking away cancels a pending wipe */
    } else {                              /* B: change the value / act */
        switch (s_menu_sel) {
        case M_SKIP:
            /* only picks the amount; the jump happens when the panel closes */
            s_skip_idx = (uint8_t)((s_skip_idx + 1) % SKIP_COUNT);
            break;
        case M_PLAY:
            if (!s_play_reset_armed) {
                s_play_reset_armed = true;      /* first B only arms the wipe */
            } else {
                s_pet_sec = 0;
                s_age_frac = 0;
                s_age_last_tick = dg_osc1_ticks();
                age_save(s_rom_idx);
                s_play_reset_armed = false;
                ESP_LOGI(TAG, "play time reset to 0");
            }
            break;
        case M_ROM:
            s_rom_sel = (s_rom_idx >= 0) ? s_rom_idx : 0;
            s_rom_confirm = 0;
            s_rom_menu_open = true;
            if (bsp_lvgl_lock(100)) {
                menu_rom_refresh();
                /* hide the settings list, otherwise it reads through the
                 * semi-transparent slot panel and the whole thing looks dirty */
                lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_rom_menu, LV_OBJ_FLAG_HIDDEN);
                bsp_lvgl_unlock();
            }
            return;
        case M_SHELL:
            s_shell = (uint8_t)((s_shell + 1) % SHELL_COUNT);
            shell_changed = true;
            break;
        case M_BRIGHT:
            s_bright = next_u8(BRIGHT_STEPS, (int)sizeof BRIGHT_STEPS, s_bright);
            bsp_display_backlight(s_bright);
            break;
        case M_VOL:
            s_vol = next_u8(VOL_STEPS, (int)sizeof VOL_STEPS, s_vol);
            bsp_audio_set_volume(s_vol);
            break;
        case M_AUTO:
            s_auto_sec = next_u16(AUTO_STEPS, (int)(sizeof AUTO_STEPS / sizeof AUTO_STEPS[0]), s_auto_sec);
            break;
        default:
            break;                               /* no EXIT row: C closes */
        }
    }

    if (bsp_lvgl_lock(100)) {
        if (shell_changed) apply_shell();
        menu_refresh();
        bsp_lvgl_unlock();
    }
}

/* ---- battery HUD --------------------------------------------------------
 * CW2017 reports SOC directly (0..100, -1 when the gauge does not answer), so
 * the HUD is nothing but a version tag and a percentage, refreshed every
 * BATT_REFRESH_US.
 *
 * No battery glyph: the bar duplicated the number and ate the only strip the
 * glass layout leaves free.  Both labels now sit in the bottom-right corner, on
 * the shell below the glass, flush to the right edge.  The glass was lifted 6 px
 * (s_view_y) so that band does not collide with the bezel, and the right edge
 * stops at -16 px because the panel's 30 px rounded-corner mask starts shaving
 * text past x = 310. */
#define BATT_REFRESH_US (30 * 1000 * 1000)
#define HUD_RIGHT_MARGIN 16
#define HUD_BOTTOM_MARGIN 4
static lv_obj_t *s_ver_lbl;
static lv_obj_t *s_pct_lbl;
static lv_obj_t *s_scr, *s_rim, *s_well;
static int64_t s_batt_next_us;
static int s_soc = -1;

/* Repaint every shell-coloured part after the colourway changes.  The glass
 * itself is untouched on purpose: its three tones belong to the LCD, not to
 * the plastic, and the icon sprites are baked against the glass colour. */
static void apply_shell(void)
{
    if (!s_scr) return;
    const shell_t *sh = &SHELLS[s_shell];
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(sh->body), 0);
    if (s_rim) {
        lv_obj_set_style_bg_color(s_rim, lv_color_hex(sh->rim), 0);
        lv_obj_set_style_border_color(s_rim, lv_color_hex(sh->hi), 0);
    }
    if (s_well) lv_obj_set_style_bg_color(s_well, lv_color_hex(sh->well), 0);
    if (s_ver_lbl) lv_obj_set_style_text_color(s_ver_lbl, lv_color_hex(sh->hud), 0);
    if (s_pct_lbl) lv_obj_set_style_text_color(s_pct_lbl, lv_color_hex(sh->hud), 0);
}

static void battery_ui_build(lv_obj_t *scr)
{
    /* "82%" is pinned to the corner; "V1" hangs off its left edge, so the pair
     * re-flows by itself when the number goes from two to three digits. */
    s_pct_lbl = lv_label_create(scr);
    lv_label_set_text(s_pct_lbl, "--");
    lv_obj_set_style_text_font(s_pct_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_pct_lbl, lv_color_hex(HUD_STRONG), 0);
    lv_obj_align(s_pct_lbl, LV_ALIGN_BOTTOM_RIGHT, -HUD_RIGHT_MARGIN, -HUD_BOTTOM_MARGIN);

    s_ver_lbl = lv_label_create(scr);
    lv_label_set_text(s_ver_lbl, "V1");
    lv_obj_set_style_text_font(s_ver_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ver_lbl, lv_color_hex(HUD_COLOR), 0);
    lv_obj_align_to(s_ver_lbl, s_pct_lbl, LV_ALIGN_OUT_LEFT_BOTTOM, -6, 0);
}

/* ---- NO ROM overlay ------------------------------------------------------
 * A board that has never been given a game image is a perfectly normal state
 * now that ROMs are uploaded instead of embedded (and a wiped 'roms' partition
 * gets here too).  Say so on the glass instead of showing an empty matrix that
 * looks like a dead toy: dark ink on the glass base, same tone as a lit dot, so
 * it reads as printing on the panel rather than as a UI popup.  The hint line
 * names the console command the PC tool sends, which is the only way out. */
static lv_obj_t *s_norom_lbl;
static lv_obj_t *s_norom_sub;

/* A skip really executes the game time (see rom_skip_minutes), which can take
 * a couple of minutes at the 2 h setting.  The panel has already closed by
 * then, so without this card the toy would just look frozen. */
static void skip_build(lv_obj_t *scr)
{
    s_skip_lbl = lv_label_create(scr);
    lv_label_set_text(s_skip_lbl, "SKIP");
    lv_obj_set_style_text_font(s_skip_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_skip_lbl, lv_color_hex(0x1B2417), 0);
    lv_obj_set_style_bg_color(s_skip_lbl, lv_color_hex(0xEDE9DA), 0);
    lv_obj_set_style_bg_opa(s_skip_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_skip_lbl, lv_color_hex(0x2C3324), 0);
    lv_obj_set_style_border_width(s_skip_lbl, 2, 0);
    lv_obj_set_style_pad_all(s_skip_lbl, 10, 0);
    lv_obj_set_style_radius(s_skip_lbl, 8, 0);
    lv_obj_align(s_skip_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_skip_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void norom_build(lv_obj_t *scr)
{
    s_norom_lbl = lv_label_create(scr);
    lv_label_set_text(s_norom_lbl, "NO ROM");
    lv_obj_set_style_text_font(s_norom_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_norom_lbl, lv_color_hex(0x1B2417), 0);
    lv_obj_align_to(s_norom_lbl, s_canvas, LV_ALIGN_CENTER, 0, -12);
    lv_obj_add_flag(s_norom_lbl, LV_OBJ_FLAG_HIDDEN);

    s_norom_sub = lv_label_create(scr);
    lv_label_set_text(s_norom_sub, "SEND ROM VIA USB");
    lv_obj_set_style_text_font(s_norom_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_norom_sub, lv_color_hex(0x2C3A20), 0);
    lv_obj_align_to(s_norom_sub, s_norom_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_add_flag(s_norom_sub, LV_OBJ_FLAG_HIDDEN);
}

/* the version tag rides on the percentage's left edge, so it is re-aligned
 * whenever the number changes width (82% -> 100%) */
static void hud_reflow(void)
{
    if (s_ver_lbl && s_pct_lbl) {
        lv_obj_align_to(s_ver_lbl, s_pct_lbl, LV_ALIGN_OUT_LEFT_BOTTOM, -6, 0);
    }
}
/* caller must hold the LVGL lock */
static void hud_set_soc(int soc)
{
    if (!s_pct_lbl) return;
    char buf[8];
    if (soc < 0) {
        snprintf(buf, sizeof buf, "--");
    } else {
        if (soc > 100) soc = 100;
        s_soc = soc;
        snprintf(buf, sizeof buf, "%d%%", soc);
    }
    lv_label_set_text(s_pct_lbl, buf);
    hud_reflow();
}

/* caller must hold the LVGL lock */
static void battery_ui_update(void)
{
    if (!s_pct_lbl) return;
    hud_set_soc(bsp_battery_soc());
    s_batt_next_us = esp_timer_get_time() + BATT_REFRESH_US;
}

static lv_obj_t *build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(PAPER_COLOR), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_display_t *disp = lv_display_get_default();
    int dw = disp ? (int)lv_display_get_horizontal_resolution(disp) : 320;
    int dh = disp ? (int)lv_display_get_vertical_resolution(disp) : 240;

    /* Glass geometry is fixed by the measurement (see DG_GLASS_* above): 256 x
     * 231 at pitch 8 on a 320x240 panel, so it lands at x = 32, y = 4 and the
     * two 32 px side strips carry the HUD. */
    s_view_w = DG_GLASS_W;
    s_view_h = DG_GLASS_H;
    s_view_x = (dw - s_view_w) / 2;
    /* Lift the glass 6 px off centre: the HUD now lives in the bottom-right
     * corner and needs a clear band under the bezel (221), before the panel's
     * 30 px rounded-corner mask starts eating text. */
    s_view_y = (dh - s_view_h) / 2 - 6;
    if (s_view_y < 4) s_view_y = 4;
    ESP_LOGI(TAG, "panel %dx%d, pitch %d, glass %dx%d at (%d,%d), matrix %dx%d",
             dw, dh, DG_PITCH, s_view_w, s_view_h, s_view_x, s_view_y,
             DG_LCD_W * DG_PITCH, DG_LCD_H * DG_PITCH);

    /* Bezel in three rings, painted back to front: a plastic rim with a 1 px
     * highlight on top, a dark well, then the glass.  With the glass 231 px
     * tall the rim is a 2 px line at the top and bottom and a full strip on
     * the sides, which is what the real shell does around that glass. */
    lv_obj_t *rim = lv_obj_create(scr);
    s_rim = rim;
    lv_obj_set_pos(rim, s_view_x - 6, s_view_y - 6);
    lv_obj_set_size(rim, s_view_w + 12, s_view_h + 12);
    lv_obj_set_style_radius(rim, 8, 0);
    lv_obj_set_style_bg_color(rim, lv_color_hex(BEZEL_COLOR), 0);
    lv_obj_set_style_border_color(rim, lv_color_hex(BEZEL_HI), 0);
    lv_obj_set_style_border_width(rim, 1, 0);
    lv_obj_set_style_pad_all(rim, 0, 0);
    lv_obj_remove_flag(rim, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *well = lv_obj_create(scr);
    s_well = well;
    lv_obj_set_pos(well, s_view_x - 3, s_view_y - 3);
    lv_obj_set_size(well, s_view_w + 6, s_view_h + 6);
    lv_obj_set_style_radius(well, 4, 0);
    lv_obj_set_style_bg_color(well, lv_color_hex(RECESS_COLOR), 0);
    lv_obj_set_style_border_width(well, 0, 0);
    lv_obj_set_style_pad_all(well, 0, 0);
    lv_obj_remove_flag(well, LV_OBJ_FLAG_SCROLLABLE);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, s_view_w, s_view_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, s_view_x, s_view_y);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);

    battery_ui_build(scr);
    norom_build(scr);
    skip_build(scr);
    apply_shell();
    ui_rom_state_locked();    /* first paint: NO ROM unless a slot was found */
    menu_build(scr);          /* last child: always on top of the pet */
    return scr;
}

// ---- ROM switching ------------------------------------------------------
/* Only a real press reaches the emulated keypad: with no ROM loaded there is
 * nothing to receive it, and dg_set_button() on an uninitialised core would
 * just poke dead RAM. */
static void rom_key(uint8_t mask, int down)
{
    if (s_have_rom) dg_set_button(mask, (int)down);
}

/* Wipes the glass back to bare tone.  Dropping to NO ROM used to leave the last
 * frame of the pet on the canvas, because render_lcd() stops running and nothing
 * repaints: the "NO ROM" label then sat on top of a frozen game screen. */
static void glass_blank(void)
{
    if (!s_canvas_buf) return;
    for (int i = 0; i < DG_GLASS_W * DG_GLASS_H; i++) {
        s_canvas_buf[i] = GLASS_TONE[DG_LCD_TONE][0];
    }
    s_pic_valid = false;
    if (s_canvas) lv_obj_invalidate(s_canvas);
}

/* Re-syncs the HUD tag and the NO ROM overlay.  Caller holds the LVGL lock. */
static void ui_rom_state_locked(void)
{
    if (s_ver_lbl) {
        lv_label_set_text(s_ver_lbl, s_have_rom ? rom_name(s_rom_idx) : "--");
        hud_reflow();
    }
    if (!s_norom_lbl) return;
    if (s_have_rom) {
        lv_obj_add_flag(s_norom_lbl, LV_OBJ_FLAG_HIDDEN);
        if (s_norom_sub) lv_obj_add_flag(s_norom_sub, LV_OBJ_FLAG_HIDDEN);
    } else {
        glass_blank();                       /* no leftover pet behind the text */
        s_icon_stale = true;                 /* bands get re-blitted unlit */
        lv_obj_remove_flag(s_norom_lbl, LV_OBJ_FLAG_HIDDEN);
        if (s_norom_sub) lv_obj_remove_flag(s_norom_sub, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_rom_state(void)
{
    if (!bsp_lvgl_lock(100)) return;
    ui_rom_state_locked();
    bsp_lvgl_unlock();
}

static void switch_rom(int idx)
{
    if (idx < 0 || idx >= DG_ROM_SLOTS || idx == s_rom_idx) return;
    save_slot(s_rom_idx);                 /* park the current pet in its slot */
    s_rom_idx = idx;

    if (!s_roms[idx].valid) {             /* erased over USB: nothing to run */
        s_have_rom = false;
        s_rom_ptr = NULL;
        s_rom_len = 0;
        ESP_LOGI(TAG, "ROM -> none (slot %d empty)", idx);
    } else {
        s_have_rom = rom_load(idx);
        if (s_have_rom) {
            save_rom_index(idx);
            load_slot(idx);
            ESP_LOGI(TAG, "ROM -> %s (%u bytes)", rom_name(idx), (unsigned)s_rom_len);
        } else {
            ESP_LOGE(TAG, "ROM -> %s: image unreadable, showing NO ROM", rom_name(idx));
        }
    }
    s_pic_valid = false;
    s_icon_state = 0xFF;
    s_icon_stale = true;
    ui_rom_state();
}

// ---- main loop ----------------------------------------------------------
static void audio_task(void *arg)
{
    (void)arg;
    audio_block_t b;
    for (;;) {
        if (xQueueReceive(s_audio_q, &b, portMAX_DELAY) == pdTRUE) {
            bsp_audio_write(b.pcm, sizeof(b.pcm));
        }
    }
}

static void fill_audio(audio_block_t *b)
{
    const double f = dg_sound_freq();
    double amp = dg_sound_amp();
    if (f < 20.0 || amp <= 0.0) {
        memset(b->pcm, 0, sizeof(b->pcm));
        return;
    }
    if (amp > 1.0) amp = 1.0;
    const double step = f / (double)AUDIO_HZ;
    const int16_t level = (int16_t)(9000.0 * amp);
    for (int i = 0; i < AUDIO_SAMPLES; i++) {
        s_phase += step;
        if (s_phase >= 1.0) s_phase -= 1.0;
        b->pcm[i] = (s_phase < 0.5) ? level : (int16_t)(-level);
    }
}

static void dg_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task entered, free heap=%u", (unsigned)esp_get_free_heap_size());
    if (!s_have_rom) {
        ESP_LOGW(TAG, "no ROM in the 'roms' partition: glass shows NO ROM until one is uploaded");
    } else {
        int64_t t0 = esp_timer_get_time();
        uint32_t tick_target = dg_osc1_ticks() + TICKS_PER_FRAME;
        int guard = 0;
        while ((int32_t)(dg_osc1_ticks() - tick_target) < 0 && guard++ < STEP_GUARD) {
            dg_step();
        }
        ESP_LOGI(TAG, "first frame (%d ticks, fastmode=%d) took %lld us",
                 TICKS_PER_FRAME, dg_cpu_fast_mode(), (long long)(esp_timer_get_time() - t0));
        t0 = esp_timer_get_time();
        if (bsp_lvgl_lock(100)) { (void)render_lcd(); bsp_lvgl_unlock(); }
        ESP_LOGI(TAG, "first render took %lld us", (long long)(esp_timer_get_time() - t0));
        for (int r = 0; r < DG_LCD_H; r++) {
            char line[DG_LCD_W + 1];
            for (int c = 0; c < DG_LCD_W; c++) line[c] = s_last_pic[r][c] ? '#' : '.';
            line[DG_LCD_W] = 0;
            ESP_LOGI(TAG, "lcd %s", line);
        }
    }
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        if (s_have_rom) {
            uint32_t tick_target = dg_osc1_ticks() + TICKS_PER_FRAME;
            int guard = 0;
            while ((int32_t)(dg_osc1_ticks() - tick_target) < 0 && guard++ < STEP_GUARD) {
                dg_step();
            }
            age_tick();          /* hatch detection + 养育总时间 */
        }

        /* Forward keys. The ROM samples K0 at ~2 Hz and needs the key held for
         * at least ~120 ms (measured), so a release that arrives sooner is
         * deferred: the emulated key stays down until KEY_MIN_HOLD_US has
         * passed. Everything else -- what A/B/C do, when the icon row is
         * active, which icon is highlighted -- is decided by the ROM. */
        dg_key_ev_t e;
        while (xQueueReceive(s_key_q, &e, 0) == pdTRUE) {
            int64_t now_us = esp_timer_get_time();
            s_last_key_us = now_us;

            /* A dark screen swallows the key that wakes it -- both edges.
             * Only the press was dropped before, so the release came through
             * with a stale s_ok_press_us and "held" looked like 1.2 s+; a short
             * tap on OK therefore woke the screen AND opened the settings. */
            if (s_screen_off) {
                if (e.down) {
                    screen_set(false);
                    s_wake_mask = e.mask;
                }
                continue;
            }
            if (s_wake_mask) {
                if (e.mask == s_wake_mask) {
                    if (!e.down) s_wake_mask = 0;
                    continue;
                }
                s_wake_mask = 0;         /* another key arrived first */
            }

            /* OK: hold it in the game to open the menu (the hold fires from the
             * periodic check below, not from the release); inside the menu a
             * press closes it again (see menu_key: A next row, B change value,
             * C close). */
            if (e.mask == 1) {
                if (e.down) {
                    s_ok_down = true;
                    s_ok_armed = true;
                    s_ok_held_consumed = false;
                    s_ok_press_us = now_us;
                } else {
                    int64_t held = now_us - s_ok_press_us;
                    s_ok_down = false;
                    bool consumed = s_ok_held_consumed;
                    bool armed = s_ok_armed;
                    s_ok_armed = false;
                    s_ok_held_consumed = false;
                    if (consumed) continue;  /* this hold already opened the menu */
                    if (!armed) continue;    /* release of a press nobody timed */
                    /* fallback: if the loop never got a chance to see the hold
                     * expire, still honour a long hold on release */
                    if (!s_menu_open && held >= MENU_HOLD_US) {
                        rom_key(1, 0);
                        s_key_pending = 0;
                        menu_set_open(true);
                        continue;
                    }
                    if (s_menu_open) {
                        menu_key(1);
                        continue;
                    }
                }
            }

            if (s_menu_open) {
                if (e.down && e.mask != 1) menu_key(e.mask);
                s_key_pending = 0;
                continue;
            }

            if (e.down) {
                /* A new press supersedes any deferred release: if it is a
                 * different key, let the old one go now; if it is the same
                 * key, the press simply continues. */
                if (s_key_pending && s_key_pending != e.mask) {
                    rom_key(s_key_pending, 0);
                }
                s_key_pending = 0;
                s_key_release_at = 0;
                rom_key(e.mask, 1);
                s_key_mask = e.mask;
                s_key_down_us = now_us;
            } else {
                int64_t earliest = (e.mask == s_key_mask)
                                 ? s_key_down_us + KEY_MIN_HOLD_US : 0;
                if (now_us >= earliest) {
                    rom_key(e.mask, 0);
                } else {
                    s_key_pending = e.mask;
                    s_key_release_at = earliest;
                }
            }
        }
        if (s_key_pending && esp_timer_get_time() >= s_key_release_at) {
            rom_key(s_key_pending, 0);
            s_key_pending = 0;
            s_key_release_at = 0;
        }

        /* Hold OK in the game for MENU_HOLD_US -> open the settings right away,
         * while the finger is still on the key.  The ROM had the C press
         * forwarded already, so let it go now: that is what the old
         * "release-opens" path did too. */
        if (s_ok_down && s_ok_armed && !s_menu_open && !s_screen_off &&
            (esp_timer_get_time() - s_ok_press_us) >= MENU_HOLD_US) {
            s_ok_armed = false;
            s_ok_held_consumed = true;
            rom_key(1, 0);
            s_key_pending = 0;
            s_key_release_at = 0;
            menu_set_open(true);
        }

        if (!s_menu_open) console_poll();

        /* idle timeout -> backlight off; any key brings it back */
        if (!s_screen_off && s_auto_sec > 0 &&
            (esp_timer_get_time() - s_last_key_us) >= (int64_t)s_auto_sec * 1000000) {
            ESP_LOGI(TAG, "auto screen off after %us", (unsigned)s_auto_sec);
            screen_set(true);
        }

        /* rolling autosave: the hardware power key cuts power silently */
        if (s_have_rom && esp_timer_get_time() - s_last_save_us >= SAVE_INTERVAL_US) {
            save_slot(s_rom_idx);
        }

        if (s_audio_q) {
            audio_block_t b;
            if (s_have_rom) {
                fill_audio(&b);
            } else {
                memset(&b, 0, sizeof b);       /* silence: nothing is running */
            }
            xQueueSend(s_audio_q, &b, 0);
        }

        if (bsp_lvgl_lock(10)) {
            /* With no ROM the glass keeps the base tone plus the unlit icon
             * bands; render_lcd() would stamp whatever the VRAM shadow holds. */
            if (s_have_rom) (void)render_lcd();
            render_icons();
            if (esp_timer_get_time() >= s_batt_next_us) battery_ui_update();
            bsp_lvgl_unlock();
        }

        /* If this frame already overran its budget, do not let vTaskDelayUntil
         * return immediately for every following frame (that degrades into a
         * busy loop and trips the task watchdog). Resync instead. */
        if ((TickType_t)(xTaskGetTickCount() - next) >= pdMS_TO_TICKS(FRAME_MS * 2)) {
            next = xTaskGetTickCount();
        }
        vTaskDelayUntil(&next, pdMS_TO_TICKS(FRAME_MS));

        if (++s_frames % 250 == 0) {          /* every 5 s at 50 fps */
            if (s_have_rom) {
                ESP_LOGI(TAG, "alive frame=%u pc=%u instr=%u",
                         s_frames, dg_pc(), dg_instr_count());
            } else {
                ESP_LOGI(TAG, "alive frame=%u (no rom)", s_frames);
            }
        }
    }
}

esp_err_t digimon_app_start(void)
{
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nerr = nvs_flash_init();
    }
    if (nerr != ESP_OK) ESP_LOGW(TAG, "nvs init: %s", esp_err_to_name(nerr));

#ifdef DG_SIM_UART0_INPUT
    /* Simulator verification build only: the console arrives on UART0 because
     * the wasm emulator has no USB-Serial-JTAG.  Never in a device build --
     * UART0 TX is the backlight pin. */
    {
        const uart_config_t ucfg = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0);
        uart_param_config(UART_NUM_0, &ucfg);
        /* stdio follows the UART: the emulator only forwards UART0, so this is
         * also what makes the firmware's own log lines readable in a simulator
         * run (on the board they go to USB-Serial-JTAG). */
        uart_vfs_dev_use_driver(UART_NUM_0);
        uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
        uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
    }
#endif

    esp_err_t rerr = dg_rom_store_init();
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "rom partition unusable (%s): running with NO ROM", esp_err_to_name(rerr));
    }
    rom_table_load();
    load_saved_rom();
    rom_crc_init();
    load_ui_settings();
    s_last_key_us = esp_timer_get_time();

    s_canvas_buf = heap_caps_malloc(DG_GLASS_W * DG_GLASS_H * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!s_canvas_buf) {
        ESP_LOGE(TAG, "no memory for glass canvas (%u bytes)",
                 (unsigned)(DG_GLASS_W * DG_GLASS_H * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < DG_GLASS_W * DG_GLASS_H; i++) {
        s_canvas_buf[i] = GLASS_TONE[DG_LCD_TONE][0];
    }

    s_last_save_us = esp_timer_get_time();
    if (s_rom_idx >= 0) {
        s_have_rom = rom_load(s_rom_idx);
        if (s_have_rom) {
            ESP_LOGI(TAG, "ROM %s (slot %d), %u bytes, %u state bytes",
                     rom_name(s_rom_idx), s_rom_idx, (unsigned)s_rom_len,
                     (unsigned)dg_state_size());
            if (load_slot(s_rom_idx)) {
                s_pic_valid = false;          /* repaint whatever the pet left on screen */
            } else {
                ESP_LOGI(TAG, "no save for %s, starting fresh", rom_name(s_rom_idx));
            }
        } else {
            ESP_LOGE(TAG, "slot %d unreadable, falling back to NO ROM", s_rom_idx);
            s_rom_idx = -1;
        }
    }
    if (!s_have_rom) {
        ESP_LOGW(TAG, "no ROM uploaded: glass shows NO ROM (send ROMIN from the PC tool)");
    }

    s_key_q = xQueueCreate(16, sizeof(dg_key_ev_t));
    if (!s_key_q) return ESP_ERR_NO_MEM;

    esp_err_t err = bsp_button_init(on_button, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "button init: %s", esp_err_to_name(err));

    if (bsp_lvgl_lock(1000)) {
        build_ui();               /* also paints NO ROM vs. the HUD tag */
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(s_bright);

    if (bsp_audio_set_format(AUDIO_HZ, 16, 1) == ESP_OK) {
        bsp_audio_set_volume(s_vol);
        s_audio_q = xQueueCreate(3, sizeof(audio_block_t));
        if (s_audio_q) xTaskCreate(audio_task, "dg_audio", 3072, NULL, 4, NULL);
    } else {
        ESP_LOGW(TAG, "audio unavailable, running silent");
    }

    BaseType_t created = xTaskCreate(dg_task, "digimon", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "started, task create=%d free heap=%u",
             (int)created, (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}




























