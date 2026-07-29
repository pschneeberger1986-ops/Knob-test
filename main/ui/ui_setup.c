// =============================================================================
// ui_setup.c — First-boot WiFi setup screen with QR code
// =============================================================================

#include "ui.h"
#include "lvgl.h"
#include "extra/libs/qrcode/lv_qrcode.h"
#include <string.h>

static lv_obj_t *s_scr     = NULL;
static lv_obj_t *s_lbl_msg = NULL;

#define WIFI_SSID       "LM-Knob-Setup"
#define WIFI_QR_STR     "WIFI:T:nopass;S:LM-Knob-Setup;P:;;"
#define QR_SIZE         160

void ui_show_setup(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Title
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LM Knob");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8C87A), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    // QR code — white on black, centered
    lv_obj_t *qr = lv_qrcode_create(scr, QR_SIZE,
                                     lv_color_black(),
                                     lv_color_white());
    lv_qrcode_update(qr, WIFI_QR_STR, strlen(WIFI_QR_STR));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, 10);

    // "Scan to connect" label
    lv_obj_t *lbl_scan = lv_label_create(scr);
    lv_label_set_text(lbl_scan, "Scan to connect");
    lv_obj_set_style_text_font(lbl_scan, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_scan, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(lbl_scan, LV_ALIGN_BOTTOM_MID, 0, -42);

    // "Then open 192.168.4.1" label
    s_lbl_msg = lv_label_create(scr);
    lv_label_set_long_mode(s_lbl_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_msg, 260);
    lv_label_set_text(s_lbl_msg, "Then open 192.168.4.1");
    lv_obj_set_style_text_font(s_lbl_msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_msg, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(s_lbl_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_msg, LV_ALIGN_BOTTOM_MID, 0, -20);

    s_scr = scr;
    lv_scr_load(scr);
}

// Called by app_main when setup status changes
void ui_setup_set_status(const char *msg)
{
    if (s_lbl_msg)
        lv_label_set_text(s_lbl_msg, msg);
}
