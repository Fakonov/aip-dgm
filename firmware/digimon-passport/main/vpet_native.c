// main/vpet_native.c
// "Enhanced 20th" prototype: digipocket sprites + the V-Pet care loop.
//
// Sprites : 89 pets x 12 animation frames, 16x16 1-bit, from the packed
//           digipocket database (see tools/gen_pet_headers.py).
// Logic   : the four care parameters, decay timers and stage ladder recovered
//           from the DoCoMo i-appli build (research/v5-iappli).
// Evolution: three-way best/middle/worst split, exactly like the V-Pet, driven
//           by how well the pet was cared for.
//
// Screen: 320x240 landscape. LCD-green pet panel on the left (sprite drawn on an
// LVGL canvas at 6x, 16 px -> 96 px), four status bars on the right, log line
// and hint line at the bottom.

#include "vpet_native.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"

#include "pet_sprites.h"
#include "pet_evo.h"

static const char *TAG = "vpet";

#define SCR_W 320

// ---------------------------------------------------------------- palette
#define C_BG      0x1E2733
#define C_PANEL   0x2C3A4A
#define C_INK     0x0E1218
#define C_LCD     0x9CBC0F
#define C_TEXT    0xE9F2D6
#define C_DIM     0xB6C48F
#define C_YELLOW  0xFFD928
#define C_ORANGE  0xFFB23E

#define SPR_SCALE  6
#define SPR_PX     (PET_W * SPR_SCALE)      // 96
#define SPR_X      33
#define SPR_Y      63

static const char *STAGE_NAME[7] = {
    "DIGITAMA", "BABY I", "BABY II", "CHILD", "ADULT", "PERFECT", "ULTIMATE",
};

enum { F_WALK1 = 0, F_WALK2 = 1, F_EAT1 = 2, F_EAT2 = 3, F_SLEEP1 = 4, F_SLEEP2 = 5,
       F_REFUSE = 6, F_HAPPY = 7, F_ANGRY = 8, F_INJURED1 = 9, F_INJURED2 = 10, F_ATTACK = 11 };

// ---------------------------------------------------------------- state
typedef struct {
    uint8_t pet;
    uint8_t hunger;
    uint8_t strength;
    uint8_t training;
    uint8_t weight;
    uint8_t dirty;
    uint8_t sick;
    uint8_t alive;
    uint8_t mood;         // 0 idle 1 happy 2 angry 3 eat 4 attack
    uint8_t mood_t;
    uint32_t age_ticks;
    uint32_t hunger_t;
    uint32_t strength_t;
    uint32_t dirty_t;
} vpet_t;

static vpet_t s_pet;
static bool s_dirty_ui = true;

static lv_obj_t *s_name;
static lv_obj_t *s_stage;
static lv_obj_t *s_fill[4];
static lv_obj_t *s_vals[4];
static lv_obj_t *s_log;
static lv_obj_t *s_hint;
static lv_obj_t *s_canvas;
static uint8_t s_canvas_buf[SPR_PX * SPR_PX * 2];   // RGB565

#define TICK_MS         100
#define HUNGER_PERIOD   40
#define STRENGTH_PERIOD 80
#define DIRTY_PERIOD    60
#define BAR_W           106
#define VPET_START_ID   8       // Botamon in the packed digipocket table

static const pet_sprite_t *sp(void) { return &PET_SPRITES[s_pet.pet]; }

// ---------------------------------------------------------------- drawing
static void blit_sprite(int frame)
{
    if (!s_canvas) return;
    const uint8_t *bits = PET_BITMAPS + (size_t)s_pet.pet * PET_RECORD_BYTES
                        + (size_t)frame * PET_FRAME_BYTES;
    const uint16_t ink = lv_color_to_u16(lv_color_hex(C_INK));
    const uint16_t paper = lv_color_to_u16(lv_color_hex(C_LCD));
    uint16_t *dst = (uint16_t *)s_canvas_buf;

    for (int y = 0; y < PET_H; y++) {
        for (int x = 0; x < PET_W; x++) {
            int bit = y * PET_W + x;
            bool on = (bits[bit >> 3] & (0x80 >> (bit & 7))) != 0;
            uint16_t c = on ? ink : paper;
            for (int sy = 0; sy < SPR_SCALE; sy++) {
                uint16_t *row = dst + (size_t)(y * SPR_SCALE + sy) * SPR_PX + x * SPR_SCALE;
                for (int sx = 0; sx < SPR_SCALE; sx++) row[sx] = c;
            }
        }
    }
    lv_obj_invalidate(s_canvas);
}

static int current_frame(void)
{
    bool blink = (lv_tick_get() / 400) % 2;
    switch (s_pet.mood) {
    case 1:  return F_HAPPY;
    case 2:  return F_ANGRY;
    case 3:  return blink ? F_EAT1 : F_EAT2;
    case 4:  return F_ATTACK;
    default: break;
    }
    if (s_pet.sick) return blink ? F_INJURED1 : F_INJURED2;
    return blink ? F_WALK1 : F_WALK2;
}

// ---------------------------------------------------------------- ui
static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void build_screen(void)
{
    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);

    block(root, 0, 0, SCR_W, 30, C_INK);
    s_name = text(root, "BOTAMON", &lv_font_montserrat_20, C_YELLOW);
    lv_obj_set_pos(s_name, 10, 4);
    s_stage = text(root, "BABY I", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(s_stage, 236, 8);

    block(root, 8, 38, 186, 150, C_INK);
    block(root, 11, 41, 180, 144, C_LCD);
    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, SPR_PX, SPR_PX, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, SPR_X, SPR_Y);

    static const char *LABEL[4] = {"HUNGER", "POWER", "STUDY", "WEIGHT"};
    static const uint32_t COLOR[4] = {C_YELLOW, C_ORANGE, C_TEXT, C_DIM};
    for (int i = 0; i < 4; i++) {
        int y = 40 + i * 38;
        lv_obj_t *l = text(root, LABEL[i], &lv_font_montserrat_14, C_DIM);
        lv_obj_set_pos(l, 204, y);
        block(root, 204, y + 18, BAR_W, 12, C_PANEL);
        s_fill[i] = block(root, 204, y + 18, 0, 12, COLOR[i]);
        s_vals[i] = text(root, "0", &lv_font_montserrat_14, C_TEXT);
        lv_obj_set_pos(s_vals[i], 282, y - 2);
    }

    block(root, 0, 194, SCR_W, 46, C_INK);
    s_log = text(root, "A:FEED  B:TRAIN  C:EVOLVE", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(s_log, 8, 198);
    s_hint = text(root, "", &lv_font_montserrat_14, C_DIM);
    lv_obj_set_pos(s_hint, 8, 220);

    lv_screen_load(root);
}

static void log_line(const char *t)
{
    s_dirty_ui = true;
    if (s_log) lv_label_set_text(s_log, t);
}

static void refresh_ui(void)
{
    const pet_sprite_t *cur = sp();
    lv_label_set_text(s_name, cur->name);
    lv_label_set_text(s_stage, STAGE_NAME[cur->stage]);

    int vals[4] = {
        s_pet.hunger,
        s_pet.strength * 25,
        cur->energy_max ? (s_pet.training * 100 / cur->energy_max) : 0,
        s_pet.weight,
    };
    char buf[8];
    for (int i = 0; i < 4; i++) {
        int v = vals[i];
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        lv_obj_set_width(s_fill[i], BAR_W * v / 100);
        snprintf(buf, sizeof(buf), "%d", vals[i]);
        lv_label_set_text(s_vals[i], buf);
    }

    blit_sprite(current_frame());

    char h[64];
    snprintf(h, sizeof(h), "age %lus  dirty %u/8  sick %u  id %u",
             (unsigned long)(s_pet.age_ticks * TICK_MS / 1000), s_pet.dirty, s_pet.sick, s_pet.pet);
    lv_label_set_text(s_hint, h);
}

// ---------------------------------------------------------------- rules
static void feed(void)
{
    const pet_sprite_t *cur = sp();
    if (s_pet.hunger >= cur->hunger_max) {
        log_line("Hunger is full");
        s_pet.mood = 2; s_pet.mood_t = 8;
        return;
    }
    s_pet.hunger++;
    s_pet.weight++;
    s_pet.mood = 3; s_pet.mood_t = 8;
    char b[48];
    snprintf(b, sizeof(b), "Feed +1 (hunger %u/%u)", s_pet.hunger, cur->hunger_max);
    log_line(b);
}

static void train(void)
{
    const pet_sprite_t *cur = sp();
    if (s_pet.strength < cur->strength_max) s_pet.strength++;
    if (s_pet.training < 255) s_pet.training++;
    s_pet.mood = 4; s_pet.mood_t = 6;
    char b[52];
    snprintf(b, sizeof(b), "Train: power %u study %u/%u",
             s_pet.strength, s_pet.training, cur->energy_max);
    log_line(b);
}

static void try_evolve(void)
{
    const pet_sprite_t *cur = sp();
    if (!s_pet.alive) return;
    if (cur->stage >= 6) {
        log_line("Already ULTIMATE");
        return;
    }
    // Three-way split on care quality (same shape as the V-Pet):
    //   >= 60% of the energy cap -> best, >= 25% -> middle, else worst.
    int pct = cur->energy_max ? (s_pet.training * 100 / cur->energy_max) : 100;
    int branch = (pct >= 60) ? 0 : (pct >= 25 ? 1 : 2);
    uint8_t next = PET_EVO[s_pet.pet][branch];
    if (next == PET_NO_EVO) {
        log_line("No further evolution");
        return;
    }
    s_pet.pet = next;
    s_pet.training = 0;
    s_pet.hunger = PET_SPRITES[next].hunger_max;
    s_pet.weight = PET_SPRITES[next].base_weight;
    s_pet.mood = 1; s_pet.mood_t = 12;
    char b[52];
    snprintf(b, sizeof(b), "EVOLVED! %s (%s)", PET_SPRITES[next].name,
             branch == 0 ? "best" : (branch == 1 ? "mid" : "worst"));
    log_line(b);
    ESP_LOGI(TAG, "evolve -> %s branch=%d pct=%d", PET_SPRITES[next].name, branch, pct);
}

static void new_pet(uint8_t id)
{
    memset(&s_pet, 0, sizeof(s_pet));
    s_pet.pet = id;
    s_pet.alive = 1;
    s_pet.hunger = PET_SPRITES[id].hunger_max;
    s_pet.strength = 1;
    s_pet.weight = PET_SPRITES[id].base_weight;
    s_pet.hunger_t = HUNGER_PERIOD;
    s_pet.strength_t = STRENGTH_PERIOD;
    s_pet.dirty_t = DIRTY_PERIOD;
    s_dirty_ui = true;
}

// ---------------------------------------------------------------- input
static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (ev != BSP_BTN_PRESS) return;
    if (!s_pet.alive) {
        if (btn == BSP_BTN_OK) { new_pet(0); log_line("A new egg hatched"); }
        return;
    }
    switch (btn) {
    case BSP_BTN_OK:   feed();  break;
    case BSP_BTN_UP:   train(); break;
    case BSP_BTN_DOWN: try_evolve(); break;
    default: break;
    }
    s_dirty_ui = true;
}

// ---------------------------------------------------------------- tick
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_pet.mood_t && --s_pet.mood_t == 0) { s_pet.mood = 0; s_dirty_ui = true; }
    if (!s_pet.alive) { if (s_dirty_ui) { refresh_ui(); s_dirty_ui = false; } return; }

    s_pet.age_ticks++;
    if (--s_pet.hunger_t == 0) {
        s_pet.hunger_t = HUNGER_PERIOD;
        if (s_pet.hunger > 0) s_pet.hunger--;
        s_dirty_ui = true;
    }
    if (--s_pet.strength_t == 0) {
        s_pet.strength_t = STRENGTH_PERIOD;
        if (s_pet.strength > 0) s_pet.strength--;
        s_dirty_ui = true;
    }
    if (--s_pet.dirty_t == 0) {
        s_pet.dirty_t = DIRTY_PERIOD;
        if (s_pet.dirty < 8) s_pet.dirty++;
        if (s_pet.dirty >= 8) s_pet.sick++;
        s_dirty_ui = true;
    }
    if (s_pet.hunger == 0 && s_pet.dirty >= 8) {
        s_pet.alive = 0;
        s_pet.mood = 2;
        log_line("It died -- press A for a new egg");
        ESP_LOGI(TAG, "pet died at %lu s", (unsigned long)(s_pet.age_ticks * TICK_MS / 1000));
        s_dirty_ui = true;
    }
    if (s_dirty_ui) { refresh_ui(); s_dirty_ui = false; }
}

// ---------------------------------------------------------------- entry
esp_err_t vpet_native_start(void)
{
    ESP_LOGI(TAG, "enhanced-20th prototype: %d pets, %d frames each",
             PET_COUNT, PET_FRAME_COUNT);
    new_pet(VPET_START_ID);      // Botamon is record 8 in the packed table

    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "lvgl lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    build_screen();
    refresh_ui();
    lv_timer_create(tick_cb, TICK_MS, NULL);
    bsp_lvgl_unlock();

    ESP_ERROR_CHECK(bsp_button_init(on_button, NULL));
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}
