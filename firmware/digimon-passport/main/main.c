// main/main.c 鈥?Digital Monster (V1) on FoloToy AI Passport
// Minimal entry point: bring up I2C, display, LVGL, audio, battery, then the emulator.

#include "esp_log.h"
#include "esp_err.h"

#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "bsp_battery.h"

static const char *TAG = "app";

esp_err_t digimon_app_start(void);

void app_main(void)
{
    ESP_LOGI(TAG, "Digital Monster on AI Passport");

    ESP_LOGI(TAG, "step 1: i2c");
    bsp_i2c_init();
    bsp_i2c_scan();
    ESP_LOGI(TAG, "step 2: display");

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed");
        return;
    }
    ESP_LOGI(TAG, "step 3: backlight");
    bsp_display_backlight(100);
    ESP_LOGI(TAG, "step 4: audio/battery");

    if (bsp_audio_init() != ESP_OK) ESP_LOGW(TAG, "audio init failed");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "battery init failed");

    ESP_LOGI(TAG, "step 5: app start");
    esp_err_t err = digimon_app_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "emulator start: %s", esp_err_to_name(err));
}

