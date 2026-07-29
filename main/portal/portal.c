// =============================================================================
// portal.c — Wi-Fi setup captive portal (with DNS hijack)
// =============================================================================

#include "portal.h"
#include "storage.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "portal";

static httpd_handle_t  s_server   = NULL;
static portal_done_cb_t s_done_cb = NULL;
static TaskHandle_t    s_dns_task = NULL;

// ─── HTML page ────────────────────────────────────────────────────────────────

static const char HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>LM Knob – Setup</title>"
"<style>"
"  body{font-family:sans-serif;background:#111;color:#eee;"
"       display:flex;flex-direction:column;align-items:center;"
"       padding:2rem;}"
"  h1{color:#e8c87a;margin-bottom:.5rem;}"
"  h2{color:#aaa;font-size:.9rem;font-weight:normal;margin:0 0 1.5rem;}"
"  label{display:block;margin-top:1rem;font-size:.9rem;color:#aaa;}"
"  input{display:block;width:100%;max-width:320px;padding:.6rem;"
"        background:#222;color:#eee;border:1px solid #444;"
"        border-radius:6px;font-size:1rem;margin-top:.3rem;box-sizing:border-box;}"
"  hr{border:none;border-top:1px solid #333;width:100%;max-width:320px;margin:1.5rem 0 .5rem;}"
"  button{margin-top:1.5rem;padding:.7rem 2rem;"
"         background:#e8c87a;color:#111;border:none;"
"         border-radius:6px;font-size:1rem;cursor:pointer;}"
"  .note{margin-top:1rem;font-size:.78rem;color:#555;text-align:center;max-width:320px;}"
"</style></head><body>"
"<h1>☕ LM Knob Setup</h1>"
"<h2>First-time configuration</h2>"
"<form method='POST' action='/save'>"
"  <label>Wi-Fi network (SSID)"
"    <input name='ssid' type='text' placeholder='MyNetwork' required>"
"  </label>"
"  <label>Wi-Fi password"
"    <input name='wpass' type='password' placeholder='••••••••'>"
"  </label>"
"  <hr>"
"  <label>La Marzocco account email"
"    <input name='lm_email' type='email' placeholder='you@example.com' required>"
"  </label>"
"  <label>La Marzocco account password"
"    <input name='lm_pass' type='password' placeholder='••••••••' required>"
"  </label>"
"  <label>Machine serial number"
"    <input name='lm_serial' type='text' placeholder='LM01234' required>"
"  </label>"
"  <hr>"
"  <label>Friendly name <span style='color:#666;font-size:.8em;'>(optional)</span>"
"    <input name='friendly_name' type='text' placeholder='e.g. Kitchen Linea Mini'>"
"  </label>"
"  <button type='submit'>Save &amp; Connect</button>"
"</form>"
"<p class='note'>Wi-Fi and the LM account are needed for backflush and cloud features.<br>"
"The friendly name appears on the device screen instead of LA MARZOCCO.</p>"
"</body></html>";

static const char HTML_OK[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Saved</title>"
"<style>body{font-family:sans-serif;background:#111;color:#eee;"
"text-align:center;padding:3rem;}h1{color:#7ac87a;}</style>"
"</head><body><h1>✓ Saved!</h1>"
"<p>The device will now connect to your network.<br>"
"You can close this page.</p></body></html>";

// ─── HTTP handlers ────────────────────────────────────────────────────────────

static esp_err_t handle_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML, sizeof(HTML) - 1);
    return ESP_OK;
}

// Very small URL-decode (handles %XX and + → space)
static void urldecode(char *dst, const char *src, size_t maxlen)
{
    size_t i = 0;
    while (*src && i < maxlen - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' '; src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = 0;
}

static esp_err_t handle_post(httpd_req_t *req)
{
    char body[512] = {};
    int  received  = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return ESP_FAIL;

    char ssid_enc[128] = {}, wpass_enc[128] = {};
    char lm_email_enc[128] = {}, lm_pass_enc[128] = {}, lm_serial_enc[64] = {};
    char fname_enc[128] = {};
    char ssid[64] = {}, wpass[64] = {};
    char lm_email[128] = {}, lm_pass[128] = {}, lm_serial[32] = {};
    char friendly_name[64] = {};

    httpd_query_key_value(body, "ssid",          ssid_enc,      sizeof(ssid_enc));
    httpd_query_key_value(body, "wpass",         wpass_enc,     sizeof(wpass_enc));
    httpd_query_key_value(body, "lm_email",      lm_email_enc,  sizeof(lm_email_enc));
    httpd_query_key_value(body, "lm_pass",       lm_pass_enc,   sizeof(lm_pass_enc));
    httpd_query_key_value(body, "lm_serial",     lm_serial_enc, sizeof(lm_serial_enc));
    httpd_query_key_value(body, "friendly_name", fname_enc,     sizeof(fname_enc));

    urldecode(ssid,          ssid_enc,      sizeof(ssid));
    urldecode(wpass,         wpass_enc,     sizeof(wpass));
    urldecode(lm_email,      lm_email_enc,  sizeof(lm_email));
    urldecode(lm_pass,       lm_pass_enc,   sizeof(lm_pass));
    urldecode(lm_serial,     lm_serial_enc, sizeof(lm_serial));
    urldecode(friendly_name, fname_enc,     sizeof(friendly_name));

    ESP_LOGI(TAG, "Saving Wi-Fi: ssid=%s", ssid);
    storage_save_wifi(ssid, wpass);

    ESP_LOGI(TAG, "Saving LM credentials: email=%s serial=%s", lm_email, lm_serial);
    storage_save_lm_credentials(lm_email, lm_pass, lm_serial);

    if (friendly_name[0] != '\0') {
        ESP_LOGI(TAG, "Saving friendly name: %s", friendly_name);
        storage_save_friendly_name(friendly_name);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_OK, sizeof(HTML_OK) - 1);

    // Give the response time to reach the browser
    vTaskDelay(pdMS_TO_TICKS(500));
    if (s_done_cb) s_done_cb();
    return ESP_OK;
}

// Redirect anything else to root (triggers captive portal detection on iOS/Android)
static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ─── DNS hijack server ────────────────────────────────────────────────────────
// Responds to every DNS query with 192.168.4.1, which triggers the captive
// portal notification on iOS and Android automatically.

#define DNS_PORT 53
#define DNS_BUF  512

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS hijack server running on port 53");

    uint8_t buf[DNS_BUF];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf) - 28, 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 12) continue;

        // Flip QR bit to make it a response, set AA bit
        buf[2] = 0x81; // QR=1, Opcode=0, AA=1, TC=0, RD=1
        buf[3] = 0x80; // RA=1, RCODE=0 (no error)
        // Keep QDCOUNT, clear ANCOUNT/NSCOUNT/ARCOUNT then set ANCOUNT=1
        buf[6]  = 0x00; buf[7]  = 0x01; // ANCOUNT = 1
        buf[8]  = 0x00; buf[9]  = 0x00; // NSCOUNT = 0
        buf[10] = 0x00; buf[11] = 0x00; // ARCOUNT = 0

        // Append answer record after the original question section
        uint8_t *ans = buf + len;
        ans[0]  = 0xC0; ans[1]  = 0x0C; // Name: pointer to offset 12 (question name)
        ans[2]  = 0x00; ans[3]  = 0x01; // TYPE A
        ans[4]  = 0x00; ans[5]  = 0x01; // CLASS IN
        ans[6]  = 0x00; ans[7]  = 0x00;
        ans[8]  = 0x00; ans[9]  = 0x3C; // TTL = 60 seconds
        ans[10] = 0x00; ans[11] = 0x04; // RDLENGTH = 4
        ans[12] = 192;  ans[13] = 168;
        ans[14] = 4;    ans[15] = 1;    // 192.168.4.1

        sendto(sock, buf, len + 16, 0, (struct sockaddr *)&client, clen);
    }

    close(sock);
    vTaskDelete(NULL);
}

// ─── AP + server lifecycle ────────────────────────────────────────────────────

void portal_start(portal_done_cb_t cb)
{
    s_done_cb = cb;

    // Create AP
    esp_netif_create_default_wifi_ap();
    wifi_config_t cfg = {
        .ap = {
            .ssid            = "LM-Knob-Setup",
            .ssid_len        = 0,
            .password        = "",
            .max_connection  = 4,
            .authmode        = WIFI_AUTH_OPEN,
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP started: LM-Knob-Setup (open)");

    // DNS hijack — must start before HTTP server
    xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, &s_dns_task);

    // HTTP server
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_server, &http_cfg));

    httpd_uri_t root     = { .uri="/",    .method=HTTP_GET,  .handler=handle_get      };
    httpd_uri_t post     = { .uri="/save",.method=HTTP_POST, .handler=handle_post     };
    httpd_uri_t catchall = { .uri="/*",   .method=HTTP_GET,  .handler=handle_redirect };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &post);
    httpd_register_uri_handler(s_server, &catchall); // catch-all last
    ESP_LOGI(TAG, "HTTP server started (captive portal active)");
}

void portal_stop(void)
{
    if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
    if (s_server)   { httpd_stop(s_server);    s_server   = NULL; }
    esp_wifi_stop();
}
