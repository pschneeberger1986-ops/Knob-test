#pragma once
// =============================================================================
// cloud_lm.h — La Marzocco cloud-only client (no BLE)
// =============================================================================

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Machine state (filled by lm_cloud_poll_state) ───────────────────────────

typedef struct {
    bool  powered_on;
    bool  coffee_boiler_ready;   // true = READY, false = HEATING
    float coffee_temp_target;    // °C target
    float coffee_temp_current;   // °C current (approx: target when ready, 0 when heating)
    int   steam_level;           // 0=off, 1-3 = Level1-3
    bool  steam_boiler_ready;
    float steam_temp_target;
    float steam_temp_current;
    bool  water_tank_empty;
} lm_machine_state_t;

// Callback invoked from poll task with latest state
typedef void (*lm_cloud_state_cb_t)(const lm_machine_state_t *state);

// ─── Lifecycle ───────────────────────────────────────────────────────────────

/**
 * Start cloud client: auth + periodic polling.
 * Call after WiFi + SNTP are connected.
 * Reads credentials from NVS automatically.
 */
void lm_cloud_start(lm_cloud_state_cb_t state_cb);

// ─── Auth ────────────────────────────────────────────────────────────────────

esp_err_t lm_cloud_authenticate(const char *email, const char *password);
bool      lm_cloud_is_authenticated(void);
void      lm_cloud_set_serial(const char *serial);

// ─── State poll ──────────────────────────────────────────────────────────────

esp_err_t lm_cloud_poll_state(lm_machine_state_t *out);

// ─── Commands (synchronous — call from a background task) ────────────────────

esp_err_t lm_cloud_set_power(bool on);
esp_err_t lm_cloud_set_coffee_temp(float celsius);
esp_err_t lm_cloud_set_steam_level(int level);   // 1-3
esp_err_t lm_cloud_backflush(void);

// ─── Fire-and-forget helpers ─────────────────────────────────────────────────

void lm_cloud_trigger_backflush(void);

#ifdef __cplusplus
}
#endif
