// main/vpet_native.c
// Native Digital Monster Ver.5 prototype (no E0C6200 emulation).
//
// Everything below comes from the reverse-engineering notes in research/v5-iappli:
//   * the 14-species roster and its per-species limits  (docs/05-data-tables.md)
//   * the four care parameters and their decay rules   (docs/07-time-care-death.md)
//   * the stage ladder and the evolution gate          (docs/05, docs/09)
//
// Scope: one pet on screen, three-button care (feed / train / evolve), time-based
// decay, stage-up when the training counter crosses the threshold.  Battles, the
// dex and the full evolution bytecode VM are out of scope for this prototype.
//
// The UI is drawn from scratch on a 320x240 landscape canvas: an LCD-green pet
// panel on the left, a four-row status column on the right, one log line below.
// Widgets are plain rectangles (LV_USE_BAR is off in this build), the bars are
// background + fill blocks that get their width rewritten every refresh.

#include "vpet_native.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"

static const char *TAG = "vpet";

#define SCR_W 320

// ---------------------------------------------------------------- palette
#define C_BG      0x1E2733
#define C_PANEL   0x2C3A4A
#define C_INK     0x0E1218
#define C_LCD     0x9CBC0F      // the same LCD green as the i-appli build
#define C_LCD_DK  0x6E8509
#define C_TEXT    0xE9F2D6
#define C_DIM     0xB6C48F
#define C_YELLOW  0xFFD928
#define C_ORANGE  0xFFB23E

// ---------------------------------------------------------------- data
typedef struct {
    const char *name;
    uint8_t stage;       // 1..5
    uint8_t wake;        // aJ[0]
    uint8_t sleep;       // aJ[1]
    uint8_t stomach;     // aJ[4]
    uint8_t train_cap;   // aJ[6]
    uint8_t atk;         // aJ[7]
    uint8_t sick_ticks;  // aJ[8]
    uint8_t hp;          // aJ[9]
} species_t;

// index 0 is the empty slot, exactly like the original (ids run 1..14)
static const species_t SPECIES[15] = {
    {"-",              0,  0,  0,  0,   0,  0, 0,  0},
    {"Zurumon",        1,  0,  0,  4,   3,  0, 0,  0},
    {"Pagumon",        2,  8, 20, 12,  33,  0, 1,  0},
    {"Gazimon",        3,  8, 20, 24,  65,  3, 1,  1},
    {"Gizamon",        3,  9, 20, 20,  49,  4, 1,  2},
    {"DarkTyrannomon", 4,  7, 21, 32,  81,  2, 1,  3},
    {"Cyclomon",       4,  8, 20, 24,  65,  3, 9,  3},
    {"Devidramon",     4,  7, 20, 40,  65,  2, 5,  4},
    {"Tuskmon",        4,  8, 21, 40,  49,  2, 3,  4},
    {"Flymon",         4,  8, 21, 16,  65,  2, 1,  5},
    {"Deltamon",       4,  8, 20, 32,  49,  3, 1, 13},
    {"Raremon",        4, 10, 23, 16,  33,  4, 1,  6},
    {"ExTyrannomon",   5,  9, 21, 10,  65,  2, 1, 14},
    {"Nanomon",        5,  7, 19,  5,  49,  1, 1,  8},
    {"MetalTyrannomon",5,  8, 21, 40,  81,  1, 1, 15},
};

static const char *STAGE_NAME[6] = {"", "BABY I", "BABY II", "CHILD", "ADULT", "PERFECT"};

static const uint8_t STAGE3[] = {3, 4};
static const uint8_t STAGE4[] = {5, 6, 7, 8, 9, 10, 11};
static const uint8_t STAGE5[] = {12, 13, 14};

// ---------------------------------------------------------------- state
typedef struct {
    uint8_t species;
    uint8_t hunger;
    uint8_t strength;
    uint8_t training;
    uint8_t weight;
    uint8_t dirty;
    uint8_t sick;
    uint8_t alive;
    uint32_t age_ticks;
    uint32_t hunger_t;
    uint32_t strength_t;
    uint32_t dirty_t;
} vpet_t;

static vpet_t s_pet;
static bool s_dirty_ui = true;

static lv_obj_t *s_name;
static lv_obj_t *s_stage;
static lv_obj_t *s_eye_l;
static lv_obj_t *s_eye_r;
static lv_obj_t *s_fill[4];
static lv_obj_t *s_vals[4];
static lv_obj_t *s_log;
static lv_obj_t *s_hint;

// Prototype pacing: one "care step" every 4 s so a session shows decay within a
// minute instead of the original's hours.
#define TICK_MS         100
#define HUNGER_PERIOD   40
#define STRENGTH_PERIOD 80
#define DIRTY_PERIOD    60
#define BAR_W           106

static const species_t *sp(void) { return &SPECIES[s_pet.species]; }

static void log_line(const char *text)
{
    s_dirty_ui = true;
    if (s_log) lv_label_set_text(s_log, text);
}

static void new_pet(uint8_t id)
{
    memset(&s_pet, 0, sizeof(s_pet));
    s_pet.species = id;
    s_pet.alive = 1;
    s_pet.hunger = sp()->stomach;
    s_pet.strength = 1;
    s_pet.weight = 5 + id;
    s_pet.hunger_t = HUNGER_PERIOD;
    s_pet.strength_t = STRENGTH_PERIOD;
    s_pet.dirty_t = DIRTY_PERIOD;
    s_dirty_ui = true;
}

static uint8_t pick_from(const uint8_t *list, size_t n)
{
    return list[esp_random() % n];
}

// Stage up when the training counter is high enough.  Replaces the original
// bytecode VM (`F()`), which is overkill for a prototype.
static void try_evolve(void)
{
    const species_t *cur = sp();
    if (!s_pet.alive || cur->stage >= 5) {
        log_line("Already at the last stage");
        return;
    }
    uint8_t need = (uint8_t)(cur->train_cap / 2);   /* half of this species cap */
    if (need < 2) need = 2;
    if (s_pet.training < need) {
        char buf[52];
        snprintf(buf, sizeof(buf), "Need %u study to evolve (now %u)", need, s_pet.training);
        log_line(buf);
        return;
    }
    uint8_t next;
    if (cur->stage <= 2)   next = pick_from(STAGE3, sizeof(STAGE3));
    else if (cur->stage == 3) next = pick_from(STAGE4, sizeof(STAGE4));
    else                   next = pick_from(STAGE5, sizeof(STAGE5));

    s_pet.species = next;
    s_pet.training = 0;
    s_pet.hunger = sp()->stomach;
    char buf[52];
    snprintf(buf, sizeof(buf), "EVOLVED! %s", sp()->name);
    log_line(buf);
    ESP_LOGI(TAG, "evolved to %s (id %u)", sp()->name, next);
}

// ---------------------------------------------------------------- input
static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (ev != BSP_BTN_PRESS) return;
    if (!s_pet.alive) {
        if (btn == BSP_BTN_OK) {
            new_pet(1);
            log_line("A new egg hatched");
        }
        return;
    }
    switch (btn) {
    case BSP_BTN_OK: {                      // feed
        const species_t *cur = sp();
        if (s_pet.hunger >= cur->stomach) {
            log_line("Hunger is full");
        } else {
            s_pet.hunger++;
            s_pet.weight++;
            char buf[48];
            snprintf(buf, sizeof(buf), "Feed +1 (hunger %u/%u)", s_pet.hunger, cur->stomach);
            log_line(buf);
        }
        break;
    }
    case BSP_BTN_UP: {                      // train
        const species_t *cur = sp();
        if (s_pet.strength < 4) s_pet.strength++;
        if (s_pet.training < cur->train_cap) s_pet.training++;
        char buf[52];
        snprintf(buf, sizeof(buf), "Train: power %u study %u/%u",
                 s_pet.strength, s_pet.training, cur->train_cap);
        log_line(buf);
        break;
    }
    case BSP_BTN_DOWN:                      // evolve check
        try_evolve();
        break;
    default:
        break;
    }
    s_dirty_ui = true;
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

    // top bar
    block(root, 0, 0, SCR_W, 30, C_INK);
    s_name = text(root, "ZURUMON", &lv_font_montserrat_20, C_YELLOW);
    lv_obj_set_pos(s_name, 10, 4);
    s_stage = text(root, "BABY I", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(s_stage, 236, 8);

    // pet panel (left)
    block(root, 8, 38, 186, 150, C_INK);
    block(root, 11, 41, 180, 144, C_LCD);
    block(root, 59, 78, 84, 82, C_LCD_DK);      // body
    block(root, 67, 70, 68, 78, C_INK);         // face plate
    s_eye_l = block(root, 81, 94, 16, 16, C_LCD);
    s_eye_r = block(root, 121, 94, 16, 16, C_LCD);
    block(root, 91, 126, 26, 8, C_LCD);         // mouth
    block(root, 45, 150, 18, 12, C_LCD_DK);     // foot L
    block(root, 143, 150, 18, 12, C_LCD_DK);    // foot R

    // status column (right)
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

    // log + hint (bottom)
    block(root, 0, 194, SCR_W, 46, C_INK);
    s_log = text(root, "A:FREE  B:TRAIN  C:EVOLVE", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(s_log, 8, 198);
    s_hint = text(root, "", &lv_font_montserrat_14, C_DIM);
    lv_obj_set_pos(s_hint, 8, 220);

    lv_screen_load(root);
}

static void refresh_ui(void)
{
    const species_t *cur = sp();
    lv_label_set_text(s_name, cur->name);
    lv_label_set_text(s_stage, STAGE_NAME[cur->stage]);

    int vals[4] = {
        s_pet.hunger,
        s_pet.strength * 25,
        cur->train_cap ? (s_pet.training * 100 / cur->train_cap) : 0,
        s_pet.weight > 99 ? 99 : s_pet.weight,
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

    lv_opa_t eye = (lv_tick_get() / 500) % 2 ? LV_OPA_COVER : LV_OPA_30;
    lv_obj_set_style_bg_opa(s_eye_l, eye, 0);
    lv_obj_set_style_bg_opa(s_eye_r, eye, 0);

    char h[48];
    snprintf(h, sizeof(h), "age %lus   dirty %u/8   sick %u",
             (unsigned long)(s_pet.age_ticks * TICK_MS / 1000), s_pet.dirty, s_pet.sick);
    lv_label_set_text(s_hint, h);
}

// ---------------------------------------------------------------- tick
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_pet.alive) {
        if (s_dirty_ui) { refresh_ui(); s_dirty_ui = false; }
        return;
    }
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
        if (s_pet.dirty >= 8) {
            s_pet.sick++;
            log_line("Got sick");
        }
        s_dirty_ui = true;
    }
    if (s_pet.hunger == 0 && s_pet.dirty >= 8) {
        s_pet.alive = 0;
        log_line("It died -- press A for a new egg");
        ESP_LOGI(TAG, "pet died at %lu s", (unsigned long)(s_pet.age_ticks * TICK_MS / 1000));
        s_dirty_ui = true;
    }

    if (s_dirty_ui) {
        refresh_ui();
        s_dirty_ui = false;
    }
}

// ---------------------------------------------------------------- entry
esp_err_t vpet_native_start(void)
{
    ESP_LOGI(TAG, "native vpet prototype starting");
    new_pet(1);

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
