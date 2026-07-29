#pragma once
// =============================================================================
// ui.h — LVGL UI public API (cloud-only)
// =============================================================================
// Hardware: Guition JC3636K718C (ESP32-S3, 360×360 round display, knob+button)
//
// Screen layout:
//   HOME     — coffee temp (target), power status, steam status
//   SETTINGS — adjust coffee temp, steam level, backflush (via knob)
// =============================================================================

#include "cloud_lm.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Init ─────────────────────────────────────────────────────────────────────

void ui_init(void);

// ─── Screen switching ─────────────────────────────────────────────────────────

void ui_show_setup(void);    // reserved (can show "connecting..." screen)
void ui_show_home(void);

// ─── Data updates (called from cloud poll callback) ───────────────────────────

/** Push fresh machine state to the home screen. */
void ui_update_state(const lm_machine_state_t *state);

/** Update cloud connection indicator. */
void ui_set_connected(bool connected);

/** Show / hide brewing overlay with running timer. */
void ui_set_brewing(bool brewing);

// ─── Input events (called from knob ISR / task) ───────────────────────────────

void ui_knob_rotate(int delta);    // delta = +1 (CW) or -1 (CCW)
void ui_knob_press(void);          // short press
void ui_knob_long_press(void);     // long press (>1 s) → settings

#ifdef __cplusplus
}
#endif
