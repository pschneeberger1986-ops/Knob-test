// =============================================================================
// ui_home.c — Carousel UI: Coffee Boiler / Steam / Temperature / Backflush
// =============================================================================
//
// Round 360×360 display layout (per carousel page):
//
//   ─────── gold ring (332 px circle) ───────
//   ┌───────────────────────────────────────┐
//   │       ───────────────  ·              │  pixel 108  separator + cloud dot
//   │          LA MARZOCCO                  │  pixel 124  brand (CENTER y=-56)
//   │                                       │
//   │               On                      │  pixel 180  big value (CENTER y=0)
//   │           Heating...                  │  pixel 202  status (CENTER y=22)
//   │                                       │
//   │            93.0 °C                    │  pixel 240  sub-label (CENTER y=60)
//   │                                       │
//   │            ● ○ ○ ○                   │  pixel 300  dots (CENTER y=120)
//   └───────────────────────────────────────┘
//
// Navigation:
//   Rotate CW/CCW  → next / prev page  (or adjust value when in edit mode)
//   Short press    → page action (toggle / confirm temp / advance backflush)
//   Long press     → open / close settings
//
// Overlays (drawn above all pages):
//   Brewing  → "EXTRACTION" + MM:SS timer
//   Water    → "EMPTY TANK" warning
//   Settings → full-screen settings panel with amber header
// =============================================================================

#include "ui.h"
#include "cloud_lm.h"
#include "storage.h"
#include "lvgl.h"
#include "ui_fonts.h"
#include "lv_img_logo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "ui_home";

extern SemaphoreHandle_t s_lvgl_mux;

// ─── Constants ───────────────────────────────────────────────────────────────

#define PAGE_COUNT  4
#define PAGE_COFFEE 0
#define PAGE_STEAM  1
#define PAGE_TEMP   2
#define PAGE_BF     3

#define TEMP_MIN   85.0f
#define TEMP_MAX  105.0f
#define TEMP_STEP   0.5f

// Colour palette (aligned to coffee.yaml)
#define COL_GOLD_DARK  0x8A5A27   // brand, separator, ON state, selected header bg
#define COL_GOLD_MID   0xC07830   // mid accent
#define COL_GOLD_LIGHT 0xE8A25D   // active dot, "On" value text
#define COL_AMBER_SEL  0xF5B83D   // settings selected pill background
#define COL_OFF        0x6B5A47   // Standby text, inactive items
#define COL_DIM        0x3A2A18   // hint text, very dim
#define COL_DIM2       0x555555   // unselected settings items
#define COL_WHITE      0xFFFFFF
#define COL_GRAY       0x888888
#define COL_BLUE       0x1E90FF   // water empty warning
#define COL_AMBER      0xE8A040   // heating status
#define COL_BG         0x000000

#define SET_ROWS_MAX 12

// ─── UI state ────────────────────────────────────────────────────────────────

typedef enum {
    UI_NORMAL,
    UI_SETTINGS_MENU,      // Display / Pre-brewing / System
    UI_SETTINGS_DISPLAY,   // brightness + auto-off
    UI_SETTINGS_PREBREW,   // enable toggle + on-time + off-time
    UI_SETTINGS_SYSTEM,    // Info / Units / Name
    UI_SETTINGS_UNITS,     // °C / °F
    UI_SETTINGS_NAME,      // LA MARZOCCO ↔ friendly name
} ui_state_t;

typedef enum {
    BF_IDLE,
    BF_READY,
    BF_PADDLE,
    BF_RUNNING,
    BF_DONE,
} bf_state_t;

static lm_machine_state_t s_state    = {};
static int        s_page             = PAGE_COFFEE;
static ui_state_t s_ui_state         = UI_NORMAL;
static bf_state_t s_bf_state         = BF_IDLE;

// Preferences
static bool  s_fahrenheit            = false;
static int   s_brightness            = 80;
static int   s_auto_off              = 5;
static float s_pb_on                 = 3.0f;
static float s_pb_off                = 1.0f;
static bool  s_pb_active             = false;   // RAM-only, not persisted
static bool  s_show_friendly         = false;
static char  s_friendly_name[64]     = {};

// Temperature edit
static float s_edit_temp             = 93.0f;
static bool  s_temp_editing          = false;

// Settings navigation
// s_pb_sel: 0=pill toggle, 1=on_time, 2=off_time
static int s_menu_sel  = 0;
static int s_sys_sel   = 0;
static int s_disp_sel  = 0;
static int s_pb_sel    = 0;

// Brewing timer
static bool       s_brewing          = false;
static uint32_t   s_brew_secs        = 0;
static lv_timer_t *s_brew_timer      = NULL;

// ─── Widget handles ──────────────────────────────────────────────────────────

static lv_obj_t *s_scr              = NULL;

// Pages
static lv_obj_t *s_pages[PAGE_COUNT];
static lv_obj_t *s_brand_lbl[PAGE_COUNT];

// Coffee page
static lv_obj_t *s_coffee_val;      // "On" / "Standby"   CENTER y=0
static lv_obj_t *s_coffee_sub;      // "93.0 °C"           CENTER y=60

// Steam page
static lv_obj_t *s_steam_val;       // "On" / "Standby"   CENTER y=0
static lv_obj_t *s_steam_sub;       // "STEAM"             CENTER y=60

// Temperature page
static lv_obj_t *s_temp_val;        // "93.0 °C"           CENTER y=0
static lv_obj_t *s_temp_sub;        // "TEMPERATURE" / edit hint  CENTER y=60

// Backflush page
static lv_obj_t *s_bf_hint;         // action text          CENTER y=0

// Shared
static lv_obj_t *s_dots[PAGE_COUNT];
static lv_obj_t *s_cloud_dot;

// Overlays
static lv_obj_t *s_brew_ov         = NULL;
static lv_obj_t *s_brew_time       = NULL;
static lv_obj_t *s_water_ov        = NULL;
static lv_obj_t *s_set_panel       = NULL;
static lv_obj_t *s_set_header      = NULL;   // amber header bar (permanent)
static lv_obj_t *s_set_title       = NULL;   // label inside header (permanent)
static lv_obj_t *s_set_row[SET_ROWS_MAX] = {};
static int       s_set_rows        = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static float c_to_f(float c) { return c * 9.0f / 5.0f + 32.0f; }

static void fmt_temp(char *buf, size_t sz, float c)
{
    if (s_fahrenheit) {
        float f = c_to_f(c);
        f = roundf(f * 2.0f) / 2.0f;
        snprintf(buf, sz, "%.1f \xc2\xb0""F", f);
    } else {
        snprintf(buf, sz, "%.1f \xc2\xb0""C", c);
    }
}

static void save_prefs(void)
{
    lm_prefs_t p = {};
    storage_load_prefs(&p);
    p.use_fahrenheit     = s_fahrenheit;
    p.brightness         = s_brightness;
    p.auto_off_min       = s_auto_off;
    p.pb_on_s            = s_pb_on;
    p.pb_off_s           = s_pb_off;
    p.show_friendly_name = s_show_friendly;
    storage_save_prefs(&p);
}

static void refresh_brand_labels(void)
{
    const char *text;
    if (s_show_friendly && s_friendly_name[0] != '\0')
        text = s_friendly_name;
    else
        text = "LA MARZOCCO";
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_brand_lbl[i]) lv_label_set_text(s_brand_lbl[i], text);
    }
}

// ─── Background cloud command tasks ──────────────────────────────────────────

typedef struct { bool on; }   t_power;
typedef struct { float temp; } t_temp;
typedef struct { int level; } t_steam;

static void cmd_power(void *a)
{ t_power *p=a; if(lm_cloud_set_power(p->on)!=ESP_OK) ESP_LOGE(TAG,"set_power failed"); free(a); vTaskDelete(NULL); }
static void cmd_temp(void *a)
{ t_temp *p=a; if(lm_cloud_set_coffee_temp(p->temp)!=ESP_OK) ESP_LOGE(TAG,"set_temp failed"); free(a); vTaskDelete(NULL); }
static void cmd_steam(void *a)
{ t_steam *p=a; if(lm_cloud_set_steam_level(p->level)!=ESP_OK) ESP_LOGE(TAG,"set_steam failed"); free(a); vTaskDelete(NULL); }

static void dispatch_power(bool on)
{ t_power *a=malloc(sizeof(*a)); if(a){a->on=on; xTaskCreate(cmd_power,"cmd_pwr",4096,a,2,NULL);} }
static void dispatch_temp(float t)
{ t_temp *a=malloc(sizeof(*a)); if(a){a->temp=t; xTaskCreate(cmd_temp,"cmd_tmp",4096,a,2,NULL);} }
static void dispatch_steam(int lv)
{ t_steam *a=malloc(sizeof(*a)); if(a){a->level=lv; xTaskCreate(cmd_steam,"cmd_stm",4096,a,2,NULL);} }

// ─── Pagination dots ─────────────────────────────────────────────────────────

static void refresh_dots(void)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_set_style_bg_color(s_dots[i],
            i == s_page ? lv_color_hex(COL_GOLD_DARK) : lv_color_hex(0x2A1A0A), 0);
    }
}

// ─── Page helpers ────────────────────────────────────────────────────────────

static void show_page(int idx)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == idx) lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_page = idx;
    refresh_dots();
}

// ─── Page content refresh ────────────────────────────────────────────────────

static void refresh_coffee(void)
{
    bool on = s_state.powered_on;
    lv_label_set_text(s_coffee_val, on ? "On" : "Standby");
    lv_obj_set_style_text_color(s_coffee_val,
        lv_color_hex(on ? COL_GOLD_LIGHT : COL_OFF), 0);

    // Sub: target temperature
    char buf[24];
    float t = s_state.coffee_temp_target > 0 ? s_state.coffee_temp_target : s_edit_temp;
    fmt_temp(buf, sizeof(buf), t);
    lv_label_set_text(s_coffee_sub, buf);
    lv_obj_set_style_text_color(s_coffee_sub, lv_color_hex(COL_GOLD_DARK), 0);
}

static void refresh_steam(void)
{
    bool on = (s_state.steam_level > 0);
    lv_label_set_text(s_steam_val, on ? "On" : "Standby");
    lv_obj_set_style_text_color(s_steam_val,
        lv_color_hex(on ? COL_GOLD_LIGHT : COL_OFF), 0);
}

static void refresh_temp(void)
{
    char buf[24];
    float display_c = s_temp_editing ? s_edit_temp : s_state.coffee_temp_target;
    if (display_c < TEMP_MIN) display_c = s_edit_temp;
    fmt_temp(buf, sizeof(buf), display_c);
    lv_label_set_text(s_temp_val, buf);

    // Sub-label always shows "TEMPERATURE" in COL_GOLD_DARK
    lv_label_set_text(s_temp_sub, "TEMPERATURE");
    lv_obj_set_style_text_color(s_temp_sub, lv_color_hex(COL_GOLD_DARK), 0);
}

static const char *bf_hint_str(void)
{
    switch (s_bf_state) {
    case BF_IDLE:    return "Tap to start";
    case BF_READY:   return "Tap to start";
    case BF_PADDLE:  return "Push the paddle";
    case BF_RUNNING: return "Running...";
    case BF_DONE:    return "Tap to close";
    default:         return "";
    }
}

static void refresh_bf(void)
{
    lv_label_set_text(s_bf_hint, bf_hint_str());
    lv_color_t col = (s_bf_state == BF_RUNNING)
        ? lv_color_hex(COL_AMBER)
        : lv_color_hex(COL_GOLD_LIGHT);
    lv_obj_set_style_text_color(s_bf_hint, col, 0);
}

// ─── Brew timer ──────────────────────────────────────────────────────────────

static void brew_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_brew_secs++;
    if (!s_brew_time) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)(s_brew_secs / 60),
             (unsigned long)(s_brew_secs % 60));
    lv_label_set_text(s_brew_time, buf);
}

// ─── Brewing overlay visibility ───────────────────────────────────────────────

static void show_brew_overlay(bool show)
{
    if (!s_brew_ov) return;
    if (show) {
        lv_obj_clear_flag(s_brew_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_brew_ov);
        if (!s_brew_timer) {
            s_brew_secs  = 0;
            s_brew_timer = lv_timer_create(brew_timer_cb, 1000, NULL);
        }
    } else {
        lv_obj_add_flag(s_brew_ov, LV_OBJ_FLAG_HIDDEN);
        if (s_brew_timer) { lv_timer_del(s_brew_timer); s_brew_timer = NULL; }
        if (s_brew_time) lv_label_set_text(s_brew_time, "00:00");
        s_brew_secs = 0;
    }
}

// ─── Settings panel ──────────────────────────────────────────────────────────

static void settings_rebuild(void);

static void settings_open(void)
{
    if (!s_set_panel) return;
    s_menu_sel = 0;
    s_ui_state = UI_SETTINGS_MENU;
    lv_obj_clear_flag(s_set_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_set_panel);
    settings_rebuild();
}

static void settings_close(void)
{
    if (!s_set_panel) return;
    s_ui_state = UI_NORMAL;
    lv_obj_add_flag(s_set_panel, LV_OBJ_FLAG_HIDDEN);
}

// ─── Settings rebuild helpers ─────────────────────────────────────────────────

// Add a menu item: amber pill if selected, plain dim text if not.
// Returns the created widget handle.
static lv_obj_t *add_set_item(const char *text, bool selected, int cy)
{
    if (selected) {
        lv_obj_t *pill = lv_obj_create(s_set_panel);
        lv_obj_set_size(pill, 220, 44);
        lv_obj_set_style_radius(pill, 22, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(COL_GOLD_DARK), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pill, 0, 0);
        lv_obj_set_style_pad_all(pill, 0, 0);
        lv_obj_align(pill, LV_ALIGN_CENTER, 0, cy);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(pill);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_roboto_16, 0);
        lv_obj_center(lbl);
        return pill;
    } else {
        lv_obj_t *lbl = lv_label_create(s_set_panel);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_DIM2), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_roboto_16, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, cy);
        return lbl;
    }
}

// Add an adjust row container with label + −/value/+ at cy.
// Returns the container (tracked for deletion).
static lv_obj_t *add_adj_row(const char *row_lbl, const char *val_str,
                               bool selected, int cy)
{
    /* Flat layout — no border box, no background highlight */
    lv_obj_t *cont = lv_obj_create(s_set_panel);
    lv_obj_set_size(cont, 280, 72);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, cy);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Row label (top, roboto_16)
    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, row_lbl);
    lv_obj_set_style_text_color(lbl,
        lv_color_hex(COL_GOLD_DARK), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_roboto_16, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -18);

    // Minus (keep montserrat_28)
    lv_obj_t *minus = lv_label_create(cont);
    lv_label_set_text(minus, "-");
    lv_obj_set_style_text_font(minus, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(minus, lv_color_hex(COL_GOLD_DARK), 0);
    lv_obj_align(minus, LV_ALIGN_LEFT_MID, 18, 14);

    // Value (roboto_28)
    lv_obj_t *val = lv_label_create(cont);
    lv_label_set_text(val, val_str);
    lv_obj_set_style_text_font(val, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(COL_GOLD_DARK), 0);
    lv_obj_align(val, LV_ALIGN_CENTER, 0, 14);

    // Plus
    lv_obj_t *plus = lv_label_create(cont);
    lv_label_set_text(plus, "+");
    lv_obj_set_style_text_font(plus, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(plus, lv_color_hex(COL_GOLD_DARK), 0);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -18, 14);

    return cont;
}

// Rebuild the visible content of the settings panel for current state
static void settings_rebuild(void)
{
    if (!s_set_panel) return;

    // Clear existing dynamic rows
    for (int i = 0; i < s_set_rows; i++) {
        if (s_set_row[i]) { lv_obj_del(s_set_row[i]); s_set_row[i] = NULL; }
    }
    s_set_rows = 0;

    // Update header title
    char title_buf[32];

    switch (s_ui_state) {
    case UI_SETTINGS_MENU:      strcpy(title_buf, "SETTINGS");    break;
    case UI_SETTINGS_DISPLAY:   strcpy(title_buf, "DISPLAY");     break;
    case UI_SETTINGS_PREBREW:   strcpy(title_buf, "PRE-BREWING"); break;
    case UI_SETTINGS_SYSTEM:    strcpy(title_buf, "SYSTEM");      break;
    case UI_SETTINGS_UNITS:     strcpy(title_buf, "UNITS");       break;
    case UI_SETTINGS_NAME:      strcpy(title_buf, "BRAND NAME");  break;
    default:                    strcpy(title_buf, "");            break;
    }
    if (s_set_title) lv_label_set_text(s_set_title, title_buf);

#define TRACK(w) do { if (s_set_rows < SET_ROWS_MAX) { s_set_row[s_set_rows++] = (w); } } while(0)

    switch (s_ui_state) {

    // ── Main menu: Display / Pre-brewing / System ────────────────────────────
    case UI_SETTINGS_MENU:
        TRACK(add_set_item("Display",       s_menu_sel == 0, -60));
        TRACK(add_set_item("Pre-brewing",   s_menu_sel == 1,   0));
        TRACK(add_set_item("System",        s_menu_sel == 2,  60));
        break;

    // ── Display sub-menu: Brightness / Auto-off ──────────────────────────────
    case UI_SETTINGS_DISPLAY: {
        static char db0[24], db1[24];
        snprintf(db0, sizeof(db0), "Brightness: %d%%", s_brightness);
        snprintf(db1, sizeof(db1), "Auto-off: %d min", s_auto_off);
        TRACK(add_set_item(db0, s_disp_sel == 0, -30));
        TRACK(add_set_item(db1, s_disp_sel == 1,  30));

        break;
    }

    // ── Pre-brewing: ON/OFF pill + on_time row + off_time row ───────────────
    case UI_SETTINGS_PREBREW: {
        // ON pill
        lv_obj_t *on_pill = lv_obj_create(s_set_panel);
        lv_obj_set_size(on_pill, 120, 44);
        lv_obj_set_style_radius(on_pill, 22, 0);
        lv_obj_set_style_bg_color(on_pill,
            lv_color_hex(s_pb_active ? COL_GOLD_DARK : 0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(on_pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(on_pill, s_pb_active ? 0 : 1, 0);
        lv_obj_set_style_border_color(on_pill, lv_color_hex(COL_GOLD_DARK), 0);
        lv_obj_set_style_pad_all(on_pill, 0, 0);
        // CENTER y=-90 (pill toggle area)
        lv_obj_align(on_pill, LV_ALIGN_CENTER, -68, -90);
        lv_obj_clear_flag(on_pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *on_lbl = lv_label_create(on_pill);
        lv_label_set_text(on_lbl, "ON");
        lv_obj_set_style_text_color(on_lbl,
            lv_color_hex(s_pb_active ? 0x000000 : COL_GOLD_DARK), 0);
        lv_obj_set_style_text_font(on_lbl, &lv_font_roboto_16, 0);
        lv_obj_center(on_lbl);
        // Highlight ring if currently focused
        if (s_pb_sel == 0) {
            lv_obj_set_style_border_width(on_pill, 2, 0);
            lv_obj_set_style_border_color(on_pill, lv_color_hex(COL_GOLD_DARK), 0);
        }
        TRACK(on_pill);

        // OFF pill
        lv_obj_t *off_pill = lv_obj_create(s_set_panel);
        lv_obj_set_size(off_pill, 120, 44);
        lv_obj_set_style_radius(off_pill, 22, 0);
        lv_obj_set_style_bg_color(off_pill,
            lv_color_hex(!s_pb_active ? COL_OFF : 0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(off_pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(off_pill, 0, 0);
        lv_obj_set_style_pad_all(off_pill, 0, 0);
        lv_obj_align(off_pill, LV_ALIGN_CENTER, 68, -90);
        lv_obj_clear_flag(off_pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *off_lbl = lv_label_create(off_pill);
        lv_label_set_text(off_lbl, "OFF");
        lv_obj_set_style_text_color(off_lbl,
            lv_color_hex(!s_pb_active ? 0x000000 : COL_OFF), 0);
        lv_obj_set_style_text_font(off_lbl, &lv_font_roboto_16, 0);
        lv_obj_center(off_lbl);
        TRACK(off_pill);

        // On-time row
        static char pb0[16];
        snprintf(pb0, sizeof(pb0), "%.1fs", s_pb_on);
        TRACK(add_adj_row("ON TIME", pb0, s_pb_sel == 1, -20));

        // Off-time row
        static char pb1[16];
        snprintf(pb1, sizeof(pb1), "%.1fs", s_pb_off);
        TRACK(add_adj_row("OFF TIME", pb1, s_pb_sel == 2, 68));

        break;
    }

    // ── System sub-menu ───────────────────────────────────────────────────────
    case UI_SETTINGS_SYSTEM:
        TRACK(add_set_item("Info",    s_sys_sel == 0, -45));
        TRACK(add_set_item("Units",   s_sys_sel == 1,   0));
        TRACK(add_set_item("Name",    s_sys_sel == 2,  45));
        break;

    // ── Units ─────────────────────────────────────────────────────────────────
    case UI_SETTINGS_UNITS: {
        // Large °C and °F labels side by side
        lv_obj_t *r0 = lv_label_create(s_set_panel);
        lv_label_set_text(r0, "\xc2\xb0""C");
        lv_obj_set_style_text_font(r0, &lv_font_roboto_42, 0);
        lv_obj_set_style_text_color(r0,
            lv_color_hex(!s_fahrenheit ? COL_GOLD_LIGHT : COL_OFF), 0);
        lv_obj_align(r0, LV_ALIGN_CENTER, -60, 10);
        TRACK(r0);

        lv_obj_t *r1 = lv_label_create(s_set_panel);
        lv_label_set_text(r1, "\xc2\xb0""F");
        lv_obj_set_style_text_font(r1, &lv_font_roboto_42, 0);
        lv_obj_set_style_text_color(r1,
            lv_color_hex(s_fahrenheit ? COL_GOLD_LIGHT : COL_OFF), 0);
        lv_obj_align(r1, LV_ALIGN_CENTER, 60, 10);
        TRACK(r1);
        break;
    }

    // ── Name ──────────────────────────────────────────────────────────────────
    case UI_SETTINGS_NAME: {
        const char *nm = (s_friendly_name[0] != '\0') ? s_friendly_name : "(no name)";
        TRACK(add_set_item("LA MARZOCCO", !s_show_friendly, -30));
        TRACK(add_set_item(nm,             s_show_friendly,  30));
        break;
    }

    default:
        break;
    }

#undef TRACK
}

// ─── Build helpers ────────────────────────────────────────────────────────────

static lv_obj_t *make_page_base(int idx)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_set_size(p, 360, 360);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    s_pages[idx] = p;
    return p;
}

static void add_ring(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, 332, 332);
    lv_obj_center(r);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(COL_GOLD_DARK), 0);
    lv_obj_set_style_border_width(r, 3, 0);
    lv_obj_set_style_border_opa(r, LV_OPA_80, 0);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

// Logo at CENTER y=-113, separator at CENTER y=-72, brand at CENTER y=-56
static void add_brand(lv_obj_t *parent, int page_idx)
{
    // Logo image: 130×74px, center at pixel (180, 67) = CENTER y=-113
    lv_obj_t *logo_img = lv_img_create(parent);
    lv_img_set_src(logo_img, &lv_img_logo);
    // Center horizontally, center vertically at pixel 67 → CENTER y=-113
    lv_obj_align(logo_img, LV_ALIGN_CENTER, 0, -113);
    lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Separator line (CENTER y=-72 → pixel 108)
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_set_size(sep, 103, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(COL_GOLD_DARK), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_60, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(sep, LV_ALIGN_CENTER, 0, -72);

    // Brand text (CENTER y=-56 → pixel 124)
    const char *brand_text = (s_show_friendly && s_friendly_name[0] != '\0')
                             ? s_friendly_name : "LA MARZOCCO";
    lv_obj_t *brand = lv_label_create(parent);
    lv_label_set_text(brand, brand_text);
    lv_obj_set_style_text_color(brand, lv_color_hex(COL_GOLD_DARK), 0);
    lv_obj_set_style_text_font(brand, &lv_font_georgia_15, 0);
    lv_obj_set_style_text_letter_space(brand, 4, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -56);
    s_brand_lbl[page_idx] = brand;
}

// ─── Build carousel pages ────────────────────────────────────────────────────

static void build_pages(void)
{
    // ── PAGE 0: Coffee Boiler ────────────────────────────────────────────────
    {
        lv_obj_t *p = make_page_base(PAGE_COFFEE);
        add_ring(p);
        add_brand(p, PAGE_COFFEE);

        // Main value: On / Standby  (CENTER y=0, roboto_42)
        s_coffee_val = lv_label_create(p);
        lv_label_set_text(s_coffee_val, "Standby");
        lv_obj_set_style_text_font(s_coffee_val, &lv_font_roboto_42, 0);
        lv_obj_set_style_text_color(s_coffee_val, lv_color_hex(COL_OFF), 0);
        lv_obj_align(s_coffee_val, LV_ALIGN_CENTER, 0, 0);

        // Sub: target temp  (CENTER y=60, roboto_30)
        s_coffee_sub = lv_label_create(p);
        lv_label_set_text(s_coffee_sub, "--\xc2\xb0""C");
        lv_obj_set_style_text_font(s_coffee_sub, &lv_font_roboto_30, 0);
        lv_obj_set_style_text_color(s_coffee_sub, lv_color_hex(COL_GOLD_DARK), 0);
        lv_obj_set_style_text_letter_space(s_coffee_sub, 2, 0);
        lv_obj_align(s_coffee_sub, LV_ALIGN_CENTER, 0, 60);
    }

    // ── PAGE 1: Steam Boiler ─────────────────────────────────────────────────
    {
        lv_obj_t *p = make_page_base(PAGE_STEAM);
        add_ring(p);
        add_brand(p, PAGE_STEAM);

        // Main value (CENTER y=0, roboto_42)
        s_steam_val = lv_label_create(p);
        lv_label_set_text(s_steam_val, "Standby");
        lv_obj_set_style_text_font(s_steam_val, &lv_font_roboto_42, 0);
        lv_obj_set_style_text_color(s_steam_val, lv_color_hex(COL_OFF), 0);
        lv_obj_align(s_steam_val, LV_ALIGN_CENTER, 0, 0);

        // Sub: "STEAM" label (CENTER y=60, roboto_20)
        s_steam_sub = lv_label_create(p);
        lv_label_set_text(s_steam_sub, "STEAM");
        lv_obj_set_style_text_font(s_steam_sub, &lv_font_roboto_20, 0);
        lv_obj_set_style_text_color(s_steam_sub, lv_color_hex(COL_GOLD_DARK), 0);
        lv_obj_set_style_text_letter_space(s_steam_sub, 2, 0);
        lv_obj_align(s_steam_sub, LV_ALIGN_CENTER, 0, 60);
    }

    // ── PAGE 2: Temperature ───────────────────────────────────────────────────
    {
        lv_obj_t *p = make_page_base(PAGE_TEMP);
        add_ring(p);
        add_brand(p, PAGE_TEMP);

        // Main value: temp (CENTER y=0, roboto_42)
        s_temp_val = lv_label_create(p);
        lv_label_set_text(s_temp_val, "93.0 \xc2\xb0""C");
        lv_obj_set_style_text_font(s_temp_val, &lv_font_roboto_42, 0);
        lv_obj_set_style_text_color(s_temp_val, lv_color_hex(COL_GOLD_LIGHT), 0);
        lv_obj_align(s_temp_val, LV_ALIGN_CENTER, 0, 0);

        // Sub: "TEMPERATURE" (CENTER y=60, roboto_20)
        s_temp_sub = lv_label_create(p);
        lv_label_set_text(s_temp_sub, "TEMPERATURE");
        lv_obj_set_style_text_font(s_temp_sub, &lv_font_roboto_20, 0);
        lv_obj_set_style_text_color(s_temp_sub, lv_color_hex(COL_GOLD_DARK), 0);
        lv_obj_set_style_text_letter_space(s_temp_sub, 2, 0);
        lv_obj_align(s_temp_sub, LV_ALIGN_CENTER, 0, 60);
    }

    // ── PAGE 3: Backflush ────────────────────────────────────────────────────
    {
        lv_obj_t *p = make_page_base(PAGE_BF);
        add_ring(p);
        add_brand(p, PAGE_BF);

        // Main content: action hint (CENTER y=0, roboto_20)
        s_bf_hint = lv_label_create(p);
        lv_label_set_text(s_bf_hint, "Tap to start");
        lv_obj_set_style_text_font(s_bf_hint, &lv_font_roboto_20, 0);
        lv_obj_set_style_text_color(s_bf_hint, lv_color_hex(COL_GOLD_LIGHT), 0);
        lv_obj_align(s_bf_hint, LV_ALIGN_CENTER, 0, 0);

        // Sub: "BACKFLUSH" (CENTER y=60)
        lv_obj_t *bf_sub = lv_label_create(p);
        lv_label_set_text(bf_sub, "BACKFLUSH");
        lv_obj_set_style_text_font(bf_sub, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(bf_sub, lv_color_hex(COL_GOLD_DARK), 0);
        lv_obj_set_style_text_letter_space(bf_sub, 2, 0);
        lv_obj_align(bf_sub, LV_ALIGN_CENTER, 0, 60);
    }
}

// ─── Build pagination dots ────────────────────────────────────────────────────

static void build_dots(void)
{
    // Container for dots row (CENTER y=120 → pixel 300)
    lv_obj_t *row = lv_obj_create(s_scr);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 120);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(row);
        lv_obj_set_size(d, 7, 7);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(0x2A1A0A), 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_dots[i] = d;
    }
}

// ─── Cloud dot ───────────────────────────────────────────────────────────────

static void build_cloud_dot(void)
{
    // Cloud connectivity dot at CENTER x=90, y=-56 (near brand text, top-right)
    s_cloud_dot = lv_obj_create(s_scr);
    lv_obj_set_size(s_cloud_dot, 6, 6);
    lv_obj_set_style_radius(s_cloud_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_cloud_dot, 0, 0);
    lv_obj_set_style_bg_color(s_cloud_dot, lv_color_hex(0xFF2222), 0);
    lv_obj_align(s_cloud_dot, LV_ALIGN_CENTER, 90, -56);
    lv_obj_clear_flag(s_cloud_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cloud_dot, LV_OBJ_FLAG_HIDDEN); /* hidden until first connection update */
}

// ─── Build brewing overlay ────────────────────────────────────────────────────

static void build_brew_overlay(void)
{
    s_brew_ov = lv_obj_create(s_scr);
    lv_obj_set_size(s_brew_ov, 360, 360);
    lv_obj_set_pos(s_brew_ov, 0, 0);
    lv_obj_set_style_bg_color(s_brew_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_brew_ov, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_brew_ov, 0, 0);
    lv_obj_set_style_radius(s_brew_ov, 0, 0);
    lv_obj_clear_flag(s_brew_ov, LV_OBJ_FLAG_SCROLLABLE);

    // "BREWING" label (CENTER y=-60, roboto_22)
    lv_obj_t *title = lv_label_create(s_brew_ov);
    lv_label_set_text(title, "BREWING");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_roboto_22, 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    // Timer (CENTER y=0, roboto_56)
    s_brew_time = lv_label_create(s_brew_ov);
    lv_label_set_text(s_brew_time, "00:00");
    lv_obj_set_style_text_color(s_brew_time, lv_color_hex(COL_GOLD_LIGHT), 0);
    lv_obj_set_style_text_font(s_brew_time, &lv_font_roboto_56, 0);
    lv_obj_align(s_brew_time, LV_ALIGN_CENTER, 0, 0);

    // Hint (CENTER y=55, roboto_16)
    lv_obj_t *hint = lv_label_create(s_brew_ov);
    lv_label_set_text(hint, "Tap to dismiss");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(hint, &lv_font_roboto_16, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 55);

    lv_obj_add_flag(s_brew_ov, LV_OBJ_FLAG_HIDDEN);
}

// ─── Build water empty overlay ────────────────────────────────────────────────

static void build_water_overlay(void)
{
    s_water_ov = lv_obj_create(s_scr);
    lv_obj_set_size(s_water_ov, 360, 360);
    lv_obj_set_pos(s_water_ov, 0, 0);
    lv_obj_set_style_bg_color(s_water_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_water_ov, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_water_ov, 0, 0);
    lv_obj_set_style_radius(s_water_ov, 0, 0);
    lv_obj_clear_flag(s_water_ov, LV_OBJ_FLAG_SCROLLABLE);

    // "EMPTY TANK" text (CENTER y=-30)
    lv_obj_t *txt = lv_label_create(s_water_ov);
    lv_label_set_text(txt, "EMPTY TANK");
    lv_obj_set_style_text_color(txt, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font(txt, &lv_font_roboto_16, 0);
    lv_obj_set_style_text_letter_space(txt, 3, 0);
    lv_obj_align(txt, LV_ALIGN_CENTER, 0, -30);

    // Water drop icon using UTF-8 emoji (CENTER y=30)
    lv_obj_t *icon = lv_label_create(s_water_ov);
    lv_label_set_text(icon, "\xf0\x9f\x92\xa7");  // 💧 emoji
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_BLUE), 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, 30);

    lv_obj_add_flag(s_water_ov, LV_OBJ_FLAG_HIDDEN);
}

// ─── Build settings panel ─────────────────────────────────────────────────────

static void build_settings_panel(void)
{
    s_set_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_set_panel, 360, 360);
    lv_obj_set_pos(s_set_panel, 0, 0);
    lv_obj_set_style_bg_color(s_set_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_set_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_set_panel, 0, 0);
    lv_obj_set_style_radius(s_set_panel, 0, 0);
    lv_obj_clear_flag(s_set_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Amber header bar (permanent, 360×46, TOP_MID)
    s_set_header = lv_obj_create(s_set_panel);
    lv_obj_set_size(s_set_header, 360, 46);
    lv_obj_set_style_bg_color(s_set_header, lv_color_hex(COL_GOLD_DARK), 0);
    lv_obj_set_style_bg_opa(s_set_header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_set_header, 0, 0);
    lv_obj_set_style_radius(s_set_header, 0, 0);
    lv_obj_set_style_pad_all(s_set_header, 0, 0);
    lv_obj_align(s_set_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(s_set_header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Title label inside header, centered
    s_set_title = lv_label_create(s_set_header);
    lv_label_set_text(s_set_title, "SETTINGS");
    lv_obj_set_style_text_color(s_set_title, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_set_title, &lv_font_roboto_16, 0);
    lv_obj_set_style_text_letter_space(s_set_title, 2, 0);
    lv_obj_align(s_set_title, LV_ALIGN_CENTER, 0, 3);

    lv_obj_add_flag(s_set_panel, LV_OBJ_FLAG_HIDDEN);
}

// ─── Public: ui_show_home ────────────────────────────────────────────────────

void ui_show_home(void)
{
    // Load preferences
    lm_prefs_t prefs = {};
    storage_load_prefs(&prefs);
    s_fahrenheit    = prefs.use_fahrenheit;
    s_brightness    = prefs.brightness > 0 ? prefs.brightness : 80;
    s_auto_off      = prefs.auto_off_min;
    s_pb_on         = prefs.pb_on_s > 0.1f ? prefs.pb_on_s : 3.0f;
    s_pb_off        = prefs.pb_off_s > 0.1f ? prefs.pb_off_s : 1.0f;
    s_edit_temp     = prefs.coffee_temp_target > 0 ? prefs.coffee_temp_target : 93.0f;
    s_show_friendly = prefs.show_friendly_name;
    s_pb_active     = false;  // RAM-only default
    storage_load_friendly_name(s_friendly_name, sizeof(s_friendly_name));

    // Create screen
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    s_scr = scr;

    // Build
    build_pages();
    build_dots();
    build_cloud_dot();
    build_brew_overlay();
    build_water_overlay();
    build_settings_panel();

    // Show first page
    show_page(PAGE_COFFEE);
    refresh_coffee();
    refresh_dots();

    lv_scr_load(scr);
    ESP_LOGI(TAG, "Home screen loaded");
}

// ─── Public: ui_update_state ────────────────────────────────────────────────

void ui_update_state(const lm_machine_state_t *state)
{
    if (!s_scr) return;
    memcpy(&s_state, state, sizeof(s_state));

    // Refresh all page content
    refresh_coffee();
    refresh_steam();
    refresh_temp();
    refresh_bf();

    // Water warning overlay
    if (state->water_tank_empty) {
        if (s_water_ov) {
            lv_obj_clear_flag(s_water_ov, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_water_ov);
        }
    } else {
        if (s_water_ov) lv_obj_add_flag(s_water_ov, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─── Public: ui_set_connected ────────────────────────────────────────────────

void ui_set_connected(bool connected)
{
    if (!s_cloud_dot) return;
    if (connected) {
        lv_obj_add_flag(s_cloud_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_cloud_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_cloud_dot, lv_color_hex(0xFF2222), 0);
    }
}

// ─── Public: ui_set_brewing ──────────────────────────────────────────────────

void ui_set_brewing(bool brewing)
{
    if (!s_scr) return;
    s_brewing = brewing;
    show_brew_overlay(brewing);
}

// ─── Knob input ──────────────────────────────────────────────────────────────

void ui_knob_rotate(int delta)
{
    if (!s_scr) return;

    switch (s_ui_state) {

    case UI_NORMAL:
        if (s_page == PAGE_TEMP) {
            if (!s_temp_editing) {
                s_temp_editing = true;
                s_edit_temp = s_state.coffee_temp_target > 0
                    ? s_state.coffee_temp_target : 93.0f;
            }
            s_edit_temp += delta * TEMP_STEP;
            if (s_edit_temp < TEMP_MIN) s_edit_temp = TEMP_MIN;
            if (s_edit_temp > TEMP_MAX) s_edit_temp = TEMP_MAX;
            refresh_temp();
        } else {
            int next = s_page + delta;
            if (next < 0)           next = PAGE_COUNT - 1;
            if (next >= PAGE_COUNT) next = 0;
            s_temp_editing = false;
            show_page(next);
            switch (next) {
            case PAGE_COFFEE: refresh_coffee(); break;
            case PAGE_STEAM:  refresh_steam();  break;
            case PAGE_TEMP:   refresh_temp();   break;
            case PAGE_BF:     refresh_bf();     break;
            }
        }
        break;

    case UI_SETTINGS_MENU:
        s_menu_sel = (s_menu_sel + delta + 3) % 3;
        settings_rebuild();
        break;

    case UI_SETTINGS_SYSTEM:
        s_sys_sel = (s_sys_sel + delta + 3) % 3;
        settings_rebuild();
        break;

    case UI_SETTINGS_DISPLAY:
        if (s_disp_sel == 0) {
            s_brightness += delta * 5;
            if (s_brightness < 5)   s_brightness = 5;
            if (s_brightness > 100) s_brightness = 100;
        } else {
            s_auto_off += delta;
            if (s_auto_off < 0)  s_auto_off = 0;
            if (s_auto_off > 60) s_auto_off = 60;
        }
        settings_rebuild();
        break;

    case UI_SETTINGS_PREBREW:
        if (s_pb_sel == 0) {
            // Rotate toggles active state
            s_pb_active = (delta > 0);
        } else if (s_pb_sel == 1) {
            s_pb_on += delta * 0.5f;
            if (s_pb_on < 0.5f)  s_pb_on = 0.5f;
            if (s_pb_on > 10.0f) s_pb_on = 10.0f;
        } else {
            s_pb_off += delta * 0.5f;
            if (s_pb_off < 0.0f)  s_pb_off = 0.0f;
            if (s_pb_off > 10.0f) s_pb_off = 10.0f;
        }
        settings_rebuild();
        break;

    case UI_SETTINGS_UNITS:
        s_fahrenheit = !s_fahrenheit;
        settings_rebuild();
        break;

    case UI_SETTINGS_NAME:
        s_show_friendly = !s_show_friendly;
        settings_rebuild();
        break;

    default:
        break;
    }
}

void ui_knob_press(void)
{
    if (!s_scr) return;

    // Tap on brew overlay dismisses it
    if (s_brewing) {
        show_brew_overlay(false);
        s_brewing = false;
        return;
    }

    switch (s_ui_state) {

    case UI_NORMAL:
        switch (s_page) {
        case PAGE_COFFEE: {
            bool new_on = !s_state.powered_on;
            s_state.powered_on = new_on;
            if (!new_on) s_state.coffee_boiler_ready = false;
            refresh_coffee();
            dispatch_power(new_on);
            break;
        }
        case PAGE_STEAM: {
            int new_lv = (s_state.steam_level > 0) ? 0 : 1;
            s_state.steam_level = new_lv;
            if (new_lv == 0) s_state.steam_boiler_ready = false;
            refresh_steam();
            dispatch_steam(new_lv);
            break;
        }
        case PAGE_TEMP:
            if (s_temp_editing) {
                s_state.coffee_temp_target = s_edit_temp;
                s_temp_editing = false;
                refresh_temp();
                dispatch_temp(s_edit_temp);
                lm_prefs_t p = {};
                storage_load_prefs(&p);
                p.coffee_temp_target = s_edit_temp;
                storage_save_prefs(&p);
            } else {
                s_temp_editing = true;
                s_edit_temp = s_state.coffee_temp_target > 0
                    ? s_state.coffee_temp_target : 93.0f;
                refresh_temp();
            }
            break;
        case PAGE_BF:
            switch (s_bf_state) {
            case BF_IDLE:
            case BF_READY:  s_bf_state = BF_PADDLE;  break;
            case BF_PADDLE: s_bf_state = BF_RUNNING; lm_cloud_trigger_backflush(); break;
            case BF_RUNNING:s_bf_state = BF_DONE;    break;
            case BF_DONE:   s_bf_state = BF_IDLE;    break;
            }
            refresh_bf();
            break;
        }
        break;

    case UI_SETTINGS_MENU:
        switch (s_menu_sel) {
        case 0: s_ui_state = UI_SETTINGS_DISPLAY; s_disp_sel = 0; break;
        case 1: s_ui_state = UI_SETTINGS_PREBREW; s_pb_sel   = 0; break;
        case 2: s_ui_state = UI_SETTINGS_SYSTEM;  s_sys_sel  = 0; break;
        }
        settings_rebuild();
        break;

    case UI_SETTINGS_DISPLAY:
        s_disp_sel++;
        if (s_disp_sel >= 2) {
            s_disp_sel = 0;
            save_prefs();
        }
        settings_rebuild();
        break;

    case UI_SETTINGS_PREBREW:
        // Press cycles selection: 0=pill → 1=on_time → 2=off_time → save → 0
        // (rotate changes value of selected item)
        s_pb_sel++;
        if (s_pb_sel >= 3) {
            s_pb_sel = 0;
            save_prefs();
        }
        settings_rebuild();
        break;

    case UI_SETTINGS_SYSTEM:
        switch (s_sys_sel) {
        case 0: break;  // Info — no sub-page
        case 1: s_ui_state = UI_SETTINGS_UNITS; settings_rebuild(); break;
        case 2: s_ui_state = UI_SETTINGS_NAME;  settings_rebuild(); break;
        }
        break;

    case UI_SETTINGS_UNITS:
        save_prefs();
        refresh_temp();
        s_ui_state = UI_SETTINGS_SYSTEM;
        settings_rebuild();
        break;

    case UI_SETTINGS_NAME:
        save_prefs();
        refresh_brand_labels();
        s_ui_state = UI_SETTINGS_SYSTEM;
        settings_rebuild();
        break;

    default:
        break;
    }
}

void ui_knob_long_press(void)
{
    if (!s_scr) return;

    // Long press on brew overlay dismisses it
    if (s_brewing) {
        show_brew_overlay(false);
        s_brewing = false;
        return;
    }

    if (s_ui_state == UI_NORMAL) {
        settings_open();
    } else if (s_ui_state == UI_SETTINGS_MENU) {
        settings_close();
    } else {
        // In any sub-menu → go back to main menu
        s_ui_state = UI_SETTINGS_MENU;
        s_menu_sel = 0;
        settings_rebuild();
    }
}
