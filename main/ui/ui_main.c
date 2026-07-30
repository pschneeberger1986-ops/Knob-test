// =============================================================================
// ui_main.c — Display + LVGL initialisation (Guition JC3636K718C)
// =============================================================================
//
// Hardware mapping (Guition JC3636K718C) — from official manufacturer demo:
//   Display  : 360×360 round, QSPI (4-data-line SPI), driver ST77916
//   QSPI CLK : GPIO 11
//   QSPI D0  : GPIO 13
//   QSPI D1  : GPIO 14
//   QSPI D2  : GPIO 15
//   QSPI D3  : GPIO 16
//   QSPI CS  : GPIO 12
//   Display RST: GPIO 17
//   Backlight: GPIO 21 (LEDC PWM)
//   Touch    : I2C CST816, SDA=9 SCL=10 INT=7 RST=8
//   Knob     : Rotary encoder GPIO 1 (A/Left) + 2 (B/Right)
//   LED ring : WS2812B GPIO 0 (handled by ws2812 component)
// =============================================================================

#include "ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_interface.h"
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

// ─── Pin definitions ──────────────────────────────────────────────────────────
#define LCD_HOST        SPI2_HOST
#define LCD_SCLK        11
#define LCD_D0          13      // MOSI / data line 0
#define LCD_D1          14      // MISO / data line 1
#define LCD_D2          15      // WP   / data line 2
#define LCD_D3          16      // HOLD / data line 3
#define LCD_CS          12
#define LCD_RST         17
#define LCD_BL          21

#define LCD_H_RES       360
#define LCD_V_RES       360

#define TOUCH_SDA       9
#define TOUCH_SCL       10
#define TOUCH_INT       7
#define TOUCH_RST       8
#define TOUCH_I2C_NUM   I2C_NUM_0
#define TOUCH_I2C_ADDR  0x15   // CST816S

#define KNOB_A_GPIO     1      // Left / CLK
#define KNOB_B_GPIO     2      // Right / DT

// ─── LVGL internals ───────────────────────────────────────────────────────────
#define LVGL_TICK_PERIOD_MS  2
#define LVGL_DRAW_BUF_LINES  40

// ST77916 QSPI write opcode — 32-bit command = [0x02][0x00][reg_hi][reg_lo]
// Command phase is sent on single MOSI, data phase is sent on all 4 lines.
#define ST77916_CMD(reg)  ((0x02UL << 24) | ((uint32_t)(reg) & 0xFF))

static SemaphoreHandle_t    s_lvgl_mux;
static esp_lcd_panel_io_handle_t s_io;
static lv_disp_t           *s_disp;

// ─── Custom ST77916 panel (QSPI DBI) ─────────────────────────────────────────

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int x_gap;
    int y_gap;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
} st77916_panel_t;

static esp_err_t st77916_reset(esp_lcd_panel_t *panel)
{
    // Hardware reset via RST pin
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t st77916_init(esp_lcd_panel_t *panel)
{
    st77916_panel_t *p = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    // Full ST77916 init sequence (from JC3636K718_knob_EN official demo / core.yaml)
    #define CMD0(c)          esp_lcd_panel_io_tx_param(io, ST77916_CMD(c), NULL, 0)
    #define CMD1(c, d0)      do { uint8_t _d[] = {d0}; \
                                  esp_lcd_panel_io_tx_param(io, ST77916_CMD(c), _d, 1); } while(0)
    #define CMD2(c, d0, d1)  do { uint8_t _d[] = {d0, d1}; \
                                  esp_lcd_panel_io_tx_param(io, ST77916_CMD(c), _d, 2); } while(0)
    #define CMDN(c, ...)     do { uint8_t _d[] = {__VA_ARGS__}; \
                                  esp_lcd_panel_io_tx_param(io, ST77916_CMD(c), _d, sizeof(_d)); } while(0)

    CMD1(0xF0, 0x28);
    CMD1(0xF2, 0x28);
    CMD1(0x73, 0xF0);
    CMD1(0x7C, 0xD1);
    CMD1(0x83, 0xE0);
    CMD1(0x84, 0x61);
    CMD1(0xF2, 0x82);
    CMD1(0xF0, 0x00);
    CMD1(0xF0, 0x01);
    CMD1(0xF1, 0x01);
    CMD1(0xB0, 0x56);
    CMD1(0xB1, 0x4D);
    CMD1(0xB2, 0x24);
    CMD1(0xB4, 0x87);
    CMD1(0xB5, 0x44);
    CMD1(0xB6, 0x8B);
    CMD1(0xB7, 0x40);
    CMD1(0xB8, 0x86);
    CMD1(0xBA, 0x00);
    CMD1(0xBB, 0x08);
    CMD1(0xBC, 0x08);
    CMD1(0xBD, 0x00);
    CMD1(0xC0, 0x80);
    CMD1(0xC1, 0x10);
    CMD1(0xC2, 0x37);
    CMD1(0xC3, 0x80);
    CMD1(0xC4, 0x10);
    CMD1(0xC5, 0x37);
    CMD1(0xC6, 0xA9);
    CMD1(0xC7, 0x41);
    CMD1(0xC8, 0x01);
    CMD1(0xC9, 0xA9);
    CMD1(0xCA, 0x41);
    CMD1(0xCB, 0x01);
    CMD1(0xD0, 0x91);
    CMD1(0xD1, 0x68);
    CMD1(0xD2, 0x68);
    CMD2(0xF5, 0x00, 0xA5);
    CMD1(0xDD, 0x4F);
    CMD1(0xDE, 0x4F);
    CMD1(0xF1, 0x10);
    CMD1(0xF0, 0x00);
    CMD1(0xF0, 0x02);
    CMDN(0xE0, 0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34);
    CMDN(0xE1, 0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33);
    CMD1(0xF0, 0x10);
    CMD1(0xF3, 0x10);
    CMD1(0xE0, 0x07);
    CMD1(0xE1, 0x00);
    CMD1(0xE2, 0x00);
    CMD1(0xE3, 0x00);
    CMD1(0xE4, 0xE0);
    CMD1(0xE5, 0x06);
    CMD1(0xE6, 0x21);
    CMD1(0xE7, 0x01);
    CMD1(0xE8, 0x05);
    CMD1(0xE9, 0x02);
    CMD1(0xEA, 0xDA);
    CMD1(0xEB, 0x00);
    CMD1(0xEC, 0x00);
    CMD1(0xED, 0x0F);
    CMD1(0xEE, 0x00);
    CMD1(0xEF, 0x00);
    CMD1(0xF8, 0x00);
    CMD1(0xF9, 0x00);
    CMD1(0xFA, 0x00);
    CMD1(0xFB, 0x00);
    CMD1(0xFC, 0x00);
    CMD1(0xFD, 0x00);
    CMD1(0xFE, 0x00);
    CMD1(0xFF, 0x00);
    CMD1(0x60, 0x40);
    CMD1(0x61, 0x04);
    CMD1(0x62, 0x00);
    CMD1(0x63, 0x42);
    CMD1(0x64, 0xD9);
    CMD1(0x65, 0x00);
    CMD1(0x66, 0x00);
    CMD1(0x67, 0x00);
    CMD1(0x68, 0x00);
    CMD1(0x69, 0x00);
    CMD1(0x6A, 0x00);
    CMD1(0x6B, 0x00);
    CMD1(0x70, 0x40);
    CMD1(0x71, 0x03);
    CMD1(0x72, 0x00);
    CMD1(0x73, 0x42);
    CMD1(0x74, 0xD8);
    CMD1(0x75, 0x00);
    CMD1(0x76, 0x00);
    CMD1(0x77, 0x00);
    CMD1(0x78, 0x00);
    CMD1(0x79, 0x00);
    CMD1(0x7A, 0x00);
    CMD1(0x7B, 0x00);
    CMD1(0x80, 0x48);
    CMD1(0x81, 0x00);
    CMD1(0x82, 0x06);
    CMD1(0x83, 0x02);
    CMD1(0x84, 0xD6);
    CMD1(0x85, 0x04);
    CMD1(0x86, 0x00);
    CMD1(0x87, 0x00);
    CMD1(0x88, 0x48);
    CMD1(0x89, 0x00);
    CMD1(0x8A, 0x08);
    CMD1(0x8B, 0x02);
    CMD1(0x8C, 0xD8);
    CMD1(0x8D, 0x04);
    CMD1(0x8E, 0x00);
    CMD1(0x8F, 0x00);
    CMD1(0x90, 0x48);
    CMD1(0x91, 0x00);
    CMD1(0x92, 0x0A);
    CMD1(0x93, 0x02);
    CMD1(0x94, 0xDA);
    CMD1(0x95, 0x04);
    CMD1(0x96, 0x00);
    CMD1(0x97, 0x00);
    CMD1(0x98, 0x48);
    CMD1(0x99, 0x00);
    CMD1(0x9A, 0x0C);
    CMD1(0x9B, 0x02);
    CMD1(0x9C, 0xDC);
    CMD1(0x9D, 0x04);
    CMD1(0x9E, 0x00);
    CMD1(0x9F, 0x00);
    CMD1(0xA0, 0x48);
    CMD1(0xA1, 0x00);
    CMD1(0xA2, 0x05);
    CMD1(0xA3, 0x02);
    CMD1(0xA4, 0xD5);
    CMD1(0xA5, 0x04);
    CMD1(0xA6, 0x00);
    CMD1(0xA7, 0x00);
    CMD1(0xA8, 0x48);
    CMD1(0xA9, 0x00);
    CMD1(0xAA, 0x07);
    CMD1(0xAB, 0x02);
    CMD1(0xAC, 0xD7);
    CMD1(0xAD, 0x04);
    CMD1(0xAE, 0x00);
    CMD1(0xAF, 0x00);
    CMD1(0xB0, 0x48);
    CMD1(0xB1, 0x00);
    CMD1(0xB2, 0x09);
    CMD1(0xB3, 0x02);
    CMD1(0xB4, 0xD9);
    CMD1(0xB5, 0x04);
    CMD1(0xB6, 0x00);
    CMD1(0xB7, 0x00);
    CMD1(0xB8, 0x48);
    CMD1(0xB9, 0x00);
    CMD1(0xBA, 0x0B);
    CMD1(0xBB, 0x02);
    CMD1(0xBC, 0xDB);
    CMD1(0xBD, 0x04);
    CMD1(0xBE, 0x00);
    CMD1(0xBF, 0x00);
    CMD1(0xC0, 0x10);
    CMD1(0xC1, 0x47);
    CMD1(0xC2, 0x56);
    CMD1(0xC3, 0x65);
    CMD1(0xC4, 0x74);
    CMD1(0xC5, 0x88);
    CMD1(0xC6, 0x99);
    CMD1(0xC7, 0x01);
    CMD1(0xC8, 0xBB);
    CMD1(0xC9, 0xAA);
    CMD1(0xD0, 0x10);
    CMD1(0xD1, 0x47);
    CMD1(0xD2, 0x56);
    CMD1(0xD3, 0x65);
    CMD1(0xD4, 0x74);
    CMD1(0xD5, 0x88);
    CMD1(0xD6, 0x99);
    CMD1(0xD7, 0x01);
    CMD1(0xD8, 0xBB);
    CMD1(0xD9, 0xAA);
    CMD1(0xF3, 0x01);
    CMD1(0xF0, 0x00);
    CMD1(0x3A, 0x55);   // Pixel format: RGB565 (16 bpp)
    CMD1(0x21, 0x00);   // Invert colors (matches ESPHome invert_colors: true)
    CMD0(0x11);         // Sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    CMD0(0x29);         // Display on

    #undef CMD0
    #undef CMD1
    #undef CMD2
    #undef CMDN

    ESP_LOGI(TAG, "ST77916 init complete");
    return ESP_OK;
}

static esp_err_t st77916_draw_bitmap(esp_lcd_panel_t *panel,
                                     int x_start, int y_start,
                                     int x_end,   int y_end,
                                     const void *color_data)
{
    st77916_panel_t *p = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    x_start += p->x_gap;
    x_end   += p->x_gap;
    y_start += p->y_gap;
    y_end   += p->y_gap;

    // Column address set (CASET 0x2A)
    uint8_t caset[4] = {
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    };
    esp_lcd_panel_io_tx_param(io, ST77916_CMD(0x2A), caset, sizeof(caset));

    // Row address set (RASET 0x2B)
    uint8_t raset[4] = {
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    };
    esp_lcd_panel_io_tx_param(io, ST77916_CMD(0x2B), raset, sizeof(raset));

    // Memory write (RAMWR 0x2C) — pixel data sent on all 4 data lines
    size_t len = (x_end - x_start) * (y_end - y_start) * sizeof(uint16_t);
    esp_lcd_panel_io_tx_color(io, ST77916_CMD(0x2C), color_data, len);

    return ESP_OK;
}

static esp_err_t st77916_mirror(esp_lcd_panel_t *panel, bool x, bool y)
{
    st77916_panel_t *p = __containerof(panel, st77916_panel_t, base);
    p->mirror_x = x;
    p->mirror_y = y;
    // MADCTL register
    uint8_t madctl = 0x00;
    if (x) madctl |= (1 << 6);  // MX
    if (y) madctl |= (1 << 7);  // MY
    uint8_t d[1] = {madctl};
    esp_lcd_panel_io_tx_param(p->io, ST77916_CMD(0x36), d, 1);
    return ESP_OK;
}

static esp_err_t st77916_swap_xy(esp_lcd_panel_t *panel, bool swap)
{
    (void)panel; (void)swap;
    return ESP_OK;
}

static esp_err_t st77916_set_gap(esp_lcd_panel_t *panel, int x, int y)
{
    st77916_panel_t *p = __containerof(panel, st77916_panel_t, base);
    p->x_gap = x;
    p->y_gap = y;
    return ESP_OK;
}

static esp_err_t st77916_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    st77916_panel_t *p = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_tx_param(p->io, ST77916_CMD(on ? 0x29 : 0x28), NULL, 0);
    return ESP_OK;
}

static esp_err_t st77916_del(esp_lcd_panel_t *panel)
{
    st77916_panel_t *p = __containerof(panel, st77916_panel_t, base);
    free(p);
    return ESP_OK;
}

static esp_err_t new_panel_st77916(esp_lcd_panel_io_handle_t io,
                                   esp_lcd_panel_handle_t *out_panel)
{
    st77916_panel_t *p = calloc(1, sizeof(st77916_panel_t));
    if (!p) return ESP_ERR_NO_MEM;

    p->io              = io;
    p->base.reset      = st77916_reset;
    p->base.init       = st77916_init;
    p->base.del        = st77916_del;
    p->base.draw_bitmap = st77916_draw_bitmap;
    p->base.mirror     = st77916_mirror;
    p->base.swap_xy    = st77916_swap_xy;
    p->base.set_gap    = st77916_set_gap;
    p->base.disp_on_off = st77916_disp_on_off;

    *out_panel = &p->base;
    return ESP_OK;
}

// ─── LCD flush callback (DMA done → tell LVGL) ───────────────────────────────

static bool lcd_flush_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_flush_ready(s_disp->driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel,
        area->x1, area->y1, area->x2 + 1, area->y2 + 1,
        color_map);
}

// ─── LVGL tick timer ──────────────────────────────────────────────────────────

static void lvgl_tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

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

// ─── Backlight (GPIO 21, LEDC PWM) ───────────────────────────────────────────

static void bl_set(uint8_t percent)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                  (percent * 8191) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void bl_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
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

// ─── Knob (rotary encoder, GPIO 1=A/Left, GPIO 2=B/Right) ────────────────────

static int     s_knob_last_a = 1;

extern void ui_knob_rotate(int delta);
extern void ui_knob_press(void);
extern void ui_knob_long_press(void);

static void knob_task(void *arg)
{
    gpio_config_t io_cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
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
    // --- RST pin
    gpio_reset_pin(LCD_RST);
    gpio_set_direction(LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_RST, 1);

    // --- Backlight
    bl_init();

    // --- QSPI bus (4 data lines: D0-D3 on 13-16, CLK on 11)
    spi_bus_config_t buscfg = {
        .mosi_io_num     = LCD_D0,   // D0
        .miso_io_num     = LCD_D1,   // D1
        .sclk_io_num     = LCD_SCLK,
        .quadwp_io_num   = LCD_D2,   // D2/WP
        .quadhd_io_num   = LCD_D3,   // D3/HOLD
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t),
        .flags           = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // --- Panel IO (QSPI, 32-bit cmd = [0x02][0x00][reg_hi][reg_lo])
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = LCD_CS,
        .dc_gpio_num         = -1,   // No D/C pin in QSPI mode
        .spi_mode            = 0,
        .pclk_hz             = 80 * 1000 * 1000,
        .trans_queue_depth   = 10,
        .on_color_trans_done = lcd_flush_cb,
        .lcd_cmd_bits        = 32,   // [opcode(8)][0(8)][reg_hi(8)][reg_lo(8)]
        .lcd_param_bits      = 8,
        .flags = {
            .quad_mode = true,       // cmd on single line, params on 4 lines
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                              &io_cfg, &s_io));

    // --- ST77916 custom panel
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(new_panel_st77916(s_io, &panel));

    // Hardware reset, then full ST77916 init sequence
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    // --- LVGL
    lv_init();
    s_lvgl_mux = xSemaphoreCreateMutex();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[LCD_H_RES * LVGL_DRAW_BUF_LINES];
    static lv_color_t buf2[LCD_H_RES * LVGL_DRAW_BUF_LINES];
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2,
                          LCD_H_RES * LVGL_DRAW_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res   = LCD_H_RES;
    disp_drv.ver_res   = LCD_V_RES;
    disp_drv.flush_cb  = lvgl_flush_cb;
    disp_drv.draw_buf  = &draw_buf;
    disp_drv.user_data = panel;
    s_disp = lv_disp_drv_register(&disp_drv);

    // --- LVGL tick timer
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb, .name = "lvgl_tick"
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000);

    // --- Tasks
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(knob_task, "knob", 4096, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "UI initialised (%d×%d, ST77916 QSPI)", LCD_H_RES, LCD_V_RES);

    ui_show_setup();
}
