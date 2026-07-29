#pragma once
// =============================================================================
// portal.h — Wi-Fi setup portal (captive AP + HTTP server)
// =============================================================================
// When no Wi-Fi credentials are stored, the device creates an access point
// "LM-Knob-Setup" and serves a simple web page at 192.168.4.1 where the
// user can enter their SSID and password.
//
// Once credentials are saved, the portal is torn down and the device connects
// to the user's Wi-Fi.  Wi-Fi is only needed for OTA updates; BLE operation
// is fully local.
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*portal_done_cb_t)(void);

/** Start captive AP + HTTP server. cb is called when credentials are saved. */
void portal_start(portal_done_cb_t cb);

/** Stop the portal and AP. */
void portal_stop(void);

#ifdef __cplusplus
}
#endif
