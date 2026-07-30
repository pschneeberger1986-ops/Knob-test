// =============================================================================
// app_main.c — LM Knob – cloud-only La Marzocco controller
// =============================================================================
//
// Boot sequence:
//   1. Init NVS, display, LVGL
//   2. If no Wi-Fi credentials → launch setup portal (AP mode)
//   3. Connect to Wi-Fi, sync SNTP
//   4. Start cloud client (auth + poll every 5 s)
//   5. UI stays on home screen; state updates arrive via callback
//
// No BLE. Everything via La Marzocco cloud API.
// =============================================================================

#include "storage.h"
#include "cloud_lm.h"
#include "ui.h"
#include "portal.h"
#include "esp_sntp.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "main";

static EventGroupHandle_t s_events;
#define EV_WIFI_DONE  BIT0
#define EV_WIFI_IP    BIT1

// ─── Cloud state callback ─────────────────────────────────────────────────────

static void on_machine_state(const lm_machine_state_t *state)
{
    ui_update_state(state);
}

// ─── Wi-Fi event handler ──────────────────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting…");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP");
        xEventGroupSetBits(s_events, EV_WIFI_IP);
    }
}

static void on_portal_done(void)
{
    portal_stop();
    xEventGroupSetBits(s_events, EV_WIFI_DONE);
    ESP_LOGI(TAG, "Wi-Fi credentials saved via portal");
}

static void wifi_start_sta(void)
{
    char ssid[64] = {}, pass[64] = {};
    storage_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass));

    esp_netif_create_default_wifi_sta();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               wifi_event_handler, NULL);

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
    ESP_LOGI(TAG, "Wi-Fi STA started (SSID: %s)", ssid);

    // Wait for IP (30 s timeout)
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_WIFI_IP,
                                            pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(30000));
    if (!(bits & EV_WIFI_IP)) {
        ESP_LOGW(TAG, "Wi-Fi: no IP after 30 s, cloud may not work");
    }

    // SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Give SNTP a moment to sync (cloud auth needs wall-clock time)
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Start cloud (auth + polling) in background
    lm_cloud_start(on_machine_state);
}

// ─── Entry point ─────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "LM Knob starting (cloud-only)…");

    s_events = xEventGroupCreate();

    // 1. Storage
    storage_init();

    // 2. Network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    // 3. Display + LVGL
    ui_init();
    ui_show_home();

    // 4. Wi-Fi: portal if no credentials, then STA
    if (!storage_has_wifi()) {
        ESP_LOGI(TAG, "No Wi-Fi credentials – launching setup portal");
        portal_start(on_portal_done);
        // Block until portal delivers credentials
        xEventGroupWaitBits(s_events, EV_WIFI_DONE,
                            pdTRUE, pdTRUE, portMAX_DELAY);
    }

    // 5. Connect + cloud
    wifi_start_sta();

    ESP_LOGI(TAG, "Boot complete");
}
