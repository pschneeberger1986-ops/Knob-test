// =============================================================================
// ble_lm.c — La Marzocco BLE client implementation (NimBLE)
// =============================================================================

#include "ble_lm.h"
#include "storage.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "lm_ble";

// ─── UUIDs ───────────────────────────────────────────────────────────────────
// Source: pylamarzocco + kaspizzo reverse engineering

static const ble_uuid128_t UUID_READ = BLE_UUID128_INIT(
    0xab,0xab,0xa9,0x22,0x09,0x8e,0x4b,0xb0,
    0xa8,0x09,0x2b,0xe1,0x47,0x78,0x0b,0x0a);

static const ble_uuid128_t UUID_WRITE = BLE_UUID128_INIT(
    0xab,0xab,0xa9,0x22,0x09,0x8e,0x4b,0xb0,
    0xa8,0x09,0x2b,0xe1,0x47,0x78,0x0b,0x0b);

static const ble_uuid128_t UUID_GET_TOKEN = BLE_UUID128_INIT(
    0xab,0xab,0xa9,0x22,0x09,0x8e,0x4b,0xb0,
    0xa8,0x09,0x2b,0xe1,0x47,0x78,0x0b,0x0c);

static const ble_uuid128_t UUID_AUTH = BLE_UUID128_INIT(
    0xab,0xab,0xa9,0x22,0x09,0x8e,0x4b,0xb0,
    0xa8,0x09,0x2b,0xe1,0x47,0x78,0x0b,0x0d);

// Known BLE advertisement name prefixes for La Marzocco machines
static const char *LM_NAME_PREFIXES[] = { "MICRA", "MINI", "GS3", NULL };

// ─── Internal state ───────────────────────────────────────────────────────────

typedef enum {
    BLE_STATE_IDLE,
    BLE_STATE_PAIRING_SCAN,
    BLE_STATE_PAIRING_CONNECT,
    BLE_STATE_PAIRING_READ_TOKEN,
    BLE_STATE_CONNECTING,
    BLE_STATE_AUTHENTICATING,
    BLE_STATE_READY,
    BLE_STATE_DISCONNECTING,
} lm_ble_state_t;

static struct {
    lm_ble_state_t      state;
    uint16_t            conn_handle;
    uint16_t            handle_read;
    uint16_t            handle_write;
    uint16_t            handle_get_token;
    uint16_t            handle_auth;
    lm_machine_state_t  machine;
    lm_ble_state_cb_t   on_state;
    lm_ble_connected_cb_t on_connect;
    lm_ble_connected_cb_t on_pair_done;
    bool                pairing_mode;
    char                token[128];
    lm_ble_addr_t       peer_addr;  // mirrors ble_addr_t, NimBLE-free type
    uint32_t            backoff_ms;
} s;

static SemaphoreHandle_t s_cmd_mutex;

// ─── Forward declarations ─────────────────────────────────────────────────────
static void lm_ble_scan_start(void);
static int  lm_ble_gap_event(struct ble_gap_event *event, void *arg);
static void lm_ble_on_sync(void);
static void lm_ble_on_reset(int reason);
static void lm_ble_discover_handles(uint16_t conn_handle);
static esp_err_t lm_ble_write_cmd(const char *json);
static void lm_ble_parse_state(const uint8_t *data, uint16_t len);

// ─── NimBLE host task ─────────────────────────────────────────────────────────

static void nimble_host_task(void *arg)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void lm_ble_init(void)
{
    s_cmd_mutex = xSemaphoreCreateMutex();
    memset(&s, 0, sizeof(s));
    s.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s.backoff_ms  = 1000;

    nimble_port_init();
    ble_hs_cfg.sync_cb  = lm_ble_on_sync;
    ble_hs_cfg.reset_cb = lm_ble_on_reset;
    ble_svc_gap_device_name_set("lm-knob");
    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE stack initialised");
}

// ─── Sync / reset callbacks ───────────────────────────────────────────────────

static void lm_ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE sync – ready");
    // Storage will tell us if we have credentials already
    if (storage_has_ble_credentials()) {
        storage_load_ble_credentials(s.token, sizeof(s.token), &s.peer_addr);
        ESP_LOGI(TAG, "Stored credentials loaded, peer=" \
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 s.peer_addr.val[5], s.peer_addr.val[4], s.peer_addr.val[3],
                 s.peer_addr.val[2], s.peer_addr.val[1], s.peer_addr.val[0]);
    }
}

static void lm_ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE reset – reason %d", reason);
}

// ─── Scanning ─────────────────────────────────────────────────────────────────

static bool lm_name_matches(const struct ble_hs_adv_fields *fields)
{
    if (!fields->name || !fields->name_len) return false;
    for (int i = 0; LM_NAME_PREFIXES[i]; i++) {
        size_t plen = strlen(LM_NAME_PREFIXES[i]);
        if (fields->name_len >= plen &&
            memcmp(fields->name, LM_NAME_PREFIXES[i], plen) == 0)
            return true;
    }
    return false;
}

static void lm_ble_scan_start(void)
{
    struct ble_gap_disc_params disc = {
        .itvl          = BLE_GAP_SCAN_FAST_INTERVAL_MAX,
        .window        = BLE_GAP_SCAN_FAST_WINDOW,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .passive       = 0,
        .filter_duplicates = 1,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                          &disc, lm_ble_gap_event, NULL);
    if (rc != 0)
        ESP_LOGE(TAG, "Scan start failed: %d", rc);
    else
        ESP_LOGI(TAG, "BLE scan started");
}

// ─── GATT – characteristic handle discovery ───────────────────────────────────

static int lm_disc_chr_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        // Discovery complete – authenticate
        ESP_LOGI(TAG, "Handle discovery done: r=%04x w=%04x gt=%04x a=%04x",
                 s.handle_read, s.handle_write,
                 s.handle_get_token, s.handle_auth);

        if (s.pairing_mode) {
            // Read the token from GET_TOKEN characteristic
            s.state = BLE_STATE_PAIRING_READ_TOKEN;
            ble_gattc_read(conn_handle, s.handle_get_token,
                           /* cb */ NULL, NULL);
        } else {
            // Write auth token
            s.state = BLE_STATE_AUTHENTICATING;
            uint16_t tlen = (uint16_t)strlen(s.token);
            ble_gattc_write_flat(conn_handle, s.handle_auth,
                                 s.token, tlen, NULL, NULL);
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "Characteristic discovery error: %d", error->status);
        return 0;
    }
    // Match each UUID
    if (ble_uuid_cmp(&chr->uuid.u, &UUID_READ.u) == 0)
        s.handle_read = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &UUID_WRITE.u) == 0)
        s.handle_write = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &UUID_GET_TOKEN.u) == 0)
        s.handle_get_token = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &UUID_AUTH.u) == 0)
        s.handle_auth = chr->val_handle;
    return 0;
}

static void lm_ble_discover_handles(uint16_t conn_handle)
{
    // Discover all characteristics of the primary service (discover all)
    ble_gattc_disc_all_chrs(conn_handle, 1, 0xffff, lm_disc_chr_cb, NULL);
}

// ─── GATT – read callback (used for GET_TOKEN + state refresh) ────────────────

static int __attribute__((used)) lm_read_cb(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGE(TAG, "Read error: %d", error->status);
        return 0;
    }
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    uint8_t *buf = malloc(len + 1);
    if (!buf) return 0;
    os_mbuf_copydata(attr->om, 0, len, buf);
    buf[len] = 0;

    if (s.state == BLE_STATE_PAIRING_READ_TOKEN) {
        // Store token + MAC
        strncpy(s.token, (char*)buf, sizeof(s.token) - 1);
        ESP_LOGI(TAG, "Got BLE token: %s", s.token);
        storage_save_ble_credentials(s.token, &s.peer_addr);
        s.pairing_mode = false;
        s.state = BLE_STATE_READY;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (s.on_pair_done) s.on_pair_done(true);
    } else {
        // Machine state JSON
        lm_ble_parse_state(buf, len);
        if (s.on_state) s.on_state(&s.machine);
    }
    free(buf);
    return 0;
}

// ─── GAP event handler ────────────────────────────────────────────────────────

static int lm_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields,
                event->disc.data, event->disc.length_data) != 0)
            break;
        if (!lm_name_matches(&fields)) break;

        char name[32] = {};
        memcpy(name, fields.name,
               fields.name_len < 31 ? fields.name_len : 31);
        ESP_LOGI(TAG, "Found La Marzocco device: %s", name);
        memcpy(&s.peer_addr, &event->disc.addr, sizeof(lm_ble_addr_t));

        ble_gap_disc_cancel();
        s.state = s.pairing_mode
                    ? BLE_STATE_PAIRING_CONNECT : BLE_STATE_CONNECTING;
        ble_gap_connect(BLE_OWN_ADDR_PUBLIC, (const ble_addr_t*)&s.peer_addr,
                        15000, NULL, lm_ble_gap_event, NULL);
        break;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s.conn_handle = event->connect.conn_handle;
            s.backoff_ms  = 1000;
            ESP_LOGI(TAG, "Connected, discovering characteristics…");
            lm_ble_discover_handles(s.conn_handle);
        } else {
            ESP_LOGE(TAG, "Connection failed: %d", event->connect.status);
            vTaskDelay(pdMS_TO_TICKS(s.backoff_ms));
            s.backoff_ms = MIN(s.backoff_ms * 2, 30000);
            lm_ble_scan_start();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected – reason %d",
                 event->disconnect.reason);
        s.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s.state = BLE_STATE_IDLE;
        if (s.on_connect) s.on_connect(false);
        if (!s.pairing_mode && storage_has_ble_credentials()) {
            // Auto-reconnect
            vTaskDelay(pdMS_TO_TICKS(s.backoff_ms));
            s.backoff_ms = MIN(s.backoff_ms * 2, 30000);
            lm_ble_scan_start();
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        // Notifications from READ characteristic = state updates
        if (event->notify_rx.attr_handle == s.handle_read) {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t *buf = malloc(len + 1);
            if (buf) {
                os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
                buf[len] = 0;
                lm_ble_parse_state(buf, len);
                free(buf);
                if (s.on_state) s.on_state(&s.machine);
            }
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        // Auth write was sent before MTU, but it's fine – the machine accepts
        s.state = BLE_STATE_READY;
        if (s.on_connect) s.on_connect(true);
        // Subscribe to notifications on READ characteristic
        lm_ble_refresh_state();
        break;

    default:
        break;
    }
    return 0;
}

// ─── Public – pairing & connect ───────────────────────────────────────────────

void lm_ble_start_pairing(lm_ble_connected_cb_t cb)
{
    ESP_LOGI(TAG, "Starting pairing scan…");
    s.pairing_mode = true;
    s.on_pair_done = cb;
    s.state = BLE_STATE_PAIRING_SCAN;
    lm_ble_scan_start();
}

void lm_ble_connect(lm_ble_state_cb_t state_cb, lm_ble_connected_cb_t connect_cb)
{
    s.on_state   = state_cb;
    s.on_connect = connect_cb;
    s.pairing_mode = false;
    lm_ble_scan_start();
}

void lm_ble_disconnect(void)
{
    if (s.conn_handle != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    s.on_state   = NULL;
    s.on_connect = NULL;
}

// ─── BLE command helper ───────────────────────────────────────────────────────

static esp_err_t lm_ble_write_cmd(const char *json)
{
    if (s.state != BLE_STATE_READY || s.conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return ESP_ERR_INVALID_STATE;

    uint16_t len = (uint16_t)strlen(json);
    uint8_t *buf = malloc(len + 1);   // +1 for null terminator required by protocol
    if (!buf) return ESP_ERR_NO_MEM;
    memcpy(buf, json, len);
    buf[len] = 0;                     // mandatory null terminator

    xSemaphoreTake(s_cmd_mutex, portMAX_DELAY);
    int rc = ble_gattc_write_flat(s.conn_handle, s.handle_write,
                                  buf, len + 1, NULL, NULL);
    xSemaphoreGive(s_cmd_mutex);
    free(buf);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

// ─── Public – commands ────────────────────────────────────────────────────────

esp_err_t lm_ble_set_power(bool on)
{
    char json[128];
    snprintf(json, sizeof(json),
             "{\"name\":\"MachineChangeMode\","
             "\"parameter\":{\"mode\":\"%s\"}}",
             on ? "BrewingMode" : "StandBy");
    return lm_ble_write_cmd(json);
}

esp_err_t lm_ble_set_coffee_temp(float celsius)
{
    celsius = fmaxf(85.0f, fminf(105.0f, celsius));
    char json[160];
    snprintf(json, sizeof(json),
             "{\"name\":\"SettingBoilerTarget\","
             "\"parameter\":{\"identifier\":\"CoffeeBoiler1\","
             "\"value\":%.1f}}", celsius);
    return lm_ble_write_cmd(json);
}

esp_err_t lm_ble_set_steam_enable(bool enable)
{
    char json[128];
    snprintf(json, sizeof(json),
             "{\"name\":\"SettingBoilerEnable\","
             "\"parameter\":{\"identifier\":\"SteamBoiler\","
             "\"state\":%s}}",
             enable ? "true" : "false");
    return lm_ble_write_cmd(json);
}

esp_err_t lm_ble_set_steam_level(int level)
{
    if (level < 1) level = 1;
    if (level > 3) level = 3;
    // Steam level maps to target temperature: 1=126°C 2=128°C 3=131°C
    float temps[] = {0, 126.0f, 128.0f, 131.0f};
    char json[160];
    snprintf(json, sizeof(json),
             "{\"name\":\"SettingBoilerTarget\","
             "\"parameter\":{\"identifier\":\"SteamBoiler\","
             "\"value\":%.1f}}", temps[level]);
    return lm_ble_write_cmd(json);
}

esp_err_t lm_ble_refresh_state(void)
{
    // Request machine state – the machine responds via READ characteristic
    const char *queries[] = {
        "{\"name\":\"machineMode\"}",
        "{\"name\":\"boilers\"}",
        "{\"name\":\"tankStatus\"}",
        NULL
    };
    esp_err_t ret = ESP_OK;
    for (int i = 0; queries[i]; i++) {
        esp_err_t r = lm_ble_write_cmd(queries[i]);
        if (r != ESP_OK) ret = r;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return ret;
}

// ─── State JSON parser ────────────────────────────────────────────────────────

static void lm_ble_parse_state(const uint8_t *data, uint16_t len)
{
    cJSON *root = cJSON_ParseWithLength((const char*)data, len);
    if (!root) return;

    // machineMode
    cJSON *mode = cJSON_GetObjectItem(root, "machineMode");
    if (mode && cJSON_IsString(mode))
        s.machine.powered_on =
            strcmp(mode->valuestring, "BrewingMode") == 0;

    // tankStatus
    cJSON *tank = cJSON_GetObjectItem(root, "tankStatus");
    if (tank && cJSON_IsBool(tank))
        s.machine.water_tank_empty = !cJSON_IsTrue(tank);

    // boilers array: [{identifier, current, target, isReady}, …]
    cJSON *boilers = cJSON_GetObjectItem(root, "boilers");
    if (boilers && cJSON_IsArray(boilers)) {
        cJSON *b;
        cJSON_ArrayForEach(b, boilers) {
            cJSON *id  = cJSON_GetObjectItem(b, "identifier");
            cJSON *cur = cJSON_GetObjectItem(b, "current");
            cJSON *tgt = cJSON_GetObjectItem(b, "target");
            cJSON *rdy = cJSON_GetObjectItem(b, "isReady");
            if (!id || !cJSON_IsString(id)) continue;
            if (strcmp(id->valuestring, "CoffeeBoiler1") == 0) {
                if (cur) s.machine.coffee_temp_current = (float)cur->valuedouble;
                if (tgt) s.machine.coffee_temp_target  = (float)tgt->valuedouble;
                if (rdy) s.machine.coffee_boiler_ready = cJSON_IsTrue(rdy);
            } else if (strcmp(id->valuestring, "SteamBoiler") == 0) {
                if (cur) s.machine.steam_temp_current  = (float)cur->valuedouble;
                if (tgt) s.machine.steam_temp_target   = (float)tgt->valuedouble;
                if (rdy) s.machine.steam_boiler_ready  = cJSON_IsTrue(rdy);
            }
        }
    }

    cJSON_Delete(root);
    ESP_LOGD(TAG, "State: power=%d coffee=%.1f/%.1f°C steam=%.1f/%.1f°C",
             s.machine.powered_on,
             s.machine.coffee_temp_current, s.machine.coffee_temp_target,
             s.machine.steam_temp_current,  s.machine.steam_temp_target);
}
