// =============================================================================
// ui_main.c — Display + LVGL initialisation (Guition JC3636K718C)
// =============================================================================
//
// Hardware mapping (Guition JC3636K718C):
//   Display  : 360×360 round, SPI, driver GC9A01 (or compatible)
//   Touch    : I2C, CST816S (on pins defined below)
//   Backlight: GPIO 46
//   Knob     : Rotary encoder on GPIO 4 (A) + 5 (B), button on GPIO 6
//   LED ring : WS2812B on GPIO 48 (handled by a separate ws2812 component)
//
// Adjust GPIO numbers to match your exact board revision.
// =============================================================================

#include "ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_gc9a01.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "ui";

// ─── Pin definitions (Guition JC3636K718C) ───────────────────────────────────
#define LCD_HOST       SPI2_HOST
#define LCD_SCLK       12
#define LCD_MOSI       11
#define LCD_CS          9
#define LCD_DC         46
#define LCD_RST        -1    // tied to EN or via RC
#define LCD_BL         45
#define LCD_H_RES      360
#define LCD_V_RES      360

#define TOUCH_SDA      38
#define TOUCH_SCL      39
#define TOUCH_INT      -1
#define TOUCH_RST      -1
#define TOUCH_I2C_ADDR 0x15  // CST816S

#define KNOB_A_GPIO     4
#define KNOB_B_GPIO     5
#define KNOB_BTN_GPIO   6

// ─── LVGL internals ───────────────────────────────────────────────────────────
#define LVGL_TICK_PERIOD_MS  2
#define LVGL_DRAW_BUF_LINES  40

static SemaphoreHandle_t s_lvgl_mux;
static esp_lcd_panel_handle_t s_panel;
static lv_disp_t *s_disp;

// ─── LCD flush callback ───────────────────────────────────────────────────────

static bool lcd_flush_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_flush_ready(s_disp->driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(s_panel,
        area->x1, area->y1, area->x2 + 1, area->y2 + 1,
        color_map);
}

// ─── LVGL tick timer ──────────────────────────────────────────────────────────

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// ─── LVGL task ────────────────────────────────────────────────────────────────

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    while (1) {
        xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);
        uint32_t delay = lv_timer_handler();
        xSemaphoreGive(s_lvgl_mux);
        vTaskDelay(pdMS_TO_TICKS(delay > 0 ? delay : 1));
    }
}

// ─── Backlight ────────────────────────────────────────────────────────────────

static void bl_set(uint8_t percent)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                  (percent * 8191) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void bl_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_13_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    ledc_channel_config_t ch = {
        .gpio_num   = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
    bl_set(80);
}

// ─── Knob (rotary encoder + button) ──────────────────────────────────────────
// Simple polling approach via FreeRTOS task.
// For production, use GPIO interrupts + quadrature decoding.

static int  s_knob_last_a = 1;
static bool s_btn_last    = true;
static int64_t s_btn_press_us = 0;

extern void ui_knob_rotate(int delta);
extern void ui_knob_press(void);
extern void ui_knob_long_press(void);

static void knob_task(void *arg)
{
    gpio_config_t io = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << KNOB_A_GPIO) |
                        (1ULL << KNOB_B_GPIO) |
                        (1ULL << KNOB_BTN_GPIO),
    };
    gpio_config(&io);

    while (1) {
        int a   = gpio_get_level(KNOB_A_GPIO);
        int b   = gpio_get_level(KNOB_B_GPIO);
        bool btn = gpio_get_level(KNOB_BTN_GPIO);

        // Quadrature step
        if (a != s_knob_last_a) {
            s_knob_last_a = a;
            if (a == 0) {
                xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);
                ui_knob_rotate(b == 0 ? 1 : -1);
                xSemaphoreGive(s_lvgl_mux);
            }
        }

        // Button press / long press
        if (!btn && s_btn_last) {
            // Falling edge: press start
            s_btn_press_us = esp_timer_get_time();
        } else if (btn && !s_btn_last) {
            // Rising edge: press end
            int64_t held_ms = (esp_timer_get_time() - s_btn_press_us) / 1000;
            xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);
            if (held_ms > 1000) ui_knob_long_press();
            else                ui_knob_press();
            xSemaphoreGive(s_lvgl_mux);
        }
        s_btn_last = btn;

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ─── Public init ─────────────────────────────────────────────────────────────

void ui_init(void)
{
    // --- Backlight
    bl_init();

    // --- SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num   = LCD_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // --- Panel IO
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num        = LCD_CS,
        .dc_gpio_num        = LCD_DC,
        .spi_mode           = 0,
        .pclk_hz            = 40 * 1000 * 1000,
        .trans_queue_depth  = 10,
        .on_color_trans_done = lcd_flush_cb,
        .lcd_cmd_bits       = 8,
        .lcd_param_bits     = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io));

    // --- Panel (GC9A01)
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .color_space    = ESP_LCD_COLOR_SPACE_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_mirror(s_panel, true, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

    // --- LVGL
    lv_init();
    s_lvgl_mux = xSemaphoreCreateMutex();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t         buf1[LCD_H_RES * LVGL_DRAW_BUF_LINES];
    static lv_color_t         buf2[LCD_H_RES * LVGL_DRAW_BUF_LINES];
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2,
                          LCD_H_RES * LVGL_DRAW_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res   = LCD_H_RES;
    disp_drv.ver_res   = LCD_V_RES;
    disp_drv.flush_cb  = lvgl_flush_cb;
    disp_drv.draw_buf  = &draw_buf;
    s_disp = lv_disp_drv_register(&disp_drv);

    // --- LVGL tick timer
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb, .name = "lvgl_tick"
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer,
                             LVGL_TICK_PERIOD_MS * 1000);

    // --- LVGL task
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, 1);

    // --- Knob task
    xTaskCreatePinnedToCore(knob_task, "knob", 4096, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "UI initialised (%d×%d)", LCD_H_RES, LCD_V_RES);

    // Show first screen
    ui_show_setup();
}
