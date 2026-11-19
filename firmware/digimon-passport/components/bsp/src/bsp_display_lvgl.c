// components/bsp/src/bsp_display_lvgl.c
// LVGL 接入单独成文件:不用 LVGL 的开发者删掉本文件 + idf_component.yml 里的两条依赖即可。
#include "bsp_display.h"
#include "bsp_display_rounding.h"
#include "bsp_pins.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bsp_lvgl";

#define BSP_LVGL_DRAW_BUFFER_LINES 40

static lv_display_t *s_disp;
static bool s_port_initialized;
static bool s_port_init_failed;

static void rounded_flush_event(lv_event_t *event)
{
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    /* Both RGB565 byte orders are fine here: this callback only ever writes 0
     * (black), which is the same word in either order.  It used to test for
     * plain RGB565 only, so switching the display to RGB565_SWAPPED would have
     * silently dropped the rounded-corner mask. */
    const lv_color_format_t cf = disp ? lv_display_get_color_format(disp)
                                      : LV_COLOR_FORMAT_UNKNOWN;
    if (!area || !draw_buf || !draw_buf->data ||
        (cf != LV_COLOR_FORMAT_RGB565 && cf != LV_COLOR_FORMAT_RGB565_SWAPPED)) {
        return;
    }

    const int32_t width = lv_area_get_width(area);
    if (draw_buf->header.stride < (uint32_t)width * sizeof(uint16_t)) return;

    for (int32_t y = area->y1; y <= area->y2; ++y) {
        uint16_t *row = (uint16_t *)(draw_buf->data +
                                     (y - area->y1) * draw_buf->header.stride);
        int32_t visible_x1;
        int32_t visible_x2;
        if (!bsp_display_rounded_row_span(y,
                                          lv_display_get_horizontal_resolution(disp),
                                          lv_display_get_vertical_resolution(disp),
                                          BSP_LVGL_SCREEN_RADIUS, &visible_x1,
                                          &visible_x2)) {
            memset(row, 0, (size_t)width * sizeof(uint16_t));
            continue;
        }
        // Only clear pixels outside the visible span. The port renders straight
        // into RGB565_SWAPPED now (no post-flush byte swap), so this runs before
        // the bytes go out; black is 0 in either byte order.
        const int32_t clear_left_end = visible_x1 > area->x2 ? area->x2 : visible_x1 - 1;
        const int32_t clear_right_start = visible_x2 < area->x1 ? area->x1 : visible_x2 + 1;
        for (int32_t x = area->x1; x <= clear_left_end; ++x) {
            row[x - area->x1] = 0;
        }
        for (int32_t x = clear_right_start; x <= area->x2; ++x) {
            row[x - area->x1] = 0;
        }
    }

}

lv_display_t *bsp_lvgl_init(void) {
    if (s_disp) return s_disp;
    if (!bsp_display_panel()) {
        ESP_LOGE(TAG, "请先成功调用 bsp_display_init()");
        return NULL;
    }

    if (!s_port_initialized) {
        // Port 2.9.0 has no public completion handshake for asynchronous deinit.
        // Never overwrite a possibly live context after a partial port failure.
        if (s_port_init_failed) {
            ESP_LOGE(TAG, "LVGL port 初始化未完成，需重启后重试");
            return NULL;
        }
        /* 20 ms tick instead of the 5 ms default: the tick timer is an esp_timer
         * that wakes the LVGL task 200 times a second, and nothing on this screen
         * needs better than the display refresh period (LV_DEF_REFR_PERIOD is
         * 20 ms too).  50 wakes/s instead of 200 is worth 1-2 mA of idle current
         * and it keeps the sleep windows long enough for light sleep later.
         * (R3 audit §4.6) */
        lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
        pc.timer_period_ms = 20;
        if (lvgl_port_init(&pc) != ESP_OK) {
            s_port_init_failed = true;
            ESP_LOGE(TAG, "lvgl_port_init 失败，需重启后重试");
            return NULL;
        }
        s_port_initialized = true;
    }

    const lvgl_port_display_cfg_t dc = {
        .panel_handle = bsp_display_panel(),
        .io_handle    = bsp_display_io(),
        // ⚠ C3 无 PSRAM,DMA 只能用内部 RAM。40 行单缓冲约 19.2KB，
        // 可减少窗口命令和队列提交次数；仍保留单缓冲，避免双缓冲挤压音频/Wi-Fi。
        .buffer_size   = (uint32_t)(BSP_LCD_ROTATED_90 ? BSP_LCD_H : BSP_LCD_W) * BSP_LVGL_DRAW_BUFFER_LINES,
        .double_buffer = false,
        .hres = BSP_LCD_ROTATED_90 ? BSP_LCD_H : BSP_LCD_W,
        .vres = BSP_LCD_ROTATED_90 ? BSP_LCD_W : BSP_LCD_H,
        // 旋转/镜像必须在这里配:esp_lvgl_port 注册显示时会重新下发 MADCTL,
        // 覆盖 bsp_display.c 里 esp_lcd_panel_mirror() 的设置。
        .rotation = { .swap_xy = BSP_LCD_ROTATED_90,
                      .mirror_x = BSP_LCD_ROTATED_90,
                      .mirror_y = false },
        // ST7789 走 SPI 要大端 RGB565。以前是让 port 在每次 flush 末尾对整个
        // 区域跑一遍 lv_draw_sw_rgb565_swap()（192x183 = 35,136 个半字，
        // ≈0.9–1.3 ms @160 MHz，纯 CPU）。改成让 LVGL 直接渲染成
        // RGB565_SWAPPED：字节序在 blend 里顺带解决，不再有那一趟拷贝。
        // 这条路径本来就在（CONFIG_LV_DRAW_SW_SUPPORT_RGB565_SWAPPED=y）。
        // (R3 audit §3.1 / §5.3)
        .color_format = LV_COLOR_FORMAT_RGB565_SWAPPED,
        .flags = { .buff_dma = true, .swap_bytes = false },
    };
    // The port mutex is recursive. Keep registration and the mask callback in
    // one critical section, before the new display can produce its first flush.
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "LVGL 初始化加锁失败");
        return NULL;
    }
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    bool mask_registered = false;
    if (disp) {
        // LVGL 9.5 returns void here; check the list while still holding the lock.
        const uint32_t count = lv_display_get_event_count(disp);
        lv_display_add_event_cb(disp, rounded_flush_event, LV_EVENT_FLUSH_START, NULL);
        mask_registered = lv_display_get_event_count(disp) == count + 1;
    }
    if (!mask_registered) {
        ESP_LOGE(TAG, "LVGL display 或圆角回调注册失败");
        if (disp) lvgl_port_remove_disp(disp);
        lvgl_port_unlock();
        // Retain the initialized port for retry. Deinit is asynchronous and can
        // race the next init (or even run before the task sets running=true).
        return NULL;
    }

    // Mask the final RGB565 flush instead of using root-screen clip_corner.
    // Full-screen rounded clipping creates an ARGB layer that does not fit the
    // 24 KB LVGL pool reliably on this no-PSRAM target.
    s_disp = disp;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL 就绪，全局圆角=%d，外部填充=黑色", BSP_LVGL_SCREEN_RADIUS);
    return s_disp;
}

bool bsp_lvgl_lock(int timeout_ms) {
    if (!s_disp) return false;
    return lvgl_port_lock(timeout_ms);
}
void bsp_lvgl_unlock(void) {
    if (s_disp) lvgl_port_unlock();
}
