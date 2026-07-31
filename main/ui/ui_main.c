// =============================================================================
// ui_main.c — Display + LVGL (Guition JC3636K718C / ST77916 QSPI DBI)
// =============================================================================
//
// SPI protocol reverse-engineered from ESPHome qspi_dbi component source:
//
//   Init/param commands — SINGLE line SPI:
//     opcode  : 0x02  (8-bit, 1 line)
//     address : reg << 8  (24-bit, 1 line)  ← register in MIDDLE byte
//     data    : param bytes (1 line)
//
//   Pixel streaming — FULL QUAD (4-4-4):
//     opcode  : 0x32  (8-bit, 4 lines)  ← quad write opcode
//     address : 0x2C << 8 = 0x2C00  (24-bit, 4 lines)
//     data    : pixel bytes (4 lines)
//
// Hardware pins (from official JC3636K718_knob_EN manufacturer demo):
//   QSPI CLK=11  D0=13  D1=14  D2=15  D3=16  CS=12  RST=17  BL=21
//   Knob: A=GPIO1  B=GPIO2
// =============================================================================

#include "ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ui";

// ─── Pin definitions ──────────────────────────────────────────────────────────
#define LCD_HOST        SPI2_HOST
#define LCD_SCLK        11
#define LCD_D0          13
#define LCD_D1          14
#define LCD_D2          15
#define LCD_D3          16
#define LCD_CS          12
#define LCD_RST         17
#define LCD_BL          21

#define LCD_H_RES       360
#define LCD_V_RES       360

#define KNOB_A_GPIO     1
#define KNOB_B_GPIO     2

#define LVGL_TICK_PERIOD_MS  2
#define LVGL_DRAW_BUF_LINES  40

// ─── Globals ──────────────────────────────────────────────────────────────────
static SemaphoreHandle_t   s_lvgl_mux;
static spi_device_handle_t s_spi;
static lv_disp_t          *s_disp;

// ─── Low-level SPI helpers ───────────────────────────────────────────────────
//
// send_cmd: single-line SPI, opcode 0x02, address = (reg << 8)
// send_pixels: full-quad SPI, opcode 0x32, address = (0x2C << 8)

static void send_cmd(uint8_t reg, const uint8_t *data, size_t len)
{
    spi_transaction_ext_t t = {
        .command_bits = 8,
        .address_bits = 24,
        .base = {
            .cmd       = 0x02,
            .addr      = (uint32_t)reg << 8,  // register in middle byte of 24-bit addr
            .tx_buffer = data,
            .length    = len * 8,
            .flags     = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR,
            // NO multiline flags → single-line SPI
        },
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t));
}

static void send_pixels(const void *data, size_t len)
{
    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    spi_transaction_ext_t t = {
        .command_bits = 8,
        .address_bits = 24,
        .dummy_bits   = 0,
        .base = {
            .cmd       = 0x32,        // quad write opcode
            .addr      = 0x2C << 8,   // RAMWR address in middle byte
            .tx_buffer = data,
            .length    = len * 8,
            .flags     = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR
                       | SPI_TRANS_VARIABLE_DUMMY
                       | SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR
                       | SPI_TRANS_MODE_QIO,      // full 4-4-4 quad mode
        },
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t));
    spi_device_release_bus(s_spi);
}

// Convenience wrappers matching the ESPHome init_sequence format
#define C0(r)         send_cmd(r, NULL, 0)
#define C1(r,a)       do { uint8_t _d[]={a};          send_cmd(r,_d,1); } while(0)
#define C2(r,a,b)     do { uint8_t _d[]={a,b};        send_cmd(r,_d,2); } while(0)
#define CN(r,...)     do { uint8_t _d[]={__VA_ARGS__}; send_cmd(r,_d,sizeof(_d)); } while(0)

// ─── ST77916 init (verbatim from JC3636K718_knob_EN + core.yaml) ─────────────

static void st77916_init(void)
{
    // Hardware reset
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Sleep-out first (matches ESPHome setup() which sends SLEEP_OUT at t=120ms
    // before the main init sequence; we just do it immediately here)
    C0(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Main init sequence
    C1(0xF0, 0x28);
    C1(0xF2, 0x28);
    C1(0x73, 0xF0);
    C1(0x7C, 0xD1);
    C1(0x83, 0xE0);
    C1(0x84, 0x61);
    C1(0xF2, 0x82);
    C1(0xF0, 0x00);
    C1(0xF0, 0x01);
    C1(0xF1, 0x01);
    C1(0xB0, 0x56);
    C1(0xB1, 0x4D);
    C1(0xB2, 0x24);
    C1(0xB4, 0x87);
    C1(0xB5, 0x44);
    C1(0xB6, 0x8B);
    C1(0xB7, 0x40);
    C1(0xB8, 0x86);
    C1(0xBA, 0x00);
    C1(0xBB, 0x08);
    C1(0xBC, 0x08);
    C1(0xBD, 0x00);
    C1(0xC0, 0x80);
    C1(0xC1, 0x10);
    C1(0xC2, 0x37);
    C1(0xC3, 0x80);
    C1(0xC4, 0x10);
    C1(0xC5, 0x37);
    C1(0xC6, 0xA9);
    C1(0xC7, 0x41);
    C1(0xC8, 0x01);
    C1(0xC9, 0xA9);
    C1(0xCA, 0x41);
    C1(0xCB, 0x01);
    C1(0xD0, 0x91);
    C1(0xD1, 0x68);
    C1(0xD2, 0x68);
    C2(0xF5, 0x00, 0xA5);
    C1(0xDD, 0x4F);
    C1(0xDE, 0x4F);
    C1(0xF1, 0x10);
    C1(0xF0, 0x00);
    C1(0xF0, 0x02);
    CN(0xE0, 0xF0,0x0A,0x10,0x09,0x09,0x36,0x35,0x33,0x4A,0x29,0x15,0x15,0x2E,0x34);
    CN(0xE1, 0xF0,0x0A,0x0F,0x08,0x08,0x05,0x34,0x33,0x4A,0x39,0x15,0x15,0x2D,0x33);
    C1(0xF0, 0x10);
    C1(0xF3, 0x10);
    C1(0xE0, 0x07);
    C1(0xE1, 0x00);
    C1(0xE2, 0x00);
    C1(0xE3, 0x00);
    C1(0xE4, 0xE0);
    C1(0xE5, 0x06);
    C1(0xE6, 0x21);
    C1(0xE7, 0x01);
    C1(0xE8, 0x05);
    C1(0xE9, 0x02);
    C1(0xEA, 0xDA);
    C1(0xEB, 0x00);
    C1(0xEC, 0x00);
    C1(0xED, 0x0F);
    C1(0xEE, 0x00);
    C1(0xEF, 0x00);
    C1(0xF8, 0x00);
    C1(0xF9, 0x00);
    C1(0xFA, 0x00);
    C1(0xFB, 0x00);
    C1(0xFC, 0x00);
    C1(0xFD, 0x00);
    C1(0xFE, 0x00);
    C1(0xFF, 0x00);
    C1(0x60, 0x40);
    C1(0x61, 0x04);
    C1(0x62, 0x00);
    C1(0x63, 0x42);
    C1(0x64, 0xD9);
    C1(0x65, 0x00);
    C1(0x66, 0x00);
    C1(0x67, 0x00);
    C1(0x68, 0x00);
    C1(0x69, 0x00);
    C1(0x6A, 0x00);
    C1(0x6B, 0x00);
    C1(0x70, 0x40);
    C1(0x71, 0x03);
    C1(0x72, 0x00);
    C1(0x73, 0x42);
    C1(0x74, 0xD8);
    C1(0x75, 0x00);
    C1(0x76, 0x00);
    C1(0x77, 0x00);
    C1(0x78, 0x00);
    C1(0x79, 0x00);
    C1(0x7A, 0x00);
    C1(0x7B, 0x00);
    C1(0x80, 0x48);
    C1(0x81, 0x00);
    C1(0x82, 0x06);
    C1(0x83, 0x02);
    C1(0x84, 0xD6);
    C1(0x85, 0x04);
    C1(0x86, 0x00);
    C1(0x87, 0x00);
    C1(0x88, 0x48);
    C1(0x89, 0x00);
    C1(0x8A, 0x08);
    C1(0x8B, 0x02);
    C1(0x8C, 0xD8);
    C1(0x8D, 0x04);
    C1(0x8E, 0x00);
    C1(0x8F, 0x00);
    C1(0x90, 0x48);
    C1(0x91, 0x00);
    C1(0x92, 0x0A);
    C1(0x93, 0x02);
    C1(0x94, 0xDA);
    C1(0x95, 0x04);
    C1(0x96, 0x00);
    C1(0x97, 0x00);
    C1(0x98, 0x48);
    C1(0x99, 0x00);
    C1(0x9A, 0x0C);
    C1(0x9B, 0x02);
    C1(0x9C, 0xDC);
    C1(0x9D, 0x04);
    C1(0x9E, 0x00);
    C1(0x9F, 0x00);
    C1(0xA0, 0x48);
    C1(0xA1, 0x00);
    C1(0xA2, 0x05);
    C1(0xA3, 0x02);
    C1(0xA4, 0xD5);
    C1(0xA5, 0x04);
    C1(0xA6, 0x00);
    C1(0xA7, 0x00);
    C1(0xA8, 0x48);
    C1(0xA9, 0x00);
    C1(0xAA, 0x07);
    C1(0xAB, 0x02);
    C1(0xAC, 0xD7);
    C1(0xAD, 0x04);
    C1(0xAE, 0x00);
    C1(0xAF, 0x00);
    C1(0xB0, 0x48);
    C1(0xB1, 0x00);
    C1(0xB2, 0x09);
    C1(0xB3, 0x02);
    C1(0xB4, 0xD9);
    C1(0xB5, 0x04);
    C1(0xB6, 0x00);
    C1(0xB7, 0x00);
    C1(0xB8, 0x48);
    C1(0xB9, 0x00);
    C1(0xBA, 0x0B);
    C1(0xBB, 0x02);
    C1(0xBC, 0xDB);
    C1(0xBD, 0x04);
    C1(0xBE, 0x00);
    C1(0xBF, 0x00);
    C1(0xC0, 0x10);
    C1(0xC1, 0x47);
    C1(0xC2, 0x56);
    C1(0xC3, 0x65);
    C1(0xC4, 0x74);
    C1(0xC5, 0x88);
    C1(0xC6, 0x99);
    C1(0xC7, 0x01);
    C1(0xC8, 0xBB);
    C1(0xC9, 0xAA);
    C1(0xD0, 0x10);
    C1(0xD1, 0x47);
    C1(0xD2, 0x56);
    C1(0xD3, 0x65);
    C1(0xD4, 0x74);
    C1(0xD5, 0x88);
    C1(0xD6, 0x99);
    C1(0xD7, 0x01);
    C1(0xD8, 0xBB);
    C1(0xD9, 0xAA);
    C1(0xF3, 0x01);
    C1(0xF0, 0x00);

    // Pixel format, invert, display on (matches ESPHome reset_params_())
    C1(0x3A, 0x55);  // RGB565
    C0(0x21);        // Invert ON  (invert_colors: true in ESPHome)
    C0(0x29);        // Display ON

    ESP_LOGI(TAG, "ST77916 init complete");
}

// ─── Draw bitmap: RASET → CASET → RAMWR (quad pixel stream) ──────────────────
// Matches ESPHome set_addr_window_() order: RASET first, then CASET.

static void set_addr_window(int x1, int y1, int x2, int y2)
{
    // RASET — row (Y) addresses
    uint8_t raset[4] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };
    send_cmd(0x2B, raset, 4);

    // CASET — column (X) addresses
    uint8_t caset[4] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
    send_cmd(0x2A, caset, 4);
}

// ─── LVGL flush callback ──────────────────────────────────────────────────────

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_map)
{
    int x1 = area->x1, y1 = area->y1;
    int x2 = area->x2, y2 = area->y2;

    set_addr_window(x1, y1, x2, y2);

    // LVGL stores RGB565 little-endian; ST77916 expects big-endian.
    // Byte-swap each pixel in-place (safe: LVGL won't reuse buf until flush_ready).
    size_t n = (size_t)(x2 - x1 + 1) * (size_t)(y2 - y1 + 1);
    uint16_t *p = (uint16_t *)color_map;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    }

    send_pixels(color_map, n * sizeof(uint16_t));
    lv_disp_flush_ready(drv);
}

// ─── LVGL tick + task ─────────────────────────────────────────────────────────

static void lvgl_tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

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

// ─── Backlight (GPIO 21 LEDC) ─────────────────────────────────────────────────

static void bl_set(uint8_t percent)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (percent * 8191) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void bl_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    ledc_channel_config_t ch = {
        .gpio_num = LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
        .duty = 0, .hpoint = 0,
    };
    ledc_channel_config(&ch);
    bl_set(80);
}

// ─── Knob task ────────────────────────────────────────────────────────────────

static int s_knob_last_a = 1;
extern void ui_knob_rotate(int delta);
extern void ui_knob_press(void);
extern void ui_knob_long_press(void);

static void knob_task(void *arg)
{
    gpio_config_t io_cfg = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << KNOB_A_GPIO) | (1ULL << KNOB_B_GPIO),
    };
    gpio_config(&io_cfg);

    while (1) {
        int a = gpio_get_level(KNOB_A_GPIO);
        int b = gpio_get_level(KNOB_B_GPIO);
        if (a != s_knob_last_a) {
            s_knob_last_a = a;
            if (a == 0) {
                xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);
                ui_knob_rotate(b == 0 ? 1 : -1);
                xSemaphoreGive(s_lvgl_mux);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ─── Public init ─────────────────────────────────────────────────────────────

void ui_init(void)
{
    // --- RST pin (start high)
    gpio_reset_pin(LCD_RST);
    gpio_set_direction(LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_RST, 1);

    // --- Backlight
    bl_init();

    // --- QSPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num   = LCD_D0,
        .miso_io_num   = LCD_D1,
        .sclk_io_num   = LCD_SCLK,
        .quadwp_io_num = LCD_D2,
        .quadhd_io_num = LCD_D3,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // --- SPI device (variable cmd+addr length, half-duplex, 80 MHz)
    spi_device_interface_config_t devcfg = {
        .command_bits   = 0,  // overridden per-transaction
        .address_bits   = 0,  // overridden per-transaction
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = LCD_CS,
        .queue_size     = 4,
        .flags          = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &s_spi));

    // --- ST77916 init
    st77916_init();

    // --- LVGL
    lv_init();
    s_lvgl_mux = xSemaphoreCreateMutex();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[LCD_H_RES * LVGL_DRAW_BUF_LINES];
    static lv_color_t buf2[LCD_H_RES * LVGL_DRAW_BUF_LINES];
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * LVGL_DRAW_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = LCD_H_RES;
    disp_drv.ver_res  = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    s_disp = lv_disp_drv_register(&disp_drv);

    // --- LVGL tick timer
    const esp_timer_create_args_t tick_args = { .callback = lvgl_tick_cb, .name = "lvgl_tick" };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000);

    // --- Tasks
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(knob_task, "knob", 4096, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "UI ready (%dx%d, ST77916)", LCD_H_RES, LCD_V_RES);
    ui_show_setup();
}
