#ifndef GUI_CAPTURE_H
#define GUI_CAPTURE_H

#include "../core/gui_app.h"

// Global exit flag (defined in misrc_gui.c)
extern volatile atomic_int do_exit;

// Capture callback function
void gui_capture_callback(void *data_info);

// Enable/disable audio capture in the hsdaoh callback.
// Normally audio is enabled during capture for monitoring.
void gui_capture_set_audio_capture(bool enabled);

// Note: Audio buffer now accessed via app->buffers (buffer_manager)
// Use BUF_CAPTURE_AUDIO with bufmgr_read_begin/bufmgr_read_end

// Check if device has timed out (no callbacks for too long)
// Returns true if device appears disconnected
bool gui_capture_device_timeout(gui_app_t *app, uint32_t timeout_ms);
// Detect a live USB stream that delivers no valid RF payload to the ring.
bool gui_capture_rf_payload_timeout(gui_app_t *app, uint32_t timeout_ms);

// Request a capture stop due to a detected dropout/error. No-op unless the
// user has enabled stop-on-dropout. Reason is recorded for the UI status line.
// Shared by hsdaoh, FX3, and DdD capture backends.
void gui_capture_request_dropout_stop(gui_app_t *app, gui_dropout_reason_t reason);

// UI thread: apply cached hsdaoh-rp2350 status/errors at a low rate (e.g. every 2s)
// Major HW issues will be obvious
void gui_capture_poll_hsdaoh_status(gui_app_t *app);


// Generic SDR live-retune dispatch. Updates the active SDR backend's center
// frequency to hz and persists it; live-retunes if capture is running.
// Device-agnostic: dispatches to whichever SDR backend is active (RTL-SDR
// today; add a branch here when a second SDR backend is added). Returns 0 on
// success, -1 if no SDR backend is active.
int gui_app_set_sdr_frequency(gui_app_t *app, uint64_t hz);

// --- Level autostop helpers (device-aware level scaling) ---
// The level autostop compares max(peak_a_pos, peak_a_neg) against a normalized
// 0.1-0.8 threshold. Peak atomics are NOT on a single common scale across
// backends (hsdaoh/cxadc/FX3/simulated/rtlsdr/simple_capture normalize to the
// 12-bit container = 2048; DdD stores native 10-bit magnitude = 512; 8-bit FLAC
// playback stays at 128). These helpers return the active capture mode's peak
// full-scale counts and ADC Vpp so the threshold + mV readout are correct.

// Peak full-scale counts for the active capture mode (2048 / 512 / 128).
uint16_t gui_app_level_autostop_full_scale(const gui_app_t *app);

// Per-backend default ADC full-scale Vpp (hsdaoh 2.0, CXADC 2.0, DdD 2.0,
// FX3 1.0, others 2.0). hsdaoh is hardware-selectable 1/2 Vpp; the stored
// level_autostop_vpp_hsdaoh memory overrides this default for hsdaoh.
float gui_app_level_autostop_default_vpp(const gui_app_t *app);

// Effective ADC full-scale Vpp for the mV readout (settings.level_autostop_vpp,
// clamped to a sane positive minimum). Re-defaulted on device change by UI.
float gui_app_level_autostop_vpp(const gui_app_t *app);

#endif // GUI_CAPTURE_H
