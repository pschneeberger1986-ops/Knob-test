// =============================================================================
// cloud_lm.c — La Marzocco Cloud API (cloud-only, no BLE)
// =============================================================================
// Auth protocol (from pylamarzocco):
//   1. Generate ECDSA P-256 key pair + UUID4 installation_id — stored in NVS.
//   2. Derive 32-byte secret from (installation_id, public_key_der).
//   3. Register: POST /auth/init  → X-App-Installation-Id + X-Request-Proof.
//   4. Sign-in:  POST /auth/signin → access + refresh tokens.
//   5. All calls: signed headers + Authorization: Bearer <token>.
//
// Machine state: GET /things/{serial}/dashboard  (polled every 5 s)
// Commands:      POST /things/{serial}/command/{CommandName}
// =============================================================================

#include "cloud_lm.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "cloud_lm";

#define CLOUD_BASE      "https://lion.lamarzocco.io/api/customer-app"
#define TOKEN_EXPIRY    3600   // seconds
#define TOKEN_MARGIN    600    // refresh 10 min before expiry
#define HTTP_RESP_CAP   8192   // dashboard response can be large

// ─── Installation key ─────────────────────────────────────────────────────────

typedef struct {
    char    installation_id[40];
    uint8_t secret[32];
    uint8_t priv_der[200];
    size_t  priv_len;
    uint8_t pub_der[100];
    size_t  pub_len;
} install_key_t;

static install_key_t s_key      = {};
static bool          s_key_ready = false;

// ─── Auth state ───────────────────────────────────────────────────────────────

static char   s_access_token[2048]  = {};
static char   s_refresh_token[2048] = {};
static time_t s_token_expires_at    = 0;
static char   s_email[128]          = {};
static char   s_password[128]       = {};
static char   s_serial[32]          = {};
static bool   s_authenticated       = false;

// ─── State cache + callback ───────────────────────────────────────────────────

static lm_machine_state_t   s_state        = {};
static lm_cloud_state_cb_t  s_state_cb     = NULL;
static SemaphoreHandle_t    s_state_mutex  = NULL;

// ─── Crypto helpers ───────────────────────────────────────────────────────────

static void gen_uuid4(char *out) /* out must be >= 37 bytes */
{
    uint8_t b[16];
    esp_fill_random(b, 16);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
        b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}

static char *b64_enc(const uint8_t *data, size_t len)
{
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, data, len);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    mbedtls_base64_encode((unsigned char *)out, olen + 1, &olen, data, len);
    out[olen] = '\0';
    return out;
}

static void sha256_buf(const uint8_t *data, size_t len, uint8_t out[32])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

// La Marzocco Y5.e proof algorithm
static char *generate_proof(const char *base_string, const uint8_t secret32[32])
{
    uint8_t work[32];
    memcpy(work, secret32, 32);
    const uint8_t *bs = (const uint8_t *)base_string;
    size_t bsl = strlen(base_string);
    for (size_t i = 0; i < bsl; i++) {
        uint8_t bv = bs[i];
        int idx   = bv % 32;
        int si    = (idx + 1) % 32;
        int shift = work[si] & 7;
        uint8_t xv = bv ^ work[idx];
        uint8_t rv = shift ? ((xv << shift) | (xv >> (8 - shift))) & 0xFF : xv;
        work[idx] = rv;
    }
    uint8_t hash[32];
    sha256_buf(work, 32, hash);
    return b64_enc(hash, 32);
}

// ─── Key generation ───────────────────────────────────────────────────────────

static esp_err_t generate_install_key(void)
{
    mbedtls_pk_context      pk;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&rng);

    esp_err_t ret = ESP_FAIL;

    if (mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &ent, NULL, 0) != 0) goto done;
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) goto done;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                             mbedtls_ctr_drbg_random, &rng) != 0) goto done;

    uint8_t pub_buf[100];
    int pub_len = mbedtls_pk_write_pubkey_der(&pk, pub_buf, sizeof(pub_buf));
    if (pub_len < 0) goto done;
    uint8_t *pub_start = pub_buf + sizeof(pub_buf) - pub_len;

    uint8_t priv_buf[200];
    int priv_len = mbedtls_pk_write_key_der(&pk, priv_buf, sizeof(priv_buf));
    if (priv_len < 0) goto done;
    uint8_t *priv_start = priv_buf + sizeof(priv_buf) - priv_len;

    gen_uuid4(s_key.installation_id);

    // Derive secret = SHA256("{inst_id}.{base64(pub)}.{base64(SHA256(inst_id))}")
    char *pub_b64 = b64_enc(pub_start, pub_len);
    if (!pub_b64) goto done;
    uint8_t inst_hash[32];
    sha256_buf((uint8_t *)s_key.installation_id, strlen(s_key.installation_id), inst_hash);
    char *inst_hash_b64 = b64_enc(inst_hash, 32);
    if (!inst_hash_b64) { free(pub_b64); goto done; }
    char triple[768];
    snprintf(triple, sizeof(triple), "%s.%s.%s",
             s_key.installation_id, pub_b64, inst_hash_b64);
    free(pub_b64); free(inst_hash_b64);
    sha256_buf((uint8_t *)triple, strlen(triple), s_key.secret);

    memcpy(s_key.pub_der,  pub_start,  pub_len);  s_key.pub_len  = pub_len;
    memcpy(s_key.priv_der, priv_start, priv_len); s_key.priv_len = priv_len;

    storage_save_install_key(s_key.installation_id, s_key.secret,
                              s_key.priv_der, s_key.priv_len,
                              s_key.pub_der,  s_key.pub_len);
    s_key_ready = true;
    ESP_LOGI(TAG, "Key generated: %s", s_key.installation_id);
    ret = ESP_OK;

done:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&ent);
    return ret;
}

// ─── Signed request headers ───────────────────────────────────────────────────

typedef struct {
    char inst_id[40];
    char timestamp[20];
    char nonce[40];
    char signature[384];
} auth_headers_t;

static esp_err_t gen_signed_headers(auth_headers_t *h)
{
    if (!s_key_ready) return ESP_ERR_INVALID_STATE;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long ts_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    snprintf(h->timestamp, sizeof(h->timestamp), "%lld", ts_ms);
    gen_uuid4(h->nonce);
    strncpy(h->inst_id, s_key.installation_id, sizeof(h->inst_id));

    char proof_input[128];
    snprintf(proof_input, sizeof(proof_input), "%s.%s.%s",
             s_key.installation_id, h->nonce, h->timestamp);

    char *proof = generate_proof(proof_input, s_key.secret);
    if (!proof) return ESP_ERR_NO_MEM;

    char sig_data[640];
    snprintf(sig_data, sizeof(sig_data), "%s.%s", proof_input, proof);
    free(proof);

    mbedtls_pk_context      pk;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&rng);

    esp_err_t ret = ESP_FAIL;
    if (mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &ent, NULL, 0) != 0) goto hclean;
    if (mbedtls_pk_parse_key(&pk, s_key.priv_der, s_key.priv_len,
                              NULL, 0, mbedtls_ctr_drbg_random, &rng) != 0) goto hclean;

    uint8_t hash[32];
    sha256_buf((uint8_t *)sig_data, strlen(sig_data), hash);

    uint8_t sig_der[128];
    size_t  sig_der_len = 0;
    if (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, 32,
                        sig_der, sizeof(sig_der), &sig_der_len,
                        mbedtls_ctr_drbg_random, &rng) != 0) goto hclean;

    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)h->signature, sizeof(h->signature),
                          &olen, sig_der, sig_der_len);
    h->signature[olen] = '\0';
    ret = ESP_OK;

hclean:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&ent);
    return ret;
}

// ─── HTTP helpers ─────────────────────────────────────────────────────────────

typedef struct { char buf[HTTP_RESP_CAP]; int len; } http_resp_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int space = HTTP_RESP_CAP - r->len - 1;
        int copy  = evt->data_len < space ? evt->data_len : space;
        if (copy > 0) {
            memcpy(r->buf + r->len, evt->data, copy);
            r->len += copy;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

typedef struct { const char *key; const char *val; } hdr_kv_t;

static int http_request(esp_http_client_method_t method,
                         const char *url,
                         const char *json_body,  /* NULL for GET */
                         const hdr_kv_t *hdrs, int n_hdrs,
                         http_resp_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_cb,
        .user_data         = resp,
        .timeout_ms        = 10000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    if (json_body)
        esp_http_client_set_header(c, "Content-Type", "application/json");
    for (int i = 0; i < n_hdrs; i++)
        esp_http_client_set_header(c, hdrs[i].key, hdrs[i].val);
    if (json_body)
        esp_http_client_set_post_field(c, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(c);
    int status = -1;
    if (err == ESP_OK)
        status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return status;
}

// ─── Registration ─────────────────────────────────────────────────────────────

static esp_err_t register_client(void)
{
    uint8_t pub_hash[32];
    sha256_buf(s_key.pub_der, s_key.pub_len, pub_hash);
    char *pub_hash_b64 = b64_enc(pub_hash, 32);
    if (!pub_hash_b64) return ESP_ERR_NO_MEM;

    char base_string[256];
    snprintf(base_string, sizeof(base_string), "%s.%s",
             s_key.installation_id, pub_hash_b64);
    free(pub_hash_b64);

    char *proof = generate_proof(base_string, s_key.secret);
    if (!proof) return ESP_ERR_NO_MEM;

    char *pub_b64 = b64_enc(s_key.pub_der, s_key.pub_len);
    if (!pub_b64) { free(proof); return ESP_ERR_NO_MEM; }

    char body[512];
    snprintf(body, sizeof(body), "{\"pk\":\"%s\"}", pub_b64);
    free(pub_b64);

    hdr_kv_t hdrs[] = {
        { "X-App-Installation-Id", s_key.installation_id },
        { "X-Request-Proof",       proof },
    };
    http_resp_t resp;
    int status = http_request(HTTP_METHOD_POST, CLOUD_BASE "/auth/init",
                               body, hdrs, 2, &resp);
    free(proof);

    if (status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Client registered (HTTP %d)", status);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Registration failed HTTP %d: %s", status, resp.buf);
    return ESP_FAIL;
}

// ─── Token management ─────────────────────────────────────────────────────────

static esp_err_t do_signin(void)
{
    auth_headers_t ah;
    if (gen_signed_headers(&ah) != ESP_OK) return ESP_FAIL;

    char body[400];
    snprintf(body, sizeof(body),
             "{\"username\":\"%s\",\"password\":\"%s\"}", s_email, s_password);

    hdr_kv_t hdrs[] = {
        { "X-App-Installation-Id", ah.inst_id   },
        { "X-Timestamp",           ah.timestamp },
        { "X-Nonce",               ah.nonce     },
        { "X-Request-Signature",   ah.signature },
    };
    http_resp_t resp;
    int status = http_request(HTTP_METHOD_POST, CLOUD_BASE "/auth/signin",
                               body, hdrs, 4, &resp);
    if (status != 200) {
        ESP_LOGE(TAG, "Sign-in failed HTTP %d: %.200s", status, resp.buf);
        return ESP_FAIL;
    }

    cJSON *j = cJSON_Parse(resp.buf);
    if (!j) return ESP_FAIL;
    const char *acc = cJSON_GetStringValue(cJSON_GetObjectItem(j, "accessToken"));
    const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(j, "refreshToken"));
    if (!acc || !ref) { cJSON_Delete(j); return ESP_FAIL; }
    strncpy(s_access_token,  acc, sizeof(s_access_token)  - 1);
    strncpy(s_refresh_token, ref, sizeof(s_refresh_token) - 1);
    s_token_expires_at = time(NULL) + TOKEN_EXPIRY;
    cJSON_Delete(j);
    storage_save_cloud_tokens(s_access_token, s_refresh_token, s_token_expires_at);
    ESP_LOGI(TAG, "Signed in OK");
    return ESP_OK;
}

static esp_err_t do_refresh(void)
{
    auth_headers_t ah;
    if (gen_signed_headers(&ah) != ESP_OK) return ESP_FAIL;

    char body[2400];
    snprintf(body, sizeof(body),
             "{\"username\":\"%s\",\"refreshToken\":\"%s\"}", s_email, s_refresh_token);

    hdr_kv_t hdrs[] = {
        { "X-App-Installation-Id", ah.inst_id   },
        { "X-Timestamp",           ah.timestamp },
        { "X-Nonce",               ah.nonce     },
        { "X-Request-Signature",   ah.signature },
    };
    http_resp_t resp;
    int status = http_request(HTTP_METHOD_POST, CLOUD_BASE "/auth/refreshtoken",
                               body, hdrs, 4, &resp);
    if (status != 200) { ESP_LOGW(TAG, "Refresh failed HTTP %d", status); return ESP_FAIL; }

    cJSON *j = cJSON_Parse(resp.buf);
    if (!j) return ESP_FAIL;
    const char *acc = cJSON_GetStringValue(cJSON_GetObjectItem(j, "accessToken"));
    const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(j, "refreshToken"));
    if (acc) strncpy(s_access_token,  acc, sizeof(s_access_token)  - 1);
    if (ref) strncpy(s_refresh_token, ref, sizeof(s_refresh_token) - 1);
    s_token_expires_at = time(NULL) + TOKEN_EXPIRY;
    cJSON_Delete(j);
    storage_save_cloud_tokens(s_access_token, s_refresh_token, s_token_expires_at);
    ESP_LOGI(TAG, "Token refreshed");
    return ESP_OK;
}

static esp_err_t ensure_token(void)
{
    time_t now = time(NULL);
    if (s_access_token[0] && now < s_token_expires_at - TOKEN_MARGIN) return ESP_OK;
    if (s_refresh_token[0] && now < s_token_expires_at)
        if (do_refresh() == ESP_OK) return ESP_OK;
    return do_signin();
}

// ─── Authenticated request ────────────────────────────────────────────────────

static int auth_request(esp_http_client_method_t method,
                         const char *url,
                         const char *json_body,
                         http_resp_t *resp)
{
    if (ensure_token() != ESP_OK) return -1;

    auth_headers_t ah;
    if (gen_signed_headers(&ah) != ESP_OK) return -1;

    char auth_hdr[2200];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_access_token);

    hdr_kv_t hdrs[] = {
        { "X-App-Installation-Id", ah.inst_id    },
        { "X-Timestamp",           ah.timestamp  },
        { "X-Nonce",               ah.nonce      },
        { "X-Request-Signature",   ah.signature  },
        { "Authorization",         auth_hdr      },
    };
    return http_request(method, url, json_body, hdrs, 5, resp);
}

// ─── Public: auth ─────────────────────────────────────────────────────────────

esp_err_t lm_cloud_authenticate(const char *email, const char *password)
{
    strncpy(s_email,    email,    sizeof(s_email)    - 1);
    strncpy(s_password, password, sizeof(s_password) - 1);

    if (!s_key_ready) {
        if (storage_has_install_key()) {
            storage_load_install_key(s_key.installation_id, s_key.secret,
                                     s_key.priv_der, &s_key.priv_len,
                                     s_key.pub_der,  &s_key.pub_len);
            s_key_ready = true;
            ESP_LOGI(TAG, "Install key loaded: %s", s_key.installation_id);
        } else {
            ESP_LOGI(TAG, "Generating new installation key…");
            if (generate_install_key() != ESP_OK) return ESP_FAIL;
            ESP_LOGI(TAG, "Registering with LM cloud…");
            if (register_client() != ESP_OK) return ESP_FAIL;
        }
    }

    // Try cached tokens first
    if (!s_access_token[0]) {
        time_t exp = 0;
        storage_load_cloud_tokens(s_access_token, sizeof(s_access_token),
                                   s_refresh_token, sizeof(s_refresh_token), &exp);
        s_token_expires_at = exp;
    }

    esp_err_t err = ensure_token();
    if (err == ESP_OK) s_authenticated = true;
    return err;
}

bool lm_cloud_is_authenticated(void) { return s_authenticated; }

void lm_cloud_set_serial(const char *serial)
{
    strncpy(s_serial, serial, sizeof(s_serial) - 1);
}

// ─── Poll machine state ───────────────────────────────────────────────────────

esp_err_t lm_cloud_poll_state(lm_machine_state_t *out)
{
    if (!s_serial[0]) return ESP_ERR_INVALID_STATE;

    char url[200];
    snprintf(url, sizeof(url), CLOUD_BASE "/things/%s/dashboard", s_serial);

    http_resp_t *resp = malloc(sizeof(http_resp_t));
    if (!resp) return ESP_ERR_NO_MEM;

    int status = auth_request(HTTP_METHOD_GET, url, NULL, resp);
    if (status != 200) {
        ESP_LOGW(TAG, "Dashboard poll failed HTTP %d", status);
        free(resp);
        return ESP_FAIL;
    }

    // Parse widgets
    cJSON *root = cJSON_Parse(resp->buf);
    free(resp);
    if (!root) return ESP_FAIL;

    lm_machine_state_t st = {};
    st.coffee_temp_target = 93.0f; // default

    cJSON *widgets = cJSON_GetObjectItem(root, "widgets");
    cJSON *w;
    cJSON_ArrayForEach(w, widgets) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(w, "code"));
        cJSON *output    = cJSON_GetObjectItem(w, "output");
        if (!code || !output) continue;

        if (strcmp(code, "CMMachineStatus") == 0) {
            const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(output, "mode"));
            st.powered_on = mode && strcmp(mode, "BrewingMode") == 0;

        } else if (strcmp(code, "CMCoffeeBoiler") == 0) {
            cJSON *tgt = cJSON_GetObjectItem(output, "targetTemperature");
            if (tgt) st.coffee_temp_target = (float)tgt->valuedouble;
            const char *sts = cJSON_GetStringValue(cJSON_GetObjectItem(output, "status"));
            st.coffee_boiler_ready = sts && strcmp(sts, "Ready") == 0;
            // Current temp not in REST; approximate: show target when ready
            st.coffee_temp_current = st.coffee_boiler_ready ? st.coffee_temp_target : 0.0f;

        } else if (strcmp(code, "CMSteamBoilerLevel") == 0) {
            const char *lvl = cJSON_GetStringValue(cJSON_GetObjectItem(output, "targetLevel"));
            const char *sts = cJSON_GetStringValue(cJSON_GetObjectItem(output, "status"));
            cJSON *ena = cJSON_GetObjectItem(output, "enabled");
            if (ena && cJSON_IsTrue(ena)) {
                if (lvl) {
                    if      (strcmp(lvl, "Level1") == 0) st.steam_level = 1;
                    else if (strcmp(lvl, "Level2") == 0) st.steam_level = 2;
                    else if (strcmp(lvl, "Level3") == 0) st.steam_level = 3;
                    else                                 st.steam_level = 2;
                }
                st.steam_boiler_ready = sts && strcmp(sts, "Ready") == 0;
            }

        } else if (strcmp(code, "CMSteamBoilerTemperature") == 0) {
            cJSON *tgt = cJSON_GetObjectItem(output, "targetTemperature");
            if (tgt) st.steam_temp_target = (float)tgt->valuedouble;
            // no current temp in REST
            st.steam_temp_current = st.steam_boiler_ready ? st.steam_temp_target : 0.0f;

        } else if (strcmp(code, "CMNoWater") == 0) {
            cJSON *al = cJSON_GetObjectItem(output, "allarm");
            st.water_tank_empty = al && cJSON_IsTrue(al);
        }
    }

    cJSON_Delete(root);

    if (out) *out = st;

    // Cache & callback
    if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = st;
    if (s_state_mutex) xSemaphoreGive(s_state_mutex);

    if (s_state_cb) s_state_cb(&st);
    return ESP_OK;
}

// ─── Commands ─────────────────────────────────────────────────────────────────

static esp_err_t send_command(const char *command, const char *json_body)
{
    if (!s_serial[0]) return ESP_ERR_INVALID_STATE;
    char url[200];
    snprintf(url, sizeof(url), CLOUD_BASE "/things/%s/command/%s", s_serial, command);
    http_resp_t resp;
    int status = auth_request(HTTP_METHOD_POST, url, json_body, &resp);
    if (status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Command %s OK (HTTP %d)", command, status);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Command %s failed HTTP %d: %.100s", command, status, resp.buf);
    return ESP_FAIL;
}

esp_err_t lm_cloud_set_power(bool on)
{
    const char *mode = on ? "BrewingMode" : "StandBy";
    char body[64];
    snprintf(body, sizeof(body), "{\"mode\":\"%s\"}", mode);
    return send_command("CoffeeMachineChangeMode", body);
}

esp_err_t lm_cloud_set_coffee_temp(float celsius)
{
    char body[80];
    snprintf(body, sizeof(body),
             "{\"boilerIndex\":1,\"targetTemperature\":%.1f}", celsius);
    return send_command("CoffeeMachineSettingCoffeeBoilerTargetTemperature", body);
}

esp_err_t lm_cloud_set_steam_level(int level)
{
    const char *lvl_str = (level == 1) ? "Level1" : (level == 3) ? "Level3" : "Level2";
    char body[80];
    snprintf(body, sizeof(body),
             "{\"boilerIndex\":1,\"targetLevel\":\"%s\"}", lvl_str);
    return send_command("CoffeeMachineSettingSteamBoilerTargetLevel", body);
}

esp_err_t lm_cloud_backflush(void)
{
    return send_command("CoffeeMachineBackFlushStartCleaning", "{\"enabled\":true}");
}

// ─── Background tasks ─────────────────────────────────────────────────────────

static void backflush_task(void *arg)
{
    ESP_LOGI(TAG, "Sending backflush…");
    if (lm_cloud_backflush() != ESP_OK)
        ESP_LOGE(TAG, "Backflush failed");
    vTaskDelete(NULL);
}

void lm_cloud_trigger_backflush(void)
{
    xTaskCreate(backflush_task, "backflush", 8192, NULL, 2, NULL);
}

static void poll_task(void *arg)
{
    lm_cloud_state_cb_t cb = (lm_cloud_state_cb_t)arg;
    s_state_cb = cb;
    while (1) {
        lm_cloud_poll_state(NULL);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void auth_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(4000));   // wait for DHCP + SNTP

    char email[128] = {}, pass[128] = {}, serial[32] = {};
    storage_load_lm_credentials(email, sizeof(email), pass, sizeof(pass),
                                 serial, sizeof(serial));
    if (!email[0]) {
        ESP_LOGW(TAG, "No LM credentials – cloud disabled");
        vTaskDelete(NULL);
        return;
    }
    lm_cloud_set_serial(serial);

    if (lm_cloud_authenticate(email, pass) != ESP_OK) {
        ESP_LOGE(TAG, "Cloud auth failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Cloud auth OK, serial=%s", serial);

    // Start state polling
    s_state_mutex = xSemaphoreCreateMutex();
    lm_cloud_state_cb_t cb = (lm_cloud_state_cb_t)arg;
    xTaskCreate(poll_task, "cloud_poll", 8192, cb, 2, NULL);

    vTaskDelete(NULL);
}

void lm_cloud_start(lm_cloud_state_cb_t state_cb)
{
    xTaskCreate(auth_task, "cloud_auth", 8192, state_cb, 2, NULL);
}
