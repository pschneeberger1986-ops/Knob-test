// =============================================================================
// storage.c — NVS implementation
// =============================================================================
#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG    = "storage";
static const char *NS     = "lm_knob";   // NVS namespace

static nvs_handle_t s_nvs;

void storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, reformatting…");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &s_nvs));
    ESP_LOGI(TAG, "NVS ready");
}

// ─── BLE ─────────────────────────────────────────────────────────────────────

bool storage_has_ble_credentials(void)
{
    char dummy[2];
    size_t len = sizeof(dummy);
    return nvs_get_str(s_nvs, "ble_token", dummy, &len) == ESP_OK;
}

void storage_save_ble_credentials(const char *token, const lm_ble_addr_t *addr)
{
    nvs_set_str(s_nvs, "ble_token", token);
    nvs_set_blob(s_nvs, "ble_addr", addr, sizeof(lm_ble_addr_t));
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "BLE credentials saved");
}

void storage_load_ble_credentials(char *token_out, size_t token_len,
                                   lm_ble_addr_t *addr_out)
{
    nvs_get_str(s_nvs, "ble_token", token_out, &token_len);
    size_t addr_len = sizeof(lm_ble_addr_t);
    nvs_get_blob(s_nvs, "ble_addr", addr_out, &addr_len);
}

void storage_clear_ble_credentials(void)
{
    nvs_erase_key(s_nvs, "ble_token");
    nvs_erase_key(s_nvs, "ble_addr");
    nvs_commit(s_nvs);
}

// ─── Wi-Fi ───────────────────────────────────────────────────────────────────

bool storage_has_wifi(void)
{
    char dummy[2];
    size_t len = sizeof(dummy);
    return nvs_get_str(s_nvs, "wifi_ssid", dummy, &len) == ESP_OK;
}

void storage_save_wifi(const char *ssid, const char *password)
{
    nvs_set_str(s_nvs, "wifi_ssid", ssid);
    nvs_set_str(s_nvs, "wifi_pass", password);
    nvs_commit(s_nvs);
}

void storage_load_wifi(char *ssid_out, size_t ssid_len,
                        char *pass_out, size_t pass_len)
{
    nvs_get_str(s_nvs, "wifi_ssid", ssid_out, &ssid_len);
    nvs_get_str(s_nvs, "wifi_pass", pass_out, &pass_len);
}

// ─── Preferences ─────────────────────────────────────────────────────────────

void storage_save_prefs(const lm_prefs_t *prefs)
{
    nvs_set_blob(s_nvs, "prefs", prefs, sizeof(lm_prefs_t));
    nvs_commit(s_nvs);
}

void storage_load_prefs(lm_prefs_t *prefs)
{
    // Defaults
    prefs->coffee_temp_target = 93.0f;
    prefs->steam_level        = 2;
    prefs->steam_enabled      = true;
    prefs->brightness         = 80;
    prefs->use_fahrenheit     = false;
    prefs->auto_off_min       = 5;
    prefs->pb_on_s            = 3.0f;
    prefs->pb_off_s           = 1.0f;

    size_t len = sizeof(lm_prefs_t);
    nvs_get_blob(s_nvs, "prefs", prefs, &len);
}

// ─── LM cloud credentials ─────────────────────────────────────────────────────

bool storage_has_lm_credentials(void)
{
    char dummy[2]; size_t len = sizeof(dummy);
    return nvs_get_str(s_nvs, "lm_email", dummy, &len) == ESP_OK;
}

void storage_save_lm_credentials(const char *email, const char *password,
                                  const char *serial)
{
    nvs_set_str(s_nvs, "lm_email",  email);
    nvs_set_str(s_nvs, "lm_pass",   password);
    nvs_set_str(s_nvs, "lm_serial", serial);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "LM credentials saved (email=%s, serial=%s)", email, serial);
}

void storage_load_lm_credentials(char *email,  size_t email_len,
                                  char *pass,   size_t pass_len,
                                  char *serial, size_t serial_len)
{
    nvs_get_str(s_nvs, "lm_email",  email,  &email_len);
    nvs_get_str(s_nvs, "lm_pass",   pass,   &pass_len);
    nvs_get_str(s_nvs, "lm_serial", serial, &serial_len);
}

// ─── Installation key ─────────────────────────────────────────────────────────

bool storage_has_install_key(void)
{
    char dummy[2]; size_t len = sizeof(dummy);
    return nvs_get_str(s_nvs, "inst_id", dummy, &len) == ESP_OK;
}

void storage_save_install_key(const char    *installation_id,
                               const uint8_t *secret,
                               const uint8_t *priv_der, size_t priv_len,
                               const uint8_t *pub_der,  size_t pub_len)
{
    nvs_set_str (s_nvs, "inst_id",   installation_id);
    nvs_set_blob(s_nvs, "inst_sec",  secret,   32);
    nvs_set_blob(s_nvs, "inst_priv", priv_der, priv_len);
    nvs_set_u32 (s_nvs, "inst_privl",(uint32_t)priv_len);
    nvs_set_blob(s_nvs, "inst_pub",  pub_der,  pub_len);
    nvs_set_u32 (s_nvs, "inst_publ", (uint32_t)pub_len);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "Installation key saved (id=%s)", installation_id);
}

void storage_load_install_key(char    *installation_id,
                               uint8_t *secret,
                               uint8_t *priv_der, size_t *priv_len,
                               uint8_t *pub_der,  size_t *pub_len)
{
    size_t id_len = 37;
    nvs_get_str(s_nvs, "inst_id", installation_id, &id_len);

    size_t sec_len = 32;
    nvs_get_blob(s_nvs, "inst_sec", secret, &sec_len);

    uint32_t pl = 0;
    nvs_get_u32(s_nvs, "inst_privl", &pl);
    *priv_len = pl;
    nvs_get_blob(s_nvs, "inst_priv", priv_der, priv_len);

    uint32_t pul = 0;
    nvs_get_u32(s_nvs, "inst_publ", &pul);
    *pub_len = pul;
    nvs_get_blob(s_nvs, "inst_pub", pub_der, pub_len);
}

// ─── Friendly name ───────────────────────────────────────────────────────────

void storage_save_friendly_name(const char *name)
{
    nvs_set_str(s_nvs, "friendly_name", name);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "Friendly name saved: %s", name);
}

void storage_load_friendly_name(char *name_out, size_t name_len)
{
    // Default to empty string; caller should check and fall back to "LA MARZOCCO"
    name_out[0] = '\0';
    nvs_get_str(s_nvs, "friendly_name", name_out, &name_len);
}

// ─── Cloud tokens ─────────────────────────────────────────────────────────────

void storage_save_cloud_tokens(const char *access, const char *refresh,
                                time_t expires_at)
{
    nvs_set_str(s_nvs, "cl_access",  access);
    nvs_set_str(s_nvs, "cl_refresh", refresh);
    nvs_set_u32(s_nvs, "cl_exp",    (uint32_t)expires_at);
    nvs_commit(s_nvs);
}

bool storage_load_cloud_tokens(char *access,  size_t acc_len,
                                char *refresh, size_t ref_len,
                                time_t *expires_at_out)
{
    uint32_t exp = 0;
    if (nvs_get_str (s_nvs, "cl_access",  access,  &acc_len) != ESP_OK) return false;
    if (nvs_get_str (s_nvs, "cl_refresh", refresh, &ref_len) != ESP_OK) return false;
    nvs_get_u32(s_nvs, "cl_exp", &exp);
    *expires_at_out = (time_t)exp;
    return true;
}
