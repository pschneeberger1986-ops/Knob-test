#pragma once
// =============================================================================
// storage.h — NVS-backed persistent config
// =============================================================================
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

void storage_init(void);

// ─── BLE credentials (pairing token + MAC) ────────────────────────────────────
// MAC stored as raw bytes {type, val[6]} – mirrors ble_addr_t without the NimBLE dependency
typedef struct { uint8_t type; uint8_t val[6]; } lm_ble_addr_t;

bool     storage_has_ble_credentials(void);
void     storage_save_ble_credentials(const char *token, const lm_ble_addr_t *addr);
void     storage_load_ble_credentials(char *token_out, size_t token_len,
                                      lm_ble_addr_t *addr_out);
void     storage_clear_ble_credentials(void);

// ─── Wi-Fi ────────────────────────────────────────────────────────────────────
bool     storage_has_wifi(void);
void     storage_save_wifi(const char *ssid, const char *password);
void     storage_load_wifi(char *ssid_out,  size_t ssid_len,
                            char *pass_out,  size_t pass_len);

// ─── User preferences ────────────────────────────────────────────────────────
typedef struct {
    float   coffee_temp_target; // °C – last target set by user
    int     steam_level;        // 1-3
    bool    steam_enabled;
    int     brightness;         // 0-100
    // v2 fields (added with carousel UI)
    bool    use_fahrenheit;     // display °F instead of °C
    int     auto_off_min;       // backlight auto-off minutes (0 = never)
    float   pb_on_s;            // pre-brew on-time  (seconds)
    float   pb_off_s;           // pre-brew off-time (seconds)
    // v3 fields
    bool    show_friendly_name; // show user-defined name instead of "LA MARZOCCO"
} lm_prefs_t;

void storage_save_prefs(const lm_prefs_t *prefs);
void storage_load_prefs(lm_prefs_t *prefs);

// ─── LM cloud credentials (email, password, serial) ──────────────────────────
bool storage_has_lm_credentials(void);
void storage_save_lm_credentials(const char *email, const char *password,
                                  const char *serial);
void storage_load_lm_credentials(char *email,  size_t email_len,
                                  char *pass,   size_t pass_len,
                                  char *serial, size_t serial_len);

// ─── Installation key (ECDSA P-256 + secret, generated once) ─────────────────
bool storage_has_install_key(void);
void storage_save_install_key(const char    *installation_id,
                               const uint8_t *secret,   /* 32 bytes */
                               const uint8_t *priv_der, size_t priv_len,
                               const uint8_t *pub_der,  size_t pub_len);
void storage_load_install_key(char    *installation_id, /* buf >= 37 */
                               uint8_t *secret,          /* buf 32 */
                               uint8_t *priv_der,        /* buf 200 */
                               size_t  *priv_len,
                               uint8_t *pub_der,         /* buf 100 */
                               size_t  *pub_len);

// ─── Friendly name (user-defined machine name) ───────────────────────────────
void storage_save_friendly_name(const char *name);
void storage_load_friendly_name(char *name_out, size_t name_len);

// ─── Cloud tokens (access + refresh, stored after signin) ────────────────────
void storage_save_cloud_tokens(const char *access, const char *refresh,
                                time_t expires_at);
bool storage_load_cloud_tokens(char *access,  size_t acc_len,
                                char *refresh, size_t ref_len,
                                time_t *expires_at_out);

#ifdef __cplusplus
}
#endif
