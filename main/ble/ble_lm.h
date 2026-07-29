#pragma once
// =============================================================================
// ble_lm.h — La Marzocco BLE client
// =============================================================================
// Protocol (reverse-engineered, confirmed by pylamarzocco + kaspizzo):
//
//   Service UUID  : found by scanning – device advertises as "MICRA", "MINI",
//                   "GS3" or custom name set in the LM app.
//
//   Characteristics (all under the same service):
//     READ      0a0b7847-e12b-09a8-b04b-8e0922a9abab  – machine state JSON
//     WRITE     0b0b7847-e12b-09a8-b04b-8e0922a9abab  – commands (JSON+\0)
//     GET_TOKEN 0c0b7847-e12b-09a8-b04b-8e0922a9abab  – pairing-mode token
//     AUTH      0d0b7847-e12b-09a8-b04b-8e0922a9abab  – write token to auth
//
//   Pairing (first use):
//     1. Put machine in pairing mode (hold button until LED blinks).
//     2. Scan for BLE advertisement.
//     3. Connect without auth.
//     4. Read GET_TOKEN → store token + MAC in NVS.
//     5. Disconnect and reconnect with auth (see below).
//
//   Normal connection:
//     1. Connect to stored MAC address.
//     2. Write stored token (UTF-8, no \0) to AUTH characteristic.
//     3. Proceed to read/write.
//
//   Commands written to WRITE characteristic:
//     JSON object, compact, null-terminated.
//     {"name":"CommandName","parameter":{...}}
//
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Machine state ────────────────────────────────────────────────────────────

typedef struct {
    bool    powered_on;           // BrewingMode vs StandBy
    float   coffee_temp_current;  // °C
    float   coffee_temp_target;   // °C
    float   steam_temp_current;   // °C (0 if unavailable)
    float   steam_temp_target;    // °C
    int     steam_level;          // 1-3 (0 if no steam boiler)
    bool    water_tank_empty;
    bool    brewing;
    bool    coffee_boiler_ready;
    bool    steam_boiler_ready;
} lm_machine_state_t;

// ─── Callbacks ────────────────────────────────────────────────────────────────

typedef void (*lm_ble_state_cb_t)(const lm_machine_state_t *state);
typedef void (*lm_ble_connected_cb_t)(bool connected);

// ─── Init / lifecycle ─────────────────────────────────────────────────────────

/**
 * Initialise the BLE stack and NimBLE host task.
 * Call once at startup before any other lm_ble_* function.
 */
void lm_ble_init(void);

/**
 * Start scanning for a La Marzocco device in pairing mode.
 * When found, reads the auth token from the GET_TOKEN characteristic,
 * stores it (+ MAC) via storage_save_ble_credentials(), then calls cb.
 *
 * @param cb  Called with true on success, false on timeout (30 s).
 */
void lm_ble_start_pairing(lm_ble_connected_cb_t cb);

/**
 * Connect to the previously paired machine (MAC + token from NVS).
 * Retries indefinitely with exponential back-off.
 *
 * @param state_cb     Called whenever machine state is refreshed.
 * @param connect_cb   Called when connection is established or lost.
 */
void lm_ble_connect(lm_ble_state_cb_t state_cb, lm_ble_connected_cb_t connect_cb);

/** Disconnect and stop reconnection attempts. */
void lm_ble_disconnect(void);

// ─── Commands ─────────────────────────────────────────────────────────────────

/** Turn machine on (BrewingMode) or off (StandBy). */
esp_err_t lm_ble_set_power(bool on);

/** Set coffee boiler target temperature (°C, clamped 85-105). */
esp_err_t lm_ble_set_coffee_temp(float celsius);

/** Enable / disable steam boiler. */
esp_err_t lm_ble_set_steam_enable(bool enable);

/** Set steam boiler level 1-3. */
esp_err_t lm_ble_set_steam_level(int level);

/** Request a fresh state read from the machine. */
esp_err_t lm_ble_refresh_state(void);

#ifdef __cplusplus
}
#endif
