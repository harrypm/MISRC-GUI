#include "../input/gui_cxadc_macos.h"
/*
 * MISRC GUI - UI Layout Implementation
 * Clay-based declarative UI layout (Clay v0.14 API)
 */

#include "gui_ui.h"
#if defined(__ANDROID__)
#include "../core/gui_android_viewport.h"
// Gesture-filtered single taps replace native mouse-press events on Android.
#define IsMouseButtonPressed(button) gui_android_viewport_click_pressed(button)
#endif
#include "gui_dropdown.h"
#include "gui_popup.h"
#include "gui_device_label.h"
#include "gui_text_edit.h"
#include "gui_font.h"
#include "../visualization/gui_fft.h"
#include "../visualization/gui_oscilloscope.h"
#include "../signal/gui_cvbs.h"
#include "../visualization/gui_panel.h"
#include "../input/gui_playback.h"
#include "../input/gui_cxadc.h"
#ifdef ENABLE_DDD
#include "../input/gui_ddd_clockgen.h"
#include "../input/gui_ddd_v1.h"
#include "gui_ddd_fifo_status.h"
#endif
#include "gui_device_buffer_status.h"
#include "../output/gui_audio.h"
#include "../output/gui_record.h"
#include "../input/gui_capture.h" // Support hsdoah-rp2350 Error & stats
#include "../net/gui_net.h"
#include "../net/gui_net_queue_limits.h"
#include "../net/gui_monitor_transport.h"
#include "../core/gui_updater.h" // Manifest-driven updater: releases page + auto-patch manifest + confirmed self-update
#include "version.h"
#include "../visualization/gui_custom_elements.h"
#include "../../common/buffer_manager.h"
#include "../../common/file_utils.h"
#include "../../common/threading.h"
#include <clay.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#if defined(__ANDROID__)
extern void android_set_keyboard_visible(int visible);
extern size_t android_drain_text_input(char *out, size_t out_len);
extern const char *android_get_storage_path(void);
#include <pthread.h>
#include <stdatomic.h>
#endif

#ifndef MIRSC_TOOLS_VERSION
#define MIRSC_TOOLS_VERSION "dev"
#endif
#ifndef MIRSC_TOOLS_COPYRIGHT
#define MIRSC_TOOLS_COPYRIGHT "licensed under GNU GPL v3 or later, (c) 2023-2026 Harry Munday, AlessandroAU, Stefan O, Vrunk11, machcnz"
#endif

// Track if UI consumed the current frame's click (prevents click-through)
static bool s_ui_consumed_click = false;
static int s_ui_scale_percent = GUI_UI_SCALE_DEFAULT_PERCENT;
static double s_ui_scale_hud_visible_until_s = 0.0;
static char s_ui_scale_hud_title[32] = "UI Scale 100%";
static bool s_toolbar_uses_two_rows = false;
static char s_device_option_labels[MAX_DEVICES][64];
// Authoritative capture mode selected by user via CaptureModeToggle.
// Keeping this outside gui_app_t protects mode from unrelated runtime mutations.
static bool s_capture_mode_state_initialized = false;
static bool s_capture_mode_state_misrc = true;
static bool s_capture_mode_trace_initialized = false;
static bool s_capture_mode_trace_last_ui = true;
static bool s_capture_mode_trace_last_user = true;
static bool s_capture_mode_trace_last_runtime = true;
static bool s_capture_mode_trace_last_settings = true;
static bool s_capture_mode_trace_last_recording = false;
static bool s_capture_mode_trace_last_capturing = false;
static bool s_capture_mode_render_trace_initialized = false;
static bool s_capture_mode_render_last_mode = true;
static bool s_capture_mode_render_last_user = true;
static bool s_capture_mode_render_last_runtime = true;
static bool s_capture_mode_render_last_settings = true;
static bool s_capture_mode_render_last_recording = false;
static bool s_capture_mode_render_last_capturing = false;
static bool s_capture_mode_render_last_source_runtime = false;
static bool s_capture_b_forced_off_by_single_channel = false;
// Track whether the clockgen audio outputs (CH3/headswitch + stereo CH1/2)
// were auto-enabled by a clockgen profile (CXADC Clockgen, MISRC Clockgen, or
// DdD Clockgen). When leaving clockgen mode for a non-clockgen device (MISRC/
// HSDAOH/etc.), if these were auto-enabled (not manually by the user), disable
// them so they don't persist into a mode that has no clockgen audio feed.
static bool s_clockgen_audio_auto_enabled = false;
static int s_cxadc_dc_anchor_device_index = -1;
static bool s_cxadc_dc_anchor_valid[2] = { false, false };
static int s_cxadc_dc_anchor_raw[2] = { 0, 0 };
static int s_cxadc_dc_relative[2] = { 0, 0 };

void gui_ui_set_scale_percent(int percent)
{
    s_ui_scale_percent = gui_ui_scale_sanitize_percent(percent);
}

float gui_ui_get_scale_factor(void)
{
    return (float)s_ui_scale_percent / 100.0f;
}

void gui_ui_show_scale_hud(int percent)
{
    int sanitized_percent = gui_ui_scale_sanitize_percent(percent);
    snprintf(s_ui_scale_hud_title,
             sizeof(s_ui_scale_hud_title),
             "UI Scale %d%%",
             sanitized_percent);
    s_ui_scale_hud_visible_until_s = GetTime() + GUI_UI_SCALE_HUD_DURATION_S;
}

static int gui_ui_get_base_layout_width(void)
{
#if defined(__APPLE__)
    int width = GetScreenWidth();
#else
    int width = GetRenderWidth();
    if (width <= 0) width = GetScreenWidth();
#endif
    return (width > 0) ? width : 1;
}

static int gui_ui_get_base_layout_height(void)
{
#if defined(__APPLE__)
    int height = GetScreenHeight();
#else
    int height = GetRenderHeight();
    if (height <= 0) height = GetScreenHeight();
#endif
    return (height > 0) ? height : 1;
}

int gui_ui_get_layout_width(void)
{
    int width = (int)ceilf((float)gui_ui_get_base_layout_width() /
                           gui_ui_get_scale_factor());
    return (width > 0) ? width : 1;
}

int gui_ui_get_layout_height(void)
{
    int height = (int)ceilf((float)gui_ui_get_base_layout_height() /
                            gui_ui_get_scale_factor());
    return (height > 0) ? height : 1;
}

Vector2 gui_ui_get_render_scale(void)
{
    float app_scale = gui_ui_get_scale_factor();
    Vector2 render_scale = { app_scale, app_scale };
    int layout_width = gui_ui_get_layout_width();
    int layout_height = gui_ui_get_layout_height();
    int render_width = GetRenderWidth();
    int render_height = GetRenderHeight();
    if (render_width > 0 && layout_width > 0) {
        render_scale.x = (float)render_width / (float)layout_width;
    }
    if (render_height > 0 && layout_height > 0) {
        render_scale.y = (float)render_height / (float)layout_height;
    }
    return render_scale;
}

Vector2 gui_ui_get_mouse_position(void)
{
#if defined(__ANDROID__)
    Vector2 position = gui_android_viewport_pointer();
#else
    Vector2 position = GetMousePosition();
#endif
    float scale = gui_ui_get_scale_factor();
    position.x /= scale;
    position.y /= scale;
    return position;
}

static const char *gui_ui_capture_mode_name(bool misrc_mode) {
    return misrc_mode ? "MISRC" : "HSDAOH";
}

static bool gui_ui_selected_device_is_cxadc(const gui_app_t *app, bool *clockgen_mode)
{
    if (clockgen_mode) *clockgen_mode = false;
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;

    const device_info_t *dev = &app->devices[app->selected_device];
    if (dev->type != DEVICE_TYPE_CXADC) return false;

    if (clockgen_mode) {
        *clockgen_mode = (dev->index > 1);
    }
    return true;
}

static bool gui_ui_selected_device_is_cxadc_misrc_clockgen(const gui_app_t *app)
{
    // MISRC Clockgen is now its own device type (DEVICE_TYPE_MISRC_CLOCKGEN).
    // Also accept the legacy CXADC-typed marker serial for older saved selections.
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;

    const device_info_t *dev = &app->devices[app->selected_device];
    if (dev->type == DEVICE_TYPE_MISRC_CLOCKGEN) return true;
    if (dev->type == DEVICE_TYPE_CXADC &&
        strcmp(dev->serial, CXADC_MARKER_SERIAL_2CARD_MISRC_CLOCKGEN) == 0) {
        return true;
    }
    return false;
}

// True iff the selected device is the first-class MISRC Clockgen entry.
static bool gui_ui_selected_device_is_misrc_clockgen(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    return app->devices[app->selected_device].type == DEVICE_TYPE_MISRC_CLOCKGEN;
}

static void format_msps_label(char *dst, size_t dst_len, float khz);
static void gui_ui_warn_low_rate(gui_app_t *app, float rate_khz);
static float gui_ui_cxadc_base_rate_khz(const gui_app_t *app, int card_idx)
{
    if (!app) return 28636.0f;
    if (card_idx < 0 || card_idx > 1) card_idx = 0;
    bool tenbit = app->settings.cxadc_tenbit_mode_card[card_idx];
    // CXADC Clockgen Mod (2-card entry, index > 1): always 40/20 MSPS
    // (40 8-bit, 20 10-bit) — the clockgen drives a true 40 MHz base.
    // Stock single card: always 28.6 MSPS base (the 8-bit crystal rate),
    // NOT halved to 14.3 for 10-bit. 8-bit is the assumed stock default.
    bool clockgen = false;
    if (gui_ui_selected_device_is_cxadc(app, &clockgen) && clockgen) {
        return tenbit ? 20000.0f : 40000.0f;
    }
    // Stock: detect the card's 8-bit rate; fall back to 28.6 MHz. Never
    // return 14.3 (the 10-bit half) as the base — stock is always 28.6.
    // Uses the measured (effective) rate when a probe has run, so a
    // stale sysfs crystal parameter on a modded card cannot cap this.
    uint32_t detected_hz = 0;
    if (gui_cxadc_get_effective_rate_hz(card_idx, false, &detected_hz) &&
        detected_hz > 0) {
        return (float)detected_hz / 1000.0f;
    }
    return 28636.0f;
}
// Tenbit-aware hardware rate: the actual capture rate for the card's current
// tenbit mode. Clockgen mod forces 40/20; stock detects or falls back to
// 28.6 (8-bit) / 14.3 (10-bit). Used as the resample baseline and cycle max.
static float gui_ui_cxadc_hw_rate_for_tenbit(const gui_app_t *app, int card_idx, bool tenbit)
{
    if (!app) return 28636.0f;
    if (card_idx < 0 || card_idx > 1) card_idx = 0;
    bool clockgen = false;
    if (gui_ui_selected_device_is_cxadc(app, &clockgen) && clockgen) {
        return tenbit ? 20000.0f : 40000.0f;
    }
    uint32_t detected_hz = 0;
    if (gui_cxadc_get_effective_rate_hz(card_idx, tenbit, &detected_hz) &&
        detected_hz > 0) {
        return (float)detected_hz / 1000.0f;
    }
    return tenbit ? 14318.0f : 28636.0f;
}
static float gui_ui_cxadc_hw_rate_khz(const gui_app_t *app, int card_idx)
{
    if (!app) return 28636.0f;
    if (card_idx < 0 || card_idx > 1) card_idx = 0;
    return gui_ui_cxadc_hw_rate_for_tenbit(app, card_idx,
        app->settings.cxadc_tenbit_mode_card[card_idx]);
}
// Format a CXADC HW-mode rate label as "HW <number> MSPS".
static void format_cxadc_hw_label(char *dst, size_t dst_len, float khz)
{
    float msps = khz / 1000.0f;
    if (fabsf(msps - roundf(msps)) < 1e-3f)
        snprintf(dst, dst_len, "HW %d MSPS", (int)lroundf(msps));
    else
        snprintf(dst, dst_len, "HW %.1f MSPS", msps);
}
// CXADC rate-box cycle: dedicated HW and SW mode options.
//   HW 8-bit (resample off, tenbit=false, rate=HW 8-bit rate)
//   HW 10-bit (resample off, tenbit=true, rate=HW 10-bit rate)
//   SW <rates> (resample on, tenbit unchanged, rate=downsample target)
// The box is always clickable (never greyed). HW modes show "HW <rate> MSPS";
// SW modes show "<rate> MSPS" via format_msps_label.
// Uses an explicit static position counter so the cycle can't break on
// rate collisions (e.g. SW 20 == HW 10-bit rate on clockgen).
static int s_cxadc_rate_pos[2] = { 0, 0 };
static int s_cxadc_rate_total[2] = { 0, 0 };

static void gui_ui_cxadc_cycle_rate(gui_app_t *app, int card_idx)
{
#if defined(__APPLE__)
    (void)card_idx;gui_app_set_status(app,"macOS CXADC: stock 28.636363 MSPS unsigned8 only; F8 for hardware settings");return;
#endif
    if (!app) return;
    if (card_idx < 0 || card_idx > 1) card_idx = 0;
    bool *tenbit_field = &app->settings.cxadc_tenbit_mode_card[card_idx];
    bool *resample_field = (card_idx == 0) ? &app->settings.enable_resample_a : &app->settings.enable_resample_b;
    float *rate_field = (card_idx == 0) ? &app->settings.resample_rate_a : &app->settings.resample_rate_b;
    uint8_t *bits_field = (card_idx == 0) ? &app->settings.rf_bits_a : &app->settings.rf_bits_b;

    float hw_8bit = gui_ui_cxadc_hw_rate_for_tenbit(app, card_idx, false);
    float hw_10bit = gui_ui_cxadc_hw_rate_for_tenbit(app, card_idx, true);

    // SW downsample targets, filtered by card type.
    float sw_presets[8];
    int sw_count = 0;
    bool stock_base = (hw_8bit < 40000.0f - 0.5f);
    if (stock_base) {
        sw_presets[sw_count++] = 5000.0f;
        sw_presets[sw_count++] = 14300.0f;
        sw_presets[sw_count++] = 17900.0f;
        sw_presets[sw_count++] = 28636.0f;
    } else {
        sw_presets[sw_count++] = 5000.0f;
        sw_presets[sw_count++] = 10000.0f;
        sw_presets[sw_count++] = 20000.0f;
        if (hw_8bit >= 54000.0f - 0.5f) sw_presets[sw_count++] = 27000.0f;
        sw_presets[sw_count++] = 40000.0f;
        if (hw_8bit >= 54000.0f - 0.5f) sw_presets[sw_count++] = 54000.0f;
    }

    int total = 2 + sw_count;

    // If the option set changed (card type / crystal mod), re-sync the
    // position to the current state instead of advancing.
    if (s_cxadc_rate_total[card_idx] != total) {
        s_cxadc_rate_total[card_idx] = total;
        if (!*resample_field) {
            s_cxadc_rate_pos[card_idx] = *tenbit_field ? 1 : 0;
        } else {
            int found = 2;
            for (int i = 0; i < sw_count; i++) {
                if (fabsf(*rate_field - sw_presets[i]) < 1.0f) { found = 2 + i; break; }
            }
            s_cxadc_rate_pos[card_idx] = found;
        }
        return; // re-sync this frame; advance next click
    }

    int next_pos = (s_cxadc_rate_pos[card_idx] + 1) % total;
    s_cxadc_rate_pos[card_idx] = next_pos;

    // Apply next option.
    if (next_pos == 0) {
        *tenbit_field = false;
        *resample_field = false;
        *rate_field = hw_8bit;
        *bits_field = 8;
    } else if (next_pos == 1) {
        *tenbit_field = true;
        *resample_field = false;
        *rate_field = hw_10bit;
        *bits_field = 16;
    } else {
        // SW resample modes always use 8-bit (tenbit=false) so the
        // downsample operates on the 8-bit HW stream.
        *tenbit_field = false;
        *bits_field = 8;
        *resample_field = true;
        *rate_field = sw_presets[next_pos - 2];
    }
    gui_settings_save(&app->settings);

    const char *ch = (card_idx == 0) ? "A" : "B";
    char label[24];
    if (!*resample_field) {
        format_cxadc_hw_label(label, sizeof(label), *rate_field);
        char msg[128];
        snprintf(msg, sizeof(msg), "CH %s: %s %s", ch, label, *tenbit_field ? "10-bit" : "8-bit");
        gui_app_set_status(app, msg);
    } else {
        format_msps_label(label, sizeof(label), *rate_field);
        char msg[128];
        snprintf(msg, sizeof(msg), "CH %s: %s SW", ch, label);
        gui_app_set_status(app, msg);
    }
    gui_ui_warn_low_rate(app, *rate_field);
}
static uint8_t gui_ui_cxadc_rf_bits(const gui_app_t *app, int card_idx)
{
    if (!app) return 8;
    if (card_idx < 0 || card_idx > 1) card_idx = 0;
    return app->settings.cxadc_tenbit_mode_card[card_idx] ? 16 : 8;
}
static void gui_ui_toggle_cxadc_bit_mode(gui_app_t *app, int card_idx)
{
#if defined(__APPLE__)
    (void)card_idx;gui_app_set_status(app,"macOS CXADC: stock 28.636363 MSPS unsigned8 only; F8 for hardware settings");return;
#endif
    if (!app) return;
    if (card_idx < 0 || card_idx > 1) card_idx = 0;
    app->settings.cxadc_tenbit_mode_card[card_idx] = !app->settings.cxadc_tenbit_mode_card[card_idx];
    uint8_t cxadc_bits = gui_ui_cxadc_rf_bits(app, card_idx);
    // Switching bit depth changes the hardware rate; use the hardware rate
    // mode (resample OFF) by default so capture leverages native hardware
    // processing. The rate box hard-cycles between the two HW rates.
    float cxadc_hw_rate_khz = gui_ui_cxadc_hw_rate_khz(app, card_idx);
    if (card_idx == 0) {
        app->settings.rf_bits_a = cxadc_bits;
        app->settings.enable_resample_a = false;
        app->settings.resample_rate_a = cxadc_hw_rate_khz;
    } else {
        app->settings.rf_bits_b = cxadc_bits;
        app->settings.enable_resample_b = false;
        app->settings.resample_rate_b = cxadc_hw_rate_khz;
    }
    gui_settings_save(&app->settings);
    const char *card_label = (card_idx == 0) ? "A" : "B";
    bool enabled = app->settings.cxadc_tenbit_mode_card[card_idx];
    char hw_rate_label[24];
    format_msps_label(hw_rate_label, sizeof(hw_rate_label), cxadc_hw_rate_khz);
    if (app->is_capturing) {
        gui_app_set_status(app, enabled
            ? ((card_idx == 0) ? "CXADC card A 10-bit mode enabled (applies on next capture start)"
                               : "CXADC card B 10-bit mode enabled (applies on next capture start)")
            : ((card_idx == 0) ? "CXADC card A 8-bit mode enabled (applies on next capture start)"
                               : "CXADC card B 8-bit mode enabled (applies on next capture start)"));
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "CXADC card %s %s (%s HW rate)",
                 card_label,
                 enabled ? "10-bit mode enabled" : "8-bit mode enabled",
                 hw_rate_label);
        gui_app_set_status(app, msg);
    }
}

static bool gui_ui_map_cxadc_channel_to_card(const gui_app_t *app, int channel, int *card_idx_out)
{
    if (!app) return false;
    if (channel < 0 || channel > 1) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;

    const device_info_t *dev = &app->devices[app->selected_device];
    if (dev->type != DEVICE_TYPE_CXADC) return false;

    int card_count = dev->index;
    if (card_count < 1) card_count = 1;
    if (card_count > 2) card_count = 2;
    if (channel >= card_count) return false;

    if (card_idx_out) {
        *card_idx_out = channel;
    }
    return true;
}
static bool gui_ui_selected_device_is_playback(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    return app->devices[app->selected_device].type == DEVICE_TYPE_PLAYBACK;
}

#if defined(__ANDROID__)
/* Android Test Signal generates its samples in software. It must not open or
 * request permission for a USB device, including when auto-reconnecting. */
static bool gui_ui_selected_device_is_simulated(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    return app->devices[app->selected_device].type == DEVICE_TYPE_SIMULATED;
}
#endif

#ifdef ENABLE_DDD
// DdD is single-channel (channel A only); channel B has no signal source.
static bool gui_ui_selected_device_is_ddd(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    return app->devices[app->selected_device].type == DEVICE_TYPE_DDD;
}

static bool gui_ui_selected_device_is_ddd_v1(const gui_app_t *app)
{
    if (!gui_ui_selected_device_is_ddd(app)) return false;
    return app->devices[app->selected_device].ddd_profile ==
           DDD_DEVICE_PROTOCOL_V1;
}

// True iff the selected device is the synthetic "[DdD] Clockgen" entry.
static bool gui_ui_selected_device_is_ddd_clockgen(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    return gui_ddd_clockgen_device_mode(&app->devices[app->selected_device]);
}
#endif

/* Translate the active backend's telemetry into one device-neutral status-bar
 * view. Unsupported devices return false and do not consume any UI space. */
static bool gui_ui_get_device_buffer_view(
    gui_app_t *app,
    gui_device_buffer_layout_t layout,
    gui_device_buffer_view_t *view)
{
    if (!app || !view) return false;
    memset(view, 0, sizeof(*view));
    (void)layout;

#ifdef ENABLE_DDD
    if (gui_ui_selected_device_is_ddd_v1(app)) {
        gui_ddd_v1_fifo_snapshot_t snapshot;
        bool capture_active = atomic_load(&app->ddd_running);
        bool telemetry_present = capture_active &&
            gui_ddd_v1_get_fifo_snapshot(&snapshot);
        ddd_device_profile_t profile =
            app->devices[app->selected_device].ddd_profile;

        if (!gui_ddd_fifo_status_visible(profile, capture_active,
                                         telemetry_present)) {
            return false;
        }
        gui_ddd_fifo_make_buffer_view(&snapshot.latest, &snapshot.totals,
                                     layout, view);
        return view->visible;
    }
#endif

    return false;
}

#ifdef ENABLE_FX3
// FX3 is a distinct USB backend; showing its name as the mode label avoids
// confusion with the hsdaoh-specific MISRC/HSDAOH A/B-swap toggle.
static bool gui_ui_selected_device_is_fx3(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    return app->devices[app->selected_device].type == DEVICE_TYPE_FX3;
}
#endif

// --- Level autostop backend classification ---
// Last device type seen by the Vpp reconcile so we re-default level_autostop_vpp
// only when the active backend actually changes (not every frame).
static int s_level_autostop_vpp_last_type = -1;

// True if the active backend is hsdaoh (MISRC v1.5/2.5), which uses the
// hardware-selectable 1/2 Vpp jumper toggle instead of a free-edit Vpp box.
static bool gui_ui_level_autostop_is_hsdaoh(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    return app->devices[app->selected_device].type == DEVICE_TYPE_HSDAOH;
}

// Per-frame reconcile: when the active capture backend type changes, re-default
// level_autostop_vpp to the new backend's default (hsdaoh uses the stored 1/2
// hardware-jumper memory). Keeps the mV readout correct per-backend without a
// global custom flag leaking across backends.
static void gui_ui_level_autostop_reconcile_vpp(gui_app_t *app)
{
    if (!app) return;
    int cur_type = DEVICE_TYPE_HSDAOH;
    if (app->selected_device >= 0 && app->selected_device < app->device_count) {
        cur_type = (int)app->devices[app->selected_device].type;
    }
    if (cur_type == s_level_autostop_vpp_last_type) return;
    s_level_autostop_vpp_last_type = cur_type;
    if (gui_ui_level_autostop_is_hsdaoh(app)) {
        app->settings.level_autostop_vpp = app->settings.level_autostop_vpp_hsdaoh;
    } else {
        app->settings.level_autostop_vpp = gui_app_level_autostop_default_vpp(app);
    }
    if (!(app->settings.level_autostop_vpp > 0.1f)) {
        app->settings.level_autostop_vpp = 2.0f;
    }
    gui_settings_save(&app->settings);
}

#ifdef ENABLE_RTLSDR
// Generic SDR device check. True for any I/Q-providing SDR backend (today
// only RTL-SDR; add future SDR backends here so the SDR controls show for
// them too). This keeps the Settings/Demod SDR controls SDR-generic, not
// tied to the RTL-SDR backend specifically.
static bool gui_ui_selected_device_is_sdr(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    return app->devices[app->selected_device].type == DEVICE_TYPE_RTLSDR;
}
#endif

static void gui_ui_trace_capture_mode_state(gui_app_t *app, const char *source, bool force) {
    if (!app) return;
    bool ui_mode = s_capture_mode_state_misrc;
    bool user_mode = app->user_capture_mode_misrc;
    bool runtime_mode = app->capture_mode_runtime_misrc;
    bool settings_mode = app->settings.misrc_mode;
    bool recording = app->is_recording;
    bool capturing = app->is_capturing;
    bool changed = force || !s_capture_mode_trace_initialized ||
                   (ui_mode != s_capture_mode_trace_last_ui) ||
                   (user_mode != s_capture_mode_trace_last_user) ||
                   (runtime_mode != s_capture_mode_trace_last_runtime) ||
                   (settings_mode != s_capture_mode_trace_last_settings) ||
                   (recording != s_capture_mode_trace_last_recording) ||
                   (capturing != s_capture_mode_trace_last_capturing);
    if (changed) {
        TraceLog(LOG_INFO,
                 "MODE TRACE: source=%s ui=%s user=%s runtime=%s settings=%s recording=%d capturing=%d",
                 (source && source[0]) ? source : "unknown",
                 gui_ui_capture_mode_name(ui_mode),
                 gui_ui_capture_mode_name(user_mode),
                 gui_ui_capture_mode_name(runtime_mode),
                 gui_ui_capture_mode_name(settings_mode),
                 recording ? 1 : 0,
                 capturing ? 1 : 0);
    }
    s_capture_mode_trace_initialized = true;
    s_capture_mode_trace_last_ui = ui_mode;
    s_capture_mode_trace_last_user = user_mode;
    s_capture_mode_trace_last_runtime = runtime_mode;
    s_capture_mode_trace_last_settings = settings_mode;
    s_capture_mode_trace_last_recording = recording;
    s_capture_mode_trace_last_capturing = capturing;
}

static void gui_ui_trace_capture_mode_render(gui_app_t *app, bool rendered_mode, bool source_runtime) {
    if (!app) return;
    bool user_mode = app->user_capture_mode_misrc;
    bool runtime_mode = app->capture_mode_runtime_misrc;
    bool settings_mode = app->settings.misrc_mode;
    bool recording = app->is_recording;
    bool capturing = app->is_capturing;
    bool changed = !s_capture_mode_render_trace_initialized ||
                   (rendered_mode != s_capture_mode_render_last_mode) ||
                   (user_mode != s_capture_mode_render_last_user) ||
                   (runtime_mode != s_capture_mode_render_last_runtime) ||
                   (settings_mode != s_capture_mode_render_last_settings) ||
                   (recording != s_capture_mode_render_last_recording) ||
                   (capturing != s_capture_mode_render_last_capturing) ||
                   (source_runtime != s_capture_mode_render_last_source_runtime);
    if (changed) {
        TraceLog(LOG_INFO,
                 "MODE RENDER TRACE: rendered=%s source=%s user=%s runtime=%s settings=%s recording=%d capturing=%d",
                 gui_ui_capture_mode_name(rendered_mode),
                 source_runtime ? "runtime" : "user",
                 gui_ui_capture_mode_name(user_mode),
                 gui_ui_capture_mode_name(runtime_mode),
                 gui_ui_capture_mode_name(settings_mode),
                 recording ? 1 : 0,
                 capturing ? 1 : 0);
    }
    s_capture_mode_render_trace_initialized = true;
    s_capture_mode_render_last_mode = rendered_mode;
    s_capture_mode_render_last_user = user_mode;
    s_capture_mode_render_last_runtime = runtime_mode;
    s_capture_mode_render_last_settings = settings_mode;
    s_capture_mode_render_last_recording = recording;
    s_capture_mode_render_last_capturing = capturing;
    s_capture_mode_render_last_source_runtime = source_runtime;
}

typedef enum {
    UI_TEXT_FIELD_NONE = 0,
    UI_TEXT_FIELD_OUTPUT_BASE_NAME,
    UI_TEXT_FIELD_OUTPUT_PATH,
    UI_TEXT_FIELD_MONITOR_LOCAL_SAVE_PATH,
    UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH,
    UI_TEXT_FIELD_FLAC_AFFINITY,
    UI_TEXT_FIELD_RF_TAG_A,
    UI_TEXT_FIELD_RF_TAG_B,
    UI_TEXT_FIELD_AUDIO_TAG_4CH,
    UI_TEXT_FIELD_AUDIO_TAG_12,
    UI_TEXT_FIELD_AUDIO_TAG_34,
    UI_TEXT_FIELD_AUDIO_LABEL_1,
    UI_TEXT_FIELD_AUDIO_LABEL_2,
    UI_TEXT_FIELD_AUDIO_LABEL_3,
    UI_TEXT_FIELD_AUDIO_LABEL_4,
    UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL,    // Level autostop threshold (normalized 0.1-0.8)
    UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION,  // Level autostop sustain seconds
    UI_TEXT_FIELD_INGEST_PROJECT,
    UI_TEXT_FIELD_INGEST_TAPE_ID,
    UI_TEXT_FIELD_INGEST_TAPE_FORMAT,
    UI_TEXT_FIELD_INGEST_TAPE_SIZE,
    UI_TEXT_FIELD_INGEST_TAPE_SPEED,
    UI_TEXT_FIELD_INGEST_TAPE_CONDITION,
    UI_TEXT_FIELD_INGEST_OPERATOR,
    UI_TEXT_FIELD_INGEST_LOCATION,
    UI_TEXT_FIELD_INGEST_NOTES,
    UI_TEXT_FIELD_RTLSDR_FREQ,         // RTL-SDR center frequency (Hz, digits only)
    UI_TEXT_FIELD_NET_SERVER_PORT,      // Network server port (digits only)
    UI_TEXT_FIELD_NET_CLIENT_HOST,      // Network client server host (IP/hostname)
    UI_TEXT_FIELD_NET_CLIENT_PORT,      // Network client server port (digits only)
} ui_text_field_t;

// Unified cursor-based text editing state (settings panel)
/* The M-Client browser navigates the SERVER filesystem, never Windows disk. */
static bool s_monitor_folder_open=false;
static unsigned s_monitor_folder_page=0;
static char s_monitor_folder_path[FILE_PATH_MAX]={0};
static char s_monitor_folder_parent[FILE_PATH_MAX]={0};
static char s_monitor_folder_edit_path[FILE_PATH_MAX]={0};
static char s_monitor_folder_path_display[FILE_PATH_MAX]={0};
static char s_monitor_folder_labels[12][256];
static bool s_monitor_folder_computer=false;
static bool s_monitor_folder_recovered=false;
static bool s_monitor_folder_error=false;
static bool s_monitor_folder_more=false;
static ui_text_field_t s_active_text_field = UI_TEXT_FIELD_NONE;
static int s_active_text_cursor = 0;
static int s_active_text_selection_anchor = -1;
static bool s_active_text_drag_selecting = false;
static Clay_ElementId s_active_text_element_id = { 0 };
static float s_active_text_left_padding = 0.0f;
static float s_active_text_right_padding = 0.0f;
static double s_active_text_last_click_time = -1.0;
static Clay_ElementId s_active_text_last_click_element_id = { 0 };
static double s_active_text_backspace_repeat_at = 0.0;

// RTL-SDR frequency text field mirrors settings.rtlsdr_freq_hz (a uint64).
// Synced in render_settings_panel: format Hz->str when not editing, parse str->Hz when editing.
static char s_rtlsdr_freq_str[32] = {0};

// Record-limit popup state (toolbar clock button)
static bool s_record_limit_window_open = false;
// Version info popup state (toolbar "i" badge button)
static bool s_version_info_window_open = false;
// Net mode hold-press menu (About window "Mode" toggle): a press arms the
// decision, a quick release cycles the mode, holding >= 450 ms opens a
// direct-select menu. Resolution runs per frame in gui_handle_interactions
// (clicks fire on press, so the toggle itself only arms).
static bool s_net_mode_menu_open = false;
static bool s_net_mode_press_armed = false;
static double s_net_mode_press_time = 0.0;
static bool s_net_mode_hold_fired = false;

// Forward declaration (defined later in this file; the net mode apply helper
// below runs before the definition site).
static void gui_ui_clear_text_edit(void);

// Net mode cycle order (keep numeric mode 3 as a saved-config alias only):
// Local -> Server -> Client -> Monitor & Remote -> Local.
static int gui_ui_net_mode_cycle_next(int cur) {
    if (cur < GUI_NET_MODE_LOCAL || cur > GUI_NET_MODE_LAST) cur = GUI_NET_MODE_LOCAL;
    if (cur == GUI_NET_MODE_MONITOR_SERVER) return GUI_NET_MODE_LOCAL;
    if (cur == GUI_NET_MODE_CLIENT) return GUI_NET_MODE_MONITOR_CLIENT;
    if (cur == GUI_NET_MODE_MONITOR_CLIENT) return GUI_NET_MODE_LOCAL;
    return cur + 1;
}

// Shared by the quick-click cycle and the hold-press menu: update the mode,
// save, and hand the stop/start sequence to the async net transition worker.
// The synchronous gui_net_apply_mode joins network worker threads that can
// sit in multi-second socket timeouts - running it inline in the click
// handler is what made switching to Monitor & Remote stall the UI.
static void gui_ui_net_mode_apply(gui_app_t *app, int next) {
    if (!app) return;
    if (gui_playback_is_starting(app)) {
        gui_app_set_status(app, "Network mode is locked while playback prepares");
        return;
    }
    int cur = app->settings.net_mode;
    if (cur < GUI_NET_MODE_LOCAL || cur > GUI_NET_MODE_LAST) cur = GUI_NET_MODE_LOCAL;
    app->settings.net_mode = next;
    gui_ui_clear_text_edit();
    gui_settings_save(&app->settings);
    char msg[96];
    snprintf(msg, sizeof(msg), "Network mode: %s", gui_net_mode_name(next));
    gui_app_set_status(app, msg);
    /* Rapid cycling: a click while a transition is running is QUEUED (the
     * worker chases settings.net_mode), never refused - the label follows
     * the click immediately. Leaving a client mode / returning to Local must
     * bring the local device list back (the worker defers that enumerate to
     * the main loop); reenum here only feeds the sync fallback below. */
    bool reenum = (next == GUI_NET_MODE_LOCAL || cur == GUI_NET_MODE_CLIENT ||
                   cur == GUI_NET_MODE_MONITOR_CLIENT);
    if (gui_net_request_apply_mode(app, cur, false) != 0) {
        // Worker unavailable (spawn failure): fall back to the synchronous
        // apply so the mode never ends up half-applied.
        (void)gui_net_apply_mode(app);
        if (reenum) gui_app_enumerate_devices(app);
    }
}
// Metadata popup state (toolbar scroll badge button)
static bool s_metadata_window_open = false;
static bool s_record_limit_armed = false;
static bool s_record_limit_timecode_edit = false;
static double s_record_limit_backspace_repeat_at = 0.0;
static char s_record_limit_timecode[16] = "00:00:00";
static char s_record_limit_timecode_edit_buffer[16] = "00:00:00";
static int s_record_limit_cursor_char = 0; // editable char index in HH:MM:SS => 0,1,3,4,6,7
static uint32_t s_record_limit_seconds = 0;
static bool s_record_limit_session_seen = false;
static bool s_record_limit_deadline_active = false;
static double s_record_limit_deadline_s = 0.0;
#define RECORD_LIMIT_TIMECODE_SCALE 1.30f
#define RECORD_LIMIT_TIMECODE_BORDER_X 5
#define RECORD_LIMIT_TIMECODE_BORDER_Y 3
#if defined(__ANDROID__)
static bool s_android_keyboard_visible = false;
#endif
// Update checker (manifest-driven; see core/gui_updater.h): the releases
// PAGE button can never 404, the auto-patch manifest fixes asset naming for
// every installed binary without a new release, and confirmed self-update
// archives the old binary (rename, never delete).
#define GUI_UI_UPDATE_CHECK_INTERVAL_SECONDS (7ULL * 24ULL * 60ULL * 60ULL)
#define VERSION_INFO_NET_DISCOVERY_MAX_ROWS 6

typedef struct {
    bool manual;   // update-check task
    bool install;  // confirmed self-update install task (fields below)
    char asset_url[GUI_UPDATER_URL_MAX];
    char asset_name[192];
    char current_version[64];
} gui_ui_update_check_task_t;

static thrd_t s_update_check_thread;
static bool s_update_check_thread_started = false;
static atomic_bool s_update_check_running = false;
static atomic_bool s_update_check_result_pending = false;
static bool s_update_check_result_manual = false;
static bool s_update_check_result_success = false;
static bool s_update_check_result_install = false;      // result is an install result
static bool s_update_check_result_install_ok = false;
static char s_update_check_result_tag[64] = {0};
static char s_update_check_result_error[160] = {0};
static char s_update_check_result_message[GUI_UPDATER_STATUS_MAX] = {0};
static gui_updater_manifest_t s_update_check_result_manifest; // worker writes, UI thread reads after pending
static bool s_update_check_result_manifest_ok = false;
// Last auto-patch manifest successfully fetched by a check (UI thread only).
static gui_updater_manifest_t s_update_manifest;
static bool s_update_manifest_ok = false;
// Two-click confirmation state for the Install button (UI thread only).
static bool s_update_install_armed = false;

static bool gui_ui_open_url_external(const char *url, char *error_out, size_t error_out_len)
{
    if (!url || !url[0] || !error_out || error_out_len == 0) return false;
    error_out[0] = '\0';

    char cmd[1024];
    int written = 0;
#if defined(_WIN32)
    written = snprintf(cmd, sizeof(cmd), "start \"\" \"%s\"", url);
#elif defined(__APPLE__)
    written = snprintf(cmd, sizeof(cmd), "open \"%s\"", url);
#elif defined(__ANDROID__)
    written = snprintf(cmd, sizeof(cmd), "am start -a android.intent.action.VIEW -d \"%s\"", url);
#elif defined(__linux__)
    written = snprintf(cmd, sizeof(cmd), "xdg-open \"%s\"", url);
#else
    snprintf(error_out, error_out_len, "unsupported platform");
    return false;
#endif
    if (written <= 0 || (size_t)written >= sizeof(cmd)) {
        snprintf(error_out, error_out_len, "download command too long");
        return false;
    }
    int rc = system(cmd);
    if (rc != 0) {
        snprintf(error_out, error_out_len, "failed to open download URL");
        return false;
    }
    return true;
}

static int gui_ui_update_check_thread_main(void *arg_ptr)
{
    gui_ui_update_check_task_t task = {0};
    if (arg_ptr) {
        task = *((gui_ui_update_check_task_t *)arg_ptr);
        free(arg_ptr);
    }

    if (task.install) {
        // Confirmed self-update install: download the asset, archive the old
        // binary (rename, never delete), move the new one into place.
        char status_text[GUI_UPDATER_STATUS_MAX] = {0};
        char archive_path[GUI_UPDATER_PATH_MAX] = {0};
        bool install_ok = gui_updater_install_asset(task.asset_url,
                                                     task.asset_name,
                                                     task.current_version,
                                                     status_text, sizeof(status_text),
                                                     archive_path, sizeof(archive_path));
        s_update_check_result_manual = false;
        s_update_check_result_install = true;
        s_update_check_result_install_ok = install_ok;
        s_update_check_result_success = install_ok;
        s_update_check_result_tag[0] = '\0';
        s_update_check_result_error[0] = '\0';
        snprintf(s_update_check_result_message, sizeof(s_update_check_result_message), "%s",
                 status_text[0] ? status_text : (install_ok ? "update installed" : "update install failed"));
        atomic_store_explicit(&s_update_check_result_pending, true, memory_order_release);
        atomic_store_explicit(&s_update_check_running, false, memory_order_release);
        return 0;
    }

    // Check task: fetch the auto-patch manifest first (it defines the tag
    // lookup URL + asset naming), then resolve the latest tag through it.
    // Manifest unreachable: still check the tag via the compiled-in default
    // lookup so "Update available" keeps working (self-update stays off).
    gui_updater_manifest_t manifest = {0};
    char manifest_error[GUI_UPDATER_ERROR_MAX] = {0};
    bool manifest_ok = gui_updater_fetch_manifest(&manifest, manifest_error, sizeof(manifest_error));

    char latest_tag[64] = {0};
    char error_text[160] = {0};
    bool ok = gui_updater_fetch_latest_release_tag(manifest_ok ? manifest.tag_lookup_url : NULL,
                                                   latest_tag, sizeof(latest_tag),
                                                   error_text, sizeof(error_text));

    s_update_check_result_manual = task.manual;
    s_update_check_result_install = false;
    s_update_check_result_install_ok = false;
    s_update_check_result_message[0] = '\0';
    s_update_check_result_success = ok;
    if (ok) {
        snprintf(s_update_check_result_tag, sizeof(s_update_check_result_tag), "%s", latest_tag);
        s_update_check_result_error[0] = '\0';
    } else {
        s_update_check_result_tag[0] = '\0';
        snprintf(s_update_check_result_error, sizeof(s_update_check_result_error), "%s",
                 error_text[0] ? error_text : "unknown error");
    }
    s_update_check_result_manifest_ok = manifest_ok;
    s_update_check_result_manifest = manifest;

    atomic_store_explicit(&s_update_check_result_pending, true, memory_order_release);
    atomic_store_explicit(&s_update_check_running, false, memory_order_release);
    return 0;
}

static bool gui_ui_start_update_task(const gui_ui_update_check_task_t *task_in)
{
    if (!task_in) {
        return false;
    }
    if (atomic_load_explicit(&s_update_check_running, memory_order_acquire)) {
        return false;
    }
    if (atomic_load_explicit(&s_update_check_result_pending, memory_order_acquire)) {
        return false;
    }
    gui_ui_update_check_task_t *task = (gui_ui_update_check_task_t *)malloc(sizeof(gui_ui_update_check_task_t));
    if (!task) {
        return false;
    }
    *task = *task_in;

    atomic_store_explicit(&s_update_check_running, true, memory_order_release);
    if (thrd_create_with_priority(&s_update_check_thread,
                                  gui_ui_update_check_thread_main,
                                  task,
                                  THRD_PRIORITY_NORMAL) != thrd_success) {
        atomic_store_explicit(&s_update_check_running, false, memory_order_release);
        free(task);
        return false;
    }
    s_update_check_thread_started = true;
    return true;
}

static bool gui_ui_start_update_check(bool manual)
{
    gui_ui_update_check_task_t task = {0};
    task.manual = manual;
    if (gui_ui_start_update_task(&task)) {
        s_update_install_armed = false; // a fresh check re-validates what to install
        return true;
    }
    return false;
}

static void gui_ui_join_update_check_thread_if_needed(void)
{
    if (!s_update_check_thread_started) return;
    (void)thrd_join(s_update_check_thread, NULL);
    s_update_check_thread_started = false;
}

static void gui_ui_process_update_check_result(gui_app_t *app)
{
    if (!app) return;
    if (!atomic_load_explicit(&s_update_check_result_pending, memory_order_acquire)) return;
    atomic_store_explicit(&s_update_check_result_pending, false, memory_order_release);
    gui_ui_join_update_check_thread_if_needed();

    uint64_t now_s = (uint64_t)time(NULL);
    app->settings.update_last_check_unix_s = now_s;

    // Install result: the running process is still the old version until
    // restart; surface the installer's own status message (includes the
    // archive path on success / the kept download path on failure).
    if (s_update_check_result_install) {
        s_update_install_armed = false;
        if (s_update_check_result_message[0]) {
            gui_app_set_status(app, s_update_check_result_message);
        }
        return;
    }

    // Check result: adopt the auto-patch manifest (drives the releases page
    // URL + self-update asset naming) and drop any stale install-armed state.
    if (s_update_check_result_manifest_ok) {
        s_update_manifest = s_update_check_result_manifest;
        s_update_manifest_ok = true;
    }
    s_update_install_armed = false;

    if (s_update_check_result_success) {
        snprintf(app->settings.update_last_release_tag,
                 sizeof(app->settings.update_last_release_tag),
                 "%s",
                 s_update_check_result_tag);
        int cmp = gui_updater_compare_versions(MIRSC_TOOLS_VERSION, app->settings.update_last_release_tag);
        app->settings.update_available_cached = (cmp < 0);
        gui_settings_save(&app->settings);

        if (s_update_check_result_manual) {
            char msg[240];
            if (cmp < 0) {
                snprintf(msg, sizeof(msg), "Update available: %s (current %s)",
                         app->settings.update_last_release_tag, MIRSC_TOOLS_VERSION);
            } else if (cmp == 0) {
                snprintf(msg, sizeof(msg), "No update available (latest %s)",
                         app->settings.update_last_release_tag);
            } else {
                snprintf(msg, sizeof(msg), "Running newer build than release %s",
                         app->settings.update_last_release_tag);
            }
            if (!s_update_check_result_manifest_ok) {
                size_t len = strlen(msg);
                snprintf(msg + len, sizeof(msg) - len, " [self-update offline]");
            }
            gui_app_set_status(app, msg);
        } else if (app->settings.update_available_cached) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Update available: %s", app->settings.update_last_release_tag);
            gui_app_set_status(app, msg);
        }
    } else {
        gui_settings_save(&app->settings);
        if (s_update_check_result_manual) {
            char msg[220];
            snprintf(msg, sizeof(msg), "Update check failed: %s",
                     s_update_check_result_error[0] ? s_update_check_result_error : "unknown error");
            gui_app_set_status(app, msg);
        }
    }
}

static bool gui_ui_update_check_due(const gui_settings_t *settings, uint64_t now_s)
{
    if (!settings) return false;
    if (settings->update_last_check_unix_s == 0) return true;
    if (now_s < settings->update_last_check_unix_s) return true;
    uint64_t elapsed = now_s - settings->update_last_check_unix_s;
    return elapsed >= GUI_UI_UPDATE_CHECK_INTERVAL_SECONDS;
}

static void gui_ui_update_check_tick(gui_app_t *app)
{
    if (!app) return;
    gui_ui_process_update_check_result(app);
    if (atomic_load_explicit(&s_update_check_running, memory_order_acquire)) return;
    if (atomic_load_explicit(&s_update_check_result_pending, memory_order_acquire)) return;

    uint64_t now_s = (uint64_t)time(NULL);
    if (gui_ui_update_check_due(&app->settings, now_s)) {
        (void)gui_ui_start_update_check(false);
    }
}

void gui_ui_sync_android_keyboard_state(void) {
#if defined(__ANDROID__)
    bool want_visible = (s_active_text_field != UI_TEXT_FIELD_NONE) || s_record_limit_timecode_edit;
    if (want_visible != s_android_keyboard_visible) {
        android_set_keyboard_visible(want_visible ? 1 : 0);
        s_android_keyboard_visible = want_visible;
    }
#endif
}


bool gui_ui_click_consumed(void) {
    return s_ui_consumed_click;
}
static void format_record_limit_timecode(char *dst, size_t dst_len, uint32_t total_seconds);
static bool parse_record_limit_timecode(const char *src, uint32_t *out_seconds);
static bool record_limit_is_digit_char_index(int idx)
{
    return (idx == 0 || idx == 1 || idx == 3 || idx == 4 || idx == 6 || idx == 7);
}

static int record_limit_nearest_digit_cursor_char(int idx)
{
    if (idx <= 0) return 0;
    if (idx <= 1) return idx;
    if (idx <= 2) return 1;
    if (idx <= 3) return 3;
    if (idx <= 4) return 4;
    if (idx <= 5) return 4;
    if (idx <= 6) return 6;
    return 7;
}

static int record_limit_move_cursor_char(int cursor, int dir)
{
    int next = record_limit_nearest_digit_cursor_char(cursor);
    while (1) {
        next += dir;
        if (next < 0) return 0;
        if (next > 7) return 7;
        if (record_limit_is_digit_char_index(next)) return next;
    }
}
static int record_limit_timecode_font_size_px(void)
{
    return (int)ceilf((float)FONT_SIZE_TITLE * RECORD_LIMIT_TIMECODE_SCALE);
}

static const char *record_limit_timecode_buffer_for_layout(void)
{
    static const char fallback_timecode[] = "00:00:00";
    const char *text = s_record_limit_timecode_edit
        ? s_record_limit_timecode_edit_buffer
        : s_record_limit_timecode;
    if (!text || strlen(text) < 8) {
        return fallback_timecode;
    }
    return text;
}

static Font record_limit_timecode_font(gui_app_t *app)
{
    Font font = GetFontDefault();
    if (app && app->fonts && app->fonts[1].texture.id != 0) {
        font = app->fonts[1];
    }
    if (!font.glyphs) {
        font = GetFontDefault();
    }
    return font;
}

static void record_limit_measure_char_widths(gui_app_t *app,
                                             const char *timecode_text,
                                             int font_size,
                                             float out_widths[8],
                                             float *out_total_width)
{
    static const char fallback_timecode[] = "00:00:00";
    const char *text = timecode_text;
    if (!text || strlen(text) < 8) {
        text = fallback_timecode;
    }

    Font font = record_limit_timecode_font(app);
    float total = 0.0f;
    for (int i = 0; i < 8; i++) {
        char glyph[2] = { text[i], '\0' };
        Vector2 m = gui_font_measure_text(font, glyph, (float)font_size, 0.0f);
        float w = m.x;
        if (w <= 0.0f) {
            w = (text[i] == ':') ? ((float)font_size * 0.35f) : ((float)font_size * 0.5f);
        }
        out_widths[i] = w;
        total += w;
    }
    if (out_total_width) {
        *out_total_width = total;
    }
}

static void record_limit_begin_timecode_edit(void)
{
    snprintf(s_record_limit_timecode_edit_buffer, sizeof(s_record_limit_timecode_edit_buffer), "%s", s_record_limit_timecode);
    if (!parse_record_limit_timecode(s_record_limit_timecode_edit_buffer, NULL)) {
        format_record_limit_timecode(s_record_limit_timecode_edit_buffer, sizeof(s_record_limit_timecode_edit_buffer), s_record_limit_seconds);
    }
    s_record_limit_cursor_char = 0;
    s_record_limit_backspace_repeat_at = 0.0;
    s_record_limit_timecode_edit = true;
}
static void record_limit_set_cursor_from_field_click(gui_app_t *app)
{
    Clay_ElementData field = Clay_GetElementData(CLAY_ID("RecordLimitTimecodeField"));
    if (!field.found) {
        s_record_limit_cursor_char = 0;
        return;
    }

    Vector2 mouse = gui_ui_get_mouse_position();
    float content_left = field.boundingBox.x + (float)RECORD_LIMIT_TIMECODE_BORDER_X;
    float content_width = field.boundingBox.width - (float)(RECORD_LIMIT_TIMECODE_BORDER_X * 2);
    if (content_width < 8.0f) content_width = 8.0f;

    float char_widths[8] = { 0 };
    float text_width = 0.0f;
    int font_size = record_limit_timecode_font_size_px();
    record_limit_measure_char_widths(app, record_limit_timecode_buffer_for_layout(), font_size, char_widths, &text_width);

    float text_left = content_left + fmaxf(0.0f, (content_width - text_width) * 0.5f);
    float x = text_left;
    int nearest_idx = 0;
    float nearest_dist = 1.0e30f;
    for (int i = 0; i < 8; i++) {
        float center = x + (char_widths[i] * 0.5f);
        float dist = fabsf(mouse.x - center);
        if (dist < nearest_dist) {
            nearest_dist = dist;
            nearest_idx = i;
        }
        x += char_widths[i];
    }

    s_record_limit_cursor_char = record_limit_nearest_digit_cursor_char(nearest_idx);
}

static inline void gui_ui_set_click_consumed(void) { // 130226 - added
    s_ui_consumed_click = true;
}
// Color conversions
static inline Clay_Color to_clay_color(Color c) {
    return (Clay_Color){ c.r, c.g, c.b, c.a };
}
static void format_playback_timecode(char *dst, size_t dst_len, double seconds)
{
    if (!dst || dst_len == 0) return;
    if (!isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }
    uint64_t total_secs = (uint64_t)seconds;
    uint64_t hours = total_secs / 3600ULL;
    uint64_t mins = (total_secs / 60ULL) % 60ULL;
    uint64_t secs = total_secs % 60ULL;
    snprintf(dst, dst_len, "%02llu:%02llu:%02llu",
             (unsigned long long)hours,
             (unsigned long long)mins,
             (unsigned long long)secs);
}
static bool gui_ui_playback_channel_timeline_info(gui_app_t *app, int channel_index,
                                                  uint64_t *out_total_samples,
                                                  double *out_duration_seconds,
                                                  bool *out_provisional, bool *out_verification_failed)
{
    if (out_total_samples) *out_total_samples = 0;
    if (out_duration_seconds) *out_duration_seconds = 0.0;
    if (out_provisional) *out_provisional = false;
    if (out_verification_failed) *out_verification_failed = false;
    if (!app) return false;

    playback_file_info_t info = {0};
    // Live playback info while running; otherwise the file selected in
    // settings (validated + cached per path) so timecodes show at idle too.
    (void)gui_playback_get_selected_file_info(app, channel_index, &info);

    if (!info.valid) return false;
    double duration_seconds = info.duration_seconds;
    if (!(duration_seconds > 0.0) || !isfinite(duration_seconds)) {
        double rate_hz = gui_playback_get_channel_rate_hz(app, channel_index);
        duration_seconds = rate_hz > 0.0 && isfinite(rate_hz)
            ? (double)info.total_samples / rate_hz : 0.0;
    }

    if (out_total_samples) *out_total_samples = info.total_samples;
    if (out_duration_seconds) *out_duration_seconds = duration_seconds;
    if (out_provisional) *out_provisional = info.length_provisional;
    if (out_verification_failed) *out_verification_failed = info.length_verification_failed;
    return true;
}
static void gui_ui_format_playback_timeline(char *dst, size_t dst_len, int *out_fill_w, bool *out_has_file,
                                            bool loaded, bool provisional, uint64_t current_sample, uint64_t total_samples,
                                            double total_duration_seconds, double source_rate_hz, int track_width_px)
{
    if (!dst || dst_len == 0) return;
    if (out_fill_w) *out_fill_w = 0;
    if (out_has_file) *out_has_file = false;
    if (!loaded) {
        snprintf(dst, dst_len, "--:--:--/--:--:--");
        return;
    }
    if (out_has_file) *out_has_file = true;
    if (total_samples == 0 || !(total_duration_seconds > 0.0) || !isfinite(total_duration_seconds)) {
        char elapsed_tc[24];
        if (source_rate_hz > 0.0 && isfinite(source_rate_hz)) {
            format_playback_timecode(elapsed_tc, sizeof(elapsed_tc), (double)current_sample / source_rate_hz);
        } else {
            snprintf(elapsed_tc, sizeof(elapsed_tc), "--:--:--");
        }
        snprintf(dst, dst_len, "%s/Unknown", elapsed_tc);
        return;
    }
    uint64_t channel_sample = current_sample;
    if (!provisional && channel_sample >= total_samples) {
        channel_sample = total_samples;
    }
    double t = (double)channel_sample / (double)total_samples;
    double playback_pos_s = source_rate_hz > 0.0 && isfinite(source_rate_hz)
        ? (double)channel_sample / source_rate_hz : t * total_duration_seconds;
    double playback_total_s = total_duration_seconds;
    char pos_tc[24];
    char total_tc[24];
    format_playback_timecode(pos_tc, sizeof(pos_tc), playback_pos_s);
    format_playback_timecode(total_tc, sizeof(total_tc), playback_total_s);
    snprintf(dst, dst_len, "%s/%s%s", pos_tc, provisional ? "~" : "", total_tc);
    double fill_ratio = t > 1.0 ? 1.0 : t;
    int fill_w = track_width_px > 0 ? (int)round(fill_ratio * (double)track_width_px) : 0;
    if (fill_w < 0) fill_w = 0;
    if (track_width_px > 0 && fill_w > track_width_px) fill_w = track_width_px;
    if (out_fill_w) *out_fill_w = fill_w;
}
// Format the per-channel playback file readout shown in-line with the scrub
// slider bar: the ACTUAL bit depth and the resolved real sample rate of the
// selected/playing FLAC file ("8-bit 20.0 MSPS", "16-bit 44.1 kHz"; compact
// tier "8b 20.0M" on narrow layouts). RF captures resolve to their true RF
// rate; genuine audio-rate files stay in kHz.
static void gui_ui_format_playback_readout(gui_app_t *app, int channel,
                                           char *dst, size_t dst_len, bool compact)
{
    if (!dst || dst_len == 0) return;
    playback_file_info_t info = {0};
    (void)gui_playback_get_selected_file_info(app, channel, &info);
    if (info.bits_per_sample == 0 ||
        !(info.real_sample_rate_hz > 0.0) || !isfinite(info.real_sample_rate_hz)) {
        snprintf(dst, dst_len, "--");
        return;
    }
    double hz = info.real_sample_rate_hz;
    if (hz >= 1.0e6) {
        snprintf(dst, dst_len, compact ? "%u-bit %.1fM" : "%u-bit %.1f MSPS",
                 (unsigned)info.bits_per_sample, hz / 1.0e6);
    } else {
        snprintf(dst, dst_len, compact ? "%u-bit %.1fk" : "%u-bit %.1f kHz",
                 (unsigned)info.bits_per_sample, hz / 1.0e3);
    }
}

//-----------------------------------------------------------------------------
// Playback timeline: right-side FILE INFO (HARD DEV NOTE)
//-----------------------------------------------------------------------------
/* HARD DEV NOTE — playback timeline file info + wheel scrub (2026-10-12)
 *
 * Next to each playback scrub slider the row shows, from the ACTUAL selected
 * /playing FLAC file (gui_playback_get_selected_file_info: live info while
 * playing, else path-cached validation — never a guess):
 *   1. basename (ellipsized slot),
 *   2. "8-bit 20.0 MSPS" — actual bit depth + resolved real RF rate
 *      (RF_SAMPLE_RATE tag > kHz-domain header x1000 > filename msps hint),
 *   3. "RAW 838.8GiB FLAC 121.5GiB 6.9x" — RAW is the per-channel packed
 *      u32 raw stream (total_samples x 4 bytes — the same accounting the
 *      capture log's rawA/rawB uses); FLAC is the on-disk size (64-bit size
 *      read, cached per path); ratio = raw / compressed.
 * Slots use fixed representative widths per tier (full -> compact -> hidden)
 * so value changes never reflow the row; stacked rows put the whole info line
 * under the track. Data is cached per path — re-select the file to refresh
 * sizes after it changes on disk.
 *
 * Mouse wheel over a playback timeline row scrubs that channel with 1-10 s
 * acceleration: notches within 400 ms ramp the step (+0.75 s/notch, capped at
 * 10 s); an idle gap resets to the 1 s base step. Wheel-up steps backward,
 * wheel-down steps forward (swap the sign in gui_handle_interactions to
 * flip). Seeks while paused push ONE preview batch (gui_playback.c
 * preview_pending) so the waveform/spectrogram render the new position, then
 * hold — the spectrogram freezes while paused (gui_spectrogram.c) like a DAW.
 *---------------------------------------------------------------------------*/

// 64-bit file size, portable across the POSIX/Win32 builds.
static uint64_t gui_ui_file_size_64(const char *path) {
    if (!path || !path[0]) return 0;
    FILE *f = file_open_read_utf8(path);
    if (!f) return 0;
    uint64_t size = 0;
#if defined(_WIN32) || defined(_WIN64)
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        __int64 end = _ftelli64(f);
        if (end >= 0) size = (uint64_t)end;
    }
#else
    if (fseeko(f, 0, SEEK_END) == 0) {
        off_t end = ftello(f);
        if (end >= 0) size = (uint64_t)end;
    }
#endif
    fclose(f);
    return size;
}

// Binary-unit size text ("838.8GiB"; compact tier "838.8G").
static void gui_ui_format_size_u64(uint64_t bytes, char *dst, size_t dst_len, bool compact) {
    if (!dst || dst_len == 0) return;
    static const char *units_full[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    static const char *units_compact[] = {"B", "K", "M", "G", "T"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (bytes == 0) {
        snprintf(dst, dst_len, "0B");
        return;
    }
    if (u == 0) {
        snprintf(dst, dst_len, "%uB", (unsigned)bytes);
        return;
    }
    snprintf(dst, dst_len, compact ? "%.1f%s" : "%.1f%s", v,
             compact ? units_compact[u] : units_full[u]);
}

// File info text: "RAW 838.8GiB FLAC 121.5GiB 6.9x" (compact "838.8G/121.5G").
static void gui_ui_format_playback_fileinfo(gui_app_t *app, int channel,
                                            char *dst, size_t dst_len, bool compact)
{
    if (!dst || dst_len == 0) return;
    playback_file_info_t info = {0};
    (void)gui_playback_get_selected_file_info(app, channel, &info);
    if (info.total_samples == 0 || info.filepath[0] == '\0') {
        snprintf(dst, dst_len, "--");
        return;
    }
    typedef struct {
        char path[FILE_PATH_MAX];
        uint64_t raw;
        uint64_t flac;
        bool valid;
    } fileinfo_cache_t;
    static fileinfo_cache_t s_fileinfo_cache[2];
    fileinfo_cache_t *c = &s_fileinfo_cache[channel];
    if (!c->valid || strncmp(c->path, info.filepath, sizeof(c->path)) != 0) {
        snprintf(c->path, sizeof(c->path), "%s", info.filepath);
        c->flac = gui_ui_file_size_64(c->path);
        c->valid = true;
    }
    // Length discovery can refine total_samples while the path stays unchanged.
    c->raw = info.total_samples * 4ULL;   // per-channel packed u32 stream
    char raw_buf[20];
    char flac_buf[20];
    gui_ui_format_size_u64(c->raw, raw_buf, sizeof(raw_buf), compact);
    gui_ui_format_size_u64(c->flac, flac_buf, sizeof(flac_buf), compact);
    if (c->flac > 0) {
        double ratio = (double)c->raw / (double)c->flac;
        if (compact) {
            snprintf(dst, dst_len, "%s/%s", raw_buf, flac_buf);
        } else {
            snprintf(dst, dst_len, "RAW %s FLAC %s %.1fx", raw_buf, flac_buf, ratio);
        }
    } else if (compact) {
        snprintf(dst, dst_len, "%s", raw_buf);
    } else {
        snprintf(dst, dst_len, "RAW %s", raw_buf);
    }
}

/* Display-only truncation must not turn a valid UTF-8 filename into invalid bytes. */
static void gui_ui_copy_text_prefix(char *dst, size_t capacity, const char *text)
{
    if (!dst || capacity == 0) return;
    size_t length = strlen(text);
    if (length >= capacity) length = (size_t)gui_text_boundary(text, (int)capacity - 1);
    memcpy(dst, text, length);
    dst[length] = '\0';
}

static void gui_ui_copy_path_tail(char *dst, size_t capacity, const char *path)
{
    size_t length = strlen(path);
    if (length <= 30) {
        gui_ui_copy_text_prefix(dst, capacity, path);
        return;
    }
    size_t start = length - 27;
    while (((unsigned char)path[start] & 0xc0) == 0x80) ++start;
    if (capacity < 4) {
        gui_ui_copy_text_prefix(dst, capacity, "...");
        return;
    }
    memcpy(dst, "...", 3);
    gui_ui_copy_text_prefix(dst + 3, capacity - 3, path + start);
}

// File basename for the info line ("Tape_08_..._video_rf_8-bit_20msps.flac").
static void gui_ui_playback_file_basename(gui_app_t *app, int channel,
                                          char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    playback_file_info_t info = {0};
    (void)gui_playback_get_selected_file_info(app, channel, &info);
    if (info.filepath[0] == '\0') return;
    const char *slash = strrchr(info.filepath, '/');
    const char *sep = slash;
#ifdef _WIN32
    const char *bslash = strrchr(info.filepath, '\\');
    if (bslash && (!sep || bslash > sep)) sep = bslash;
#endif
    const char *base = sep ? (sep + 1) : info.filepath;
    gui_ui_copy_text_prefix(dst, dst_len, base);
}
static bool gui_ui_seek_playback_from_track(gui_app_t *app, int track_index, float mouse_x)
{
    if (!app) return false;
    Clay_ElementData track = Clay_GetElementData(CLAY_IDI("PlaybackTimelineTrack", track_index));
    if (!track.found) return false;
    float track_width = track.boundingBox.width;
    if (track_width <= 1.0f) return false;
    uint64_t channel_total_samples = 0;
    double channel_duration_seconds = 0.0;
    if (!gui_ui_playback_channel_timeline_info(app, track_index, &channel_total_samples, &channel_duration_seconds,
                                              NULL, NULL) ||
        channel_total_samples == 0 || !(channel_duration_seconds > 0.0) || !isfinite(channel_duration_seconds)) {
        return false;
    }
    float t = (mouse_x - track.boundingBox.x) / track_width;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint64_t target_sample = (uint64_t)floor((double)t * (double)channel_total_samples);
    if (target_sample >= channel_total_samples) {
        target_sample = channel_total_samples - 1;
    }
    gui_playback_seek_sample_channel(app, track_index, target_sample);
    return true;
}

// Format helpers - use separate buffers to avoid overwriting
static char device_dropdown_buf[64];

// Per-channel stat buffers (separate for A and B to avoid overwrite)
static char stat_a_peak_pos[16];
static char stat_a_peak_neg[16];
static char stat_a_clip_pos[16];
static char stat_a_clip_neg[16];
static char stat_b_peak_pos[16];
static char stat_b_peak_neg[16];
static char stat_b_clip_pos[16];
static char stat_b_clip_neg[16];
static char stat_rec_raw[2][32];
static char stat_rec_flac[2][32];
static char stat_rec_ratio[2][24];
static char stat_rec_duration[2][24];

// Snapshot of what is visible in each channel's recording card. The UI
// renders live values, while the existing info popup shows a click-time
// snapshot. Keep this out of gui_app_t: no recording or network state changes.
typedef struct {
    bool visible;
    bool recording;
    bool buffer_known;
    bool local_save;
    unsigned buffer_fill_pct;
    uint64_t write_waits;
    uint64_t dropped_blocks;
    uint64_t send_queue_bytes;
    uint64_t send_queue_capacity;
} gui_record_details_view_t;
static gui_record_details_view_t s_record_details_view[2];

// Playback file display buffers
static char playback_file_a_display[64];
static char playback_file_b_display[64];
static char playback_file_audio_display[64];
static char playback_timeline_display_a[64];
static char playback_timeline_display_b[64];
static char playback_readout_display_a[32];
static char playback_readout_display_b[32];
/* Full filenames (gui_ui_playback_file_basename): sized for whole basenames,
   not abbreviated ones - the timeline name slot budgets from these. */
static char playback_name_display_a[128];
static char playback_name_display_b[128];
static char playback_fileinfo_display_a[48];
static char playback_fileinfo_display_b[48];
static char playback_index_display[2][96];
static bool s_playback_scrub_active = false;
static int s_playback_scrub_track_index = 0;

// Audio meter channel labels (static buffers)
static char audio_ch_label[4][8];

// Settings panel stable display buffers (avoid reuse of temp_buf* across layout)
static char settings_rf_bits_a_display[16];
static char settings_rf_bits_b_display[16];
static char settings_flac_level_display[64];
static char settings_flac_threads_display[64];
static char settings_resample_a_display[32];
static char settings_resample_b_display[32];
static char status_sample_rate_display[32];
static char status_samples_display[32];
static char status_frames_display[32];
static char status_missed_display[16];
static char status_errors_display[16];
static char status_rf_buf_display[16];
static char status_aud_buf_display[16];
static char status_free_space_display[120];
static char status_message_display[256];
static char status_record_timer_display[16];
static gui_device_buffer_view_t status_device_buffer_view;
static char record_limit_state_display[96];
static char record_limit_timecode_display[20];
static bool s_status_free_space_valid = false;
static uint64_t s_status_free_space_cached_bytes = 0;
static double s_status_free_space_last_update_s = 0.0;
static uint64_t s_status_output_last_bytes = 0;
static double s_status_output_last_sample_s = 0.0;
static double s_status_output_rate_bps = 0.0;
#define STATUS_FREE_SPACE_REFRESH_INTERVAL_S 1.0
#define STATUS_FREE_SPACE_LOW_BYTES ((uint64_t)10 * 1000 * 1000 * 1000)
#define STATUS_FREE_SPACE_WARN_BYTES ((uint64_t)25 * 1000 * 1000 * 1000)

void gui_ui_sync_capture_mode_state(gui_app_t *app) {
    if (!app) return;
    if (!s_capture_mode_state_initialized) {
        s_capture_mode_state_misrc = app->settings.misrc_mode;
        s_capture_mode_state_initialized = true;
        gui_ui_trace_capture_mode_state(app, "ui_init_from_settings", true);
    }
    /* Client mode mirrors server controls; never let local UI mode state
     * override the peer-provided mode snapshot. */
    if (gui_net_is_client(app)) {
        bool mirrored_mode = app->settings.misrc_mode;
        s_capture_mode_state_misrc = mirrored_mode;
        app->user_capture_mode_misrc = mirrored_mode;
        if (!app->is_recording) {
            app->capture_mode_runtime_misrc = mirrored_mode;
        }
        gui_ui_trace_capture_mode_state(app, "gui_ui_sync_capture_mode_state_client", false);
        return;
    }
#ifdef ENABLE_DDD
    bool ddd_mode = gui_ui_selected_device_is_ddd(app);
#else
    bool ddd_mode = false;
#endif
#ifdef ENABLE_FX3
    bool fx3_mode = gui_ui_selected_device_is_fx3(app);
#else
    bool fx3_mode = false;
#endif
    bool cxadc_mode = gui_ui_selected_device_is_cxadc(app, NULL);
    bool cxadc_clockgen = false;
    if (cxadc_mode) {
        gui_ui_selected_device_is_cxadc(app, &cxadc_clockgen);
    }
    /* The Playback device is another fixed A/B mapping: file A IS channel A
     * (the per-channel seek sliders scrub their own file), so the MISRC
     * hardware-input swap must not apply there either — the leaked swap
     * crossed the panel feeds against the sliders. Like cxadc, preserve the
     * sticky settings.misrc_mode preference; only force the live flags. */
    bool playback_device = app->selected_device >= 0 && app->selected_device < app->device_count &&
                           app->devices[app->selected_device].type == DEVICE_TYPE_PLAYBACK;
    bool cxadc_has_channel_b = false;
    if (cxadc_mode && app->selected_device >= 0 && app->selected_device < app->device_count) {
        cxadc_has_channel_b = (app->devices[app->selected_device].index > 1);
    }
    // Clockgen mode: CXADC Clockgen (2-card), MISRC Clockgen, or DdD Clockgen.
    // These auto-enable CH3 (headswitch) + stereo CH1/2 in their capture profiles.
    bool misrc_clockgen = gui_ui_selected_device_is_misrc_clockgen(app);
#ifdef ENABLE_DDD
    bool ddd_clockgen = gui_ui_selected_device_is_ddd_clockgen(app);
#else
    bool ddd_clockgen = false;
#endif
    bool is_clockgen_mode = cxadc_clockgen || misrc_clockgen || ddd_clockgen;
    bool single_channel_device = ddd_mode || fx3_mode || (cxadc_mode && !cxadc_has_channel_b);
    bool expected_mode = s_capture_mode_state_misrc;
    if (cxadc_mode || playback_device) {
        expected_mode = false;
    }
    bool mismatch_user = (app->user_capture_mode_misrc != expected_mode);
    bool mismatch_settings = (!cxadc_mode && !playback_device && app->settings.misrc_mode != expected_mode);
    bool mismatch_runtime = (!app->is_recording && app->capture_mode_runtime_misrc != expected_mode);
    if (mismatch_user || mismatch_settings || mismatch_runtime) {
        TraceLog(LOG_INFO,
                 "MODE TRACE: source=gui_ui_sync_capture_mode_state reconcile expected=%s before_user=%s before_runtime=%s before_settings=%s recording=%d",
                 gui_ui_capture_mode_name(expected_mode),
                 gui_ui_capture_mode_name(app->user_capture_mode_misrc),
                 gui_ui_capture_mode_name(app->capture_mode_runtime_misrc),
                 gui_ui_capture_mode_name(app->settings.misrc_mode),
                 app->is_recording ? 1 : 0);
    }
    app->user_capture_mode_misrc = expected_mode;
    if (!cxadc_mode && !playback_device) {
        app->settings.misrc_mode = expected_mode;
    }
    if (!app->is_recording) {
        app->capture_mode_runtime_misrc = expected_mode;
    }
    if (cxadc_mode) {
        bool cxadc_settings_changed = false;
        uint8_t cxadc_rf_bits_a = gui_ui_cxadc_rf_bits(app, 0);
        uint8_t cxadc_rf_bits_b = gui_ui_cxadc_rf_bits(app, cxadc_has_channel_b ? 1 : 0);
        float cxadc_base_rate_a_khz = gui_ui_cxadc_hw_rate_khz(app, 0);
        float cxadc_base_rate_b_khz = gui_ui_cxadc_hw_rate_khz(app, cxadc_has_channel_b ? 1 : 0);
        // Single-card CXADC has no RF-B source.
        if (!cxadc_has_channel_b && app->settings.capture_b) {
            app->settings.capture_b = false;
            cxadc_settings_changed = true;
            s_capture_b_forced_off_by_single_channel = true;
        }
        if (app->settings.rf_bits_a != cxadc_rf_bits_a) {
            app->settings.rf_bits_a = cxadc_rf_bits_a;
            cxadc_settings_changed = true;
        }
        if (app->settings.rf_bits_b != cxadc_rf_bits_b) {
            app->settings.rf_bits_b = cxadc_rf_bits_b;
            cxadc_settings_changed = true;
        }
        // In HW mode (resample off), sync the rate to the HW rate.
        // In SW mode (resample on), leave the user's downsample target
        // alone - the cycle sets it explicitly and the sync must not
        // override it back to the HW rate.
        if (!app->settings.enable_resample_a) {
            if (fabsf(app->settings.resample_rate_a - cxadc_base_rate_a_khz) > 0.5f) {
                app->settings.resample_rate_a = cxadc_base_rate_a_khz;
                cxadc_settings_changed = true;
            }
        }
        if (!app->settings.enable_resample_b) {
            if (fabsf(app->settings.resample_rate_b - cxadc_base_rate_b_khz) > 0.5f) {
                app->settings.resample_rate_b = cxadc_base_rate_b_khz;
                cxadc_settings_changed = true;
            }
        }
        if (cxadc_settings_changed) {
            gui_settings_save(&app->settings);
        }
    }
#if defined(ENABLE_DDD) || defined(ENABLE_FX3)
    if (ddd_mode || fx3_mode) {
        // DdD and FX3 are single-channel: force channel A on, channel B off.
        // Channel B has no signal source, so recording it would only create a
        // silent/empty output.
        bool single_channel_settings_changed = false;
        if (!app->settings.capture_a) {
            app->settings.capture_a = true;
            single_channel_settings_changed = true;
        }
        if (app->settings.capture_b) {
            app->settings.capture_b = false;
            single_channel_settings_changed = true;
            s_capture_b_forced_off_by_single_channel = true;
        }
        if (single_channel_settings_changed) {
            gui_settings_save(&app->settings);
        }
    }
#endif
    // Clockgen audio auto-enable tracking: when in clockgen mode, the clockgen
    // profile auto-enables CH3 (headswitch) + stereo CH1/2. Mark that so when
    // leaving clockgen mode for a non-clockgen device (MISRC/HSDAOH/etc.),
    // we can disable them if the user didn't manually enable them.
    if (is_clockgen_mode) {
        s_clockgen_audio_auto_enabled = true;
    } else if (s_clockgen_audio_auto_enabled) {
        // Leaving clockgen mode: disable the clockgen-specific audio outputs
        // (CH3/headswitch + stereo CH1/2) that were auto-enabled by the
        // clockgen profile, unless the user manually enabled them in this
        // non-clockgen mode. The user can re-enable them manually if needed.
        bool clockgen_audio_changed = false;
        if (app->settings.enable_audio_1ch[2]) {
            app->settings.enable_audio_1ch[2] = false;
            clockgen_audio_changed = true;
        }
        if (app->settings.enable_audio_2ch_12) {
            app->settings.enable_audio_2ch_12 = false;
            clockgen_audio_changed = true;
        }
        s_clockgen_audio_auto_enabled = false;
        if (clockgen_audio_changed) {
            gui_settings_save(&app->settings);
        }
    }

    if (!single_channel_device) {
        bool restore_capture_b = false;
        if (s_capture_b_forced_off_by_single_channel && !app->settings.capture_b) {
            app->settings.capture_b = true;
            restore_capture_b = true;
        }
        s_capture_b_forced_off_by_single_channel = false;
        if (restore_capture_b) {
            gui_settings_save(&app->settings);
        }
    }
    gui_ui_trace_capture_mode_state(app, "gui_ui_sync_capture_mode_state", false);
}

static void gui_ui_set_capture_mode_state(gui_app_t *app, bool misrc_mode) {
    if (!app) return;
    bool old_mode = s_capture_mode_state_misrc;
    s_capture_mode_state_misrc = misrc_mode;
    s_capture_mode_state_initialized = true;
    app->user_capture_mode_misrc = misrc_mode;
    app->settings.misrc_mode = misrc_mode;
    if (!app->is_recording) {
        app->capture_mode_runtime_misrc = misrc_mode;
    }
    if (old_mode != misrc_mode) {
        TraceLog(LOG_INFO,
                 "MODE TRACE: source=CaptureModeToggle old=%s new=%s recording=%d capturing=%d",
                 gui_ui_capture_mode_name(old_mode),
                 gui_ui_capture_mode_name(misrc_mode),
                 app->is_recording ? 1 : 0,
                 app->is_capturing ? 1 : 0);
    }
    gui_ui_trace_capture_mode_state(app, "gui_ui_set_capture_mode_state", true);
}


static Clay_String make_string(const char *str) {
    return (Clay_String){ .isStaticallyAllocated = false, .length = (int32_t)strlen(str), .chars = str };
}

static void format_record_limit_timecode(char *dst, size_t dst_len, uint32_t total_seconds)
{
    if (!dst || dst_len == 0) return;
    uint32_t hh = total_seconds / 3600u;
    uint32_t mm = (total_seconds / 60u) % 60u;
    uint32_t ss = total_seconds % 60u;
    snprintf(dst, dst_len, "%02u:%02u:%02u", hh, mm, ss);
}

static bool parse_record_limit_timecode(const char *src, uint32_t *out_seconds)
{
    if (!src) return false;

    unsigned int hh = 0;
    unsigned int mm = 0;
    unsigned int ss = 0;
    char sep1 = 0;
    char sep2 = 0;

    int matched = sscanf(src, " %u%1[:/]%u%1[:/]%u ", &hh, &sep1, &mm, &sep2, &ss);
    if (matched != 5) {
        return false;
    }
    if (mm > 59u || ss > 59u) {
        return false;
    }

    uint64_t total = ((uint64_t)hh * 3600u) + ((uint64_t)mm * 60u) + (uint64_t)ss;
    if (total > (uint64_t)UINT32_MAX) {
        return false;
    }

    if (out_seconds) {
        *out_seconds = (uint32_t)total;
    }
    return true;
}

static void gui_record_limit_sync_settings(gui_app_t *app)
{
    if (!app) return;
    uint32_t parsed_seconds = 0;
    bool timecode_valid = parse_record_limit_timecode(s_record_limit_timecode, &parsed_seconds) && parsed_seconds > 0;
    app->settings.capture_limit_seconds = 0;
    app->settings.record_limit_seconds = (s_record_limit_armed && timecode_valid) ? parsed_seconds : 0;
}

static void gui_record_limit_log_state(gui_app_t *app, const char *prefix)
{
    if (!app || !prefix || !prefix[0]) return;
    char msg[192];
    uint32_t parsed_seconds = 0;
    bool timecode_valid = parse_record_limit_timecode(s_record_limit_timecode, &parsed_seconds) && parsed_seconds > 0;
    if (timecode_valid) {
        snprintf(msg, sizeof(msg), "%s: armed=%s timecode=%s seconds=%u",
                 prefix, s_record_limit_armed ? "yes" : "no",
                 s_record_limit_timecode, parsed_seconds);
    } else {
        snprintf(msg, sizeof(msg), "%s: armed=%s timecode=%s (invalid)",
                 prefix, s_record_limit_armed ? "yes" : "no",
                 s_record_limit_timecode);
    }
    gui_record_log_capture_event(app, "INFO", msg, GUI_ERROR_CLASS_NONE, 0);
}

static void format_status_free_space_label(char *dst, size_t dst_len, uint64_t free_bytes)
{
    if (!dst || dst_len == 0) return;
    if (free_bytes >= 1000000000ULL) {
        snprintf(dst, dst_len, "Free: %.2f GB", (double)free_bytes / 1000000000.0);
    } else if (free_bytes >= 1000000ULL) {
        snprintf(dst, dst_len, "Free: %.2f MB", (double)free_bytes / 1000000.0);
    } else if (free_bytes >= 1000ULL) {
        snprintf(dst, dst_len, "Free: %.2f KB", (double)free_bytes / 1000.0);
    } else {
        snprintf(dst, dst_len, "Free: %llu B", (unsigned long long)free_bytes);
    }
}
static void format_status_free_space_compact_label(char *dst, size_t dst_len, uint64_t free_bytes)
{
    if (!dst || dst_len == 0) return;
    if (free_bytes >= 1000000000ULL) {
        snprintf(dst, dst_len, "%.1fG", (double)free_bytes / 1000000000.0);
    } else if (free_bytes >= 1000000ULL) {
        snprintf(dst, dst_len, "%.1fM", (double)free_bytes / 1000000.0);
    } else if (free_bytes >= 1000ULL) {
        snprintf(dst, dst_len, "%.1fK", (double)free_bytes / 1000.0);
    } else {
        snprintf(dst, dst_len, "%lluB", (unsigned long long)free_bytes);
    }
}

// Keep growing status counters inside a predictable four-character budget.
// The detailed panels retain exact values; the footer only needs a compact
// at-a-glance magnitude once a count reaches four digits.
static void format_status_counter(char *dst, size_t dst_len, uint32_t value)
{
    if (!dst || dst_len == 0) return;
    if (value >= 1000000000U) {
        snprintf(dst, dst_len, "%uG", value / 1000000000U);
    } else if (value >= 1000000U) {
        snprintf(dst, dst_len, "%uM", value / 1000000U);
    } else if (value >= 1000U) {
        snprintf(dst, dst_len, "%uK", value / 1000U);
    } else {
        snprintf(dst, dst_len, "%u", value);
    }
}

// Defined below; tier 0 of the sample-rate readout reuses it verbatim.
static void format_live_msps_label(char *dst, size_t dst_len, uint32_t sample_rate_raw);

// Tiered status-bar sample count. Tier 0 is the full readout; higher tiers
// shorten the text so the width budget can reclaim space on narrow windows
// without hiding any readout.
static void format_status_samples_tier(char *dst, size_t dst_len, uint64_t value, int tier)
{
    if (!dst || dst_len == 0) return;
    if (tier <= 0) {
        if (value >= 1000000000ULL) {
            snprintf(dst, dst_len, "%.2fG", (double)value / 1000000000.0);
        } else if (value >= 1000000ULL) {
            snprintf(dst, dst_len, "%.2fM", (double)value / 1000000.0);
        } else if (value >= 1000ULL) {
            snprintf(dst, dst_len, "%.1fK", (double)value / 1000.0);
        } else {
            snprintf(dst, dst_len, "%llu", (unsigned long long)value);
        }
    } else if (tier == 1) {
        if (value >= 1000000000ULL) {
            snprintf(dst, dst_len, "%.1fG", (double)value / 1000000000.0);
        } else if (value >= 1000000ULL) {
            snprintf(dst, dst_len, "%.1fM", (double)value / 1000000.0);
        } else if (value >= 1000ULL) {
            snprintf(dst, dst_len, "%.0fK", (double)value / 1000.0);
        } else {
            snprintf(dst, dst_len, "%llu", (unsigned long long)value);
        }
    } else {
        if (value >= 1000000000ULL) {
            snprintf(dst, dst_len, "%.0fG", (double)value / 1000000000.0);
        } else if (value >= 1000000ULL) {
            snprintf(dst, dst_len, "%.0fM", (double)value / 1000000.0);
        } else if (value >= 1000ULL) {
            snprintf(dst, dst_len, "%.0fK", (double)value / 1000.0);
        } else {
            snprintf(dst, dst_len, "%llu", (unsigned long long)value);
        }
    }
}

// Tiered status-bar sample-rate readout (tier 0 = full "192.0 MSPS",
// tier 1 = integer "192 MSPS", tier 2 = magnitude form "192M").
static void format_status_sample_rate_tier(char *dst, size_t dst_len,
                                           uint32_t sample_rate_raw, int tier)
{
    if (!dst || dst_len == 0) return;
    if (sample_rate_raw == 0) {
        dst[0] = '\0';
        return;
    }
    /* Backward compatibility: some paths report kHz-style RF rates (40000=40MSPS),
     * while others report Hz. Normalize before display. */
    double hz = (double)sample_rate_raw;
    if (sample_rate_raw <= 100000U) {
        hz *= 1000.0;
    }
    double msps = hz / 1000000.0;
    if (tier <= 0) {
        format_live_msps_label(dst, dst_len, sample_rate_raw);
    } else if (tier == 1) {
        snprintf(dst, dst_len, "%d MSPS", (int)lround(msps));
    } else {
        snprintf(dst, dst_len, "%dM", (int)lround(msps));
    }
}

static uint64_t gui_ui_recording_output_total_bytes(const gui_app_t *app)
{
    if (!app) return 0;
    uint64_t raw_total = atomic_load(&app->recording_raw_a) + atomic_load(&app->recording_raw_b);
    if (!app->settings.use_flac) {
        return raw_total;
    }
    uint64_t encoded_total = atomic_load(&app->recording_compressed_a) + atomic_load(&app->recording_compressed_b);
    return (encoded_total > 0) ? encoded_total : raw_total;
}

static void format_status_runway_hhmmss(char *dst, size_t dst_len, double seconds)
{
    if (!dst || dst_len == 0) return;
    if (seconds < 0.0) seconds = 0.0;
    uint64_t total = (uint64_t)seconds;
    uint64_t hh = total / 3600ULL;
    uint64_t mm = (total / 60ULL) % 60ULL;
    uint64_t ss = total % 60ULL;
    snprintf(dst, dst_len, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, hh, mm, ss);
}

static void update_status_free_space(gui_app_t *app)
{
    if (!app) return;
    double now = GetTime();
    if (s_status_free_space_last_update_s > 0.0 &&
        (now - s_status_free_space_last_update_s) < STATUS_FREE_SPACE_REFRESH_INTERVAL_S) {
        return;
    }
    s_status_free_space_last_update_s = now;

    bool m_client=app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT;
    gui_monitor_record_stats_t remote={0};
    bool have_remote=m_client && gui_monitor_client_record_stats(&remote);
    bool remote_recording=have_remote && gui_monitor_client_peer_state()==2;
    uint64_t free_bytes = 0;
    bool free_valid;
    if(m_client){
        /* Local Save ON targets the client's disk. Otherwise the server's
         * configured Output folder, which may not exist on Windows. */
        bool use_local=app->settings.monitor_local_save_enabled;
        if(remote_recording)use_local=remote.local_save;
        free_valid=use_local
          ? file_disk_free_space_utf8(app->settings.monitor_local_save_path,&free_bytes)
          : have_remote && remote.server_free_valid;
        if(!use_local && free_valid)free_bytes=remote.server_free_bytes;
    }else free_valid=gui_record_get_output_free_space_bytes(app,&free_bytes);
    if (free_valid) {
        s_status_free_space_cached_bytes = free_bytes;
        s_status_free_space_valid = true;
    } else {
        s_status_free_space_valid = false;
    }

    if (!app->is_recording && !remote_recording) {
        s_status_output_last_bytes = 0;
        s_status_output_last_sample_s = 0.0;
        s_status_output_rate_bps = 0.0;
        return;
    }

    uint64_t output_bytes = gui_ui_recording_output_total_bytes(app);
    if(m_client && have_remote){
        output_bytes=remote.use_flac
            ? remote.compressed[0]+remote.compressed[1]
            : remote.raw[0]+remote.raw[1];
    }
    if (s_status_output_last_sample_s > 0.0 && output_bytes >= s_status_output_last_bytes) {
        double elapsed_s = now - s_status_output_last_sample_s;
        if (elapsed_s > 0.0) {
            double instant_bps = (double)(output_bytes - s_status_output_last_bytes) / elapsed_s;
            if (s_status_output_rate_bps <= 0.0) {
                s_status_output_rate_bps = instant_bps;
            } else {
                s_status_output_rate_bps = (s_status_output_rate_bps * 0.75) + (instant_bps * 0.25);
            }
        }
    }
    s_status_output_last_bytes = output_bytes;
    s_status_output_last_sample_s = now;
}

static void gui_record_limit_runtime_tick(gui_app_t *app)
{
    if (!app) return;

    if (!app->is_recording) {
        s_record_limit_session_seen = false;
        s_record_limit_deadline_active = false;
        s_record_limit_deadline_s = 0.0;
        gui_record_limit_sync_settings(app);
        return;
    }

    if (!s_record_limit_session_seen) {
        s_record_limit_session_seen = true;
        s_record_limit_deadline_active = false;
        s_record_limit_deadline_s = 0.0;
    }

    uint32_t parsed_seconds = 0;
    bool timecode_valid = parse_record_limit_timecode(s_record_limit_timecode, &parsed_seconds) && parsed_seconds > 0;
    if (timecode_valid) {
        s_record_limit_seconds = parsed_seconds;
    }
    gui_record_limit_sync_settings(app);

    if (!s_record_limit_armed || !timecode_valid) {
        s_record_limit_deadline_active = false;
        s_record_limit_deadline_s = 0.0;
        return;
    }

    double now = GetTime();
    double requested_deadline_s = app->recording_start_time + (double)s_record_limit_seconds;

    if (!s_record_limit_deadline_active) {
        // If the requested limit is already behind elapsed recording time,
        // ignore it while recording (only longer extensions are applied live).
        if (requested_deadline_s > now) {
            s_record_limit_deadline_active = true;
            s_record_limit_deadline_s = requested_deadline_s;
        }
        return;
    }

    // Allow only extensions while currently recording.
    if (requested_deadline_s > s_record_limit_deadline_s) {
        s_record_limit_deadline_s = requested_deadline_s;
    }

    if (now >= s_record_limit_deadline_s) {
        gui_record_log_capture_event(app, "INFO", "Record timer reached deadline; stopping recording",
                                     GUI_ERROR_CLASS_NONE, 0);
        gui_app_set_status(app, "Record time limit reached");
        gui_app_stop_recording(app);
        s_record_limit_deadline_active = false;
        s_record_limit_deadline_s = 0.0;
        s_record_limit_session_seen = false;
    }
}

static Color ui_disabled_color(Color c) {
    // Dim and slightly transparent.
    return (Color){ (unsigned char)(c.r * 0.55f), (unsigned char)(c.g * 0.55f), (unsigned char)(c.b * 0.55f), (unsigned char)(c.a * 0.80f) };
}
static int gui_ui_clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static int gui_ui_measure_button_width(const gui_app_t *app,
                                       const char *text,
                                       int font_size,
                                       int horizontal_padding,
                                       int extra_width,
                                       int min_width,
                                       int max_width)
{
    Font font = GetFontDefault();
    if (app && app->fonts && app->fonts[0].texture.id != 0) {
        font = app->fonts[0];
    }
    if (!font.glyphs) {
        font = GetFontDefault();
    }
    const char *safe_text = (text && text[0]) ? text : " ";
    Vector2 m = gui_font_measure_text(font, safe_text, (float)font_size, 0.0f);
    int measured_width = (int)ceilf(m.x) + (horizontal_padding * 2) + extra_width;
    return gui_ui_clamp_int(measured_width, min_width, max_width);
}

static int gui_ui_measure_text_width(const gui_app_t *app,
                                     const char *text,
                                     int font_size,
                                     int font_id)
{
    Font font = GetFontDefault();
    if (app && app->fonts) {
        if (font_id == 1) {
            if (app->fonts[1].texture.id != 0) {
                font = app->fonts[1];
            }
        } else if (app->fonts[0].texture.id != 0) {
            font = app->fonts[0];
        }
    }
    if (!font.glyphs) {
        font = GetFontDefault();
    }
    const char *safe_text = (text && text[0]) ? text : " ";
    Vector2 m = gui_font_measure_text(font, safe_text, (float)font_size, 0.0f);
    return (int)ceilf(m.x);
}

static void gui_ui_ellipsize_text(const gui_app_t *app,
                                  char *text,
                                  size_t text_capacity,
                                  int font_size,
                                  int font_id,
                                  int max_text_width)
{
    if (!text || text_capacity == 0 || text[0] == '\0') return;
    Font font = GetFontDefault();
    if (app && app->fonts) {
        if (font_id == 1) {
            if (app->fonts[1].texture.id != 0) {
                font = app->fonts[1];
            }
        } else if (app->fonts[0].texture.id != 0) {
            font = app->fonts[0];
        }
    }
    if (!font.glyphs) {
        font = GetFontDefault();
    }
    if (gui_font_measure_text(font, text, (float)font_size, 0.0f).x <=
        (float)max_text_width) {
        return;
    }

    size_t cut = strlen(text);
    while (cut > 0) {
        cut--;
        while (cut > 0 && (((unsigned char)text[cut] & 0xC0U) == 0x80U)) {
            cut--;
        }
        text[cut] = '\0';
        strncat(text, "...", text_capacity - strlen(text) - 1);
        if (gui_font_measure_text(font, text, (float)font_size, 0.0f).x <=
            (float)max_text_width) {
            return;
        }
        text[cut] = '\0';
    }
    snprintf(text, text_capacity, "...");
}

/* Keep display aliases separate from names used by capture logs/metadata. */
static void gui_ui_format_device_label(const device_info_t *device,
                                       bool compact, char *label, size_t size)
{
    ddd_device_profile_t profile = DDD_DEVICE_NOT_DDD;
    bool clockgen = false;
#ifdef ENABLE_DDD
    if (device && device->type == DEVICE_TYPE_DDD) {
        profile = device->ddd_profile;
        clockgen = gui_ddd_clockgen_device_mode(device);
    }
#endif
    gui_device_format_label(device ? device->name : "No devices",
                             profile, clockgen, compact, label, size);
}

static const char *rf_bits_label(uint8_t bits) {
    switch (bits) {
        case 8: return "8";
        case 12: return "12";
        default: return "16";
    }
}

static void format_msps_label(char *dst, size_t dst_len, float khz) {
    if (!dst || dst_len == 0) return;
    double msps = (double)khz / 1000.0;
    // Trim trailing .0
    if (fabs(msps - (double)((int)msps)) < 1e-6) {
        snprintf(dst, dst_len, "%d MSPS", (int)msps);
    } else {
        snprintf(dst, dst_len, "%.1f MSPS", msps);
    }
}
static void format_live_msps_label(char *dst, size_t dst_len, uint32_t sample_rate_raw) {
    if (!dst || dst_len == 0) return;
    if (sample_rate_raw == 0) {
        dst[0] = '\0';
        return;
    }

    /* Backward compatibility: some paths report kHz-style RF rates (40000=40MSPS),
     * while others report Hz. Normalize before display. */
    double hz = (double)sample_rate_raw;
    if (sample_rate_raw <= 100000U) {
        hz *= 1000.0;
    }

    double msps = hz / 1000000.0;
    if (fabs(msps - round(msps)) < 1e-6) {
        snprintf(dst, dst_len, "%d MSPS", (int)lround(msps));
    } else {
        snprintf(dst, dst_len, "%.1f MSPS", msps);
    }
}



static float cycle_resample_khz(float current_khz, float max_khz) {
    // User-facing presets (stored as kHz). The set depends on whether the
    // card is stock (28.6 MHz base) or clockgen/modded (40+ MHz base):
    //   Stock (base ~28.6): 5, 14.3, 17.9, 28.6
    //   Clockgen/40-mod (base ~40): 5, 10, 20, 40  (no 14.3/17.9/28.6)
    //   54-mod (base ~54): 5, 10, 20, 27, 40, 54  (no 14.3/17.9/28.6)
    // 14.3/17.9 are stock-28.6 software modes, hidden when clockgen (40+)
    // is detected. 5 = HiFi audio SW-resample (always available). 35.8
    // (upsampled 28.6) is never offered. 8-bit is the assumed stock default.
    if (max_khz < 5000.0f) max_khz = 5000.0f;
    bool stock_base = (max_khz < 40000.0f - 0.5f);

    float presets[16];
    int n = 0;
    presets[n++] = 5000.0f;
    if (stock_base) {
        presets[n++] = 14300.0f;
        presets[n++] = 17900.0f;
        presets[n++] = 28636.0f;
    } else {
        presets[n++] = 10000.0f;
        presets[n++] = 20000.0f;
        if (max_khz >= 54000.0f - 0.5f) presets[n++] = 27000.0f;
        presets[n++] = 40000.0f;
        if (max_khz >= 54000.0f - 0.5f) presets[n++] = 54000.0f;
    }

    float allowed[16];
    int allowed_count = 0;
    for (int i = 0; i < n; i++) {
        if (presets[i] <= max_khz + 0.5f) {
            allowed[allowed_count++] = presets[i];
        }
    }
    if (allowed_count <= 0) {
        return 5000.0f;
    }

    // Find nearest preset (within 1 kHz), otherwise start from first.
    int idx = -1;
    for (int i = 0; i < allowed_count; i++) {
        if (fabsf(current_khz - allowed[i]) < 1.0f) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return allowed[0];
    return allowed[(idx + 1) % allowed_count];
}

#ifdef ENABLE_DDD
static uint32_t gui_ui_ddd_v1_resample_setting_khz(const gui_app_t *app)
{
    float rate_khz;
    if (!app) return 0;
    rate_khz = app->settings.resample_rate_a;
    if (!isfinite(rate_khz) || rate_khz <= 0.0f ||
        rate_khz > (float)ddd_sample_rate_khz(
            DDD_DECIMATION_FULL_RATE) + 0.5f) {
        return 0;
    }
    return (uint32_t)lroundf(rate_khz);
}

static bool gui_ui_ddd_v1_rate_plan(const gui_app_t *app,
                                    ddd_v1_rate_plan_t *plan)
{
    uint32_t output_rate_khz;
    if (!app || !plan) return false;
    output_rate_khz = ddd_v1_effective_output_rate_khz(
        app->settings.enable_resample_a,
        gui_ui_ddd_v1_resample_setting_khz(app),
        app->settings.ddd_decimation);
    return ddd_v1_plan_output_rate_khz(output_rate_khz, plan);
}

static bool gui_ui_set_ddd_v1_output_rate(gui_app_t *app,
                                          uint32_t output_rate_khz)
{
    ddd_v1_rate_plan_t plan;
    char hardware_label[32];
    char output_label[32];
    char message[96];

    if (!app || !ddd_v1_plan_output_rate_khz(output_rate_khz, &plan)) {
        return false;
    }

    app->settings.ddd_decimation = plan.decimation_factor;
    app->settings.enable_resample_a = plan.software_resample;
    app->settings.resample_rate_a = (float)plan.output_rate_khz;
    gui_settings_save(&app->settings);

    format_msps_label(hardware_label, sizeof(hardware_label),
                      (float)plan.hardware_rate_khz);
    format_msps_label(output_label, sizeof(output_label),
                      (float)plan.output_rate_khz);
    if (plan.software_resample) {
        snprintf(message, sizeof(message), "%s HW -> %s SW",
                 hardware_label, output_label);
    } else {
        snprintf(message, sizeof(message), "DdD: %s HW", output_label);
    }
    gui_app_set_status(app, message);
    return true;
}
#endif

static bool gui_ui_flac_affinity_supported(void) {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

static bool gui_ui_flac_affinity_char_allowed(int ch) {
    return ((ch >= '0' && ch <= '9') || ch == ',' || ch == '-' || ch == ' ' || ch == '\t');
}

static bool gui_ui_is_text_field_active(ui_text_field_t field)
{
    return s_active_text_field == field;
}

static void gui_ui_clear_text_edit(void)
{
    s_active_text_field = UI_TEXT_FIELD_NONE;
    s_active_text_cursor = 0;
    s_active_text_selection_anchor = -1;
    s_active_text_drag_selecting = false;
    s_active_text_element_id = (Clay_ElementId){ 0 };
    s_active_text_left_padding = 0.0f;
    s_active_text_right_padding = 0.0f;
    s_active_text_last_click_time = -1.0;
    s_active_text_last_click_element_id = (Clay_ElementId){ 0 };
    s_active_text_backspace_repeat_at = 0.0;
}

static bool gui_ui_settings_locked(const gui_app_t *app)
{
    return app && (app->is_recording ||
        (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT &&
         gui_monitor_client_peer_state() == 2) ||
        gui_playback_is_starting((gui_app_t *)app));
}

// One-time per-session low-rate warning: pops a dismissible info dialog
// only when the resample cycle lands on the 14.3 MSPS stock 10-bit CXADC
// rate. 5/10 MSPS are intentional SW-resample targets for HiFi audio-only
// capture and don't trigger the warning.
#define CXADC_LOW_RATE_WARN_KHZ 14300.0f
static bool s_low_rate_warned_this_session = false;

static void gui_ui_warn_low_rate(gui_app_t *app, float rate_khz)
{
    if (!app) return;
    if (s_low_rate_warned_this_session) return;
    if (fabsf(rate_khz - CXADC_LOW_RATE_WARN_KHZ) > 50.0f) return;
    s_low_rate_warned_this_session = true;
    char msg[384];
    snprintf(msg, sizeof(msg),
        "RF rate set to 14.3 MSPS - stock 10-bit CXADC rate.\n\n"
        "Minimum bandwidths for reliable video decode:\n"
        "  20 MSPS  - VHS / Video8 / Betamax\n"
        "  24 MSPS+ - S-VHS / Hi8 / U-matic\n"
        "  40 MSPS  - LaserDisc / 1\" SMPTE / 2\" Quad\n\n"
        "HW rates: 14.3 / 20 / 28.6 / 40 / 54 MSPS.\n\n"
        "Below 20 MSPS is not viable for archival video capture.");
    gui_dropdown_close_all();
    gui_ui_clear_text_edit();
    gui_popup_info("Low RF sample rate", msg);
}



static bool gui_ui_text_field_get_buffer(gui_app_t *app, ui_text_field_t field, char **dst, size_t *cap)
{
    if (!app || !dst || !cap) return false;

    switch (field) {
        case UI_TEXT_FIELD_OUTPUT_BASE_NAME:
            *dst = app->settings.output_base_name;
            *cap = sizeof(app->settings.output_base_name);
            return true;
        case UI_TEXT_FIELD_OUTPUT_PATH:
            *dst = app->settings.output_path;
            *cap = sizeof(app->settings.output_path);
            return true;
        case UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH:
            *dst=s_monitor_folder_edit_path;
            *cap=sizeof(s_monitor_folder_edit_path);
            return true;
        case UI_TEXT_FIELD_MONITOR_LOCAL_SAVE_PATH:
            *dst = app->settings.monitor_local_save_path;
            *cap = sizeof(app->settings.monitor_local_save_path);
            return true;
        case UI_TEXT_FIELD_FLAC_AFFINITY:
            *dst = app->settings.flac_affinity_cpu_list;
            *cap = sizeof(app->settings.flac_affinity_cpu_list);
            return true;
        case UI_TEXT_FIELD_RF_TAG_A:
            *dst = app->settings.rf_channel_tags[0];
            *cap = sizeof(app->settings.rf_channel_tags[0]);
            return true;
        case UI_TEXT_FIELD_RF_TAG_B:
            *dst = app->settings.rf_channel_tags[1];
            *cap = sizeof(app->settings.rf_channel_tags[1]);
            return true;
        case UI_TEXT_FIELD_AUDIO_TAG_4CH:
            *dst = app->settings.audio_output_tags[0];
            *cap = sizeof(app->settings.audio_output_tags[0]);
            return true;
        case UI_TEXT_FIELD_AUDIO_TAG_12:
            *dst = app->settings.audio_output_tags[1];
            *cap = sizeof(app->settings.audio_output_tags[1]);
            return true;
        case UI_TEXT_FIELD_AUDIO_TAG_34:
            *dst = app->settings.audio_output_tags[2];
            *cap = sizeof(app->settings.audio_output_tags[2]);
            return true;
        case UI_TEXT_FIELD_AUDIO_LABEL_1:
            *dst = app->settings.audio_1ch_labels[0];
            *cap = sizeof(app->settings.audio_1ch_labels[0]);
            return true;
        case UI_TEXT_FIELD_AUDIO_LABEL_2:
            *dst = app->settings.audio_1ch_labels[1];
            *cap = sizeof(app->settings.audio_1ch_labels[1]);
            return true;
        case UI_TEXT_FIELD_AUDIO_LABEL_3:
            *dst = app->settings.audio_1ch_labels[2];
            *cap = sizeof(app->settings.audio_1ch_labels[2]);
            return true;
        case UI_TEXT_FIELD_AUDIO_LABEL_4:
            *dst = app->settings.audio_1ch_labels[3];
            *cap = sizeof(app->settings.audio_1ch_labels[3]);
            return true;
        case UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL:
            *dst = app->settings.level_autostop_level_str;
            *cap = sizeof(app->settings.level_autostop_level_str);
            return true;
        case UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION:
            *dst = app->settings.level_autostop_duration_str;
            *cap = sizeof(app->settings.level_autostop_duration_str);
            return true;
        case UI_TEXT_FIELD_INGEST_PROJECT:
            *dst = app->settings.ingest_project;
            *cap = sizeof(app->settings.ingest_project);
            return true;
        case UI_TEXT_FIELD_INGEST_TAPE_ID:
            *dst = app->settings.ingest_tape_id;
            *cap = sizeof(app->settings.ingest_tape_id);
            return true;
        case UI_TEXT_FIELD_INGEST_TAPE_FORMAT:
            *dst = app->settings.ingest_tape_format;
            *cap = sizeof(app->settings.ingest_tape_format);
            return true;
        case UI_TEXT_FIELD_INGEST_TAPE_SIZE:
            *dst = app->settings.ingest_tape_size;
            *cap = sizeof(app->settings.ingest_tape_size);
            return true;
        case UI_TEXT_FIELD_INGEST_TAPE_SPEED:
            *dst = app->settings.ingest_tape_speed;
            *cap = sizeof(app->settings.ingest_tape_speed);
            return true;
        case UI_TEXT_FIELD_INGEST_TAPE_CONDITION:
            *dst = app->settings.ingest_tape_condition;
            *cap = sizeof(app->settings.ingest_tape_condition);
            return true;
        case UI_TEXT_FIELD_INGEST_OPERATOR:
            *dst = app->settings.ingest_operator;
            *cap = sizeof(app->settings.ingest_operator);
            return true;
        case UI_TEXT_FIELD_INGEST_LOCATION:
            *dst = app->settings.ingest_location;
            *cap = sizeof(app->settings.ingest_location);
            return true;
        case UI_TEXT_FIELD_INGEST_NOTES:
            *dst = app->settings.ingest_notes;
            *cap = sizeof(app->settings.ingest_notes);
            return true;
        case UI_TEXT_FIELD_NET_SERVER_PORT:
            *dst = app->settings.net_server_port_str;
            *cap = sizeof(app->settings.net_server_port_str);
            return true;
        case UI_TEXT_FIELD_NET_CLIENT_HOST:
            *dst = app->settings.net_client_host;
            *cap = sizeof(app->settings.net_client_host);
            return true;
        case UI_TEXT_FIELD_NET_CLIENT_PORT:
            *dst = app->settings.net_client_port_str;
            *cap = sizeof(app->settings.net_client_port_str);
            return true;
        default:
            return false;
    }
}

static bool gui_ui_text_field_can_edit(gui_app_t *app, ui_text_field_t field)
{
    if (!app) return false;
    // Level autostop fields live in the record-limit (timer) window, which allows
    // live edits while recording (like the timecode), so they don't require the
    // settings panel and bypass the recording lock.
    if (field == UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL ||
        field == UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION) {
        return s_record_limit_window_open && app->settings.level_autostop_enabled;
    }
    if (field == UI_TEXT_FIELD_INGEST_PROJECT ||
        field == UI_TEXT_FIELD_INGEST_TAPE_ID ||
        field == UI_TEXT_FIELD_INGEST_TAPE_FORMAT ||
        field == UI_TEXT_FIELD_INGEST_TAPE_SIZE ||
        field == UI_TEXT_FIELD_INGEST_TAPE_SPEED ||
        field == UI_TEXT_FIELD_INGEST_TAPE_CONDITION ||
        field == UI_TEXT_FIELD_INGEST_OPERATOR ||
        field == UI_TEXT_FIELD_INGEST_LOCATION ||
        field == UI_TEXT_FIELD_INGEST_NOTES) {
        return s_metadata_window_open;
    }
    // Network fields live in the info ("About") window and are editable while
    // that window is open, independent of the settings panel / recording lock.
    if (field == UI_TEXT_FIELD_NET_SERVER_PORT ||
        field == UI_TEXT_FIELD_NET_CLIENT_HOST ||
        field == UI_TEXT_FIELD_NET_CLIENT_PORT) {
        return s_version_info_window_open;
    }
    if (!app->settings_panel_open || gui_ui_settings_locked(app)) return false;
    switch (field) {
        case UI_TEXT_FIELD_OUTPUT_BASE_NAME:
            return app->settings.auto_names_enabled;
        case UI_TEXT_FIELD_OUTPUT_PATH:
            /* Server Output path is authoritative in M-Client. Use browser. */
            return app->settings.net_mode!=GUI_NET_MODE_MONITOR_CLIENT;
        case UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH:
            return app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT && s_monitor_folder_open;
        case UI_TEXT_FIELD_MONITOR_LOCAL_SAVE_PATH:
            return app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT;
        case UI_TEXT_FIELD_FLAC_AFFINITY:
            return app->settings.show_core_pinning_in_settings &&
                   app->settings.use_flac &&
                   app->settings.flac_affinity_enabled &&
                   gui_ui_flac_affinity_supported();
        case UI_TEXT_FIELD_RF_TAG_A:
        case UI_TEXT_FIELD_RF_TAG_B:
        case UI_TEXT_FIELD_AUDIO_TAG_4CH:
        case UI_TEXT_FIELD_AUDIO_TAG_12:
        case UI_TEXT_FIELD_AUDIO_TAG_34:
        case UI_TEXT_FIELD_AUDIO_LABEL_1:
        case UI_TEXT_FIELD_AUDIO_LABEL_2:
        case UI_TEXT_FIELD_AUDIO_LABEL_3:
        case UI_TEXT_FIELD_AUDIO_LABEL_4:
            return app->settings.auto_names_enabled;
        case UI_TEXT_FIELD_RTLSDR_FREQ: {
#ifdef ENABLE_RTLSDR
            return gui_ui_selected_device_is_sdr(app);
#else
            return false;
#endif
        }
        default:
            return false;
    }
}

static bool gui_ui_text_field_char_allowed(ui_text_field_t field, int ch)
{
    if (field == UI_TEXT_FIELD_FLAC_AFFINITY) {
        return gui_ui_flac_affinity_char_allowed(ch);
    }
    if (field == UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL) {
        // Normalized 0.X level (0.1-0.8): digits and a single '.' (allow typing;
        // parse clamps on commit).
        return (ch >= '0' && ch <= '9') || ch == '.';
    }
    if (field == UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION) {
        // Decimal seconds: digits and a single '.' (allow typing; parse clamps).
        return (ch >= '0' && ch <= '9') || ch == '.';
    }
    if (field == UI_TEXT_FIELD_INGEST_PROJECT ||
        field == UI_TEXT_FIELD_INGEST_TAPE_ID ||
        field == UI_TEXT_FIELD_INGEST_TAPE_FORMAT ||
        field == UI_TEXT_FIELD_INGEST_TAPE_SIZE ||
        field == UI_TEXT_FIELD_INGEST_TAPE_SPEED ||
        field == UI_TEXT_FIELD_INGEST_TAPE_CONDITION ||
        field == UI_TEXT_FIELD_INGEST_OPERATOR ||
        field == UI_TEXT_FIELD_INGEST_LOCATION ||
        field == UI_TEXT_FIELD_INGEST_NOTES) {
        // Keep these permissive for ingest entry, but still block JSON-breaking quote chars.
        return (ch >= 32 && ch < 127 && ch != '\"');
    }
    if (field == UI_TEXT_FIELD_RTLSDR_FREQ) {
        // Frequency in Hz: digits only (parsed to uint64 on commit).
        return (ch >= '0' && ch <= '9');
    }
    if (field == UI_TEXT_FIELD_NET_SERVER_PORT ||
        field == UI_TEXT_FIELD_NET_CLIENT_PORT) {
        // TCP port: digits only.
        return (ch >= '0' && ch <= '9');
    }
    if (field == UI_TEXT_FIELD_NET_CLIENT_HOST) {
        // Hostname or IP: allow printable ASCII except quote (JSON-safe).
        return (ch >= 32 && ch < 127 && ch != '\"');
    }
    return gui_text_path_char_allowed(ch,
            field == UI_TEXT_FIELD_OUTPUT_PATH ||
            field == UI_TEXT_FIELD_MONITOR_LOCAL_SAVE_PATH ||
            field == UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH);
}

static void gui_ui_text_field_font(ui_text_field_t field, int *font_size, int *font_id)
{
    int size = FONT_SIZE_NORMAL;
    int id = 0;
    switch (field) {
        case UI_TEXT_FIELD_OUTPUT_BASE_NAME:
        case UI_TEXT_FIELD_OUTPUT_PATH:
        case UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH:
        case UI_TEXT_FIELD_MONITOR_LOCAL_SAVE_PATH:
            size = FONT_SIZE_NORMAL;
            id = 0;
            break;
        case UI_TEXT_FIELD_FLAC_AFFINITY:
            size = FONT_SIZE_STATS;
            id = 0;
            break;
        case UI_TEXT_FIELD_RF_TAG_A:
        case UI_TEXT_FIELD_RF_TAG_B:
        case UI_TEXT_FIELD_AUDIO_TAG_4CH:
        case UI_TEXT_FIELD_AUDIO_TAG_12:
        case UI_TEXT_FIELD_AUDIO_TAG_34:
        case UI_TEXT_FIELD_AUDIO_LABEL_1:
        case UI_TEXT_FIELD_AUDIO_LABEL_2:
        case UI_TEXT_FIELD_AUDIO_LABEL_3:
        case UI_TEXT_FIELD_AUDIO_LABEL_4:
        case UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL:
        case UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION:
        case UI_TEXT_FIELD_INGEST_PROJECT:
        case UI_TEXT_FIELD_INGEST_TAPE_ID:
        case UI_TEXT_FIELD_INGEST_TAPE_FORMAT:
        case UI_TEXT_FIELD_INGEST_TAPE_SIZE:
        case UI_TEXT_FIELD_INGEST_TAPE_SPEED:
        case UI_TEXT_FIELD_INGEST_TAPE_CONDITION:
        case UI_TEXT_FIELD_INGEST_OPERATOR:
        case UI_TEXT_FIELD_INGEST_LOCATION:
        case UI_TEXT_FIELD_INGEST_NOTES:
            size = FONT_SIZE_STATS;
            id = 1;
            break;
        default:
            size = FONT_SIZE_NORMAL;
            id = 0;
            break;
    }
    if (font_size) *font_size = size;
    if (font_id) *font_id = id;
}

static Font gui_ui_text_get_font(const gui_app_t *app, int font_id)
{
    Font font = GetFontDefault();
    if (app && app->fonts && font_id >= 0 && font_id < 2 && app->fonts[font_id].texture.id != 0) {
        font = app->fonts[font_id];
    }
    if (!font.glyphs) {
        font = GetFontDefault();
    }
    return font;
}

static float gui_ui_text_char_width_px(const gui_app_t *app, int font_id, int font_size, const char *text, int count)
{
    Font font = gui_ui_text_get_font(app, font_id);
    char glyph[5] = {0};
    if (count > 4) count = 1;
    memcpy(glyph, text, (size_t)count);
    Vector2 m = gui_font_measure_text(font, glyph, (float)font_size, 0.0f);
    if (m.x <= 0.0f) {
        return (float)font_size * 0.5f;
    }
    return m.x;
}

static int gui_ui_text_cursor_from_click(gui_app_t *app,
                                         ui_text_field_t field,
                                         Clay_ElementId element_id,
                                         const char *text,
                                         float left_padding,
                                         float right_padding)
{
    size_t len = text ? strlen(text) : 0;
    Clay_ElementData element_data = Clay_GetElementData(element_id);
    if (!element_data.found || len == 0) return (int)len;

    Vector2 mouse = gui_ui_get_mouse_position();
    float content_left = element_data.boundingBox.x + left_padding;
    float content_width = element_data.boundingBox.width - (left_padding + right_padding);
    if (content_width < 1.0f) return (int)len;

    float local_x = mouse.x - content_left;
    if (local_x < 0.0f) local_x = 0.0f;
    if (local_x > content_width) local_x = content_width;
    int font_size = FONT_SIZE_NORMAL;
    int font_id = 0;
    gui_ui_text_field_font(field, &font_size, &font_id);

    float x = 0.0f;
    for (int i = 0; i < (int)len; ) {
        int next = gui_text_next(text, i);
        float w = gui_ui_text_char_width_px(app, font_id, font_size, text + i, next - i);
        if (local_x < x + (w * 0.5f)) {
            return i;
        }
        x += w;
        i = next;
    }
    return (int)len;
}

static void gui_ui_text_clamp_state(const char *dst)
{
    size_t len = dst ? strlen(dst) : 0;
    if (s_active_text_cursor < 0) s_active_text_cursor = 0;
    if ((size_t)s_active_text_cursor > len) s_active_text_cursor = (int)len;
    s_active_text_cursor = gui_text_boundary(dst ? dst : "", s_active_text_cursor);
    if (s_active_text_selection_anchor >= 0) {
        if (s_active_text_selection_anchor < 0) s_active_text_selection_anchor = 0;
        if ((size_t)s_active_text_selection_anchor > len) s_active_text_selection_anchor = (int)len;
        s_active_text_selection_anchor = gui_text_boundary(dst ? dst : "", s_active_text_selection_anchor);
        if (s_active_text_selection_anchor == s_active_text_cursor) {
            s_active_text_selection_anchor = -1;
        }
    }
}

static bool gui_ui_text_get_selection_range(const char *dst, int *start, int *end)
{
    if (!dst || !start || !end || s_active_text_selection_anchor < 0) return false;
    gui_ui_text_clamp_state(dst);
    if (s_active_text_selection_anchor < 0 || s_active_text_selection_anchor == s_active_text_cursor) {
        return false;
    }
    if (s_active_text_selection_anchor < s_active_text_cursor) {
        *start = s_active_text_selection_anchor;
        *end = s_active_text_cursor;
    } else {
        *start = s_active_text_cursor;
        *end = s_active_text_selection_anchor;
    }
    return (*end > *start);
}

static bool gui_ui_text_delete_selection(char *dst)
{
    if (!dst) return false;
    int start = 0, end = 0;
    if (!gui_ui_text_get_selection_range(dst, &start, &end)) return false;
    size_t len = strlen(dst);
    memmove(dst + start, dst + end, len - (size_t)end + 1);
    s_active_text_cursor = start;
    s_active_text_selection_anchor = -1;
    return true;
}

static void gui_ui_text_set_cursor_position(const char *dst, int new_cursor, bool keep_selection)
{
    size_t len = dst ? strlen(dst) : 0;
    if (new_cursor < 0) new_cursor = 0;
    if ((size_t)new_cursor > len) new_cursor = (int)len;
    new_cursor = gui_text_boundary(dst ? dst : "", new_cursor);
    if (keep_selection) {
        if (s_active_text_selection_anchor < 0) {
            s_active_text_selection_anchor = s_active_text_cursor;
        }
    } else {
        s_active_text_selection_anchor = -1;
    }
    s_active_text_cursor = new_cursor;
}

static bool gui_ui_text_insert_char(char *dst, size_t cap, int ch)
{
    if (!dst || cap == 0 || ch <= 0 || ch > 0x10ffff ||
        (ch >= 0xd800 && ch <= 0xdfff)) return false;
    int count = 0;
    const char *text = CodepointToUTF8(ch, &count);
    int start = s_active_text_cursor, end = start;
    gui_ui_text_get_selection_range(dst, &start, &end);
    if (!gui_text_replace(dst, cap, &s_active_text_cursor, start, end, text, (size_t)count)) {
        gui_popup_info("Text is too long", "The text was not changed because it exceeds this field's capacity.");
        return false;
    }
    s_active_text_selection_anchor = -1;
    return true;
}

static bool gui_ui_text_insert_filtered(ui_text_field_t field, char *dst, size_t cap, const char *src)
{
    if (!dst || !src || cap == 0) return false;
    char *filtered = (char *)malloc(cap);
    if (!filtered) return false;
    size_t count = 0;
    bool valid = true;
    bool path_field = field == UI_TEXT_FIELD_OUTPUT_BASE_NAME ||
                      field == UI_TEXT_FIELD_OUTPUT_PATH ||
                      field == UI_TEXT_FIELD_MONITOR_LOCAL_SAVE_PATH ||
                      field == UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH ||
                      (field >= UI_TEXT_FIELD_RF_TAG_A && field <= UI_TEXT_FIELD_AUDIO_LABEL_4);
    for (const char *p = src; *p; ) {
        int size = 0;
        int ch = GetCodepointNext(p, &size);
        int encoded_size = 0;
        const char *encoded = CodepointToUTF8(ch, &encoded_size);
        if (size <= 0 || size != encoded_size || memcmp(p, encoded, (size_t)size) != 0) {
            valid = false;
            break;
        }
        if (!gui_ui_text_field_char_allowed(field, ch)) {
            if (path_field) { valid = false; break; }
            p += size;
            continue;
        }
        if ((size_t)size >= cap - count) { valid = false; break; }
        memcpy(filtered + count, p, (size_t)size);
        count += (size_t)size;
        p += size;
    }
    int start = s_active_text_cursor, end = start;
    gui_ui_text_get_selection_range(dst, &start, &end);
    bool changed = valid && count > 0 &&
                   gui_text_replace(dst, cap, &s_active_text_cursor, start, end, filtered, count);
    free(filtered);
    if (changed) s_active_text_selection_anchor = -1;
    else if (!valid || count > 0) {
        gui_popup_info("Cannot paste text", "The text was not changed. It is too long or contains characters this field cannot accept.");
    }
    return changed;
}

static void gui_ui_text_copy_selection_to_clipboard(const char *dst)
{
    if (!dst) return;
    int start = 0, end = 0;
    if (!gui_ui_text_get_selection_range(dst, &start, &end)) return;
    size_t count = (size_t)(end - start);
    char *copy_buf = (char *)malloc(count + 1);
    if (!copy_buf) return;
    memcpy(copy_buf, dst + start, count);
    copy_buf[count] = '\0';
    SetClipboardText(copy_buf);
    free(copy_buf);
}

static Clay_String gui_ui_make_string_slice(const char *src, int start, int end)
{
    if (!src) src = "";
    int len = (int)strlen(src);
    if (start < 0) start = 0;
    if (end < start) end = start;
    if (start > len) start = len;
    if (end > len) end = len;
    return (Clay_String){
        .isStaticallyAllocated = false,
        .length = (int32_t)(end - start),
        .chars = src + start
    };
}

static void gui_ui_sort_unique_ints(int *values, int *count)
{
    if (!values || !count || *count <= 1) return;
    for (int i = 1; i < *count; i++) {
        int key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }
    int out = 1;
    for (int i = 1; i < *count; i++) {
        if (values[i] != values[out - 1]) {
            values[out++] = values[i];
        }
    }
    *count = out;
}

static void gui_ui_render_active_text(ui_text_field_t field,
                                      const char *text,
                                      int font_size,
                                      int font_id,
                                      Color text_color)
{
    if (!text) text = "";
    int len = (int)strlen(text);
    int cursor = s_active_text_cursor;
    if (cursor < 0) cursor = 0;
    if (cursor > len) cursor = len;
    cursor = gui_text_boundary(text, cursor);
    int anchor = s_active_text_selection_anchor;
    if (anchor < 0) anchor = cursor;
    if (anchor > len) anchor = len;
    anchor = gui_text_boundary(text, anchor);
    bool has_selection = (anchor != cursor);
    int sel_start = has_selection ? ((anchor < cursor) ? anchor : cursor) : cursor;
    int sel_end = has_selection ? ((anchor > cursor) ? anchor : cursor) : cursor;

    int points[5];
    int point_count = 0;
    points[point_count++] = 0;
    points[point_count++] = cursor;
    points[point_count++] = len;
    if (has_selection) {
        points[point_count++] = sel_start;
        points[point_count++] = sel_end;
    }
    gui_ui_sort_unique_ints(points, &point_count);
    bool caret_visible = ((int)(GetTime() * 1.8) % 2) == 0;

    CLAY(CLAY_IDI("TextEditRow", (int)field), {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = 0
        }
    }) {
        for (int i = 0; i < point_count; i++) {
            int p = points[i];
            if (p == cursor) {
                Color caret_color = caret_visible ? (Color){240, 240, 240, 255} : (Color){240, 240, 240, 0};
                CLAY(CLAY_IDI("TextEditCaret", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(font_size + 4) }
                    },
                    .backgroundColor = to_clay_color(caret_color),
                    .cornerRadius = CLAY_CORNER_RADIUS(1)
                }) {}
            }

            if (i + 1 < point_count && points[i + 1] > p) {
                int next = points[i + 1];
                bool highlighted = has_selection && p >= sel_start && next <= sel_end;
                Clay_String seg = gui_ui_make_string_slice(text, p, next);
                if (seg.length <= 0) continue;

                if (highlighted) {
                    CLAY(CLAY_IDI("TextEditSel", i), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
                        },
                        .backgroundColor = to_clay_color((Color){76, 128, 255, 255}),
                        .cornerRadius = CLAY_CORNER_RADIUS(2)
                    }) {
                        CLAY_TEXT(seg, CLAY_TEXT_CONFIG({
                            .fontSize = font_size,
                            .fontId = font_id,
                            .textColor = to_clay_color((Color){255, 255, 255, 255})
                        }));
                    }
                } else {
                    CLAY_TEXT(seg, CLAY_TEXT_CONFIG({
                        .fontSize = font_size,
                        .fontId = font_id,
                        .textColor = to_clay_color(text_color)
                    }));
                }
            }
        }
    }
}

static void gui_ui_begin_text_edit(gui_app_t *app, ui_text_field_t field, Clay_ElementId element_id, float left_padding, float right_padding)
{
    char *dst = NULL;
    size_t cap = 0;
    if (!gui_ui_text_field_can_edit(app, field) || !gui_ui_text_field_get_buffer(app, field, &dst, &cap)) {
        gui_ui_clear_text_edit();
        return;
    }

    (void)cap;
    double now = GetTime();
    bool same_click_target = (s_active_text_last_click_element_id.id == element_id.id);
    bool is_double_click = same_click_target &&
                           s_active_text_last_click_time >= 0.0 &&
                           (now - s_active_text_last_click_time) <= 0.35;
    s_active_text_last_click_time = now;
    s_active_text_last_click_element_id = element_id;
    bool same_field = (s_active_text_field == field);
    bool extend_selection = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!same_field) {
        s_active_text_selection_anchor = -1;
    } else if (extend_selection && s_active_text_selection_anchor < 0 && !is_double_click) {
        s_active_text_selection_anchor = s_active_text_cursor;
    } else if (!extend_selection && !is_double_click) {
        s_active_text_selection_anchor = -1;
    }
    s_active_text_field = field;
    s_active_text_element_id = element_id;
    s_active_text_left_padding = left_padding;
    s_active_text_right_padding = right_padding;
    s_active_text_backspace_repeat_at = 0.0;
    if (is_double_click) {
        s_active_text_selection_anchor = 0;
        s_active_text_cursor = (int)strlen(dst);
        s_active_text_drag_selecting = false;
    } else {
        s_active_text_cursor = gui_ui_text_cursor_from_click(app, field, element_id, dst, left_padding, right_padding);
        s_active_text_drag_selecting = true;
    }
    gui_ui_text_clamp_state(dst);
}

static bool gui_ui_text_backspace(char *dst, int *cursor)
{
    if (!dst || !cursor) return false;
    size_t len = strlen(dst);
    if (len == 0 || *cursor <= 0) return false;
    *cursor = gui_text_boundary(dst, *cursor);
    return gui_text_replace(dst, len + 1, cursor, gui_text_previous(dst, *cursor), *cursor, NULL, 0);
}

static bool gui_ui_text_delete(char *dst, int *cursor)
{
    if (!dst || !cursor) return false;
    size_t len = strlen(dst);
    *cursor = gui_text_boundary(dst, *cursor);
    if ((size_t)*cursor >= len) return false;
    return gui_text_replace(dst, len + 1, cursor, *cursor, gui_text_next(dst, *cursor), NULL, 0);
}

static void gui_ui_handle_active_text_edit(gui_app_t *app)
{
    if (s_active_text_field == UI_TEXT_FIELD_NONE) return;

    char *dst = NULL;
    size_t cap = 0;
    if (!gui_ui_text_field_get_buffer(app, s_active_text_field, &dst, &cap) ||
        !gui_ui_text_field_can_edit(app, s_active_text_field)) {
        gui_ui_clear_text_edit();
        return;
    }
    gui_ui_text_clamp_state(dst);

    // RTL-SDR frequency text field mirrors settings.rtlsdr_freq_hz (a uint64).
    // Sync string -> uint64 here so every gui_settings_save() below commits the
    // current value (including on Enter/Esc). Non-digits parse to 0 and are ignored.
    if (s_active_text_field == UI_TEXT_FIELD_RTLSDR_FREQ && s_rtlsdr_freq_str[0]) {
        unsigned long long parsed = strtoull(s_rtlsdr_freq_str, NULL, 10);
        if (parsed > 0) app->settings.rtlsdr_freq_hz = (uint64_t)parsed;
    }
    // Level-autostop level text field: clamp the edited 0.X into 0.1-0.8 on commit.
    // The live string is allowed to be partial while typing; final clamping happens
    // here so the saved value is always in range.
    if (s_active_text_field == UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL &&
        app->settings.level_autostop_level_str[0]) {
        float lvl = (float)atof(app->settings.level_autostop_level_str);
        if (lvl > 0.0f) {
            if (lvl < 0.1f) lvl = 0.1f;
            if (lvl > 0.8f) lvl = 0.8f;
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%.2f", (double)lvl);
            size_t len = strlen(tmp);
            while (len > 0 && tmp[len - 1] == '0') { tmp[--len] = '\0'; }
            if (len > 0 && tmp[len - 1] == '.') { tmp[--len] = '\0'; }
            if (tmp[0] == '\0') snprintf(tmp, sizeof(tmp), "0.1");
            snprintf(app->settings.level_autostop_level_str,
                     sizeof(app->settings.level_autostop_level_str), "%s", tmp);
        }
    }

    bool changed = false;
    bool shift_down = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool primary_mod_down = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
    primary_mod_down = primary_mod_down || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#endif

    if (s_active_text_drag_selecting && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        int drag_cursor = gui_ui_text_cursor_from_click(app, s_active_text_field, s_active_text_element_id, dst, s_active_text_left_padding, s_active_text_right_padding);
        gui_ui_text_set_cursor_position(dst, drag_cursor, true);
        gui_ui_text_clamp_state(dst);
    }
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
        s_active_text_drag_selecting = false;
    }

    if (primary_mod_down && IsKeyPressed(KEY_A)) {
        s_active_text_selection_anchor = 0;
        s_active_text_cursor = (int)strlen(dst);
        gui_ui_text_clamp_state(dst);
    }
    if (primary_mod_down && IsKeyPressed(KEY_C)) {
        gui_ui_text_copy_selection_to_clipboard(dst);
    }
    if (primary_mod_down && IsKeyPressed(KEY_X)) {
        gui_ui_text_copy_selection_to_clipboard(dst);
        if (gui_ui_text_delete_selection(dst)) changed = true;
    }
    if (primary_mod_down && IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        if (clip && clip[0]) {
            if (gui_ui_text_insert_filtered(s_active_text_field, dst, cap, clip)) {
                changed = true;
            }
        }
    }
#if defined(__ANDROID__)
    {
        char android_text[512];
        size_t android_len = android_drain_text_input(android_text, sizeof(android_text));
        bool finish_edit_requested = false;
        if (android_len > 0) {
            for (size_t i = 0; i < android_len; ) {
                int size = 0;
                int ach = GetCodepointNext(android_text + i, &size);
                if (size <= 0 || (size_t)size > android_len - i) break;
                i += (size_t)size;
                if (ach == '\0') break;
                if (ach == '\r' || ach == '\n') {
                    finish_edit_requested = true;
                    continue;
                }
                if (ach == '\b' || ach == 127) {
                    if (!gui_ui_text_delete_selection(dst)) {
                        if (gui_ui_text_backspace(dst, &s_active_text_cursor)) changed = true;
                    } else {
                        changed = true;
                    }
                    continue;
                }
                if (primary_mod_down) continue;
                if (!gui_ui_text_field_char_allowed(s_active_text_field, (int)ach)) continue;
                if (gui_ui_text_insert_char(dst, cap, (int)ach)) changed = true;
            }
        }
        if (finish_edit_requested) {
            gui_ui_text_clamp_state(dst);
            gui_settings_save(&app->settings);
            gui_ui_clear_text_edit();
            return;
        }
    }
#endif
    int ch = GetCharPressed();
    while (ch > 0) {
        if (!primary_mod_down && gui_ui_text_field_char_allowed(s_active_text_field, ch)) {
            if (gui_ui_text_insert_char(dst, cap, ch)) changed = true;
        }
        ch = GetCharPressed();
    }

    if (IsKeyPressed(KEY_LEFT)) {
        gui_ui_text_set_cursor_position(dst, gui_text_previous(dst, s_active_text_cursor), shift_down);
    }
    if (IsKeyPressed(KEY_RIGHT)) {
        gui_ui_text_set_cursor_position(dst, gui_text_next(dst, s_active_text_cursor), shift_down);
    }
    if (IsKeyPressed(KEY_HOME)) {
        gui_ui_text_set_cursor_position(dst, 0, shift_down);
    }
    if (IsKeyPressed(KEY_END)) {
        gui_ui_text_set_cursor_position(dst, (int)strlen(dst), shift_down);
    }

    if (IsKeyPressed(KEY_BACKSPACE)) {
        s_active_text_backspace_repeat_at = GetTime() + 0.25;
        if (!gui_ui_text_delete_selection(dst)) {
            if (gui_ui_text_backspace(dst, &s_active_text_cursor)) changed = true;
        } else {
            changed = true;
        }
    } else if (IsKeyDown(KEY_BACKSPACE)) {
        double now = GetTime();
        if (now >= s_active_text_backspace_repeat_at) {
            s_active_text_backspace_repeat_at = now + 0.05;
            if (s_active_text_selection_anchor >= 0) {
                if (gui_ui_text_delete_selection(dst)) changed = true;
            } else {
                if (gui_ui_text_backspace(dst, &s_active_text_cursor)) changed = true;
            }
        }
    }

    if (IsKeyPressed(KEY_DELETE)) {
        if (!gui_ui_text_delete_selection(dst)) {
            if (gui_ui_text_delete(dst, &s_active_text_cursor)) changed = true;
        } else {
            changed = true;
        }
    }

    gui_ui_text_clamp_state(dst);
    if (changed && s_active_text_field != UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH) {
        gui_settings_save(&app->settings);
    }

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
        if(s_active_text_field==UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH){
            if(!IsKeyPressed(KEY_ESCAPE)){
                s_monitor_folder_page=0;
                gui_monitor_client_browse_folders(s_monitor_folder_edit_path,0);
            }
        }else gui_settings_save(&app->settings);
        gui_ui_clear_text_edit();
    }
}

// Static storage for custom element data (persists during render)
static CustomLayoutElement s_osc_a_element;
static CustomLayoutElement s_osc_b_element;
static CustomLayoutElement s_vu_a_element;
static CustomLayoutElement s_vu_b_element;
static CustomLayoutElement s_settings_icon_element;
static CustomLayoutElement s_record_limit_icon_element;
static CustomLayoutElement s_version_icon_element;
static CustomLayoutElement s_metadata_icon_element;

// Render settings panel (floating modal)
static void render_settings_panel(gui_app_t *app) {
    if(app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT){
        char server_path[FILE_PATH_MAX];
        if(gui_monitor_client_output_path(server_path,sizeof(server_path)) &&
           !gui_ui_is_text_field_active(UI_TEXT_FIELD_OUTPUT_PATH))
            snprintf(app->settings.output_path,sizeof(app->settings.output_path),"%s",server_path);
    }
    if (!app->settings_panel_open) return;
    int settings_max_width = gui_ui_modal_max_extent(gui_ui_get_layout_width(), 1080);
    int settings_max_height = gui_ui_modal_max_extent(gui_ui_get_layout_height(), 780);
    int settings_min_width = gui_ui_clamp_int(settings_max_width, 1, 620);
    int settings_min_height = gui_ui_clamp_int(settings_max_height, 1, 420);
    bool settings_cxadc_has_channel_b = false;
    bool settings_cxadc_mode = gui_ui_selected_device_is_cxadc(app, &settings_cxadc_has_channel_b);
#ifdef ENABLE_DDD
    bool settings_ddd_mode = gui_ui_selected_device_is_ddd(app);
    bool settings_ddd_v1_mode = gui_ui_selected_device_is_ddd_v1(app);
#else
    bool settings_ddd_mode = false;
    bool settings_ddd_v1_mode = false;
#endif
#ifdef ENABLE_FX3
    bool settings_fx3_mode = gui_ui_selected_device_is_fx3(app);
#else
    bool settings_fx3_mode = false;
#endif
    // DdD/FX3 are single-channel and single-card CXADC has no RF-B source.
    // In both cases RF-B controls are disabled (grayed out).
    bool settings_b_disabled = settings_ddd_mode || settings_fx3_mode || (settings_cxadc_mode && !settings_cxadc_has_channel_b);
    // CH-B settings controls (bits/tags/resample) are editable only when
    // channel B is both available and enabled for capture.
    bool settings_b_controls_disabled = settings_b_disabled || !app->settings.capture_b;

    // Backdrop
    CLAY(CLAY_ID("SettingsBackdrop"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP }
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 140}
    }) {}

    // Panel
    CLAY(CLAY_ID("SettingsPanel"), {
        .layout = {
            .sizing = {
                CLAY_SIZING_FIT(.min = settings_min_width, .max = settings_max_width),
                CLAY_SIZING_FIT(.min = settings_min_height, .max = settings_max_height)
            },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 16, 16, 16, 16 },
            .childGap = 12
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER }
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8)
    }) {
        // Header row
        CLAY(CLAY_ID("SettingsHeader"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Settings"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_TITLE, .textColor = to_clay_color(COLOR_TEXT) }));

            CLAY(CLAY_ID("SettingsHeaderSpacer"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
            }) {}

            CLAY(CLAY_ID("SettingsCloseButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("X"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }


        // Auto naming (moved to top segment, above Output folder)
        CLAY_TEXT(CLAY_STRING("Auto naming:"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        CLAY(CLAY_ID("AutoNameToggleRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
            CLAY(CLAY_ID("ToggleAutoNames"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.auto_names_enabled ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(app->settings.auto_names_enabled ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            CLAY_TEXT(CLAY_STRING("Generate filenames automatically"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

            // Add Time/Date toggle on the right side of the same row
            Color ts_bg = app->settings.auto_names_enabled ? (app->settings.append_timestamp_on_capture_start ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON) : ui_disabled_color(COLOR_BUTTON);
            Color ts_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
            CLAY(CLAY_ID("AppendTimestampToggle"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(ts_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(app->settings.append_timestamp_on_capture_start ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(ts_fg) }));
            }
            CLAY_TEXT(CLAY_STRING("Add Time/Date"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(ts_fg) }));

            Color stop_drop_bg = app->settings.stop_on_dropout ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
            Color stop_drop_fg = COLOR_TEXT;
            CLAY(CLAY_ID("StopOnDropoutToggle"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(stop_drop_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(app->settings.stop_on_dropout ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(stop_drop_fg) }));
            }
            CLAY_TEXT(CLAY_STRING("Stop on Dropout"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(stop_drop_fg) }));
        }

        CLAY(CLAY_ID("BaseNameRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
            CLAY_TEXT(CLAY_STRING("Capture Name:"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

            Color base_box_bg = (Color){25,25,30,255};
            Color base_box_fg = COLOR_TEXT;
            if (!app->settings.auto_names_enabled) {
                base_box_bg = ui_disabled_color(base_box_bg);
                base_box_fg = ui_disabled_color(base_box_fg);
            }

            CLAY(CLAY_ID("OutputBaseNameField"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color(base_box_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                const char *base = app->settings.output_base_name[0] ? app->settings.output_base_name : "capture";
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_OUTPUT_BASE_NAME) && app->settings.auto_names_enabled) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_OUTPUT_BASE_NAME, base, FONT_SIZE_NORMAL, 0, base_box_fg);
                } else {
                    CLAY_TEXT(make_string(base), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(base_box_fg) }));
                }
            }

            CLAY(CLAY_ID("OutputBaseNameHint"), { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(CLAY_STRING("(click to edit)"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
        }

/* Local Save controls are M-Client-exclusive. Never silently record
 * on the M-Server when the client-owned receiver cannot be attached. */
if (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT) {
    CLAY(CLAY_ID("MonitorLocalSaveRow"), {
        .layout = { .sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(32)},
                    .layoutDirection=CLAY_LEFT_TO_RIGHT,
                    .childAlignment={.y=CLAY_ALIGN_Y_CENTER}, .childGap=10 }
    }) {
        CLAY_TEXT(CLAY_STRING("Local Save:"),
            CLAY_TEXT_CONFIG({ .fontSize=FONT_SIZE_NORMAL, .textColor=to_clay_color(COLOR_TEXT) }));
        CLAY(CLAY_ID("MonitorLocalSaveToggle"), {
            .layout={ .sizing={CLAY_SIZING_FIXED(104),CLAY_SIZING_FIXED(28)},
                      .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER} },
            .backgroundColor=to_clay_color(app->settings.monitor_local_save_enabled ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
            .cornerRadius=CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(app->settings.monitor_local_save_enabled ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,.textColor=to_clay_color(COLOR_TEXT_DIM)}));
        }
        CLAY_TEXT(CLAY_STRING("client-owned recording; .partial until commit"),
            CLAY_TEXT_CONFIG({ .fontSize=FONT_SIZE_STATS, .textColor=to_clay_color(COLOR_TEXT_DIM) }));
    }
    // Display only the currently selected recording destination. The
    // alternate path remains saved, but is hidden until its mode is selected.
    if (app->settings.monitor_local_save_enabled) {
    // Local Save is M-Client-only; its destination must be editable on
    // the CLIENT computer. Never reuse OutputPathBox / settings.output_path:
    // those describe the separate capture-output configuration.
    CLAY_TEXT(CLAY_STRING("Local folder (this computer; click to edit):"),
        CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT_DIM)}));
    CLAY(CLAY_ID("MonitorLocalSavePathRow"), {
        .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(32)},
                 .layoutDirection=CLAY_LEFT_TO_RIGHT,
                 .childAlignment={.y=CLAY_ALIGN_Y_CENTER},.childGap=8}
    }) {
        CLAY(CLAY_ID("MonitorLocalSavePathBox"), {
            .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(32)},
                     .childAlignment={.x=CLAY_ALIGN_X_LEFT,.y=CLAY_ALIGN_Y_CENTER},
                     .padding={10,10,0,0}},
            .backgroundColor=to_clay_color((Color){25,25,30,255}),
            .cornerRadius=CLAY_CORNER_RADIUS(4),
            .clip={.horizontal=true}
        }) {
            const char *local_path=app->settings.monitor_local_save_path;
            if (gui_ui_is_text_field_active(UI_TEXT_FIELD_MONITOR_LOCAL_SAVE_PATH)) {
                gui_ui_render_active_text(UI_TEXT_FIELD_MONITOR_LOCAL_SAVE_PATH,
                    local_path, FONT_SIZE_NORMAL, 0, COLOR_TEXT);
            } else {
                CLAY_TEXT(make_string(local_path[0] ? local_path : "Click here or Choose..."),
                    CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,
                                      .textColor=to_clay_color(local_path[0] ? COLOR_TEXT : COLOR_TEXT_DIM)}));
            }
        }
        CLAY(CLAY_ID("MonitorLocalSaveChooseFolder"), {
            .layout={.sizing={CLAY_SIZING_FIXED(96),CLAY_SIZING_FIXED(30)},
                     .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
            .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(CLAY_STRING("Choose..."),
                CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,.textColor=to_clay_color(COLOR_TEXT)}));
        }
    }
    } // Local Save ON: client directory row only
}

// Show the server/normal Output folder only when Local Save is not selected.
// Non-M-Client modes retain their original folder controls unconditionally.
if (app->settings.net_mode != GUI_NET_MODE_MONITOR_CLIENT ||
    !app->settings.monitor_local_save_enabled) {
// Output path display + choose button
CLAY(CLAY_ID("SettingsOutputPath"), {
    .layout = {
        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
        .layoutDirection = CLAY_TOP_TO_BOTTOM,
        .childGap = 6
    }
}) {
    CLAY_TEXT(app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT
        ? CLAY_STRING("Server Output folder:")
        : CLAY_STRING("Output folder:"),
        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

    CLAY(CLAY_ID("OutputPathRow"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = 8
        }
    }) {
        // Editable path box (click to edit)
        CLAY(CLAY_ID("OutputPathBox"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                .padding = { 10, 10, 0, 0 }
            },
            .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            if (gui_ui_is_text_field_active(UI_TEXT_FIELD_OUTPUT_PATH)) {
                gui_ui_render_active_text(UI_TEXT_FIELD_OUTPUT_PATH, app->settings.output_path, FONT_SIZE_NORMAL, 0, COLOR_TEXT);
            } else {
                CLAY_TEXT(make_string(app->settings.output_path),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Choose output folder button (so the handler has a real element)
        CLAY(CLAY_ID("ChooseOutputFolderButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(96), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(COLOR_BUTTON),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(CLAY_STRING("Choose..."),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }
    }
}
} // Local Save OFF (or non-M-Client): server/normal output directory


        // Scrollable settings body
        CLAY(CLAY_ID("SettingsScroll"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .childGap = 10
            },
            .clip = {
                .vertical = true,
                .horizontal = true,
                .childOffset = Clay_GetScrollOffset()
            }
        }) {
            // Two-column layout to reduce vertical overflow
            CLAY(CLAY_ID("SettingsColumns"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 18
                }
            }) {
                // Left column
                CLAY(CLAY_ID("SettingsColLeft"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childGap = 8
                    }
                }) {
                    // helper-like rows
#ifdef ENABLE_RTLSDR
                    if (gui_ui_selected_device_is_sdr(app)) {
                        // Display sync: when not editing, format uint64 -> string.
                        if (s_active_text_field != UI_TEXT_FIELD_RTLSDR_FREQ) {
                            snprintf(s_rtlsdr_freq_str, sizeof(s_rtlsdr_freq_str), "%llu",
                                     (unsigned long long)app->settings.rtlsdr_freq_hz);
                        }

                        CLAY_TEXT(CLAY_STRING("SDR:"),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                        // Frequency (Hz) row
                        CLAY(CLAY_ID("RtlsdrFreqRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            CLAY_TEXT(CLAY_STRING("Frequency (Hz):"),
                                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                            CLAY(CLAY_ID("RtlsdrFreqField"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_RTLSDR_FREQ)) {
                                    gui_ui_render_active_text(UI_TEXT_FIELD_RTLSDR_FREQ, s_rtlsdr_freq_str, FONT_SIZE_STATS, 1, COLOR_TEXT);
                                } else {
                                    CLAY_TEXT(make_string(s_rtlsdr_freq_str), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
                                }
                            }
                            CLAY(CLAY_ID("RtlsdrFreqHint"), { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER } } }) {
                                CLAY_TEXT(CLAY_STRING("(click to edit)"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                            }
                        }

                        // Sample rate row (cycle box)
                        CLAY(CLAY_ID("RtlsdrSampleRateRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            CLAY_TEXT(CLAY_STRING("Sample rate:"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            char sr_buf[24];
                            snprintf(sr_buf, sizeof(sr_buf), "%.2f MSPS", (double)app->settings.rtlsdr_sample_rate_hz / 1.0e6);
                            CLAY(CLAY_ID("RtlsdrSampleRateBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(make_string(sr_buf), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                            }
                        }

                        // Gain mode toggle row
                        CLAY(CLAY_ID("RtlsdrGainModeRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            Color gm_bg = app->settings.rtlsdr_gain_mode ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                            CLAY(CLAY_ID("RtlsdrGainModeToggle"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(gm_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(app->settings.rtlsdr_gain_mode ? CLAY_STRING("Manual") : CLAY_STRING("Auto"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            }
                            CLAY_TEXT(CLAY_STRING("Gain mode"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }

                        // Gain stepper row
                        CLAY(CLAY_ID("RtlsdrGainRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            CLAY(CLAY_ID("RtlsdrGainMinus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                            char gain_buf[24];
                            snprintf(gain_buf, sizeof(gain_buf), "Gain: %.1f dB", (double)app->settings.rtlsdr_gain_tenths_db / 10.0);
                            CLAY(CLAY_ID("RtlsdrGainValue"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(make_string(gain_buf), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                            CLAY(CLAY_ID("RtlsdrGainPlus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                        }

                        // AGC toggle row
                        CLAY(CLAY_ID("RtlsdrAgcRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            Color agc_bg = app->settings.rtlsdr_agc ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                            CLAY(CLAY_ID("RtlsdrAgcToggle"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(agc_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(app->settings.rtlsdr_agc ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            }
                            CLAY_TEXT(CLAY_STRING("AGC"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }

                        // Offset tuning toggle row
                        CLAY(CLAY_ID("RtlsdrOffsetRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            Color off_bg = app->settings.rtlsdr_offset_corr ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                            CLAY(CLAY_ID("RtlsdrOffsetToggle"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(off_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(app->settings.rtlsdr_offset_corr ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            }
                            CLAY_TEXT(CLAY_STRING("Offset tuning"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }
#endif // ENABLE_RTLSDR
                    CLAY_TEXT(CLAY_STRING("Capture:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    CLAY(CLAY_ID("ToggleRowCaptureA"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleCaptureA"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.capture_a ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.capture_a ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("RF A"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

                        // RF bit depth selector (moved up into Capture segment)
                        CLAY(CLAY_ID("CaptureRowSpacerA"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } } }) { }
                        snprintf(settings_rf_bits_a_display, sizeof(settings_rf_bits_a_display), "%s-bit", rf_bits_label(app->settings.rf_bits_a));
                        Color rf_bits_a_bg = COLOR_BUTTON;
                        Color rf_bits_a_fg = COLOR_TEXT;
                        CLAY(CLAY_ID("RfBitsABox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(rf_bits_a_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(settings_rf_bits_a_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(rf_bits_a_fg) }));
                        }
                        Color rf_tag_a_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color rf_tag_a_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("RfTagAField"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(rf_tag_a_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *rf_tag_a = app->settings.rf_channel_tags[0][0] ? app->settings.rf_channel_tags[0] : "(tag)";
                            if (gui_ui_is_text_field_active(UI_TEXT_FIELD_RF_TAG_A) && app->settings.auto_names_enabled) {
                                gui_ui_render_active_text(UI_TEXT_FIELD_RF_TAG_A, rf_tag_a, FONT_SIZE_STATS, 1, rf_tag_a_fg);
                            } else {
                                CLAY_TEXT(make_string(rf_tag_a), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(rf_tag_a_fg) }));
                            }
                        }
                    }

                    CLAY(CLAY_ID("ToggleRowCaptureB"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        Color cap_b_toggle_bg = settings_b_disabled ? ui_disabled_color(app->settings.capture_b ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON) : (app->settings.capture_b ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
                        Color cap_b_toggle_fg = settings_b_disabled ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
                        CLAY(CLAY_ID("ToggleCaptureB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(cap_b_toggle_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.capture_b ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(cap_b_toggle_fg) }));
                        }
                        Color rf_b_label_fg = settings_b_disabled ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
                        CLAY_TEXT(CLAY_STRING("RF B"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(rf_b_label_fg) }));

                        CLAY(CLAY_ID("CaptureRowSpacerB"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } } }) { }
                        snprintf(settings_rf_bits_b_display, sizeof(settings_rf_bits_b_display), "%s-bit", rf_bits_label(app->settings.rf_bits_b));
                        Color rf_bits_b_bg = settings_b_disabled ? ui_disabled_color(COLOR_BUTTON) : COLOR_BUTTON;
                        Color rf_bits_b_fg = settings_b_disabled ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
                        CLAY(CLAY_ID("RfBitsBBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(rf_bits_b_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(settings_rf_bits_b_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(rf_bits_b_fg) }));
                        }
                        Color rf_tag_b_bg = (app->settings.auto_names_enabled && !settings_b_controls_disabled) ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color rf_tag_b_fg = (app->settings.auto_names_enabled && !settings_b_controls_disabled) ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("RfTagBField"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(rf_tag_b_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *rf_tag_b = app->settings.rf_channel_tags[1][0] ? app->settings.rf_channel_tags[1] : "(tag)";
                            if (gui_ui_is_text_field_active(UI_TEXT_FIELD_RF_TAG_B) && app->settings.auto_names_enabled && !settings_b_controls_disabled) {
                                gui_ui_render_active_text(UI_TEXT_FIELD_RF_TAG_B, rf_tag_b, FONT_SIZE_STATS, 1, rf_tag_b_fg);
                            } else {
                                CLAY_TEXT(make_string(rf_tag_b), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(rf_tag_b_fg) }));
                            }
                        }
                    }


                    CLAY(CLAY_ID("ToggleRowFlac"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleUseFlac"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.use_flac ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.use_flac ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("RF FLAC compression"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                    }

                    // FLAC verify toggle (moved directly under enable)
                    CLAY(CLAY_ID("ToggleRowFlacVerify"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleFlacVerify"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.flac_verification ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.flac_verification ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Verify FLAC output"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                    }

                    CLAY(CLAY_ID("ToggleRowOverwrite"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleOverwrite"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.overwrite_files ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.overwrite_files ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Overwrite output files"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                    // Compression section
                    CLAY_TEXT(CLAY_STRING("Compression (RF):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    // FLAC level stepper
                    CLAY(CLAY_ID("FlacLevelRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("FlacLevelMinus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                        snprintf(settings_flac_level_display, sizeof(settings_flac_level_display), "FLAC level: %d", app->settings.flac_level);
                        CLAY(CLAY_ID("FlacLevelValue"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(make_string(settings_flac_level_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                        CLAY(CLAY_ID("FlacLevelPlus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                    }

                    // FLAC threads stepper (0=auto)
                    CLAY(CLAY_ID("FlacThreadsRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("FlacThreadsMinus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                        snprintf(settings_flac_threads_display, sizeof(settings_flac_threads_display), "FLAC threads: %d", app->settings.flac_threads);
                        CLAY(CLAY_ID("FlacThreadsValue"), { .layout = { .sizing = { CLAY_SIZING_FIXED(170), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(make_string(settings_flac_threads_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                        CLAY(CLAY_ID("FlacThreadsPlus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                    }

                    if (app->settings.show_core_pinning_in_settings) {
#if defined(__linux__)
                        bool flac_affinity_supported = true;
#else
                        bool flac_affinity_supported = false;
#endif
                        bool flac_affinity_editable = app->settings.use_flac && app->settings.flac_affinity_enabled && flac_affinity_supported;
                        Color affinity_toggle_bg = (app->settings.use_flac && flac_affinity_supported)
                            ? (app->settings.flac_affinity_enabled ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON)
                            : ui_disabled_color(COLOR_BUTTON);
                        Color affinity_toggle_fg = (app->settings.use_flac && flac_affinity_supported)
                            ? COLOR_TEXT
                            : ui_disabled_color(COLOR_TEXT);
                        Color affinity_list_bg = flac_affinity_editable
                            ? (Color){25,25,30,255}
                            : ui_disabled_color((Color){25,25,30,255});
                        Color affinity_list_fg = flac_affinity_editable ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);

                        CLAY(CLAY_ID("FlacAffinityToggleRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            CLAY(CLAY_ID("ToggleFlacAffinity"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(affinity_toggle_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT((app->settings.flac_affinity_enabled && flac_affinity_supported) ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(affinity_toggle_fg) }));
                            }
                            CLAY_TEXT(CLAY_STRING("FLAC core pinning (Linux)"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(affinity_toggle_fg) }));
                        }

                        CLAY(CLAY_ID("FlacAffinityListRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            CLAY_TEXT(CLAY_STRING("CPU list:"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(affinity_list_fg) }));
                            CLAY(CLAY_ID("FlacAffinityListField"), { .layout = { .sizing = { CLAY_SIZING_FIXED(170), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color(affinity_list_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                const char *cpu_list = app->settings.flac_affinity_cpu_list[0] ? app->settings.flac_affinity_cpu_list : "10-17";
                                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_FLAC_AFFINITY) && flac_affinity_editable) {
                                    gui_ui_render_active_text(UI_TEXT_FIELD_FLAC_AFFINITY, cpu_list, FONT_SIZE_STATS, 0, affinity_list_fg);
                                } else {
                                    CLAY_TEXT(make_string(cpu_list), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(affinity_list_fg) }));
                                }
                            }
                            CLAY_TEXT(flac_affinity_supported ? CLAY_STRING("e.g. 10-17,20") : CLAY_STRING("Linux only"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                        }
                    }

                    if (settings_ddd_v1_mode) {
                        CLAY_TEXT(CLAY_STRING("RF sample rate:"),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
#ifdef ENABLE_DDD
                        ddd_v1_rate_plan_t rate_plan;
                        if (!gui_ui_ddd_v1_rate_plan(app, &rate_plan)) {
                            (void)ddd_v1_plan_output_rate_khz(
                                ddd_sample_rate_khz(DDD_DECIMATION_FULL_RATE),
                                &rate_plan);
                        }
                        format_msps_label(settings_resample_a_display,
                                          sizeof(settings_resample_a_display),
                                          (float)rate_plan.output_rate_khz);
                        Color ddd_mode_bg = app->is_capturing
                            ? ui_disabled_color(COLOR_BUTTON_ACTIVE)
                            : COLOR_BUTTON_ACTIVE;
                        Color ddd_rate_bg = app->is_capturing
                            ? ui_disabled_color(COLOR_BUTTON) : COLOR_BUTTON;
                        Color ddd_rate_fg = app->is_capturing
                            ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
                        Color ddd_path_fg = app->is_capturing
                            ? ui_disabled_color(COLOR_TEXT_DIM) : COLOR_TEXT_DIM;
                        CLAY(CLAY_ID("ToggleRowResampleA"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            CLAY(CLAY_ID("DddRateModeBadge"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(ddd_mode_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(rate_plan.software_resample ? CLAY_STRING("SW") : CLAY_STRING("HW"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(ddd_rate_fg) }));
                            }
                            CLAY_TEXT(CLAY_STRING("RF ChA"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(ddd_rate_fg) }));
                            CLAY(CLAY_ID("ResampleRateABox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(ddd_rate_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(make_string(settings_resample_a_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(ddd_rate_fg) }));
                            }
                            CLAY_TEXT(rate_plan.software_resample ? CLAY_STRING("from 20 MSPS HW") : CLAY_STRING("hardware"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(ddd_path_fg) }));
                        }
#endif
                    } else {
                        CLAY_TEXT(CLAY_STRING("Resample (RF):"),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                        CLAY(CLAY_ID("ToggleRowResampleA"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            Color resample_a_toggle_bg = app->settings.enable_resample_a ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                            Color resample_a_toggle_fg = COLOR_TEXT;
                            CLAY(CLAY_ID("ToggleResampleA"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(resample_a_toggle_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(app->settings.enable_resample_a ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(resample_a_toggle_fg) }));
                            }
                            CLAY_TEXT(CLAY_STRING("Resample A"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(resample_a_toggle_fg) }));

                            // Rate selector (kHz stored; display MSPS, or "HW <rate>" in HW mode)
                            bool cxadc_hw_a = settings_cxadc_mode && !app->settings.enable_resample_a;
                            if (cxadc_hw_a) {
                                format_cxadc_hw_label(settings_resample_a_display,
                                    sizeof(settings_resample_a_display), app->settings.resample_rate_a);
                            } else {
                                format_msps_label(settings_resample_a_display, sizeof(settings_resample_a_display), app->settings.resample_rate_a);
                            }
                            Color rate_bg = (settings_cxadc_mode || app->settings.enable_resample_a) ? COLOR_BUTTON : ui_disabled_color(COLOR_BUTTON);
                            Color rate_fg = (settings_cxadc_mode || app->settings.enable_resample_a) ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                            CLAY(CLAY_ID("ResampleRateABox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(rate_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(make_string(settings_resample_a_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(rate_fg) }));
                            }
                        }
                    }

                    CLAY(CLAY_ID("ToggleRowResampleB"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        Color resample_b_toggle_bg = settings_b_controls_disabled
                            ? ui_disabled_color(COLOR_BUTTON)
                            : (app->settings.enable_resample_b ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
                        Color resample_b_toggle_fg = settings_b_controls_disabled ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
                        CLAY(CLAY_ID("ToggleResampleB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(resample_b_toggle_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_resample_b ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(resample_b_toggle_fg) }));
                        }
                        CLAY_TEXT(settings_ddd_v1_mode ? CLAY_STRING("RF ChB") : CLAY_STRING("Resample B"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(resample_b_toggle_fg) }));

                        bool cxadc_hw_b = (settings_cxadc_mode && !settings_b_controls_disabled) && !app->settings.enable_resample_b;
                        if (cxadc_hw_b) {
                            format_cxadc_hw_label(settings_resample_b_display,
                                sizeof(settings_resample_b_display), app->settings.resample_rate_b);
                        } else {
                            format_msps_label(settings_resample_b_display, sizeof(settings_resample_b_display), app->settings.resample_rate_b);
                        }
                        Color rate_bg = (settings_b_controls_disabled || !(settings_cxadc_mode || app->settings.enable_resample_b)) ? ui_disabled_color(COLOR_BUTTON) : COLOR_BUTTON;
                        Color rate_fg = (settings_b_controls_disabled || !(settings_cxadc_mode || app->settings.enable_resample_b)) ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
                        CLAY(CLAY_ID("ResampleRateBBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(rate_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(settings_resample_b_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(rate_fg) }));
                        }
                    }

                }

                // Right column
                CLAY(CLAY_ID("SettingsColRight"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childGap = 8
                    }
                }) {
                    // Audio outputs
                    CLAY_TEXT(CLAY_STRING("Audio output (WAV):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    CLAY(CLAY_ID("ToggleRowAudio4ch"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleAudio4ch"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_audio_4ch ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_audio_4ch ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Quad Ch1-4"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        Color audio_tag_4ch_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color audio_tag_4ch_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("AudioTag4chField"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(audio_tag_4ch_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *tag4 = app->settings.audio_output_tags[0][0] ? app->settings.audio_output_tags[0] : "(tag)";
                            if (gui_ui_is_text_field_active(UI_TEXT_FIELD_AUDIO_TAG_4CH) && app->settings.auto_names_enabled) {
                                gui_ui_render_active_text(UI_TEXT_FIELD_AUDIO_TAG_4CH, tag4, FONT_SIZE_STATS, 1, audio_tag_4ch_fg);
                            } else {
                                CLAY_TEXT(make_string(tag4), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(audio_tag_4ch_fg) }));
                            }
                        }
                    }

                    CLAY(CLAY_ID("ToggleRowAudio2ch12"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleAudio2ch12"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_audio_2ch_12 ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_audio_2ch_12 ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Stereo Ch1/Ch2"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        Color audio_tag_12_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color audio_tag_12_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("AudioTag2ch12Field"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(audio_tag_12_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *tag12 = app->settings.audio_output_tags[1][0] ? app->settings.audio_output_tags[1] : "(tag)";
                            if (gui_ui_is_text_field_active(UI_TEXT_FIELD_AUDIO_TAG_12) && app->settings.auto_names_enabled) {
                                gui_ui_render_active_text(UI_TEXT_FIELD_AUDIO_TAG_12, tag12, FONT_SIZE_STATS, 1, audio_tag_12_fg);
                            } else {
                                CLAY_TEXT(make_string(tag12), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(audio_tag_12_fg) }));
                            }
                        }
                    }

                    CLAY(CLAY_ID("ToggleRowAudio2ch34"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleAudio2ch34"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_audio_2ch_34 ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_audio_2ch_34 ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Stereo Ch3/Ch4"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        Color audio_tag_34_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color audio_tag_34_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("AudioTag2ch34Field"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(audio_tag_34_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *tag34 = app->settings.audio_output_tags[2][0] ? app->settings.audio_output_tags[2] : "(tag)";
                            if (gui_ui_is_text_field_active(UI_TEXT_FIELD_AUDIO_TAG_34) && app->settings.auto_names_enabled) {
                                gui_ui_render_active_text(UI_TEXT_FIELD_AUDIO_TAG_34, tag34, FONT_SIZE_STATS, 1, audio_tag_34_fg);
                            } else {
                                CLAY_TEXT(make_string(tag34), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(audio_tag_34_fg) }));
                            }
                        }
                    }

                    // Audio 1ch (WAV) - mono CH1/CH2/CH3/CH4 list (do not alter)
                    CLAY_TEXT(CLAY_STRING("Audio 1ch (WAV):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    for (int i = 0; i < 4; i++) {
                        Clay_ElementId row_id = CLAY_IDI("ToggleRowAudio1ch", i);
                        Clay_ElementId toggle_id = CLAY_IDI("ToggleAudio1ch", i);
                        Clay_ElementId label_id = CLAY_IDI("Audio1chLabelField", i);

                        CLAY(row_id, { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            CLAY(toggle_id, { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_audio_1ch[i] ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(app->settings.enable_audio_1ch[i] ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            }

                            if (i == 0) CLAY_TEXT(CLAY_STRING("CH1"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            else if (i == 1) CLAY_TEXT(CLAY_STRING("CH2"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            else if (i == 2) CLAY_TEXT(CLAY_STRING("CH3"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            else CLAY_TEXT(CLAY_STRING("CH4"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

                            // Per-channel audio tag (used in auto naming)
                            Color tag_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                            Color tag_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                            CLAY(label_id, { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(tag_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                const char *tag = app->settings.audio_1ch_labels[i][0] ? app->settings.audio_1ch_labels[i] : "(tag)";
                                if (gui_ui_is_text_field_active((ui_text_field_t)(UI_TEXT_FIELD_AUDIO_LABEL_1 + i)) && app->settings.auto_names_enabled) {
                                    gui_ui_render_active_text((ui_text_field_t)(UI_TEXT_FIELD_AUDIO_LABEL_1 + i), tag, FONT_SIZE_STATS, 1, tag_fg);
                                } else {
                                    CLAY_TEXT(make_string(tag), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(tag_fg) }));
                                }
                            }

                            // Filename preview intentionally hidden to keep settings rows compact.
                        }
                    }

                    // Playback files section
                    CLAY_TEXT(CLAY_STRING("Playback files (FLAC / drop onto a channel panel):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    // Channel A playback file
                    CLAY(CLAY_ID("PlaybackFileARow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 6 } }) {
                        CLAY(CLAY_ID("PlaybackFileBrowseA"), { .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("Ch A..."), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        // Show filename or "(none)"
                        const char *file_a = app->settings.playback_file_a[0] ? app->settings.playback_file_a : "(none)";
                        gui_ui_copy_path_tail(playback_file_a_display, sizeof(playback_file_a_display), file_a);
                        CLAY(CLAY_ID("PlaybackFileAPath"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(playback_file_a_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(app->settings.playback_file_a[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                        }
                        CLAY(CLAY_ID("PlaybackFileClearA"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("X"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }

                    // Channel B playback file
                    CLAY(CLAY_ID("PlaybackFileBRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 6 } }) {
                        CLAY(CLAY_ID("PlaybackFileBrowseB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("Ch B..."), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        const char *file_b = app->settings.playback_file_b[0] ? app->settings.playback_file_b : "(none)";
                        gui_ui_copy_path_tail(playback_file_b_display, sizeof(playback_file_b_display), file_b);
                        CLAY(CLAY_ID("PlaybackFileBPath"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(playback_file_b_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(app->settings.playback_file_b[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                        }
                        CLAY(CLAY_ID("PlaybackFileClearB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("X"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }

                    // Playback audio (drag-and-drop monitor source; loads with
                    // the RF files in one drop - a full capture set)
                    CLAY(CLAY_ID("PlaybackFileAudioRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 6 } }) {
                        CLAY_TEXT(CLAY_STRING("Audio:"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                        const char *file_audio = app->settings.playback_audio_file[0] ? app->settings.playback_audio_file : "(drop an audio file)";
                        gui_ui_copy_path_tail(playback_file_audio_display, sizeof(playback_file_audio_display), file_audio);
                        CLAY(CLAY_ID("PlaybackFileAudioPath"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(playback_file_audio_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(app->settings.playback_audio_file[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                        }
                        CLAY(CLAY_ID("PlaybackFileClearAudio"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("X"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }
                }
            }
        }
    }
}

static void render_record_limit_window(gui_app_t *app)
{
    if (!s_record_limit_window_open) return;

    // Fixed narrow width: window stays compact and can't scroll sideways.
    int record_limit_width = gui_ui_modal_max_extent(gui_ui_get_layout_width(), 340);
    int record_limit_max_height = gui_ui_modal_max_extent(gui_ui_get_layout_height(), 420);
    int record_limit_min_height = gui_ui_clamp_int(record_limit_max_height, 1, 200);

    uint32_t parsed_seconds = 0;
    bool timecode_valid = parse_record_limit_timecode(s_record_limit_timecode, &parsed_seconds);
    bool timecode_usable = timecode_valid && parsed_seconds > 0;
    const char *display_timecode = s_record_limit_timecode_edit ? s_record_limit_timecode_edit_buffer : s_record_limit_timecode;
    bool display_timecode_valid = parse_record_limit_timecode(display_timecode, NULL);
    double now = GetTime();
    record_limit_state_display[0] = '\0';

    if (app->is_recording && s_record_limit_deadline_active) {
        double remaining_s = s_record_limit_deadline_s - now;
        if (remaining_s < 0.0) remaining_s = 0.0;
        uint32_t remaining_ceil = (uint32_t)ceil(remaining_s);
        char rem_tc[16];
        format_record_limit_timecode(rem_tc, sizeof(rem_tc), remaining_ceil);
        snprintf(record_limit_state_display, sizeof(record_limit_state_display), "Remaining: %s", rem_tc);
    } else if (s_record_limit_armed) {
        if (timecode_usable) {
            snprintf(record_limit_state_display, sizeof(record_limit_state_display), "Armed at %s", s_record_limit_timecode);
        } else {
            snprintf(record_limit_state_display, sizeof(record_limit_state_display), "Armed: invalid timecode");
        }
    } else {
        snprintf(record_limit_state_display, sizeof(record_limit_state_display), "Disarmed");
    }

    CLAY(CLAY_ID("RecordLimitBackdrop"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP }
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 140}
    }) {}

    CLAY(CLAY_ID("RecordLimitWindow"), {
        .layout = {
            .sizing = {
                CLAY_SIZING_FIXED(record_limit_width),
                CLAY_SIZING_FIT(.min = record_limit_min_height, .max = record_limit_max_height)
            },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 16, 16, 16, 16 },
            .childGap = 12
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER }
        },
        .clip = {
            .horizontal = false,
            .vertical = true,
            .childOffset = Clay_GetScrollOffset()
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8)
    }) {
        CLAY(CLAY_ID("RecordLimitHeader"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Record time limit"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_TITLE, .textColor = to_clay_color(COLOR_TEXT) }));

            CLAY(CLAY_ID("RecordLimitHeaderSpacer"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
            }) {}

            CLAY(CLAY_ID("RecordLimitCloseButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("X"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        CLAY(CLAY_ID("RecordLimitArmRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 8
            }
        }) {
            CLAY(CLAY_ID("RecordLimitArmToggle"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(96), CLAY_SIZING_FIXED(34) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(s_record_limit_armed ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(s_record_limit_armed ? CLAY_STRING("Disarm") : CLAY_STRING("Arm"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }

            CLAY_TEXT(make_string(record_limit_state_display),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        CLAY_TEXT(CLAY_STRING("Timecode (HH:MM:SS):"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        Color timecode_bg = (Color){25, 25, 30, 255};
        Color timecode_fg = display_timecode_valid ? COLOR_TEXT : COLOR_CLIP_RED;
        int record_limit_timecode_font_size = record_limit_timecode_font_size_px();
        Font record_limit_font = record_limit_timecode_font(app);
        Vector2 record_limit_timecode_text_size = gui_font_measure_text(record_limit_font,
                                                                "00:00:00",
                                                                (float)record_limit_timecode_font_size,
                                                                0.0f);
        if (record_limit_timecode_text_size.x <= 0.0f || record_limit_timecode_text_size.y <= 0.0f) {
            record_limit_timecode_text_size = (Vector2){
                (float)record_limit_timecode_font_size * 4.8f,
                (float)record_limit_timecode_font_size
            };
        }
        int record_limit_timecode_width = (int)ceilf(record_limit_timecode_text_size.x) + (RECORD_LIMIT_TIMECODE_BORDER_X * 2);
        int record_limit_timecode_height = (int)ceilf(record_limit_timecode_text_size.y) + (RECORD_LIMIT_TIMECODE_BORDER_Y * 2);
        int record_limit_indicator_height = (int)roundf(2.0f * RECORD_LIMIT_TIMECODE_SCALE);
        if (record_limit_indicator_height < 1) record_limit_indicator_height = 1;
        float record_limit_indicator_char_widths[8] = { 0 };
        float record_limit_indicator_text_width = 0.0f;
        record_limit_measure_char_widths(app,
                                         record_limit_timecode_buffer_for_layout(),
                                         record_limit_timecode_font_size,
                                         record_limit_indicator_char_widths,
                                         &record_limit_indicator_text_width);
        float record_limit_indicator_content_width = (float)record_limit_timecode_width - (float)(RECORD_LIMIT_TIMECODE_BORDER_X * 2);
        if (record_limit_indicator_content_width < 0.0f) record_limit_indicator_content_width = 0.0f;
        float record_limit_indicator_left_pad = (float)RECORD_LIMIT_TIMECODE_BORDER_X +
                                                fmaxf(0.0f, (record_limit_indicator_content_width - record_limit_indicator_text_width) * 0.5f);
        float record_limit_indicator_right_pad = (float)record_limit_timecode_width -
                                                 record_limit_indicator_left_pad -
                                                 record_limit_indicator_text_width;
        if (record_limit_indicator_right_pad < 0.0f) record_limit_indicator_right_pad = 0.0f;
        bool record_limit_digit_indicator_visible = ((int)(GetTime() * 1.8f) % 2) == 0;
        int active_digit_char = record_limit_nearest_digit_cursor_char(s_record_limit_cursor_char);
        CLAY(CLAY_ID("RecordLimitTimecodeCenterRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
            }
        }) {
            CLAY(CLAY_ID("RecordLimitTimecodeCenterSpacerLeft"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } }
            }) {}

            CLAY(CLAY_ID("RecordLimitTimecodeBlock"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(record_limit_timecode_width), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .childGap = 6
                }
            }) {
                CLAY(CLAY_ID("RecordLimitTimecodeField"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(record_limit_timecode_width), CLAY_SIZING_FIXED(record_limit_timecode_height) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER },
                        .padding = { RECORD_LIMIT_TIMECODE_BORDER_X, RECORD_LIMIT_TIMECODE_BORDER_X, RECORD_LIMIT_TIMECODE_BORDER_Y, RECORD_LIMIT_TIMECODE_BORDER_Y }
                    },
                    .backgroundColor = to_clay_color(timecode_bg),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    if (s_record_limit_timecode_edit) {
                        snprintf(record_limit_timecode_display, sizeof(record_limit_timecode_display), "%s", s_record_limit_timecode_edit_buffer);
                        CLAY_TEXT(make_string(record_limit_timecode_display),
                            CLAY_TEXT_CONFIG({ .fontSize = record_limit_timecode_font_size, .fontId = 1, .textColor = to_clay_color(timecode_fg) }));
                    } else {
                        CLAY_TEXT(make_string(display_timecode),
                            CLAY_TEXT_CONFIG({ .fontSize = record_limit_timecode_font_size, .fontId = 1, .textColor = to_clay_color(timecode_fg) }));
                    }
                }

                if (s_record_limit_timecode_edit) {
                    CLAY(CLAY_ID("RecordLimitDigitIndicatorRow"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIXED(record_limit_timecode_width), CLAY_SIZING_FIXED(record_limit_indicator_height) },
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childGap = 0
                        }
                    }) {
                        if (record_limit_indicator_left_pad > 0.0f) {
                            CLAY(CLAY_ID("RecordLimitDigitIndicatorLeftPad"), {
                                .layout = {
                                    .sizing = { CLAY_SIZING_FIXED(record_limit_indicator_left_pad), CLAY_SIZING_FIXED(record_limit_indicator_height) }
                                }
                            }) {}
                        }
                        for (int i = 0; i < 8; i++) {
                            bool active_digit = record_limit_is_digit_char_index(i) && (i == active_digit_char);
                            Color indicator_color = (active_digit && record_limit_digit_indicator_visible)
                                ? COLOR_SYNC_GREEN
                                : (Color){ 0, 0, 0, 0 };
                            CLAY(CLAY_IDI("RecordLimitDigitIndicator", i), {
                                .layout = {
                                    .sizing = { CLAY_SIZING_FIXED(record_limit_indicator_char_widths[i]), CLAY_SIZING_FIXED(record_limit_indicator_height) }
                                },
                                .backgroundColor = to_clay_color(indicator_color),
                                .cornerRadius = CLAY_CORNER_RADIUS(2)
                            }) {}
                        }
                        if (record_limit_indicator_right_pad > 0.0f) {
                            CLAY(CLAY_ID("RecordLimitDigitIndicatorRightPad"), {
                                .layout = {
                                    .sizing = { CLAY_SIZING_FIXED(record_limit_indicator_right_pad), CLAY_SIZING_FIXED(record_limit_indicator_height) }
                                }
                            }) {}
                        }
                    }
                }
            }

            CLAY(CLAY_ID("RecordLimitTimecodeCenterSpacerRight"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } }
            }) {}
        }
        CLAY_TEXT(CLAY_STRING("Live rule: only longer limits apply while recording."),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        CLAY_TEXT(CLAY_STRING("Shorter changes are ignored until the next recording."),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        // Level autostop (tape-end detection): ON/OFF toggle on one row,
        // then a tidy "Level: <box> Duration: <box>" row underneath — tight,
        // centered, no extra readout text on the row.
        // Lives in the timer window alongside the record time limit. Independent from
        // the digital dropout (frame error/missed frame) logic in the main settings.
        // Level is a normalized 0.X value (0.1-0.8).
        bool las_enabled = app->settings.level_autostop_enabled;
        Color las_box_bg = las_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
        Color las_box_fg = las_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);

        // Row 1: ON/OFF toggle on the left, with the label text beside it.
        CLAY(CLAY_ID("LevelAutostopRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            Color las_bg = las_enabled ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
            CLAY(CLAY_ID("LevelAutostopToggle"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(las_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(las_enabled ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            CLAY_TEXT(CLAY_STRING("Level autostop (tape end)"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY(CLAY_ID("LevelAutostopSpacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } } }) { }
        }

        // Row 2: "Level: [box] Duration: [box]" — tight, centered as one block,
        // labels centered to boxes, no extra readout text on the line.
        CLAY(CLAY_ID("LevelAutostopBoxesRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 6
            }
        }) {
            CLAY(CLAY_ID("LevelAutostopBoxesPadLeft"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } } }) { }
            CLAY_TEXT(CLAY_STRING("Trigger Level:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY(CLAY_ID("LevelAutostopLevelField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(56), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 6, 6, 0, 0 }
                },
                .backgroundColor = to_clay_color(las_box_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *lvl = app->settings.level_autostop_level_str[0] ? app->settings.level_autostop_level_str : "0.2";
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL) && las_enabled) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL, lvl, FONT_SIZE_STATS, 1, las_box_fg);
                } else {
                    CLAY_TEXT(make_string(lvl), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(las_box_fg) }));
                }
            }
            CLAY_TEXT(CLAY_STRING("Trigger Time:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY(CLAY_ID("LevelAutostopDurationField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(64), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 6, 6, 0, 0 }
                },
                .backgroundColor = to_clay_color(las_box_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *dur = app->settings.level_autostop_duration_str[0] ? app->settings.level_autostop_duration_str : "30";
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION) && las_enabled) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION, dur, FONT_SIZE_STATS, 1, las_box_fg);
                } else {
                    CLAY_TEXT(make_string(dur), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(las_box_fg) }));
                }
            }
            CLAY(CLAY_ID("LevelAutostopBoxesPadRight"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } } }) { }
        }
    }
}

static const char *gui_ui_device_type_name(device_type_t type) {
    switch (type) {
        case DEVICE_TYPE_HSDAOH:         return "HSDAOH";
        case DEVICE_TYPE_SIMPLE_CAPTURE: return "Simple Capture";
        case DEVICE_TYPE_CXADC:          return "CXADC";
        case DEVICE_TYPE_SIMULATED:      return "Simulated";
        case DEVICE_TYPE_PLAYBACK:       return "Playback";
#ifdef ENABLE_FX3
        case DEVICE_TYPE_FX3:            return "FX3ADC";
#endif
#ifdef ENABLE_DDD
        case DEVICE_TYPE_DDD:            return "DdD";
#endif
        default:                         return "Unknown";
    }
}

// Version info popup (opened by clicking the toolbar "i" badge)
static void render_version_info_window(gui_app_t *app)
{
    if (!s_version_info_window_open) return;

    // Slightly wider so network controls stay readable and the client Connect
    // action remains visible without clipping on common desktop sizes.
    int version_max_width = gui_ui_modal_max_extent(gui_ui_get_layout_width(), 680);
    int version_min_width = gui_ui_clamp_int(version_max_width, 1, 560);
    int version_max_height = gui_ui_modal_max_extent(gui_ui_get_layout_height(), 820);

    static char vi_version[64];
    static char vi_state[24];
    static char vi_device[96];
    static char vi_rate[32];
    static char vi_update_status[160];

    snprintf(vi_version, sizeof(vi_version), "%s", MIRSC_TOOLS_VERSION);

    const char *state_label;
    Color state_col;
    int m_peer = (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT &&
                  gui_monitor_client_connected()) ? gui_monitor_client_peer_state() : -1;
    if (m_peer == 2 || (m_peer < 0 && app->is_recording))
        { state_label = "Recording"; state_col = COLOR_CLIP_RED; }
    else if (m_peer == 1 || (m_peer < 0 && app->is_capturing))
        { state_label = "Capturing"; state_col = COLOR_SYNC_GREEN; }
    else { state_label = "Idle"; state_col = COLOR_TEXT_DIM; }
    snprintf(vi_state, sizeof(vi_state), "%s", state_label);

    if (app->device_count > 0 &&
        app->selected_device >= 0 && app->selected_device < app->device_count) {
        const device_info_t *dev = &app->devices[app->selected_device];
        snprintf(vi_device, sizeof(vi_device), "%s (%s)",
                 dev->name, gui_ui_device_type_name(dev->type));
    } else {
        snprintf(vi_device, sizeof(vi_device), "No device selected");
    }

    uint32_t sr = atomic_load(&app->sample_rate);
    if (sr >= 1000000u) {
        snprintf(vi_rate, sizeof(vi_rate), "%.3f MSPS", (double)sr / 1000000.0);
    } else if (sr >= 1000u) {
        snprintf(vi_rate, sizeof(vi_rate), "%.3f kSPS", (double)sr / 1000.0);
    } else {
        snprintf(vi_rate, sizeof(vi_rate), "%u Hz", sr);
    }

    bool update_check_running = atomic_load_explicit(&s_update_check_running, memory_order_acquire);
    /* Dev/unversioned builds (dev-YYYY-MM-DD-<sha>, unknown_version) are never
     * comparable with a release tag; show what they are instead of claiming an
     * update. The old lexical compare ('d' < 'v') permanently reported
     * "Available: vN.N.N" on every dev build. */
    bool update_current_comparable = gui_updater_version_is_comparable(MIRSC_TOOLS_VERSION);
    Color update_status_color = COLOR_TEXT_DIM;
    if (update_check_running) {
        snprintf(vi_update_status, sizeof(vi_update_status), "Checking...");
    } else if (!update_current_comparable && app->settings.update_last_release_tag[0]) {
        snprintf(vi_update_status, sizeof(vi_update_status),
                 "Dev build (%s) - latest release %s",
                 MIRSC_TOOLS_VERSION, app->settings.update_last_release_tag);
    } else if (app->settings.update_last_release_tag[0]) {
        int update_cmp = gui_updater_compare_versions(MIRSC_TOOLS_VERSION, app->settings.update_last_release_tag);
        if (update_cmp < 0) {
            snprintf(vi_update_status, sizeof(vi_update_status), "Available: %s", app->settings.update_last_release_tag);
            update_status_color = COLOR_METER_YELLOW;
        } else if (update_cmp == 0) {
            snprintf(vi_update_status, sizeof(vi_update_status), "Up to date (%s)", app->settings.update_last_release_tag);
            update_status_color = COLOR_SYNC_GREEN;
        } else {
            snprintf(vi_update_status, sizeof(vi_update_status), "Running newer build (latest %s)", app->settings.update_last_release_tag);
        }
    } else {
        snprintf(vi_update_status, sizeof(vi_update_status), "Not checked yet");
    }
    // Releases Page button: opens the releases PAGE (never a per-platform
    // asset URL that can 404). Always available.
    Color update_page_btn_bg = COLOR_BUTTON;
    Color update_page_btn_fg = COLOR_TEXT;
    // Install button: shown only once a check found a newer release; blocked
    // (click explains why) while a check runs, while capturing/recording, or
    // without a manifest. Two-click confirm: first click arms, second runs.
    bool update_install_available =
        !update_check_running &&
        update_current_comparable &&
        app->settings.update_last_release_tag[0] &&
        (gui_updater_compare_versions(MIRSC_TOOLS_VERSION, app->settings.update_last_release_tag) < 0);
    bool update_install_blocked = update_check_running ||
                                  app->is_capturing || app->is_recording ||
                                  !s_update_manifest_ok;
    Color update_install_btn_bg = (update_install_available && !update_install_blocked)
                                      ? COLOR_BUTTON
                                      : ui_disabled_color(COLOR_BUTTON);
    Color update_install_btn_fg = (update_install_available && !update_install_blocked)
                                      ? COLOR_TEXT
                                      : ui_disabled_color(COLOR_TEXT);
    char update_install_label[48];
    if (s_update_install_armed) {
        snprintf(update_install_label, sizeof(update_install_label), "Confirm install");
    } else {
        snprintf(update_install_label, sizeof(update_install_label), "Install %s",
                 app->settings.update_last_release_tag);
    }
    bool ab_swap_cxadc = gui_ui_selected_device_is_cxadc(app, NULL);
#ifdef ENABLE_FX3
    bool ab_swap_fx3 = gui_ui_selected_device_is_fx3(app);
#else
    bool ab_swap_fx3 = false;
#endif
#ifdef ENABLE_DDD
    bool ab_swap_ddd = gui_ui_selected_device_is_ddd(app);
#else
    bool ab_swap_ddd = false;
#endif
    bool ab_swap_supported_backend = !(ab_swap_cxadc || ab_swap_fx3 || ab_swap_ddd);
    bool ab_swap_toggle_enabled = ab_swap_supported_backend && !app->is_recording;

    CLAY(CLAY_ID("VersionInfoBackdrop"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP }
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 140}
    }) {}

    CLAY(CLAY_ID("VersionInfoWindow"), {
        .layout = {
            .sizing = {
                CLAY_SIZING_FIT(.min = version_min_width, .max = version_max_width),
                CLAY_SIZING_FIT(.max = version_max_height)
            },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 16, 16, 16, 16 },
            .childGap = 10
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER }
        },
        .clip = {
            .horizontal = true,
            .vertical = true,
            .childOffset = Clay_GetScrollOffset()
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8)
    }) {
        // Header
        CLAY(CLAY_ID("VersionInfoHeader"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("About MISRC Capture"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_TITLE, .textColor = to_clay_color(COLOR_TEXT) }));
            CLAY(CLAY_ID("VersionInfoHeaderSpacer"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
            }) {}
            CLAY(CLAY_ID("VersionInfoCloseButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("X"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Version row
        CLAY(CLAY_ID("VersionInfoVersionRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
        }) {
            CLAY(CLAY_ID("VersionInfoVersionLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Version:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(vi_version),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
            Color update_btn_bg = atomic_load_explicit(&s_update_check_running, memory_order_acquire)
                ? ui_disabled_color(COLOR_BUTTON)
                : COLOR_BUTTON;
            Color update_btn_fg = atomic_load_explicit(&s_update_check_running, memory_order_acquire)
                ? ui_disabled_color(COLOR_TEXT)
                : COLOR_TEXT;
            const char *update_btn_label = atomic_load_explicit(&s_update_check_running, memory_order_acquire)
                ? "Checking..."
                : "Check for Update";
            CLAY(CLAY_ID("VersionInfoCheckUpdateButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(132), CLAY_SIZING_FIXED(24) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(update_btn_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(make_string(update_btn_label),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(update_btn_fg) }));
            }
        }

        CLAY(CLAY_ID("VersionInfoUpdateRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("VersionInfoUpdateLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Update:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("VersionInfoUpdateStatus"), {
                .layout = { .sizing = { CLAY_SIZING_FIT(.min = 0, .max = 260), CLAY_SIZING_FIT(0) } }
            }) {
                CLAY_TEXT(make_string(vi_update_status),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(update_status_color) }));
            }
            CLAY(CLAY_ID("VersionInfoReleasesPageButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(24) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(update_page_btn_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("Releases Page"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(update_page_btn_fg) }));
            }
        }

        // Install row: rendered only when a check found a newer release.
        // Two-click confirm; the old binary is archived (renamed), never deleted.
        if (update_install_available) {
            CLAY(CLAY_ID("VersionInfoInstallRow"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                    .childGap = 10
                }
            }) {
                CLAY(CLAY_ID("VersionInfoInstallLabel"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } }
                }) {
                    CLAY_TEXT(CLAY_STRING("Install:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
                CLAY(CLAY_ID("VersionInfoInstallButton"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(150), CLAY_SIZING_FIXED(24) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(update_install_btn_bg),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    CLAY_TEXT(make_string(update_install_label),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(update_install_btn_fg) }));
                }
                CLAY(CLAY_ID("VersionInfoInstallHint"), {
                    .layout = { .sizing = { CLAY_SIZING_FIT(.min = 0, .max = 260), CLAY_SIZING_FIT(0) } }
                }) {
                    CLAY_TEXT(CLAY_STRING("old version archived, not deleted"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
            }
        }

        // Capture state row
        CLAY(CLAY_ID("VersionInfoStateRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 10 }
        }) {
            CLAY(CLAY_ID("VersionInfoStateLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Capture:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(vi_state),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(state_col) }));
        }

        // Device row
        CLAY(CLAY_ID("VersionInfoDeviceRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 10 }
        }) {
            CLAY(CLAY_ID("VersionInfoDeviceLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Device:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(vi_device),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // Sample rate row
        CLAY(CLAY_ID("VersionInfoRateRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 10 }
        }) {
            CLAY(CLAY_ID("VersionInfoRateLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Sample rate:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(vi_rate),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        CLAY(CLAY_ID("VersionInfoMisrcAbSwapRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
        }) {
            CLAY(CLAY_ID("VersionInfoMisrcAbSwapLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("A/B Swap:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            Color ab_swap_toggle_bg = app->settings.misrc_v15_v25_ab_swap ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
            if (!ab_swap_toggle_enabled) {
                ab_swap_toggle_bg = ui_disabled_color(ab_swap_toggle_bg);
            }
            Color ab_swap_toggle_text = ab_swap_toggle_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
            CLAY(CLAY_ID("VersionInfoMisrcAbSwapToggle"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(ab_swap_toggle_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(app->settings.misrc_v15_v25_ab_swap ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(ab_swap_toggle_text) }));
            }
            CLAY_TEXT(CLAY_STRING("MISRC V1.5/V2.5 Swap"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }

        // V4L2 Device List toggle (opt-in simple_capture/V4L2 device discovery).
        // Disabled by default; enabling lists OS video capture devices in the
        // device dropdown. Lives here in the info panel since it is not a
        // daily-use setting.
        CLAY(CLAY_ID("VersionInfoV4l2Row"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
        }) {
            CLAY(CLAY_ID("VersionInfoV4l2Label"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("V4L2 devices:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("VersionInfoV4l2Toggle"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(app->settings.discover_simple_capture ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(app->settings.discover_simple_capture ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            CLAY_TEXT(CLAY_STRING("list OS video capture devices"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }

        CLAY(CLAY_ID("VersionInfoCorePinningRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
        }) {
            CLAY(CLAY_ID("VersionInfoCorePinningLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Enable Core Pinning:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("VersionInfoCorePinningToggle"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(app->settings.show_core_pinning_in_settings ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(app->settings.show_core_pinning_in_settings ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            CLAY_TEXT(CLAY_STRING("show/hide core pinning in Settings"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }

        // Memory budget cycle (1/2/4/8/16 GB). Applies immediately when idle
        // by re-initializing the buffer manager; disabled while capturing or
        // recording since re-init would disrupt the live data path.
        {
            static char mem_budget_label[24];
            uint32_t gb = app->settings.memory_budget_gb;
            if (gb < 1) gb = 1;
            if (gb > 16) gb = 16;
            snprintf(mem_budget_label, sizeof(mem_budget_label), "%u GB", (unsigned)gb);
            bool mem_busy = (app->is_capturing || app->is_recording);
            Color mem_bg = mem_busy ? ui_disabled_color(COLOR_BUTTON)
                                    : (COLOR_BUTTON_ACTIVE);
            Color mem_fg = mem_busy ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
            CLAY(CLAY_ID("VersionInfoMemoryBudgetRow"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
            }) {
                CLAY(CLAY_ID("VersionInfoMemoryBudgetLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Memory budget:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
                CLAY(CLAY_ID("VersionInfoMemoryBudgetToggle"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(mem_bg),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    CLAY_TEXT(make_string(mem_budget_label),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .fontId = 1, .textColor = to_clay_color(mem_fg) }));
                }
                CLAY_TEXT(CLAY_STRING("max buffer RAM (1/2/4/8/16 GB; applies when idle)"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
        }

        // Network (Server/Client) section. Stock mode is Local (no networking).
        // Server hosts the HTTP control + RF/audio stream; Client connects to a
        // host and mirrors its device list/controls/capture state (master/slave).
        // v1 is LAN-only, no auth/encryption (mirrors the reference cxadc server).
        CLAY_TEXT(CLAY_STRING("Network mode:"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_HEADING, .textColor = to_clay_color(COLOR_TEXT) }));

        {
            int mode = app->settings.net_mode;
            if (mode < GUI_NET_MODE_LOCAL || mode > GUI_NET_MODE_LAST) mode = GUI_NET_MODE_LOCAL;
            const char *mode_label = gui_net_mode_name(mode);
            Color mode_bg = (mode == GUI_NET_MODE_LOCAL) ? COLOR_BUTTON : COLOR_BUTTON_ACTIVE;
            /* Reverted pre-PR compact toggle style, sized to the longest mode
             * label so "Monitor & Remote" never clips; the hint names the
             * modes again instead of the count. */
            int mode_btn_w = gui_ui_measure_button_width(app, gui_net_mode_name(GUI_NET_MODE_MONITOR_CLIENT),
                                                         FONT_SIZE_NORMAL, 8, 0, 90, 220);
            CLAY(CLAY_ID("VersionInfoNetModeRow"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
            }) {
                CLAY(CLAY_ID("VersionInfoNetModeLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Mode:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
                CLAY(CLAY_ID("VersionInfoNetModeToggle"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(mode_btn_w), CLAY_SIZING_FIXED(28) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(mode_bg),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    CLAY_TEXT(make_string(mode_label),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                }
                CLAY_TEXT(CLAY_STRING("Local / Server / Client / Monitor & Remote (hold for list)"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            /* Hold-press direct-select menu (armed by a >= 450 ms press on
             * the toggle; consumed in the version-window click gate). */
            if (s_net_mode_menu_open) {
                static const int menu_ids[4] = {
                    GUI_NET_MODE_LOCAL, GUI_NET_MODE_SERVER,
                    GUI_NET_MODE_CLIENT, GUI_NET_MODE_MONITOR_CLIENT
                };
                for (int i = 0; i < 4; i++) {
                    bool sel = (app->settings.net_mode == menu_ids[i]);
                    Color item_bg = sel ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                    CLAY(CLAY_IDI("VersionInfoNetModeMenuItem", i), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIXED(mode_btn_w), CLAY_SIZING_FIXED(26) },
                            .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                            .padding = { 8, 8, 0, 0 }
                        },
                        .backgroundColor = to_clay_color(item_bg),
                        .cornerRadius = CLAY_CORNER_RADIUS(4)
                    }) {
                        CLAY_TEXT(make_string(gui_net_mode_name(menu_ids[i])),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                }
                CLAY_TEXT(CLAY_STRING("click a mode to switch directly"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
        }

        // Server: port field.
        if (app->settings.net_mode == GUI_NET_MODE_SERVER) {
            CLAY(CLAY_ID("VersionInfoNetServerPortRow"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
            }) {
                CLAY(CLAY_ID("VersionInfoNetServerPortLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Port:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
                CLAY(CLAY_ID("VersionInfoNetServerPortField"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(32) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER },
                        .padding = { 6, 6, 0, 0 }
                    },
                    .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    const char *pv = app->settings.net_server_port_str[0] ? app->settings.net_server_port_str : "8080";
                    if (gui_ui_is_text_field_active(UI_TEXT_FIELD_NET_SERVER_PORT)) {
                        gui_ui_render_active_text(UI_TEXT_FIELD_NET_SERVER_PORT, pv, FONT_SIZE_STATS, 1, COLOR_TEXT);
                    } else {
                        CLAY_TEXT(make_string(pv),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                }
                CLAY_TEXT(CLAY_STRING("TCP port to listen on (LAN only, no auth)"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
        }

        // The legacy Client and discovery-only M-Client share the same UI.
        // M-Client must never connect through the legacy RF ingest backend.
        if (app->settings.net_mode == GUI_NET_MODE_CLIENT ||
            app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT) {
            CLAY_TEXT(CLAY_STRING("Discovered servers:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            {
                int disc_n = gui_net_discovered_count();
                if (disc_n <= 0) {
                    CLAY_TEXT(CLAY_STRING("Scanning for servers on the LAN..."),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                } else {
                    int disc_visible = disc_n;
                    if (disc_visible > VERSION_INFO_NET_DISCOVERY_MAX_ROWS) {
                        disc_visible = VERSION_INFO_NET_DISCOVERY_MAX_ROWS;
                    }
                    static char disc_labels[VERSION_INFO_NET_DISCOVERY_MAX_ROWS][160];
                    for (int i = 0; i < disc_visible; i++) {
                        char dhost[64] = {0};
                        uint16_t dport = 0;
                        char dname[64] = {0};
                        if (!gui_net_get_discovered(i, dhost, sizeof(dhost), &dport, dname, sizeof(dname))) break;
                        char *disc_label = disc_labels[i];
                        if (dname[0]) {
                            snprintf(disc_label, sizeof(disc_labels[i]), "%s:%u  (%s)", dhost, (unsigned)dport, dname);
                        } else {
                            snprintf(disc_label, sizeof(disc_labels[i]), "%s:%u", dhost, (unsigned)dport);
                        }
                        bool sel = (strcmp(app->settings.net_client_host, dhost) == 0 &&
                                    app->settings.net_client_port == dport);
                        Color item_bg = sel ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                        /* Compact row: fit the "ip:port (name)" text width (capped)
                         * instead of growing to the full window width, so the
                         * list reads as a normal IPv4+port line of info. */
                        CLAY(CLAY_IDI("VersionInfoNetDiscItem", i), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIT(.min = 0, .max = 420), CLAY_SIZING_FIXED(26) },
                                .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                                .padding = { 8, 8, 0, 0 }
                            },
                            .backgroundColor = to_clay_color(item_bg),
                            .cornerRadius = CLAY_CORNER_RADIUS(4)
                        }) {
                            CLAY_TEXT(make_string(disc_label),
                                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }
                    if (disc_n > disc_visible) {
                        static char disc_more_label[96];
                        snprintf(disc_more_label, sizeof(disc_more_label),
                                 "%d more server(s) not shown", disc_n - disc_visible);
                        CLAY_TEXT(make_string(disc_more_label),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    }
                    if (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT) {
                        CLAY_TEXT(CLAY_STRING("click a server to connect (MISRC-M/2 monitor protocol)"),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    } else {
                        CLAY_TEXT(CLAY_STRING("click a server to connect (LAN only, no auth)"),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    }
                }
            }
            // Compact manual host:port fallback (fixed widths so the window stays narrow).
            CLAY(CLAY_ID("VersionInfoNetClientManualRow"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 6 }
            }) {
                CLAY(CLAY_ID("VersionInfoNetClientHostField"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(150), CLAY_SIZING_FIXED(32) },
                        .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                        .padding = { 8, 8, 0, 0 }
                    },
                    .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    const char *hv = app->settings.net_client_host[0] ? app->settings.net_client_host : "host";
                    if (gui_ui_is_text_field_active(UI_TEXT_FIELD_NET_CLIENT_HOST)) {
                        gui_ui_render_active_text(UI_TEXT_FIELD_NET_CLIENT_HOST, hv, FONT_SIZE_STATS, 1, COLOR_TEXT);
                    } else {
                        CLAY_TEXT(make_string(hv),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(hv[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                    }
                }
                CLAY_TEXT(CLAY_STRING(":"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                CLAY(CLAY_ID("VersionInfoNetClientPortField"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(64), CLAY_SIZING_FIXED(32) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER },
                        .padding = { 6, 6, 0, 0 }
                    },
                    .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    const char *pv = app->settings.net_client_port_str[0] ? app->settings.net_client_port_str : "8080";
                    if (gui_ui_is_text_field_active(UI_TEXT_FIELD_NET_CLIENT_PORT)) {
                        gui_ui_render_active_text(UI_TEXT_FIELD_NET_CLIENT_PORT, pv, FONT_SIZE_STATS, 1, COLOR_TEXT);
                    } else {
                        CLAY_TEXT(make_string(pv),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                }
            }
            // Connect button on its own row so it remains visible even when the
            // discovery list has multiple entries.
            CLAY(CLAY_ID("VersionInfoNetClientConnectRow"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
            }) {
                CLAY(CLAY_ID("VersionInfoNetClientConnectLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Action:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
                bool net_connected = gui_net_active(app);
                bool connection_running=gui_net_client_connection_running(app);
                Color connect_bg = net_connected ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                const char *connect_label = connection_running ? "Disconnect" : "Connect";
                CLAY(CLAY_ID("VersionInfoNetConnectButton"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(96), CLAY_SIZING_FIXED(28) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(connect_bg),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    CLAY_TEXT(make_string(connect_label),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                }
                if (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT) {
                    CLAY_TEXT(CLAY_STRING("Phosphor preview (Zstd-1)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                } else {
                    CLAY_TEXT(CLAY_STRING("toggle client connection"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
            }
        }

        // Network status line.
        {
            static char net_status[160];
            gui_net_status_string(app, net_status, sizeof(net_status));
            /* Warn about actual RF queue pressure/dropouts without claiming the
             * physical LAN is necessarily the bottleneck. Server and legacy
             * Client statistics are independent from the monitor transport. */
            bool rf_transport_warning = false;
            if (app->settings.net_mode == GUI_NET_MODE_SERVER) {
                rf_transport_warning = atomic_load(&app->net_rf_server_stream_drops) > 0 ||
                    atomic_load(&app->net_audio_server_stream_drops) > 0 ||
                    atomic_load(&app->net_rf_server_queue_bytes) >=
                        gui_net_queue_warn_bytes(GUI_NET_RF_QUEUE_LIMIT_BYTES) ||
                    atomic_load(&app->net_audio_server_queue_bytes) >=
                        gui_net_queue_warn_bytes(GUI_NET_AUDIO_QUEUE_LIMIT_BYTES);
            } else if (app->settings.net_mode == GUI_NET_MODE_CLIENT) {
                rf_transport_warning = atomic_load(&app->net_rf_client_interruptions) > 0 ||
                    atomic_load(&app->net_audio_client_interruptions) > 0 ||
                    atomic_load(&app->net_rf_server_stream_drops) > 0 ||
                    atomic_load(&app->net_rf_server_queue_bytes) >=
                        gui_net_queue_warn_bytes(GUI_NET_RF_QUEUE_LIMIT_BYTES);
            }
            Color net_status_color = rf_transport_warning
                ? (Color){255, 170, 80, 255} : COLOR_TEXT;
            CLAY(CLAY_ID("VersionInfoNetStatusRow"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
            }) {
                CLAY(CLAY_ID("VersionInfoNetStatusLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Status:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
                CLAY_TEXT(make_string(net_status),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(net_status_color) }));
            }
        }
        // Credits (without license/year text).
        CLAY_TEXT(CLAY_STRING("(c) Harry Munday, AlessandroAU, Stefan O, Vrunk11, machcnz"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

    }
}
// Metadata popup (opened by clicking the toolbar scroll badge)
static void render_metadata_window(gui_app_t *app)
{
    if (!s_metadata_window_open) return;

    int metadata_max_width = gui_ui_modal_max_extent(gui_ui_get_layout_width(), 840);
    int metadata_min_width = gui_ui_clamp_int(metadata_max_width, 1, 640);
    int metadata_max_height = gui_ui_modal_max_extent(gui_ui_get_layout_height(), 780);

    CLAY(CLAY_ID("MetadataBackdrop"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP }
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 140}
    }) {}

    CLAY(CLAY_ID("MetadataWindow"), {
        .layout = {
            .sizing = {
                CLAY_SIZING_FIT(.min = metadata_min_width, .max = metadata_max_width),
                CLAY_SIZING_FIT(.max = metadata_max_height)
            },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 16, 16, 16, 16 },
            .childGap = 10
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER }
        },
        .clip = {
            .horizontal = true,
            .vertical = true,
            .childOffset = Clay_GetScrollOffset()
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8)
    }) {
        CLAY(CLAY_ID("MetadataHeader"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Capture Ingest Metadata"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_TITLE, .textColor = to_clay_color(COLOR_TEXT) }));
            CLAY(CLAY_ID("MetadataHeaderSpacer"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
            }) {}
            CLAY(CLAY_ID("MetadataCloseButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("X"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        CLAY_TEXT(CLAY_STRING("These fields are saved to settings and written to the capture log at recording start."),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        CLAY(CLAY_ID("MetadataProjectRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("MetadataProjectLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Project:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("MetadataProjectField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 8, 8, 0, 0 }
                },
                .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *v = app->settings.ingest_project;
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_INGEST_PROJECT)) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_INGEST_PROJECT, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
                } else {
                    CLAY_TEXT(v[0] ? make_string(v) : CLAY_STRING("(empty)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(v[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                }
            }
        }

        CLAY(CLAY_ID("MetadataTapeIdRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("MetadataTapeIdLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Tape ID:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("MetadataTapeIdField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 8, 8, 0, 0 }
                },
                .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *v = app->settings.ingest_tape_id;
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_INGEST_TAPE_ID)) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_INGEST_TAPE_ID, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
                } else {
                    CLAY_TEXT(v[0] ? make_string(v) : CLAY_STRING("(empty)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(v[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                }
            }
        }

        CLAY(CLAY_ID("MetadataTapeFormatRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("MetadataTapeFormatLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Tape Format:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("MetadataTapeFormatField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 8, 8, 0, 0 }
                },
                .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *v = app->settings.ingest_tape_format;
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_INGEST_TAPE_FORMAT)) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_INGEST_TAPE_FORMAT, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
                } else {
                    CLAY_TEXT(v[0] ? make_string(v) : CLAY_STRING("(empty)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(v[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                }
            }
        }

        CLAY(CLAY_ID("MetadataTapeSizeRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("MetadataTapeSizeLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Tape Size:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("MetadataTapeSizeField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 8, 8, 0, 0 }
                },
                .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *v = app->settings.ingest_tape_size;
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_INGEST_TAPE_SIZE)) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_INGEST_TAPE_SIZE, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
                } else {
                    CLAY_TEXT(v[0] ? make_string(v) : CLAY_STRING("(empty)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(v[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                }
            }
        }

        CLAY(CLAY_ID("MetadataTapeSpeedRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("MetadataTapeSpeedLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Tape Speed:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("MetadataTapeSpeedField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 8, 8, 0, 0 }
                },
                .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *v = app->settings.ingest_tape_speed;
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_INGEST_TAPE_SPEED)) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_INGEST_TAPE_SPEED, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
                } else {
                    CLAY_TEXT(v[0] ? make_string(v) : CLAY_STRING("(empty)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(v[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                }
            }
        }

        CLAY(CLAY_ID("MetadataTapeConditionRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("MetadataTapeConditionLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Tape Condition:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("MetadataTapeConditionField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 8, 8, 0, 0 }
                },
                .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *v = app->settings.ingest_tape_condition;
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_INGEST_TAPE_CONDITION)) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_INGEST_TAPE_CONDITION, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
                } else {
                    CLAY_TEXT(v[0] ? make_string(v) : CLAY_STRING("(empty)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(v[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                }
            }
        }
        CLAY(CLAY_ID("MetadataOperatorRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("MetadataOperatorLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Operator:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("MetadataOperatorField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 8, 8, 0, 0 }
                },
                .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *v = app->settings.ingest_operator;
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_INGEST_OPERATOR)) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_INGEST_OPERATOR, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
                } else {
                    CLAY_TEXT(v[0] ? make_string(v) : CLAY_STRING("(empty)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(v[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                }
            }
        }

        CLAY(CLAY_ID("MetadataLocationRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("MetadataLocationLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Location:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("MetadataLocationField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 8, 8, 0, 0 }
                },
                .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *v = app->settings.ingest_location;
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_INGEST_LOCATION)) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_INGEST_LOCATION, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
                } else {
                    CLAY_TEXT(v[0] ? make_string(v) : CLAY_STRING("(empty)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(v[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                }
            }
        }

        CLAY(CLAY_ID("MetadataNotesRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            CLAY(CLAY_ID("MetadataNotesLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Notes:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_ID("MetadataNotesField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 8, 8, 0, 0 }
                },
                .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *v = app->settings.ingest_notes;
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_INGEST_NOTES)) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_INGEST_NOTES, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
                } else {
                    CLAY_TEXT(v[0] ? make_string(v) : CLAY_STRING("(empty)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(v[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                }
            }
        }
    }
}

static const char *toolbar_audio_channel_label(const gui_app_t *app,
                                               bool cxadc_mode, bool compact)
{
#if defined(_WIN32)
    if (cxadc_mode) {
        return app->settings.audio_monitor_ch34
            ? (compact ? "HSW" : "HSW CH3")
            : (compact ? "A1/2" : "AUD 1/2");
    }
#else
    (void)cxadc_mode;
#endif
    return app->settings.audio_monitor_ch34
        ? (compact ? "3/4" : "CH3/4")
        : (compact ? "1/2" : "CH1/2");
}

static void render_toolbar_audio_group(gui_app_t *app,
                                       bool cxadc_mode,
                                       bool toolbar_ultra_narrow,
                                       bool toolbar_very_narrow,
                                       bool show_audio_meter_labels,
                                       int toolbar_text_size,
                                       int toolbar_gap,
                                       int audio_mon_width,
                                       int audio_ch_width,
                                       int audio_bars_panel_width,
                                       int audio_meter_col_width,
                                       int audio_meter_width,
                                       int audio_meter_height,
                                       int audio_meter_gap)
{
    CLAY(CLAY_ID("ToolbarAudioGroup"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(32) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = toolbar_gap
        }
    }) {
        Color mon_bg = app->settings.audio_monitor_playback ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        // Fall back to "Mon" whenever the long label measurably cannot fit
        // the fixed-width button — otherwise Clay wraps "Audio Mon" into two
        // stacked lines inside the 32px-high box. The tier flags short-circuit
        // the compact tiers; the measured check covers every width in between.
        const char *audio_mon_label = "Audio Mon";
        if (toolbar_very_narrow || toolbar_ultra_narrow ||
            gui_ui_measure_text_width(app, "Audio Mon", toolbar_text_size, 0) > audio_mon_width - 4) {
            audio_mon_label = "Mon";
        }
        CLAY(CLAY_ID("AudioPlaybackToggle"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(audio_mon_width), CLAY_SIZING_FIXED(32) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = to_clay_color(mon_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(make_string(audio_mon_label),
                CLAY_TEXT_CONFIG({ .fontSize = toolbar_text_size, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        Color ch_bg = app->settings.audio_monitor_ch34 ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        const char *audio_ch_toggle_label =
            toolbar_audio_channel_label(app, cxadc_mode, toolbar_ultra_narrow);
        CLAY(CLAY_ID("AudioChannelToggle"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(audio_ch_width), CLAY_SIZING_FIXED(32) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = to_clay_color(ch_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(make_string(audio_ch_toggle_label),
                CLAY_TEXT_CONFIG({ .fontSize = toolbar_text_size, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        CLAY(CLAY_ID("AudioLevelBars"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(audio_bars_panel_width), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = audio_meter_gap,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .padding = { 4, 4, 4, 4 }
            },
            .backgroundColor = to_clay_color((Color){25,25,30,255}),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            for (int i = 0; i < 4; i++) {
                uint32_t p = atomic_load(&app->audio_peak[i]);
                float frac = (p > 0) ? (float)p / 8388607.0f : 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                int fill_w = (int)(frac * (float)audio_meter_width);
                if (fill_w < 0) fill_w = 0;
                if (fill_w > audio_meter_width) fill_w = audio_meter_width;

                Color bar_col = (frac > 0.95f) ? COLOR_CLIP_RED : (frac > 0.75f) ? COLOR_METER_YELLOW : COLOR_SYNC_GREEN;

                CLAY(CLAY_IDI("AudioMeterCol", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(audio_meter_col_width), CLAY_SIZING_FIXED(24) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childGap = show_audio_meter_labels ? 1 : 0,
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER }
                    }
                }) {
                    if (show_audio_meter_labels) {
                        snprintf(audio_ch_label[i], sizeof(audio_ch_label[i]), "CH%d", i + 1);
                        CLAY(CLAY_IDI("AudioChLabel", i), { .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) } } }) {
                            CLAY_TEXT(make_string(audio_ch_label[i]),
                                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                        }
                    }

                    CLAY(CLAY_IDI("AudioMeter", i), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIXED(audio_meter_width), CLAY_SIZING_FIXED(audio_meter_height) },
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childGap = 0
                        },
                        .backgroundColor = to_clay_color((Color){40,40,48,255}),
                        .cornerRadius = CLAY_CORNER_RADIUS(2)
                    }) {
                        if (fill_w > 0) {
                            CLAY(CLAY_IDI("AudioMeterFill", i), {
                                .layout = { .sizing = { CLAY_SIZING_FIXED(fill_w), CLAY_SIZING_GROW(0) } },
                                .backgroundColor = to_clay_color(bar_col),
                                .cornerRadius = CLAY_CORNER_RADIUS(2)
                            }) { }
                        }
                    }
                }
            }
        }
    }
}

static int gui_ui_toolbar_connection_width(const gui_app_t *app, bool compact,
                                            bool control_capturing, int text_size)
{
    const char *label = compact && !control_capturing ? "Connect" : "Disconnect";
    return gui_ui_measure_button_width(app, label, text_size,
        compact ? 4 : 6, 0, compact ? 32 : 100, compact ? INT_MAX : 200);
}

static void render_toolbar_connection_group(bool control_capturing,
                                            bool control_starting,
                                            int toolbar_text_size,
                                            int toolbar_gap,
                                            int connect_button_width,
                                            int mode_toggle_width,
                                            Color mode_bg,
                                            Color mode_fg,
                                            const char *mode_label)
{
    CLAY(CLAY_ID("ToolbarConnectionGroup"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(32) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = toolbar_gap
        }
    }) {
        Color connect_color = (control_capturing || control_starting) ? COLOR_CLIP_RED : COLOR_SYNC_GREEN;
        const char *connect_label = control_starting ? "Cancel" : control_capturing ? "Disconnect" : "Connect";
        CLAY(CLAY_ID("ConnectButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(connect_button_width), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(connect_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(make_string(connect_label),
                CLAY_TEXT_CONFIG({
                    .fontSize = toolbar_text_size,
                    .textColor = { 255, 255, 255, 255 },
                    .wrapMode = CLAY_TEXT_WRAP_NONE
                }));
        }

        // Capture mode toggle also selects HSDAOH backend at connect time.
        CLAY(CLAY_ID("CaptureModeToggle"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(mode_toggle_width), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(control_starting ? COLOR_BUTTON : mode_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(make_string(mode_label),
                CLAY_TEXT_CONFIG({
                    .fontSize = toolbar_text_size,
                    .textColor = to_clay_color(control_starting ? COLOR_TEXT_DIM : mode_fg),
                    .wrapMode = CLAY_TEXT_WRAP_NONE
                }));
        }
    }
}

// Render the toolbar
// Toolbar layout profiles. Tiers mirror the historical breakpoint ternaries
// exactly, but the active tier is selected by measured fit (largest profile
// whose single-row requirement fits the current width) instead of raw width
// ranges. Every chosen profile fits by construction, so there is no width
// window where the toolbar overflows and clips the right-side controls off
// the window, and no breakpoint bounce while dragging the window edge.
enum {
    GUI_UI_TOOLBAR_TIER_FULL = 0,
    GUI_UI_TOOLBAR_TIER_NARROW,
    GUI_UI_TOOLBAR_TIER_VERY_NARROW,
    GUI_UI_TOOLBAR_TIER_ULTRA_NARROW,
    GUI_UI_TOOLBAR_TIER_TINY,
};

typedef struct gui_ui_toolbar_profile {
    int padding_h;
    int gap;
    int text_size;
    int icon_button_size;
    int version_icon_size;
    int metadata_icon_size;
    int dd_min_width;
    int dd_max_width;
    int connect_width;
    int mode_min_width;
    int mode_max_width;
    int audio_mon_width;
    int audio_ch_width;
    int bars_panel_width;
    int meter_col_width;
    int meter_width;
    int meter_height;
    int meter_gap;
    int record_width;
    int dropdown_padding;
    bool tiny;
    bool ultra_narrow;
    bool very_narrow;
    bool show_audio_meter_labels;
    bool show_version_icon;
    bool show_device_label;
} gui_ui_toolbar_profile_t;

static gui_ui_toolbar_profile_t gui_ui_toolbar_profile_for_tier(int tier)
{
    gui_ui_toolbar_profile_t p;
    memset(&p, 0, sizeof(p));
    switch (tier) {
    case GUI_UI_TOOLBAR_TIER_NARROW:
        p = (gui_ui_toolbar_profile_t) {
            .padding_h = 8, .gap = 12, .text_size = FONT_SIZE_NORMAL,
            .icon_button_size = 32, .version_icon_size = 20, .metadata_icon_size = 18,
            .dd_min_width = 160, .dd_max_width = 230, .connect_width = 92,
            .mode_min_width = 112, .mode_max_width = 178,
            .audio_mon_width = 90, .audio_ch_width = 70, .bars_panel_width = 240,
            .meter_col_width = 54, .meter_width = 50, .meter_height = 8,
            .meter_gap = 4,
            .record_width = 80, .dropdown_padding = 10,
            .show_audio_meter_labels = true, .show_version_icon = true,
            .show_device_label = true,
        };
        break;
    case GUI_UI_TOOLBAR_TIER_VERY_NARROW:
        p = (gui_ui_toolbar_profile_t) {
            .padding_h = 8, .gap = 6, .text_size = FONT_SIZE_DROPDOWN,
            .icon_button_size = 32, .version_icon_size = 20, .metadata_icon_size = 18,
            .dd_min_width = 138, .dd_max_width = 210, .connect_width = 82,
            .mode_min_width = 96, .mode_max_width = 156,
            .audio_mon_width = 68, .audio_ch_width = 64, .bars_panel_width = 200,
            .meter_col_width = 44, .meter_width = 38, .meter_height = 8,
            .meter_gap = 4,
            .record_width = 74, .dropdown_padding = 6,
            .show_audio_meter_labels = true, .show_version_icon = true,
            .show_device_label = false,
        };
        break;
    case GUI_UI_TOOLBAR_TIER_ULTRA_NARROW:
        p = (gui_ui_toolbar_profile_t) {
            .padding_h = 8, .gap = 4, .text_size = FONT_SIZE_DROPDOWN,
            .icon_button_size = 32, .version_icon_size = 20, .metadata_icon_size = 18,
            .dd_min_width = 120, .dd_max_width = 190, .connect_width = 66,
            .mode_min_width = 78, .mode_max_width = 136,
            .audio_mon_width = 56, .audio_ch_width = 56, .bars_panel_width = 164,
            .meter_col_width = 36, .meter_width = 30, .meter_height = 8,
            .meter_gap = 4,
            .record_width = 68, .dropdown_padding = 6,
            .show_audio_meter_labels = true, .show_version_icon = false,
            .show_device_label = false,
        };
        break;
    case GUI_UI_TOOLBAR_TIER_TINY:
        p = (gui_ui_toolbar_profile_t) {
            .padding_h = 4, .gap = 2, .text_size = FONT_SIZE_DROPDOWN,
            .icon_button_size = 28, .version_icon_size = 16, .metadata_icon_size = 14,
            .dd_min_width = 100, .dd_max_width = 150, .connect_width = 52,
            .mode_min_width = 58, .mode_max_width = 76,
            .audio_mon_width = 44, .audio_ch_width = 46, .bars_panel_width = 82,
            .meter_col_width = 17, .meter_width = 13, .meter_height = 6,
            .meter_gap = 2,
            .record_width = 56, .dropdown_padding = 6,
            .show_audio_meter_labels = false, .show_version_icon = false,
            .show_device_label = false,
            .tiny = true, .ultra_narrow = true, .very_narrow = true,
        };
        break;
    case GUI_UI_TOOLBAR_TIER_FULL:
    default:
        p = (gui_ui_toolbar_profile_t) {
            .padding_h = 8, .gap = 12, .text_size = FONT_SIZE_NORMAL,
            .icon_button_size = 32, .version_icon_size = 20, .metadata_icon_size = 18,
            .dd_min_width = 180, .dd_max_width = 280, .connect_width = 100,
            .mode_min_width = 112, .mode_max_width = 230,
            .audio_mon_width = 90, .audio_ch_width = 70, .bars_panel_width = 240,
            .meter_col_width = 54, .meter_width = 50, .meter_height = 8,
            .record_width = 80, .dropdown_padding = 10,
            .show_audio_meter_labels = true, .show_version_icon = true,
            .show_device_label = true,
        };
        break;
    }
    return p;
}

static int gui_ui_toolbar_required_width(const gui_ui_toolbar_profile_t *p,
                                         int device_label_width,
                                         int dd_width,
                                         int mode_width,
                                         bool compact)
{
    bool show_metadata_icon = true;
    int child_count = 8 +
        (p->show_version_icon ? 1 : 0) +
        (show_metadata_icon ? 1 : 0) +
        (p->show_device_label ? 1 : 0);
    return (p->padding_h * 2) +
        (p->show_version_icon ? p->icon_button_size : 0) +
        (show_metadata_icon ? p->icon_button_size : 0) +
        (show_metadata_icon ? 8 : (p->show_version_icon ? 4 : 0)) +
        device_label_width + dd_width +
        p->connect_width + mode_width + (mode_width > 0 ? p->gap : 0) +
        p->audio_mon_width + p->audio_ch_width + p->bars_panel_width +
        (p->gap * 2) +
        p->record_width + (p->icon_button_size * 2) +
        // All actual gaps and the fixed spacer are already counted above.
        // Keep the normal layout's guard, but reclaim it under width pressure.
        ((child_count - 1) * p->gap) + (compact ? 0 : 8);
}

static const char *gui_ui_toolbar_mode_label(bool mode_misrc,
                                             bool cxadc_mode,
                                             bool cxadc_clockgen_mode,
                                             bool cxadc_misrc_clockgen_mode,
                                             bool fx3_mode,
                                             bool ddd_mode,
                                             bool ddd_clockgen_mode,
                                             int tier)
{
    if (tier >= GUI_UI_TOOLBAR_TIER_TINY) {
        if (cxadc_mode) {
            return cxadc_clockgen_mode
                ? (cxadc_misrc_clockgen_mode ? "MiCg" : "CxCg")
                : "CXA";
        } else if (fx3_mode) {
            return "FX3ADC";
        } else if (ddd_mode) {
            return ddd_clockgen_mode ? "DdDCg" : "DdD";
        }
        return mode_misrc ? "MIS" : "HSD";
    }
    if (tier >= GUI_UI_TOOLBAR_TIER_VERY_NARROW) {
        if (cxadc_mode) {
            return cxadc_clockgen_mode
                ? (cxadc_misrc_clockgen_mode ? "MisClk" : "CxClk")
                : "CXADC";
        } else if (fx3_mode) {
            return "FX3ADC";
        } else if (ddd_mode) {
            return ddd_clockgen_mode ? "DdDClk" : "DdD";
        }
        return mode_misrc ? "MISRC" : "HSDAOH";
    }
    if (cxadc_mode) {
        if (cxadc_clockgen_mode) {
            return cxadc_misrc_clockgen_mode ? "Mode: MISRC Clockgen" : "Mode: CXADC Clockgen";
        }
        return "Mode: CXADC";
    } else if (fx3_mode) {
        return "Mode: FX3ADC";
    } else if (ddd_mode) {
        return ddd_clockgen_mode ? "Mode: DdD Clockgen" : "Mode: DdD";
    }
    return mode_misrc ? "Mode: MISRC" : "Mode: HSDAOH";
}

static void render_toolbar(gui_app_t *app, bool ddd_compact_labels) {
    s_settings_icon_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_SETTINGS_ICON;
    s_record_limit_icon_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_CLOCK_ICON;
    s_metadata_icon_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_SCROLL_ICON;
    // Fixed left-side version/status badge: color reflects current MISRC capture state.
    // The version string itself lives only in the OS window title (set in misrc_gui.c),
    // so the toolbar left anchor is constant width and never shifts on any platform.
    s_version_icon_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_VERSION_ICON;
    gui_version_icon_state_t version_icon_state = GUI_VERSION_ICON_IDLE;
    if (app->is_recording) {
        version_icon_state = GUI_VERSION_ICON_RECORDING;
    } else if (app->is_capturing) {
        version_icon_state = GUI_VERSION_ICON_CAPTURING;
    }
    s_version_icon_element.customData.version_icon.state = version_icon_state;
    int toolbar_width = gui_ui_get_layout_width();
    bool toolbar_compact = toolbar_width < GUI_UI_STATUS_COMPACT_BREAKPOINT;
    // Use the same local/remote state for measuring and drawing this frame.
    bool control_capturing = gui_net_is_client(app)
        ? gui_net_client_peer_capturing(app) : app->is_capturing;
    bool playback_starting = gui_playback_is_starting(app);
    const device_info_t *selected_device =
        app->selected_device >= 0 && app->selected_device < app->device_count
            ? &app->devices[app->selected_device] : NULL;
    gui_ui_format_device_label(selected_device,
        selected_device && selected_device->type == DEVICE_TYPE_DDD ? ddd_compact_labels : toolbar_compact,
                               device_dropdown_buf, sizeof(device_dropdown_buf));
    bool cxadc_clockgen_mode = false;
    bool cxadc_mode = gui_ui_selected_device_is_cxadc(app, &cxadc_clockgen_mode);
    bool cxadc_misrc_clockgen_mode = gui_ui_selected_device_is_cxadc_misrc_clockgen(app);
#ifdef ENABLE_FX3
    bool fx3_mode = gui_ui_selected_device_is_fx3(app);
#else
    bool fx3_mode = false;
#endif
#ifdef ENABLE_DDD
    bool ddd_mode = gui_ui_selected_device_is_ddd(app);
    bool ddd_clockgen_mode = gui_ui_selected_device_is_ddd_clockgen(app);
#else
    bool ddd_mode = false;
    bool ddd_clockgen_mode = false;
#endif
    bool mode_source_runtime = app->is_recording;
    bool mode_misrc = mode_source_runtime ? app->capture_mode_runtime_misrc
                                          : app->user_capture_mode_misrc;
    if (cxadc_mode || fx3_mode || ddd_mode) {
        mode_misrc = false;
    }
    gui_ui_trace_capture_mode_render(app, mode_misrc, mode_source_runtime);
    // Toggle is only clickable for hsdaoh/simple_capture backends where
    // the MISRC/HSDAOH A/B-swap is meaningful.
    bool mode_change_allowed = !app->is_recording &&
                               !cxadc_mode &&
                               !fx3_mode &&
                               !ddd_mode &&
                               !gui_net_is_client(app);
    Color mode_bg = mode_misrc ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
    if (!mode_change_allowed) {
        mode_bg = ui_disabled_color(mode_bg);
    }
    Color mode_fg = mode_change_allowed ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);

    // Choose the most spacious toolbar profile whose minimum single-row
    // requirement fits the current width. The device dropdown is the row's
    // only compressible element (it ellipsizes), so the fit check uses its
    // dd_min floor instead of its natural width: a tier stays active until
    // the row cannot fit even with the dropdown fully shortened. Every
    // chosen profile fits by construction, so the row can never overflow and
    // clip the right-side controls off the window (the old fixed breakpoints
    // left a ~1020-1233px window where no profile fit and the buttons
    // vanished), the profile switch is monotonic while dragging so nothing
    // bounces, and the row consumes its dead space before stepping down.
    int chosen_tier = -1;
    int chosen_required_width = 0;
    int tiny_required_width = 0;
    gui_ui_toolbar_profile_t prof = gui_ui_toolbar_profile_for_tier(GUI_UI_TOOLBAR_TIER_TINY);
    {
        for (int tier = GUI_UI_TOOLBAR_TIER_FULL; tier <= GUI_UI_TOOLBAR_TIER_TINY; tier++) {
            gui_ui_toolbar_profile_t p = gui_ui_toolbar_profile_for_tier(tier);
            // Keep the full-name details reachable when a DdD alias is shown.
            p.show_version_icon = p.show_version_icon || ddd_mode;
            // Start with the normal width; compact layouts may reclaim only
            // the unused space before moving to a smaller tier or another row.
            p.connect_width = gui_ui_toolbar_connection_width(app, false,
                control_capturing, p.text_size);
            p.audio_ch_width = gui_ui_measure_button_width(app,
                toolbar_audio_channel_label(app, cxadc_mode, p.ultra_narrow),
                p.text_size, 4, 0, p.audio_ch_width, 200);
            if (ddd_mode) {
                // Keep the full name while the sample-rate readout is visible, and
                // reserve its width so legacy/Clockgen suffixes remain visible.
                char device_label[256];
                gui_ui_format_device_label(selected_device, ddd_compact_labels,
                    device_label, sizeof(device_label));
                p.dd_min_width = gui_ui_measure_button_width(app, device_label,
                    p.text_size, p.dropdown_padding, p.tiny ? 0 : 16,
                    p.dd_min_width, 1000);
                if (p.dd_max_width < p.dd_min_width) p.dd_max_width = p.dd_min_width;
            }
            const char *tier_label = gui_ui_toolbar_mode_label(mode_misrc,
                                                               cxadc_mode,
                                                               cxadc_clockgen_mode,
                                                               cxadc_misrc_clockgen_mode,
                                                               fx3_mode,
                                                               ddd_mode,
                                                               ddd_clockgen_mode,
                                                               tier);
            int mode_w = gui_ui_measure_button_width(app,
                                                     tier_label,
                                                     p.text_size,
                                                     6,
                                                     16,
                                                     p.mode_min_width,
                                                     p.mode_max_width);
            int dev_label_w = p.show_device_label
                ? gui_ui_measure_button_width(app, "Device:", p.text_size, 0, 0, 0, 200)
                : 0;
            int tier_required = gui_ui_toolbar_required_width(&p, dev_label_w,
                p.dd_min_width, mode_w, toolbar_compact);
            if (toolbar_compact && tier_required > toolbar_width) {
                int min_connect_width = gui_ui_toolbar_connection_width(app, true,
                    control_capturing, p.text_size);
                int spare_width = p.connect_width - min_connect_width;
                if (spare_width > 0) {
                    int shrink = gui_ui_clamp_int(tier_required - toolbar_width, 0, spare_width);
                    p.connect_width -= shrink;
                    tier_required -= shrink;
                }
            }
            if (tier == GUI_UI_TOOLBAR_TIER_TINY) {
                tiny_required_width = tier_required;
                prof = p;
            }
            if (tier_required <= toolbar_width) {
                prof = p;
                chosen_tier = tier;
                chosen_required_width = tier_required;
                break;
            }
        }
    }
    // Two-row fallback only when even the tiny profile does not fit (extreme
    // narrow widths); wrapping keeps every control visible instead of
    // clipping it off the window.
    bool toolbar_two_rows = chosen_tier < 0 &&
        gui_ui_toolbar_uses_two_rows(toolbar_width, tiny_required_width);
    if (chosen_tier < 0) {
        chosen_tier = GUI_UI_TOOLBAR_TIER_TINY;
        chosen_required_width = tiny_required_width;
    }

    bool toolbar_tiny = prof.tiny;
    bool toolbar_ultra_narrow = prof.ultra_narrow;
    bool toolbar_very_narrow = prof.very_narrow;
    int toolbar_padding_h = prof.padding_h;
    int toolbar_gap = prof.gap;
    int toolbar_text_size = prof.text_size;
    int toolbar_icon_button_size = prof.icon_button_size;
    int toolbar_version_icon_size = prof.version_icon_size;
    int toolbar_metadata_icon_size = prof.metadata_icon_size;
    int device_dropdown_min_width = prof.dd_min_width;
    int device_dropdown_max_width = prof.dd_max_width;
    int connect_button_width = prof.connect_width;
    int mode_toggle_min_width = prof.mode_min_width;
    int mode_toggle_max_width = prof.mode_max_width;
    int audio_mon_width = prof.audio_mon_width;
    int audio_ch_width = prof.audio_ch_width;
    int record_button_width = prof.record_width;
    int icon_button_size = prof.icon_button_size;
    int dropdown_padding = prof.dropdown_padding;
    // The tiny profile still shows all four meters, but compresses them enough
    // for the 640px minimum window at 200% scale (320 logical pixels).
    int audio_bars_panel_width = prof.bars_panel_width;
    int audio_meter_col_width = prof.meter_col_width;
    int audio_meter_width = prof.meter_width;
    int audio_meter_height = prof.meter_height;
    int audio_meter_gap = prof.meter_gap;
    bool show_audio_meter_labels = prof.show_audio_meter_labels;
    bool show_version_icon = prof.show_version_icon;
    bool show_metadata_icon = true;
    bool show_device_label = prof.show_device_label;

    // Hand the row's dead space to the device dropdown first: grow it from
    // its dd_min floor toward its natural (capped) width with whatever slack
    // the chosen tier left, then ellipsize the label to the final width. The
    // device name un-truncates dynamically as the window widens instead of
    // leaving a dead gap in the middle of the toolbar.
    int toolbar_slack = toolbar_width - chosen_required_width;
    if (toolbar_slack < 0) toolbar_slack = 0;
    int device_dropdown_extra_width = ddd_mode && toolbar_tiny ? 0 : 16;
    int device_dropdown_width = gui_ui_measure_button_width(app,
                                                            device_dropdown_buf,
                                                            toolbar_text_size,
                                                            dropdown_padding,
                                                            device_dropdown_extra_width,
                                                            device_dropdown_min_width,
                                                            device_dropdown_max_width);
    device_dropdown_width = device_dropdown_min_width +
        gui_ui_clamp_int(toolbar_slack, 0, device_dropdown_width - device_dropdown_min_width);
    gui_ui_format_device_label(selected_device,
        ddd_mode ? ddd_compact_labels : toolbar_very_narrow,
        device_dropdown_buf, sizeof(device_dropdown_buf));
    gui_ui_ellipsize_text(app,
                          device_dropdown_buf,
                          sizeof(device_dropdown_buf),
                          toolbar_text_size,
                          0,
                          device_dropdown_width - (dropdown_padding * 2) - device_dropdown_extra_width);
    const char *mode_label = gui_ui_toolbar_mode_label(mode_misrc,
                                                       cxadc_mode,
                                                       cxadc_clockgen_mode,
                                                       cxadc_misrc_clockgen_mode,
                                                       fx3_mode,
                                                       ddd_mode,
                                                       ddd_clockgen_mode,
                                                       chosen_tier);
    int mode_toggle_width = gui_ui_measure_button_width(app,
                                                        mode_label,
                                                        toolbar_text_size,
                                                        6,
                                                        16,
                                                        mode_toggle_min_width,
                                                        mode_toggle_max_width);
    if (toolbar_two_rows && toolbar_tiny) {
        int secondary_gaps = toolbar_gap * (mode_toggle_width > 0 ? 5 : 4);
        int secondary_width = toolbar_padding_h * 2 + secondary_gaps +
            connect_button_width + mode_toggle_width +
            audio_mon_width + audio_ch_width + audio_bars_panel_width;
        if (secondary_width > toolbar_width) {
            // At the minimum viewport, give the complete connection caption
            // room by removing spare padding and shortening all four meters.
            if (mode_toggle_width > 0) {
                mode_toggle_width = gui_ui_measure_button_width(app, mode_label,
                    toolbar_text_size, 4, 0, 32, mode_toggle_width);
            }
            audio_mon_width = gui_ui_measure_button_width(app, "Mon",
                toolbar_text_size, 4, 0, 32, audio_mon_width);
            audio_ch_width = gui_ui_measure_button_width(app,
                toolbar_audio_channel_label(app, cxadc_mode, true),
                toolbar_text_size, 4, 0, 32, audio_ch_width);
            int meter_space = toolbar_width - toolbar_padding_h * 2 - secondary_gaps -
                connect_button_width - mode_toggle_width - audio_mon_width - audio_ch_width;
            audio_meter_col_width = gui_ui_clamp_int(
                (meter_space - 8 - audio_meter_gap * 3) / 4, 8, audio_meter_col_width);
            audio_meter_width = audio_meter_col_width - 4;
            audio_bars_panel_width = 8 + audio_meter_gap * 3 + audio_meter_col_width * 4;
        }
    }
    s_toolbar_uses_two_rows = toolbar_two_rows;
    CLAY(CLAY_ID("Toolbar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(toolbar_two_rows ? 84 : 48) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { toolbar_padding_h, toolbar_padding_h, 8, 8 },
            .childGap = toolbar_two_rows ? 4 : 0
        },
        .backgroundColor = to_clay_color(COLOR_TOOLBAR_BG)
    }) {
        CLAY(CLAY_ID("ToolbarPrimaryRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = toolbar_gap
            }
        }) {
        // Keep device details reachable even when selector labels are compact.
        if (show_version_icon) {
            CLAY(CLAY_ID("VersionIconButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(toolbar_icon_button_size), CLAY_SIZING_FIXED(toolbar_icon_button_size) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY(CLAY_ID("VersionIcon"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(toolbar_version_icon_size), CLAY_SIZING_FIXED(toolbar_version_icon_size) } },
                    .custom = { .customData = &s_version_icon_element }
                }) {}
            }
        }
        if (show_metadata_icon) {
            CLAY(CLAY_ID("MetadataIconButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(toolbar_icon_button_size), CLAY_SIZING_FIXED(toolbar_icon_button_size) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY(CLAY_ID("MetadataIcon"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(toolbar_metadata_icon_size), CLAY_SIZING_FIXED(toolbar_metadata_icon_size) } },
                    .custom = { .customData = &s_metadata_icon_element }
                }) {}
            }
        }

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer1"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(show_metadata_icon ? 8 : (show_version_icon ? 4 : 0)), CLAY_SIZING_GROW(0) } }
        }) {}

        // Device label
        if (show_device_label) {
            CLAY_TEXT(CLAY_STRING("Device:"),
                CLAY_TEXT_CONFIG({ .fontSize = toolbar_text_size, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }

        // Device dropdown button
        bool device_dropdown_open = gui_dropdown_is_open(DROPDOWN_DEVICE, 0);
        Color dropdown_color = !playback_starting && device_dropdown_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        CLAY(CLAY_ID("DeviceDropdown"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(device_dropdown_width), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                .padding = { dropdown_padding, dropdown_padding, 0, 0 }
            },
            .backgroundColor = to_clay_color(dropdown_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(make_string(device_dropdown_buf),
                CLAY_TEXT_CONFIG({
                    .fontSize = toolbar_text_size,
                    .textColor = to_clay_color(playback_starting ? COLOR_TEXT_DIM : COLOR_TEXT),
                    .wrapMode = CLAY_TEXT_WRAP_NONE
                }));
        }

        if (!toolbar_two_rows) {
            render_toolbar_connection_group(control_capturing,
                                            playback_starting,
                                            toolbar_text_size,
                                            toolbar_gap,
                                            connect_button_width,
                                            mode_toggle_width,
                                            mode_bg,
                                            mode_fg,
                                            mode_label);
        }

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer2"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }
            }
        }) {}

        if (!toolbar_two_rows) {
            render_toolbar_audio_group(app,
                                       cxadc_mode,
                                       toolbar_ultra_narrow,
                                       toolbar_very_narrow,
                                       show_audio_meter_labels,
                                       toolbar_text_size,
                                       toolbar_gap,
                                       audio_mon_width,
                                       audio_ch_width,
                                       audio_bars_panel_width,
                                       audio_meter_col_width,
                                       audio_meter_width,
                                       audio_meter_height,
                                       audio_meter_gap);
        }
        bool playback_mode = app->settings.net_mode != GUI_NET_MODE_MONITOR_CLIENT &&
                             gui_ui_selected_device_is_playback(app);
        bool playback_running = playback_mode && gui_playback_is_running(app);
        playback_state_t playback_state = playback_running
            ? gui_playback_get_state(app)
            : PLAYBACK_STATE_STOPPED;
        bool playback_paused = (playback_state == PLAYBACK_STATE_PAUSED);

        // Record button (capture) / Play-Pause button (playback mode)
        bool monitor_client = app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT;
        int remote_state = monitor_client && gui_monitor_client_connected()
                          ? gui_monitor_client_peer_state() : 0;
        bool record_active = monitor_client ? remote_state == 2 : app->is_recording;
        bool can_record = monitor_client ? remote_state >= 1 : app->is_capturing;
        bool record_finalizing = !monitor_client && gui_record_is_finalizing();
        Color record_color = record_finalizing ? (Color){184, 118, 20, 255} : (record_active ? COLOR_CLIP_RED : COLOR_BUTTON);
        const char *record_label = record_finalizing ? "Finalize" : (record_active ? "Stop Rec" : "Record");
        // Flash the finalize icon red if a persistent output-file write error
        // is active (e.g. file locked by another app) so the user knows the
        // recording had write issues. Blink at ~1 Hz between the finalize
        // orange and clip red.
        if (record_finalizing && gui_record_has_write_error()) {
            bool blink_on = (fmod(GetTime(), 1.0) < 0.5);
            record_color = blink_on ? COLOR_CLIP_RED : (Color){184, 118, 20, 255};
        }
        if (playback_mode) {
            record_label = (!app->is_capturing || playback_paused) ? "Play" : "Pause";
            record_color = playback_paused ? COLOR_SYNC_GREEN : COLOR_BUTTON_ACTIVE;
        }
        const char *record_display_label = record_label;
        if (!playback_mode && toolbar_very_narrow) {
            if (record_finalizing) {
                record_display_label = "Fin";
            } else if (record_active) {
                record_display_label = "Stop";
            } else {
                record_display_label = "Rec";
            }
        }
        if (!can_record) record_color = (Color){ 50, 50, 55, 255 };
        CLAY(CLAY_ID("RecordButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(record_button_width), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(record_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            Color text_color = can_record ? COLOR_TEXT : COLOR_TEXT_DIM;
            CLAY_TEXT(make_string(record_display_label),
                CLAY_TEXT_CONFIG({ .fontSize = toolbar_text_size, .textColor = to_clay_color(text_color) }));
        }
        // Record-limit button (normal mode) / Loop button (playback mode)
        bool playback_loop_on = playback_mode && gui_playback_get_loop(app);
        s_record_limit_icon_element.type = playback_mode
            ? CUSTOM_LAYOUT_ELEMENT_TYPE_LOOP_ICON
            : CUSTOM_LAYOUT_ELEMENT_TYPE_CLOCK_ICON;
        int record_limit_icon_size = playback_mode ? 20 : 18;
        Color limit_button_color = COLOR_BUTTON;
        if (playback_mode) {
            if (!app->is_capturing) {
                limit_button_color = (Color){ 50, 50, 55, 255 };
            } else {
                limit_button_color = playback_loop_on ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
            }
        } else {
            limit_button_color = s_record_limit_window_open
                ? COLOR_BUTTON_ACTIVE
                : (s_record_limit_armed ? COLOR_SYNC_GREEN : COLOR_BUTTON);
        }
        CLAY(CLAY_ID("RecordLimitButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(icon_button_size), CLAY_SIZING_FIXED(icon_button_size) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(limit_button_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY(CLAY_ID("RecordLimitIcon"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(toolbar_tiny ? (record_limit_icon_size - 3) : record_limit_icon_size), CLAY_SIZING_FIXED(toolbar_tiny ? (record_limit_icon_size - 3) : record_limit_icon_size) } },
                .custom = { .customData = &s_record_limit_icon_element }
            }) {}
        }

        // Settings button
        CLAY(CLAY_ID("SettingsButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(icon_button_size), CLAY_SIZING_FIXED(icon_button_size) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(app->settings_panel_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            // Font-independent settings icon (rendered as a custom Clay element)
            CLAY(CLAY_ID("SettingsIcon"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(toolbar_tiny ? 14 : 18), CLAY_SIZING_FIXED(toolbar_tiny ? 14 : 18) } },
                .custom = { .customData = &s_settings_icon_element }
            }) {}
        }

        }
        if (toolbar_two_rows) {
            CLAY(CLAY_ID("ToolbarAudioRow"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                    .childGap = toolbar_gap
                }
            }) {
                render_toolbar_connection_group(control_capturing,
                                                playback_starting,
                                                toolbar_text_size,
                                                toolbar_gap,
                                                connect_button_width,
                                                mode_toggle_width,
                                                mode_bg,
                                                mode_fg,
                                                mode_label);

                CLAY(CLAY_ID("ToolbarSecondarySpacer"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }
                    }
                }) {}

                render_toolbar_audio_group(app,
                                           cxadc_mode,
                                           toolbar_ultra_narrow,
                                           toolbar_very_narrow,
                                           show_audio_meter_labels,
                                           toolbar_text_size,
                                           toolbar_gap,
                                           audio_mon_width,
                                           audio_ch_width,
                                           audio_bars_panel_width,
                                           audio_meter_col_width,
                                           audio_meter_width,
                                           audio_meter_height,
                                           audio_meter_gap);
            }
        }
    }
}

// Helper macro for stat row layout
#define STAT_ROW_LAYOUT { \
    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, \
    .layoutDirection = CLAY_LEFT_TO_RIGHT, \
    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, \
    .childGap = row_gap \
}

// Fixed width for labels to ensure alignment
#define LABEL_WIDTH 50
// Recording values keep a readable size; show fewer items in the narrow sidebar.
#define FONT_SIZE_RECORD_READOUT 16

typedef struct {
    uint32_t clip_pos, clip_neg;
    float peak_pos, peak_neg;
    uint64_t raw_bytes, compressed_bytes;
} gui_channel_stats_values_t;

typedef struct {
    float width;
    int label_width, view_width, padding, gap;
} gui_channel_stats_layout_t;

// Geometry depends on zoom, available space and static font metrics only.
// Never measure live peaks/counters here: even equal-length proportional
// digits would otherwise resize the sidebar and both plots every frame.
static gui_channel_stats_layout_t gui_ui_stats_layout(gui_app_t *app,
                                                       bool compact, int gap)
{
    gap = compact ? (gap > 0 ? gap : 1) : 4;
    int padding = compact ? gap : 6;
    int label_width = compact ? 0 : LABEL_WIDTH;
    const char *labels[] = { "Peak:", "Clip:", "Layout:", "Left:", "Right:", "View:", "DC:" };
    for (unsigned int i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        int width = gui_ui_measure_text_width(app, labels[i], FONT_SIZE_STATS, 0);
        if (width > label_width) label_width = width;
    }
    int view_width = 65;
    for (int view = 0; view < PANEL_VIEW_COUNT; view++) {
        if (!panel_view_type_available((panel_view_type_t)view)) continue;
        int width = gui_ui_measure_text_width(app, panel_view_type_name((panel_view_type_t)view),
                                              FONT_SIZE_DROPDOWN_OPT, 0) + 4;
        if (width > view_width) view_width = width;
    }

    // Two equal value slots hold the full peak range or a signed compact
    // counter (up to 999K/999M). Exact counts remain available on click.
    int value_width = gui_ui_measure_text_width(app, "+100%", FONT_SIZE_STATS, 0);
    int negative_width = gui_ui_measure_text_width(app, "-100%", FONT_SIZE_STATS, 0);
    if (negative_width > value_width) value_width = negative_width;
    const char *signs = "+-";
    const char *suffixes = "KMG";
    for (int sign = 0; sign < 2; sign++) {
        for (int suffix = 0; suffix < 3; suffix++) {
            for (char digit = '0'; digit <= '9'; digit++) {
                // Measure complete static samples so fractional glyph widths
                // round up only once. Raylib adds glyph advances without kerning;
                // repeated digits also bound every mixed three-digit counter.
                char text[] = {signs[sign], digit, digit, digit, suffixes[suffix], '\0'};
                int width = gui_ui_measure_text_width(app, text, FONT_SIZE_STATS, 0);
                if (width > value_width) value_width = width;
            }
        }
    }
    int numeric_width = value_width * 2 + gap;
    if (numeric_width > view_width) view_width = numeric_width;
    // Also keep both existing 22px CXADC DC buttons intact.
    if (44 + gap > view_width) view_width = 44 + gap;
    float width = label_width + gap + view_width + padding * 2;
    // Preserve room for the full RAW size readout in Space Mono, even at
    // UINT64_MAX, without letting live recording bytes change the width.
    char record_size_max[32];
    snprintf(record_size_max, sizeof(record_size_max), "RAW: %.2f GB",
             (double)UINT64_MAX / 1000000000.0);
    int record_width = gui_ui_measure_text_width(app, record_size_max, FONT_SIZE_STATS, 0);
    width = fmaxf(width, record_width + padding * 2);
    if (!compact) width = fmaxf(width, ceilf(185 * gui_ui_stats_width_scale(s_ui_scale_percent)));
    view_width = (int)width - label_width - gap - padding * 2;
    return (gui_channel_stats_layout_t){width, label_width, view_width, padding, gap};
}

static void gui_ui_format_clip_counter(gui_app_t *app, char *dst, size_t size,
                                        uint32_t value, char sign, int width)
{
    snprintf(dst, size, "%c%u", sign, value);
    if (gui_ui_measure_text_width(app, dst, FONT_SIZE_STATS, 0) > width) {
        char compact[16];
        format_status_counter(compact, sizeof(compact), value);
        snprintf(dst, size, "%c%s", sign, compact);
    }
}

// Keep menus inside the logical viewport, independently of the channel's
// scroll clip. Prefer below the button, then above; scroll only when neither
// side can hold all options. Live readouts never participate in this budget.
static Clay_BoundingBox gui_ui_channel_menu_bounds(Clay_BoundingBox button,
                                                   float width, float height,
                                                   int screen_width, int screen_height)
{
    float right = fmaxf(1, screen_width - 4);
    float bottom = fmaxf(1, screen_height - 4);
    float left = fminf(4, right - 1);
    float top = fminf(4, bottom - 1);
    float below = fmaxf(0, bottom - fmaxf(top, button.y + button.height));
    float above = fmaxf(0, fminf(bottom, button.y) - top);
    bool open_above = height > below && above > below;
    float available = open_above ? above : below;
    height = fminf(height, fmaxf(1, available));
    width = fminf(width, right - left);
    float x = fmaxf(left, fminf(button.x, right - width));
    float y = open_above ? button.y - height : button.y + button.height;
    y = fmaxf(top, fminf(y, bottom - height));
    return (Clay_BoundingBox){x, y, width, height};
}

static Clay_ElementDeclaration gui_ui_channel_menu(Clay_ElementId button_id,
                                                    int width, int option_count)
{
    // A menu opens after the button's layout and click have completed, so its
    // last laid-out bounds are available. Root positioning also clamps the
    // menu during a viewport resize instead of inheriting a stale offset.
    Clay_BoundingBox bounds = gui_ui_channel_menu_bounds(
        Clay_GetElementData(button_id).boundingBox, width, option_count * 20,
        gui_ui_get_layout_width(), gui_ui_get_layout_height());
    return (Clay_ElementDeclaration){
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(bounds.width), CLAY_SIZING_FIXED(bounds.height) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP },
            .offset = {bounds.x, bounds.y},
            .clipTo = CLAY_CLIP_TO_NONE
        },
        .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(3)
    };
}

// Render per-channel stats panel (trigger controls moved to waveform panel overlay)
static void render_channel_stats(gui_app_t *app, int channel,
                                   const gui_channel_stats_values_t *values,
                                   const gui_channel_stats_layout_t *layout,
                                   const gui_monitor_record_stats_t *m_record) {
    // Only the sidebar width grows more slowly than the global zoom.
    // Text, controls and their hit areas retain the normal UI scale.
    float stats_width = layout->width;
    int label_width = layout->label_width;
    int view_width = layout->view_width;
    int padding = layout->padding;
    int row_gap = layout->gap;
    int value_width = (view_width - row_gap) / 2;
    int screen_w = gui_ui_get_layout_width();
    int screen_h = gui_ui_get_layout_height();
    bool quarter_scale_layout = (screen_w <= 1000 && screen_h <= 700);
    // Get per-channel stats
    uint32_t clip_pos, clip_neg;
    float peak_pos, peak_neg;
    char *buf_peak_pos, *buf_peak_neg, *buf_clip_pos, *buf_clip_neg;
    char *buf_rec_raw, *buf_rec_flac, *buf_rec_ratio, *buf_rec_duration;
    Color channel_value_color = (channel == 0) ? COLOR_CHANNEL_A : COLOR_CHANNEL_B;

    if (channel == 0) {
        buf_peak_pos = stat_a_peak_pos;
        buf_peak_neg = stat_a_peak_neg;
        buf_clip_pos = stat_a_clip_pos;
        buf_clip_neg = stat_a_clip_neg;
        buf_rec_raw = stat_rec_raw[0];
        buf_rec_flac = stat_rec_flac[0];
        buf_rec_ratio = stat_rec_ratio[0];
        buf_rec_duration = stat_rec_duration[0];
    } else {
        buf_peak_pos = stat_b_peak_pos;
        buf_peak_neg = stat_b_peak_neg;
        buf_clip_pos = stat_b_clip_pos;
        buf_clip_neg = stat_b_clip_neg;
        buf_rec_raw = stat_rec_raw[1];
        buf_rec_flac = stat_rec_flac[1];
        buf_rec_ratio = stat_rec_ratio[1];
        buf_rec_duration = stat_rec_duration[1];
    }

    clip_pos = values->clip_pos;
    clip_neg = values->clip_neg;
    peak_pos = values->peak_pos;
    peak_neg = values->peak_neg;
    // Format stats (peak/clip/errors)
    snprintf(buf_peak_pos, 16, "+%.0f%%", peak_pos * 100.0f);
    snprintf(buf_peak_neg, 16, "-%.0f%%", peak_neg * 100.0f);
    gui_ui_format_clip_counter(app, buf_clip_pos, 16, clip_pos, '+', value_width);
    gui_ui_format_clip_counter(app, buf_clip_neg, 16, clip_neg, '-', value_width);

    CLAY(CLAY_IDI("StatsPanel", channel), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(stats_width), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { padding, padding, 4, 4 },
            .childGap = 2
        },
        // Let the channel row keep its height when recording details grow.
        // Clay handles wheel/drag scrolling and clips off-screen hit targets.
        .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        .backgroundColor = to_clay_color((Color){ 35, 35, 42, 255 })
    }) {
        // Channel label
        // CLAY_TEXT(channel == 0 ? CLAY_STRING("Channel A") : CLAY_STRING("Channel B"),
        //     CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS_LABEL, .textColor = to_clay_color(channel_color) }));

        // Samples row removed (shown in status bar)

        // Peak row (shows both + and -)
        CLAY(CLAY_IDI("StatPeak", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblPeak", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(label_width), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Peak:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_IDI("PeakPositive", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(value_width), CLAY_SIZING_FIXED(18) } } }) {
                CLAY_TEXT(make_string(buf_peak_pos),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(peak_pos > 0.95f ? COLOR_CLIP_RED : COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
            CLAY(CLAY_IDI("PeakNegative", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(value_width), CLAY_SIZING_FIXED(18) } } }) {
                CLAY_TEXT(make_string(buf_peak_neg),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(peak_neg > 0.95f ? COLOR_CLIP_RED : COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
        }

        // Clip row (shows both + and -)
        CLAY(CLAY_IDI("StatClip", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblClip", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(label_width), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Clip:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_IDI("ClipPositive", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(value_width), CLAY_SIZING_FIXED(18) } } }) {
                CLAY_TEXT(make_string(buf_clip_pos),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(clip_pos > 0 ? COLOR_CLIP_RED : COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
            CLAY(CLAY_IDI("ClipNegative", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(value_width), CLAY_SIZING_FIXED(18) } } }) {
                CLAY_TEXT(make_string(buf_clip_neg),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(clip_neg > 0 ? COLOR_CLIP_RED : COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
        }

        // Reset button row (kept separate so it scales better)
        CLAY(CLAY_IDI("ResetClipRow", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblClipReset", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(label_width), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING(""),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_IDI("ResetClipBtn", channel), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(60), CLAY_SIZING_FIXED(18) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                CLAY_TEXT(CLAY_STRING("RST"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }
        if (gui_ui_selected_device_is_cxadc(app, NULL)) {
            int card_idx = -1;
            bool dc_card_available = gui_ui_map_cxadc_channel_to_card(app, channel, &card_idx);
            Color dc_btn_bg = dc_card_available ? COLOR_BUTTON : ui_disabled_color(COLOR_BUTTON);
            Color dc_btn_fg = dc_card_available ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);

            CLAY(CLAY_IDI("DcOffsetRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                CLAY(CLAY_IDI("LblDcOffset", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(label_width), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("DC:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
                CLAY(CLAY_IDI("DcOffsetDown", channel), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(18) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                    .backgroundColor = to_clay_color(dc_btn_bg),
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    CLAY_TEXT(CLAY_STRING("\\/"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(dc_btn_fg) }));
                }
                CLAY(CLAY_IDI("DcOffsetUp", channel), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(18) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                    .backgroundColor = to_clay_color(dc_btn_bg),
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    CLAY_TEXT(CLAY_STRING("/\\"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(dc_btn_fg) }));
                }
            }
        }


        {
            uint64_t raw_bytes = values->raw_bytes;
            uint64_t comp_bytes = values->compressed_bytes;
            bool remote_recording=m_record && gui_monitor_client_peer_state()==2;
            bool remote_active=m_record && gui_monitor_client_peer_state()>0;
            bool remote_ch_enabled=!m_record || (channel==0?m_record->capture_a:m_record->capture_b);
            bool show_record_stats = remote_active
                ? (remote_ch_enabled && (remote_recording || raw_bytes>0 || comp_bytes>0 || m_record->elapsed_ms>0))
                : (app->is_capturing && (app->is_recording || raw_bytes > 0 || comp_bytes > 0 || app->last_recording_duration_s > 0.0));
            if (!show_record_stats) s_record_details_view[channel].visible = false;
            if (show_record_stats) {
                double shown_duration = m_record ? (double)m_record->elapsed_ms/1000.0
                    : (app->is_recording ? (GetTime() - app->recording_start_time) : app->last_recording_duration_s);
                int d_hours = (int)(shown_duration / 3600.0);
                int d_mins = ((int)(shown_duration / 60.0)) % 60;
                int d_secs = ((int)(shown_duration)) % 60;

                snprintf(buf_rec_duration, 24, "Dur: %02d:%02d:%02d", d_hours, d_mins, d_secs);
                // Decimal (SI) MB/GB so the readout matches `ls`/file-manager
                // and is 1:1 with the on-disk file byte count (stat-driven
                // above), irrespective of capture mode.
                if (raw_bytes >= 1000000000ULL) {
                    double raw_gb = (double)raw_bytes / 1000000000.0;
                    snprintf(buf_rec_raw, 32, "RAW: %.2f GB", raw_gb);
                } else {
                    double raw_mb = (double)raw_bytes / 1000000.0;
                    snprintf(buf_rec_raw, 32, "RAW: %.2f MB", raw_mb);
                }
                if (comp_bytes > 0 || (m_record ? m_record->use_flac : app->settings.use_flac)) {
                    double ratio = (comp_bytes > 0) ? ((double)raw_bytes / (double)comp_bytes) : 0.0;
                    if (comp_bytes >= 1000000000ULL) {
                        double comp_gb = (double)comp_bytes / 1000000000.0;
                        snprintf(buf_rec_flac, 32, "FLAC: %.2f GB", comp_gb);
                    } else {
                        double comp_mb = (double)comp_bytes / 1000000.0;
                        snprintf(buf_rec_flac, 32, "FLAC: %.2f MB", comp_mb);
                    }
                    snprintf(buf_rec_ratio, 24, "Ratio: %.1fx", ratio);
                } else {
                    buf_rec_flac[0] = '\0';
                    buf_rec_ratio[0] = '\0';
                }
                // Detailed stats live behind a single button; do not crowd the
                // narrow sidebar with waits, drops and per-buffer percentages.
                gui_record_details_view_t *detail = &s_record_details_view[channel];
                memset(detail, 0, sizeof(*detail));
                detail->visible = true;
                detail->recording = remote_active ? remote_recording : app->is_recording;
                detail->local_save = m_record && m_record->local_save;
                if (m_record) {
                    detail->buffer_known = true;
                    detail->buffer_fill_pct = m_record->rec_fill_pct[channel];
                    detail->write_waits = m_record->rec_waits[channel];
                    detail->dropped_blocks = m_record->rec_drops[channel];
                    if (channel == 0 && m_record->local_save) {
                        detail->send_queue_bytes = m_record->send_queue_bytes;
                        detail->send_queue_capacity = m_record->send_queue_capacity;
                    }
                } else {
                    buffer_id_t id = channel == 0 ? BUF_RECORD_A : BUF_RECORD_B;
                    if (app->buffers.manager_initialized && app->buffers.initialized[id]) {
                        buffer_stats_t bstats;
                        bufmgr_get_stats(&app->buffers, id, &bstats);
                        float fill = bufmgr_fill_percent(&app->buffers, id);
                        if (fill < 0.0f) fill = 0.0f;
                        if (fill > 1.0f) fill = 1.0f;
                        detail->buffer_known = true;
                        detail->buffer_fill_pct = (unsigned)roundf(fill * 100.0f);
                        detail->write_waits = atomic_load(&bstats.write_waits);
                        detail->dropped_blocks = atomic_load(&bstats.write_drops);
                    }
                }
                const char *buffer_health = "Buffer info";
                Color buffer_health_color = COLOR_TEXT_DIM;
                if (detail->buffer_known) {
                    if (detail->dropped_blocks > 0) {
                        buffer_health = "Data lost!";
                        buffer_health_color = COLOR_CLIP_RED;
                    } else if (detail->buffer_fill_pct >= 75 || detail->write_waits > 0) {
                        buffer_health = "Buffer high";
                        buffer_health_color = COLOR_METER_YELLOW;
                    } else {
                        buffer_health = "Buffer OK";
                        buffer_health_color = COLOR_SYNC_GREEN;
                    }
                }

                // Keep recording state and duration together in the narrow
                // sidebar. Use a short inactive label so the 18px clock fits.
                CLAY(CLAY_IDI("RecordingStateRow", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 4,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
                    }
                }) {
                    CLAY(CLAY_IDI("RecordingStateDot", channel), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(7), CLAY_SIZING_FIXED(7) } },
                        .backgroundColor = to_clay_color(detail->recording ? COLOR_SYNC_GREEN : COLOR_TEXT_DIM),
                        .cornerRadius = CLAY_CORNER_RADIUS(8)
                    }) {}
                    CLAY_TEXT(detail->recording ? CLAY_STRING("Recording") : CLAY_STRING("Last rec"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 0,
                            .textColor = to_clay_color(COLOR_TEXT_DIM),
                            .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    CLAY_TEXT(make_string(buf_rec_duration + 5),
                        CLAY_TEXT_CONFIG({ .fontSize = 18, .fontId = 0,
                            .wrapMode = CLAY_TEXT_WRAP_NONE,
                            .textColor = to_clay_color(COLOR_TEXT) }));
                }
                // Full-width rows keep values readable inside the narrow
                // channel sidebar. Two equal-sized columns forced the labels
                // and compression readout to use tiny text.
                if (!quarter_scale_layout) {
                    CLAY(CLAY_IDI("RecRawRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                        CLAY(CLAY_IDI("RecRawLabel", channel), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(52), CLAY_SIZING_FIT(0) } }
                        }) {
                            CLAY_TEXT(CLAY_STRING("RAW:"), CLAY_TEXT_CONFIG({
                                .fontSize = FONT_SIZE_STATS, .fontId = 0,
                                .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                        }
                        CLAY_TEXT(make_string(buf_rec_raw + 5), CLAY_TEXT_CONFIG({
                            .fontSize = 18, .fontId = 0, .wrapMode = CLAY_TEXT_WRAP_NONE,
                            .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                    if (buf_rec_flac[0]) {
                        CLAY(CLAY_IDI("RecFlacRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                            CLAY(CLAY_IDI("RecFlacLabel", channel), {
                                .layout = { .sizing = { CLAY_SIZING_FIXED(52), CLAY_SIZING_FIT(0) } }
                            }) {
                                CLAY_TEXT(CLAY_STRING("FLAC:"), CLAY_TEXT_CONFIG({
                                    .fontSize = FONT_SIZE_STATS, .fontId = 0,
                                    .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                            }
                            CLAY_TEXT(make_string(buf_rec_flac + 6), CLAY_TEXT_CONFIG({
                                .fontSize = 18, .fontId = 0, .wrapMode = CLAY_TEXT_WRAP_NONE,
                                .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }
                }
                if (buf_rec_ratio[0]) {
                    CLAY(CLAY_IDI("RecRatioRow", channel), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                            .childGap = 6
                        }
                    }) {
                        CLAY_TEXT(CLAY_STRING("Compression"), CLAY_TEXT_CONFIG({
                            .fontSize = FONT_SIZE_STATS, .fontId = 0,
                            .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                        CLAY_TEXT(make_string(buf_rec_ratio + 7), CLAY_TEXT_CONFIG({
                            .fontSize = 18, .fontId = 0, .wrapMode = CLAY_TEXT_WRAP_NONE,
                            .textColor = to_clay_color(COLOR_SYNC_GREEN) }));
                    }
                }
                CLAY(CLAY_IDI("RecordingDetailsButton", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                        .childGap = 4,
                        .padding = { 4, 4, 0, 0 }
                    },
                    .backgroundColor = to_clay_color(COLOR_BUTTON),
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    // Use the same size/font as the Split and Waveform controls.
                    // Keep warning labels short so both captions fit this narrow button.
                    CLAY_TEXT(make_string(buffer_health), CLAY_TEXT_CONFIG({
                        .fontSize = FONT_SIZE_DROPDOWN_OPT, .fontId = 0,
                        .textColor = to_clay_color(buffer_health_color),
                        .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    CLAY_TEXT(CLAY_STRING("Details >"), CLAY_TEXT_CONFIG({
                        .fontSize = FONT_SIZE_DROPDOWN_OPT, .fontId = 0,
                        .textColor = to_clay_color(COLOR_TEXT),
                        .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
            }
        }

        // Separator line before panel configuration
        // Note: Trigger controls have moved to the waveform panel overlay (per-panel)
        CLAY(CLAY_IDI("StatSep", channel), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
            .backgroundColor = to_clay_color(COLOR_TEXT_DIM)
        }) {}

        // Get panel config for this channel
        bool panel_split = (channel == 0) ? app->panel_config_a.split : app->panel_config_b.split;
        int left_view = (channel == 0) ? app->panel_config_a.left_view : app->panel_config_b.left_view;
        int right_view = (channel == 0) ? app->panel_config_a.right_view : app->panel_config_b.right_view;
        int view_count = 0;
        for (int vt = 0; vt < PANEL_VIEW_COUNT; vt++) {
            if (panel_view_type_available((panel_view_type_t)vt)) view_count++;
        }

        // Layout row (Single/Split toggle)
        CLAY(CLAY_IDI("LayoutRow", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblLayout", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(label_width), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Layout:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }

            const char *layout_name = panel_split ? "Split" : "Single";
            bool layout_dropdown_open = gui_dropdown_is_open(DROPDOWN_LAYOUT, channel);
            CLAY(CLAY_IDI("LayoutBtn", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(view_width), CLAY_SIZING_FIXED(18) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(layout_dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                CLAY_TEXT(make_string(layout_name),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Layout dropdown options
        if (gui_dropdown_is_open(DROPDOWN_LAYOUT, channel)) {
            CLAY(CLAY_IDI("LayoutOpts", channel),
                 gui_ui_channel_menu(CLAY_IDI("LayoutBtn", channel), view_width, 2)) {
                bool single_hover = Clay_PointerOver(CLAY_IDI("LayoutOptSingle", channel));
                Color single_color = gui_dropdown_option_color(!panel_split, single_hover);
                CLAY(CLAY_IDI("LayoutOptSingle", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(single_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("Single"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                }

                bool split_hover = Clay_PointerOver(CLAY_IDI("LayoutOptSplit", channel));
                Color split_color = gui_dropdown_option_color(panel_split, split_hover);
                CLAY(CLAY_IDI("LayoutOptSplit", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(split_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("Split"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }
        }

        // Left view row (always shown)
        CLAY(CLAY_IDI("LeftViewRow", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblLeft", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(label_width), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(panel_split ? CLAY_STRING("Left:") : CLAY_STRING("View:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }

            const char *left_name = panel_view_type_name((panel_view_type_t)left_view);
            bool left_dropdown_open = gui_dropdown_is_open(DROPDOWN_LEFT_VIEW, channel);
            CLAY(CLAY_IDI("LeftViewBtn", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(view_width), CLAY_SIZING_FIXED(18) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(left_dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                CLAY_TEXT(make_string(left_name),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Left view dropdown options
        if (gui_dropdown_is_open(DROPDOWN_LEFT_VIEW, channel)) {
            CLAY(CLAY_IDI("LeftViewOpts", channel),
                 gui_ui_channel_menu(CLAY_IDI("LeftViewBtn", channel), view_width, view_count)) {
                for (int vt = 0; vt < PANEL_VIEW_COUNT; vt++) {
                    if (!panel_view_type_available((panel_view_type_t)vt)) continue;
                    // Use channel * 10 + vt to create unique IDs per channel
                    bool opt_hover = Clay_PointerOver(CLAY_IDI("LeftViewOpt", channel * 10 + vt));
                    Color opt_color = gui_dropdown_option_color(left_view == vt, opt_hover);
                    CLAY(CLAY_IDI("LeftViewOpt", channel * 10 + vt), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                        },
                        .backgroundColor = to_clay_color(opt_color)
                    }) {
                        CLAY_TEXT(make_string(panel_view_type_name((panel_view_type_t)vt)),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                }
            }
        }

        // Right view row (only shown when split)
        if (panel_split) {
            CLAY(CLAY_IDI("RightViewRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                CLAY(CLAY_IDI("LblRight", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(label_width), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Right:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }

                const char *right_name = panel_view_type_name((panel_view_type_t)right_view);
                bool right_dropdown_open = gui_dropdown_is_open(DROPDOWN_RIGHT_VIEW, channel);
                CLAY(CLAY_IDI("RightViewBtn", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(view_width), CLAY_SIZING_FIXED(18) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(right_dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON),
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    CLAY_TEXT(make_string(right_name),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }

            // Right view dropdown options
            if (gui_dropdown_is_open(DROPDOWN_RIGHT_VIEW, channel)) {
                CLAY(CLAY_IDI("RightViewOpts", channel),
                     gui_ui_channel_menu(CLAY_IDI("RightViewBtn", channel), view_width, view_count)) {
                    for (int vt = 0; vt < PANEL_VIEW_COUNT; vt++) {
                        if (!panel_view_type_available((panel_view_type_t)vt)) continue;
                        // Use channel * 10 + vt to create unique IDs per channel
                        bool opt_hover = Clay_PointerOver(CLAY_IDI("RightViewOpt", channel * 10 + vt));
                        Color opt_color = gui_dropdown_option_color(right_view == vt, opt_hover);
                        CLAY(CLAY_IDI("RightViewOpt", channel * 10 + vt), {
                            .layout = {
                                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                            },
                            .backgroundColor = to_clay_color(opt_color)
                        }) {
                            CLAY_TEXT(make_string(panel_view_type_name((panel_view_type_t)vt)),
                                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }
                }
            }
        }

    }
}

typedef struct {
    int left_padding, right_padding, label_width, track_width;
    int readout_width;      // "8-bit 20.0 MSPS" slot; 0 = no horizontal room
    int fileinfo_width;     // "RAW x FLAC y" slot; 0 = hidden
    int name_width;         // filename slot; 0 = hidden
    bool readout_compact;   // Compact text tier chosen for the readout
    bool fileinfo_compact;  // Compact text tier chosen for the fileinfo
    bool stacked;
} gui_playback_timeline_layout_t;

static gui_playback_timeline_layout_t gui_ui_playback_timeline_layout(gui_app_t *app,
                                                                      int screen_width,
                                                                      gui_ui_channel_spacing_t spacing)
{
    int left_padding = screen_width < 900 ? 60 : 74;
    if (spacing.vu_column_width < GUI_UI_VU_BAR_WIDTH * 2.0f)
        left_padding = (int)spacing.vu_column_width;
    int right_padding = screen_width < 900 ? 0 : 189;
    // Reserve a complete ordinary timecode with static font metrics. Long
    // hour counts may abbreviate inside this slot but never move the track.
    int label_width = gui_ui_measure_text_width(app, "88:88:88/~88:88:88", FONT_SIZE_STATUS, 1);
    int label_min = screen_width < 900 ? 120 : 150;
    if (label_width < label_min) label_width = label_min;
    int available = screen_width - spacing.horizontal_gap * 4 - left_padding - right_padding;
    if (available < 1) available = 1;
    int track_width = screen_width < 900 ? 180 : (screen_width < 1150 ? 240 : 300);
    bool stacked = available - label_width - 8 < 64;
    // Right-side file info slots (see the HARD DEV NOTE above the helpers):
    // fixed representative widths per tier so value changes never reflow the
    // row. The FILENAME slot now budgets from the actual loaded filenames
    // (full names, not a fixed sample width - see gui_ui_playback_file_basename);
    // tiers degrade full -> compact-fileinfo -> name+readout -> readout only.
    // Stacked rows fit the info line separately below the track.
    int readout_full = gui_ui_measure_text_width(app, "16-bit 999.9 MSPS", FONT_SIZE_STATUS, 1);
    int readout_compact_w = gui_ui_measure_text_width(app, "16-bit 999M", FONT_SIZE_STATUS, 1);
    int fileinfo_full = gui_ui_measure_text_width(app, "RAW 9999.9GiB FLAC 9999.9GiB 99.9x", FONT_SIZE_STATUS, 1);
    int fileinfo_compact_w = gui_ui_measure_text_width(app, "9999.9G/999.9G", FONT_SIZE_STATUS, 1);
    // Full-name budget: the widest loaded basename this session (sticky, so
    // loading a longer name resizes the slot and it never shrinks mid-file).
    // Capped at 420 px (the discovery-row cap) so a pathological basename
    // cannot starve the readouts; only beyond the cap does the name ellipsize.
    #define PLAYBACK_NAME_SLOT_MAX_W 420
    static int s_playback_name_slot_w = 0;
    if (app) {
        static char s_name_probe[128];
        int session_w = 0;
        for (int ch = 0; ch < 2; ch++) {
            s_name_probe[0] = '\0';
            gui_ui_playback_file_basename(app, ch, s_name_probe, sizeof(s_name_probe));
            if (s_name_probe[0]) {
                int w = gui_ui_measure_text_width(app, s_name_probe, FONT_SIZE_STATUS, 1);
                if (w > session_w) session_w = w;
            }
        }
        if (session_w > s_playback_name_slot_w) s_playback_name_slot_w = session_w;
        if (s_playback_name_slot_w > PLAYBACK_NAME_SLOT_MAX_W) s_playback_name_slot_w = PLAYBACK_NAME_SLOT_MAX_W;
    }
    int name_slot = s_playback_name_slot_w > 0 ? s_playback_name_slot_w
        : gui_ui_measure_text_width(app, "Tape_08_2026.10.11_01.49.01.flac", FONT_SIZE_STATUS, 1);
    int readout_width = 0;
    int fileinfo_width = 0;
    int name_width = 0;
    bool readout_compact = false;
    bool fileinfo_compact = false;
    {
        int room = stacked ? available : available - label_width - 8 - 64 - 8;
        int full_all = name_slot + 8 + readout_full + 8 + fileinfo_full;
        int mid_all = name_slot + 8 + readout_full + 8 + fileinfo_compact_w;
        int name_readout = name_slot + 8 + readout_full;
        int name_compact_readout = name_slot + 8 + readout_compact_w;
        if (room >= full_all) {
            name_width = name_slot;
            readout_width = readout_full;
            fileinfo_width = fileinfo_full;
        } else if (room >= mid_all) {
            name_width = name_slot;
            readout_width = readout_full;
            fileinfo_width = fileinfo_compact_w;
            fileinfo_compact = true;
        } else if (room >= name_readout) {
            name_width = name_slot;
            readout_width = readout_full;
        } else if (room >= name_compact_readout) {
            name_width = name_slot;
            readout_width = readout_compact_w;
            readout_compact = true;
        } else if (room >= readout_compact_w) {
            readout_width = readout_compact_w;
            readout_compact = true;
        }
    }
    int info_width = 0;
    if (name_width > 0) info_width += name_width;
    if (readout_width > 0) info_width += (info_width > 0 ? 8 : 0) + readout_width;
    if (fileinfo_width > 0) info_width += (info_width > 0 ? 8 : 0) + fileinfo_width;
    int track_available = stacked
        ? available
        : available - label_width - 8 - ((info_width > 0) ? info_width + 8 : 0);
    if (track_width > track_available) track_width = track_available;
    if (label_width > available) label_width = available;
    return (gui_playback_timeline_layout_t){left_padding, right_padding,
                                           label_width, track_width,
                                           readout_width, fileinfo_width, name_width,
                                           readout_compact, fileinfo_compact, stacked};
}

// Discovery and verification report file progress independently from playback.
static void gui_ui_format_playback_index_progress(char *text, size_t size,
                                                  const playback_index_progress_t *progress,
                                                  bool compact, bool verifying) {
    if (!text || size == 0) return;
    text[0] = '\0';
    if (!progress || !progress->active) return;
    if (progress->queued) {
        snprintf(text, size, "%s", verifying ? "Verification queued" : "Length scan queued");
        return;
    }
    if (progress->total_bytes == 0) {
        snprintf(text, size, "%s", verifying ? "Starting verification..." : "Starting length scan...");
        return;
    }
    uint64_t scanned = progress->bytes_scanned;
    if (scanned > progress->total_bytes) scanned = progress->total_bytes;
    unsigned percent = (unsigned)((long double)scanned * 100.0L / progress->total_bytes);
    const char *state = verifying
        ? (progress->waiting ? "Verification waiting" : (compact ? "Verifying" : "Verifying length"))
        : (progress->waiting ? "Scan waiting" : "Scanning length");
    if (compact) {
        snprintf(text, size, "%s: %u%%", state, percent);
    } else {
        snprintf(text, size, "%s: %u%% (%.1f/%.1f GB)", state, percent,
                 scanned / 1000000000.0, progress->total_bytes / 1000000000.0);
    }
}

// Render one playback timeline row (used for Channel A and Channel B in playback mode).
// name_text / readout_text / fileinfo_text form the right-side file info line
// (basename, "8-bit 20.0 MSPS", "RAW x FLAC y"); pass NULL / empty strings to
// skip slots the layout hid. These buffers must live through frame rendering.
// See the HARD DEV NOTE above the helpers.
static void render_playback_timeline_row(gui_app_t *app, int channel_index, char *timeline_text,
                                         int fill_w, bool loaded, bool seekable, bool indexing,
                                         bool provisional, bool verification_failed,
                                         gui_ui_channel_spacing_t spacing,
                                         const gui_playback_timeline_layout_t *layout,
                                         char *name_text, char *readout_text,
                                         char *fileinfo_text)
{
    Color timeline_text_color = loaded ? COLOR_TEXT : COLOR_TEXT_DIM;
    Color timeline_track_color = seekable ? (Color){45, 45, 52, 255} : (Color){33, 33, 38, 255};
    Color timeline_fill_color = seekable ? (provisional ? COLOR_METER_YELLOW : COLOR_SYNC_GREEN) : COLOR_TEXT_DIM;
    Color readout_color = loaded ? COLOR_TEXT_DIM : ui_disabled_color(COLOR_TEXT_DIM);
    bool show_length_status = loaded && (provisional || verification_failed);
    char *index_text = playback_index_display[channel_index == 1 ? 1 : 0];
    if (show_length_status) {
        if (verification_failed) {
            snprintf(index_text, sizeof(playback_index_display[0]), "Length unavailable");
        } else {
            playback_index_progress_t progress = {0};
            gui_playback_get_index_progress(app, channel_index, &progress);
            gui_ui_format_playback_index_progress(index_text, sizeof(playback_index_display[0]),
                                                  &progress, layout->track_width < 220, true);
            if (!index_text[0]) snprintf(index_text, sizeof(playback_index_display[0]), "Length unconfirmed");
        }
        gui_ui_ellipsize_text(app, index_text, sizeof(playback_index_display[0]),
                              9, 1, layout->track_width);
    }
    gui_ui_ellipsize_text(app, timeline_text, sizeof(playback_timeline_display_a),
                          FONT_SIZE_STATUS, 1, layout->label_width);
    CLAY(CLAY_IDI("PlaybackTimelineRow", channel_index), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((layout->stacked ? 56 : 24) + (show_length_status ? 14 : 0)) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = spacing.horizontal_gap
        }
        }) {
        CLAY(CLAY_IDI("PlaybackTimelineLeftPad", channel_index), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(layout->left_padding), CLAY_SIZING_GROW(0) } }
        }) {}

        CLAY(CLAY_IDI("PlaybackTimeline", channel_index), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((layout->stacked ? 54 : 22) + (show_length_status ? 14 : 0)) },
                .layoutDirection = layout->stacked ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = layout->stacked ? 4 : 8
            }
        }) {
            CLAY(CLAY_IDI("PlaybackTimelineLabel", channel_index), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(layout->label_width), CLAY_SIZING_FIT(0) } }
            }) {
                CLAY_TEXT(make_string(timeline_text),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(timeline_text_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }

            CLAY(CLAY_IDI("PlaybackTimelineTrackColumn", channel_index), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(layout->track_width), CLAY_SIZING_FIXED(show_length_status ? 24 : 10) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .childGap = show_length_status ? 4 : 0
                }
            }) {
                if (show_length_status) {
                    CLAY_TEXT(make_string(index_text),
                        CLAY_TEXT_CONFIG({ .fontSize = 9, .fontId = 1,
                            .textColor = to_clay_color(verification_failed ? COLOR_SYNC_RED : COLOR_METER_YELLOW),
                            .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
                CLAY(CLAY_IDI("PlaybackTimelineTrack", channel_index), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(layout->track_width), CLAY_SIZING_FIXED(10) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childAlignment = { .x = seekable ? CLAY_ALIGN_X_LEFT : CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(timeline_track_color),
                    .cornerRadius = CLAY_CORNER_RADIUS(5)
                }) {
                    if (fill_w > 0) {
                        CLAY(CLAY_IDI("PlaybackTimelineFill", channel_index), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(fill_w), CLAY_SIZING_GROW(0) } },
                            .backgroundColor = to_clay_color(timeline_fill_color),
                            .cornerRadius = CLAY_CORNER_RADIUS(5)
                        }) {}
                    } else if (loaded && !seekable && indexing) {
                        playback_index_progress_t progress = {0};
                        gui_playback_get_index_progress(app, channel_index, &progress);
                        gui_ui_format_playback_index_progress(index_text, sizeof(playback_index_display[0]),
                                                              &progress, layout->track_width < 220, false);
                        gui_ui_ellipsize_text(app, index_text, sizeof(playback_index_display[0]),
                                              9, 1, layout->track_width > 8 ? layout->track_width - 8 : 1);
                        CLAY_TEXT(make_string(index_text),
                            CLAY_TEXT_CONFIG({ .fontSize = 9, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }
            }

            // Right-side file info line: [basename][bit-depth + rate][RAW/FLAC]
            // — flows after the scrub track horizontally / its own line stacked.
            if (layout->name_width > 0 || layout->readout_width > 0 || layout->fileinfo_width > 0) {
                CLAY(CLAY_IDI("PlaybackTimelineInfo", channel_index), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                        .childGap = 8
                    }
                }) {
                    if (layout->name_width > 0 && name_text && name_text[0]) {
                        gui_ui_ellipsize_text(app, name_text, sizeof(playback_name_display_a),
                                              FONT_SIZE_STATUS, 1, layout->name_width);
                        CLAY(CLAY_IDI("PlaybackTimelineName", channel_index), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIXED(layout->name_width), CLAY_SIZING_FIT(0) },
                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
                            }
                        }) {
                            CLAY_TEXT(make_string(name_text),
                                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(timeline_text_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }

                    if (layout->readout_width > 0 && readout_text && readout_text[0]) {
                        gui_ui_ellipsize_text(app, readout_text, sizeof(playback_readout_display_a),
                                              FONT_SIZE_STATUS, 1, layout->readout_width);
                        CLAY(CLAY_IDI("PlaybackTimelineReadout", channel_index), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIXED(layout->readout_width), CLAY_SIZING_FIT(0) },
                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
                            }
                        }) {
                            CLAY_TEXT(make_string(readout_text),
                                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(readout_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }

                    if (layout->fileinfo_width > 0 && fileinfo_text && fileinfo_text[0]) {
                        gui_ui_ellipsize_text(app, fileinfo_text, sizeof(playback_fileinfo_display_a),
                                              FONT_SIZE_STATUS, 1, layout->fileinfo_width);
                        CLAY(CLAY_IDI("PlaybackTimelineFileInfo", channel_index), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIXED(layout->fileinfo_width), CLAY_SIZING_FIT(0) },
                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
                            }
                        }) {
                            CLAY_TEXT(make_string(fileinfo_text),
                                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(readout_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }
                }
            }
        }

        CLAY(CLAY_IDI("PlaybackTimelineRightPad", channel_index), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(layout->right_padding), CLAY_SIZING_GROW(0) } }
        }) {}
    }
}

// Render the channels panel - each channel has VU meter + waveform + stats grouped together
static void render_channels_panel(gui_app_t *app, gui_ui_channel_spacing_t spacing,
                                    bool compact) {
#ifdef ENABLE_DDD
    bool ddd_single_channel = gui_ui_selected_device_is_ddd(app);
#else
    bool ddd_single_channel = false;
#endif
#ifdef ENABLE_FX3
    bool fx3_single_channel = gui_ui_selected_device_is_fx3(app);
#else
    bool fx3_single_channel = false;
#endif
    // Single-card CXADC has no RF channel-B source (capture_b is forced off in
    // gui_ui_sync_capture_mode_state / gui_capture_apply_cxadc_profile), so
    // treat it as single-channel too: hide the empty Channel B row and let
    // Channel A fill the panel. dev->index is the CXADC card count (1 or 2);
    // gui_ui_selected_device_is_cxadc sets clockgen = (index > 1), so a single
    // stock card (index == 1) is cxadc_mode && !clockgen.
    bool cxadc_clockgen_for_preview = false;
    bool cxadc_single_card = gui_ui_selected_device_is_cxadc(app, &cxadc_clockgen_for_preview) &&
                              !cxadc_clockgen_for_preview;
    // DdD, FX3 and single-card CXADC are single-channel: hide the Channel B
    // row and apply a natural fill layout for Channel A height (no reserved
    // spacer/dead area).
    bool single_channel_preview = ddd_single_channel || fx3_single_channel || cxadc_single_card;
    Clay_SizingAxis channel_a_height = CLAY_SIZING_GROW(0);
    bool playback_mode = gui_ui_selected_device_is_playback(app);

    bool m_client_stats=app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT;
    gui_monitor_record_stats_t m_record={0};
    bool have_m_record=m_client_stats && gui_monitor_client_record_stats(&m_record);
    gui_channel_stats_values_t stats[2] = {
        { atomic_load(&app->clip_count_a_pos), atomic_load(&app->clip_count_a_neg),
          app->vu_a.peak_pos, app->vu_a.peak_neg,
          atomic_load(&app->recording_raw_a), atomic_load(&app->recording_compressed_a) },
        { atomic_load(&app->clip_count_b_pos), atomic_load(&app->clip_count_b_neg),
          app->vu_b.peak_pos, app->vu_b.peak_neg,
          atomic_load(&app->recording_raw_b), atomic_load(&app->recording_compressed_b) }
    };
    if (have_m_record) {
        for (int ch=0;ch<2;ch++) {
            stats[ch].clip_pos=m_record.clip_pos[ch];
            stats[ch].clip_neg=m_record.clip_neg[ch];
            stats[ch].raw_bytes=m_record.raw[ch];
            stats[ch].compressed_bytes=m_record.compressed[ch];
        }
    }
    // Drive the on-disk size readout from stat() of the actual output file so
    // it is 1:1 with `ls`/file-manager (kills the writer-atomic > file
    // "over" report from the FILE* buffer lead, and the decimal-MB unit below
    // kills the MiB-vs-decimal "under" report). Throttled to 5/s; falls back
    // to the writer atomic when stat fails or recording is stopped.
    {
        static uint64_t s_live_file_size[2] = {0, 0};
        static double s_live_file_size_last_update_s = 0.0;
        #define LIVE_FILE_SIZE_REFRESH_INTERVAL_S 0.2
        if (app->is_recording) {
            double now = GetTime();
            if (s_live_file_size_last_update_s == 0.0 ||
                (now - s_live_file_size_last_update_s) >= LIVE_FILE_SIZE_REFRESH_INTERVAL_S) {
                s_live_file_size_last_update_s = now;
                uint64_t fa = 0, fb = 0;
                if (gui_record_get_live_output_bytes(&fa, &fb)) {
                    s_live_file_size[0] = fa;
                    s_live_file_size[1] = fb;
                }
            }
        } else {
            s_live_file_size[0] = 0;
            s_live_file_size[1] = 0;
            s_live_file_size_last_update_s = 0.0;
        }
        if (app->is_recording) {
            if (app->settings.use_flac) {
                // FLAC mode: "FLAC:" readout = on-disk compressed file;
                // "RAW:" readout stays the uncompressed input atomic (for ratio).
                if (s_live_file_size[0] > 0) stats[0].compressed_bytes = s_live_file_size[0];
                if (s_live_file_size[1] > 0) stats[1].compressed_bytes = s_live_file_size[1];
            } else {
                // RAW mode: "RAW:" readout = on-disk file.
                if (s_live_file_size[0] > 0) stats[0].raw_bytes = s_live_file_size[0];
                if (s_live_file_size[1] > 0) stats[1].raw_bytes = s_live_file_size[1];
            }
        }
    }
    gui_channel_stats_layout_t stats_layout = gui_ui_stats_layout(app, compact,
                                                                 spacing.horizontal_gap);

    // Setup custom element data for this frame
    s_vu_a_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER;
    s_vu_a_element.customData.vu_meter.meter = &app->vu_a;
    s_vu_a_element.customData.vu_meter.label = "CH A";
    s_vu_a_element.customData.vu_meter.is_clipping_pos = (have_m_record ? m_record.clip_pos[0] : atomic_load(&app->clip_count_a_pos)) > 0;
    s_vu_a_element.customData.vu_meter.is_clipping_neg = (have_m_record ? m_record.clip_neg[0] : atomic_load(&app->clip_count_a_neg)) > 0;
    s_vu_a_element.customData.vu_meter.channel_color = COLOR_CHANNEL_A;

    s_osc_a_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_CHANNEL_PANEL;
    s_osc_a_element.customData.channel_panel.app = app;
    s_osc_a_element.customData.channel_panel.channel = 0;

    s_vu_b_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER;
    s_vu_b_element.customData.vu_meter.meter = &app->vu_b;
    s_vu_b_element.customData.vu_meter.label = "CH B";
    s_vu_b_element.customData.vu_meter.is_clipping_pos = (have_m_record ? m_record.clip_pos[1] : atomic_load(&app->clip_count_b_pos)) > 0;
    s_vu_b_element.customData.vu_meter.is_clipping_neg = (have_m_record ? m_record.clip_neg[1] : atomic_load(&app->clip_count_b_neg)) > 0;
    s_vu_b_element.customData.vu_meter.channel_color = COLOR_CHANNEL_B;

    s_osc_b_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_CHANNEL_PANEL;
    s_osc_b_element.customData.channel_panel.app = app;
    s_osc_b_element.customData.channel_panel.channel = 1;

    s_settings_icon_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_SETTINGS_ICON;

    CLAY(CLAY_ID("ChannelsPanel"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { spacing.horizontal_gap, spacing.horizontal_gap, 4, 4 },
            .childGap = 8
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG)
    }) {
        int screen_width = gui_ui_get_layout_width();
        gui_playback_timeline_layout_t timeline_layout = gui_ui_playback_timeline_layout(app, screen_width, spacing);
        int playback_track_width_px = timeline_layout.track_width;
        int playback_fill_w_a = 0;
        int playback_fill_w_b = 0;
        bool playback_has_file_a = false;
        bool playback_has_file_b = false;
        bool playback_seekable_a = false;
        bool playback_seekable_b = false;
        bool playback_indexing_a = false;
        bool playback_indexing_b = false;
        bool playback_provisional_a = false;
        bool playback_provisional_b = false;
        bool playback_verification_failed_a = false;
        bool playback_verification_failed_b = false;
        snprintf(playback_timeline_display_a, sizeof(playback_timeline_display_a), "--:--:--/--:--:--");
        snprintf(playback_timeline_display_b, sizeof(playback_timeline_display_b), "--:--:--/--:--:--");
        if (playback_mode) {
            uint64_t current_sample_a = gui_playback_get_position_samples_channel(app, 0);
            uint64_t current_sample_b = gui_playback_get_position_samples_channel(app, 1);
            uint64_t total_samples_a = 0;
            uint64_t total_samples_b = 0;
            double duration_seconds_a = 0.0;
            double duration_seconds_b = 0.0;
            bool loaded_a = gui_ui_playback_channel_timeline_info(app, 0, &total_samples_a, &duration_seconds_a,
                                                                 &playback_provisional_a, &playback_verification_failed_a);
            bool loaded_b = gui_ui_playback_channel_timeline_info(app, 1, &total_samples_b, &duration_seconds_b,
                                                                 &playback_provisional_b, &playback_verification_failed_b);
            playback_seekable_a = loaded_a && total_samples_a > 0 && duration_seconds_a > 0.0;
            playback_seekable_b = loaded_b && total_samples_b > 0 && duration_seconds_b > 0.0;
            playback_indexing_a = gui_playback_is_indexing(app, 0);
            playback_indexing_b = gui_playback_is_indexing(app, 1);
            gui_ui_format_playback_timeline(playback_timeline_display_a, sizeof(playback_timeline_display_a),
                                            &playback_fill_w_a, &playback_has_file_a,
                                            loaded_a, playback_provisional_a, current_sample_a, total_samples_a, duration_seconds_a,
                                            gui_playback_get_channel_rate_hz(app, 0), playback_track_width_px);
            gui_ui_format_playback_timeline(playback_timeline_display_b, sizeof(playback_timeline_display_b),
                                            &playback_fill_w_b, &playback_has_file_b,
                                            loaded_b, playback_provisional_b, current_sample_b, total_samples_b, duration_seconds_b,
                                            gui_playback_get_channel_rate_hz(app, 1), playback_track_width_px);

            // Right-side file info per channel (HARD DEV NOTE above the
            // helpers): basename + actual bit depth/resolved real rate +
            // RAW/FLAC sizes, next to the scrub track on the same row.
            gui_ui_playback_file_basename(app, 0, playback_name_display_a,
                                          sizeof(playback_name_display_a));
            gui_ui_playback_file_basename(app, 1, playback_name_display_b,
                                          sizeof(playback_name_display_b));
            gui_ui_format_playback_readout(app, 0, playback_readout_display_a,
                                           sizeof(playback_readout_display_a),
                                           timeline_layout.readout_compact);
            gui_ui_format_playback_readout(app, 1, playback_readout_display_b,
                                           sizeof(playback_readout_display_b),
                                           timeline_layout.readout_compact);
            gui_ui_format_playback_fileinfo(app, 0, playback_fileinfo_display_a,
                                            sizeof(playback_fileinfo_display_a),
                                            timeline_layout.fileinfo_compact);
            gui_ui_format_playback_fileinfo(app, 1, playback_fileinfo_display_b,
                                            sizeof(playback_fileinfo_display_b),
                                            timeline_layout.fileinfo_compact);

            // Playback scrub row aligned with Channel A preview.
            render_playback_timeline_row(app, 0, playback_timeline_display_a, playback_fill_w_a,
                                          playback_has_file_a, playback_seekable_a, playback_indexing_a,
                                          playback_provisional_a, playback_verification_failed_a,
                                          spacing, &timeline_layout,
                                          playback_name_display_a, playback_readout_display_a,
                                          playback_fileinfo_display_a);
        }
        // Channel A row: VU meter + waveform + stats
        CLAY(CLAY_ID("ChannelARow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), channel_a_height },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = spacing.horizontal_gap
            }
        }) {
            // VU meter A - custom element
            CLAY(CLAY_ID("VUMeterA"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(spacing.vu_column_width), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_vu_a_element }
            }) {}

            // Oscilloscope canvas A - custom element
            CLAY(CLAY_ID("OscilloscopeCanvasA"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_osc_a_element }
            }) {}

            // Stats panel A
            render_channel_stats(app, 0, &stats[0], &stats_layout, have_m_record?&m_record:NULL);
        }

        // Channel B row: VU meter + waveform + stats
        // Hidden entirely for single-channel devices (DdD/FX3).
        if (!single_channel_preview) {
            if (playback_mode) {
                // Second playback scrub row aligned with Channel B preview.
                render_playback_timeline_row(app, 1, playback_timeline_display_b, playback_fill_w_b,
                                              playback_has_file_b, playback_seekable_b, playback_indexing_b,
                                              playback_provisional_b, playback_verification_failed_b,
                                              spacing, &timeline_layout,
                                              playback_name_display_b, playback_readout_display_b,
                                              playback_fileinfo_display_b);
            }
            CLAY(CLAY_ID("ChannelBRow"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = spacing.horizontal_gap
                }
            }) {
                // VU meter B - custom element
                CLAY(CLAY_ID("VUMeterB"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(spacing.vu_column_width), CLAY_SIZING_GROW(0) } },
                    .custom = { .customData = &s_vu_b_element }
                }) {}

                // Oscilloscope canvas B - custom element
                CLAY(CLAY_ID("OscilloscopeCanvasB"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                    .custom = { .customData = &s_osc_b_element }
                }) {}

                // Stats panel B
                render_channel_stats(app, 1, &stats[1], &stats_layout, have_m_record?&m_record:NULL);
            }
        }
    }
}

// Budget the status first so the toolbar and channels share this frame's choices.
static bool render_main_panels(gui_app_t *app) {
    int status_width = gui_ui_get_layout_width();
    int status_height = gui_ui_get_layout_height();
    // Remote recording occupies the same status-bar space as local recording.
    // Determine it before deciding which status layout/message to display.
    gui_monitor_record_stats_t m_status = {0};
    bool m_status_valid = app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT &&
        gui_monitor_client_record_stats(&m_status);
    bool m_recording = m_status_valid && gui_monitor_client_peer_state() == 2;
    gui_ui_status_layout_mode_t status_layout =
        gui_ui_get_status_layout_mode(status_width, status_height,
                                      app->is_recording || m_recording);
    bool status_quarter_scale =
        status_width <= GUI_UI_STATUS_QUARTER_MAX_WIDTH &&
        status_height <= GUI_UI_STATUS_QUARTER_MAX_HEIGHT;
    bool status_tiny = status_width < GUI_UI_STATUS_TINY_BREAKPOINT;
    bool status_minimal = status_layout == GUI_UI_STATUS_LAYOUT_MINIMAL_SINGLE;
    bool show_sync_status = !status_tiny;
    bool show_sample_rate = !status_tiny;
    bool show_frame_count = !status_tiny;
    bool show_missed_count = !status_tiny;
    bool show_error_count = !status_tiny;
    bool show_samples_count = true;
    gui_device_buffer_view_t device_buffer_views[3];
    bool show_device_buffer_status = !gui_net_is_client(app) &&
        gui_ui_get_device_buffer_view(app, GUI_DEVICE_BUFFER_LAYOUT_FULL,
                                      &device_buffer_views[0]);
    if (show_device_buffer_status) {
        for (int tier = 1; tier < 3; tier++) {
            if (!gui_ui_get_device_buffer_view(app,
                    (gui_device_buffer_layout_t)tier, &device_buffer_views[tier])) {
                show_device_buffer_status = false;
                break;
            }
        }
    }
    int device_buffer_level = GUI_DEVICE_BUFFER_LAYOUT_FULL;
    // Detect an error/denied/failed or critical capture-stop status so the bar can
    // yield space to it: when an error is being shown, hide the free-space
    // readout (and widen the message budget) so the actual error text isn't
    // truncated behind the free-space label. This is what makes CXADC/USB
    // permission failures readable in the bottom bar instead of "CXADC permi...".
    /* Preview failure is a live M-Client state, not a permanent status
     * message.  This prevents an old failure from surviving preview recovery,
     * Disconnect/Connect, or switching away from Monitor Client mode. */
    const char *const preview_status = "Monitor preview error: showing last valid frame";
    const char *raw_status_gate = (app->status_message[0] != '\0') ? app->status_message : NULL;
    if (raw_status_gate && strcmp(raw_status_gate, preview_status) == 0) {
        /* Ignore the legacy sticky text if an older code path stored it. */
        raw_status_gate = NULL;
    }
    bool status_is_error = false;
    if (raw_status_gate) {
        status_is_error =
            strstr(raw_status_gate, "denied") != NULL ||
            strstr(raw_status_gate, "Denied") != NULL ||
            strstr(raw_status_gate, "error") != NULL ||
            strstr(raw_status_gate, "Error") != NULL ||
            strstr(raw_status_gate, "failed") != NULL ||
            strstr(raw_status_gate, "Failed") != NULL ||
            strstr(raw_status_gate, "not granted") != NULL ||
            strstr(raw_status_gate, "timed out") != NULL ||
            strstr(raw_status_gate, "Capture stopped:") != NULL;
    }
    if (!status_is_error && app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT &&
        gui_monitor_client_running() && gui_monitor_client_preview_error()) {
        raw_status_gate = preview_status;
        status_is_error = true;
    }
    /* Network backpressure is distinct from USB/capture buffer errors.
     * Server-reported RF queue overflow proves that the transfer path cannot
     * sustain source rate, but not whether NIC/Wi-Fi or client CPU is to blame. */
    bool rf_transport_banner = false;
    bool rf_transport_critical = false;
    static char rf_transport_banner_text[96];
    if (!status_is_error && app->settings.net_mode == GUI_NET_MODE_SERVER) {
        uint_fast64_t rf_drops = atomic_load(&app->net_rf_server_stream_drops);
        uint_fast64_t au_drops = atomic_load(&app->net_audio_server_stream_drops);
        size_t rf_q = atomic_load(&app->net_rf_server_queue_bytes);
        size_t au_q = atomic_load(&app->net_audio_server_queue_bytes);
        if (rf_drops) {
            snprintf(rf_transport_banner_text, sizeof(rf_transport_banner_text),
                     "RF TRANSFER TOO SLOW - server dropped %llu stream(s)",
                     (unsigned long long)rf_drops);
            rf_transport_banner = rf_transport_critical = true;
        } else if (au_drops) {
            snprintf(rf_transport_banner_text, sizeof(rf_transport_banner_text),
                     "Audio stream overload - dropped %llu stream(s)",
                     (unsigned long long)au_drops);
            rf_transport_banner = rf_transport_critical = true;
        } else if (rf_q >= gui_net_queue_warn_bytes(GUI_NET_RF_QUEUE_LIMIT_BYTES)) {
            snprintf(rf_transport_banner_text, sizeof(rf_transport_banner_text),
                     "RF NETWORK BOTTLENECK - queue %u%% of 256MiB (link/receiver)",
                     gui_net_queue_percent(rf_q, GUI_NET_RF_QUEUE_LIMIT_BYTES));
            rf_transport_banner = true;
        } else if (au_q >= gui_net_queue_warn_bytes(GUI_NET_AUDIO_QUEUE_LIMIT_BYTES)) {
            snprintf(rf_transport_banner_text, sizeof(rf_transport_banner_text),
                     "Audio network backlog - queue %u%% of 64MiB",
                     gui_net_queue_percent(au_q, GUI_NET_AUDIO_QUEUE_LIMIT_BYTES));
            rf_transport_banner = true;
        }
    } else if (!status_is_error && app->settings.net_mode == GUI_NET_MODE_CLIENT) {
        uint_fast64_t rf = atomic_load(&app->net_rf_client_interruptions);
        uint_fast64_t au = atomic_load(&app->net_audio_client_interruptions);
        uint_fast64_t server_drops = atomic_load(&app->net_rf_server_stream_drops);
        size_t server_queue = atomic_load(&app->net_rf_server_queue_bytes);
        if (server_drops) {
            snprintf(rf_transport_banner_text, sizeof(rf_transport_banner_text),
                     "RF TRANSFER TOO SLOW - server queue overflow (%llu drop(s))",
                     (unsigned long long)server_drops);
            rf_transport_banner = rf_transport_critical = true;
        } else if (server_queue >= gui_net_queue_warn_bytes(GUI_NET_RF_QUEUE_LIMIT_BYTES)) {
            snprintf(rf_transport_banner_text, sizeof(rf_transport_banner_text),
                     "RF transfer backlog on server - %u%% (network/receiver)",
                     gui_net_queue_percent(server_queue, GUI_NET_RF_QUEUE_LIMIT_BYTES));
            rf_transport_banner = true;
        } else if (rf || au) {
            snprintf(rf_transport_banner_text, sizeof(rf_transport_banner_text),
                     "RF/Audio stream interrupted RF:%llu Audio:%llu (check link/receiver)",
                     (unsigned long long)rf, (unsigned long long)au);
            rf_transport_banner = rf_transport_critical = true;
        }
    }
    if (rf_transport_banner) {
        raw_status_gate = rf_transport_banner_text;
        if (rf_transport_critical) status_is_error = true;
    }
    // Decide error rows below, once the complete message and fixed readout
    // widths are known. Normal recording status is redundant with the timer.
    bool status_two_rows = false;
    // An acknowledged remote recording supersedes the stale "start recording
    // requested" message, just as an active local recording does. Keep
    // transport warnings and real errors visible in either mode.
    bool show_status_message = rf_transport_banner || status_is_error ||
        (!status_minimal && !app->is_recording && !m_recording);
    /* Keep free-space visible by default and let the dynamic budget contract
     * gaps/widths first; only hide it as a last resort. Error status still
     * reserves the left-side for readable failure text. */
    bool show_free_space = !status_is_error;
    /* Full-quality starting values: the budget loop contracts them only
     * under measured pressure, preserving spacing and readout precision
     * whenever the available width allows it. Only the absolute tiny
     * floors stay pre-seeded (200% scale at the minimum window cannot fit
     * anything larger). */
    int sample_rate_value_width = 80;
    int samples_value_width = status_tiny ? 48 : 60;
    int frames_value_width = 32;
    int small_counter_width = 32;
    int buffer_value_width = status_tiny ? 28 : 35;
    int status_counter_inner_gap = status_tiny ? 2 : 3;
    int status_bar_gap = status_tiny ? 6 : 20;
    int status_right_gap = status_tiny ? 8 : 10;
    int status_left_gap = status_tiny ? 4 : 8;
    int status_font_size = FONT_SIZE_STATUS;
    // Inter is clearer than the small Space Mono glyphs in the status bar.
    // Use the same font for measuring, truncating and rendering readouts.
    const int status_readout_font_id = 0;
    int device_buffer_tier_width[3] = { 0, 0, 0 };
    int device_buffer_caption_width[3] = { 0, 0, 0 };
    const char *device_buffer_caption_budget[3] = {
        "Peak 100% BP", "P100% BP", "P100%"
    };
    const int device_buffer_bar_width = 48;
    if (show_device_buffer_status) {
        for (int tier = 0; tier < 3; tier++) {
            bool full = tier == GUI_DEVICE_BUFFER_LAYOUT_FULL;
            int inner_gap = tier == GUI_DEVICE_BUFFER_LAYOUT_TINY ? 2 : 4;
            device_buffer_caption_width[tier] = gui_ui_measure_text_width(
                app, device_buffer_caption_budget[tier], status_font_size, status_readout_font_id);
            device_buffer_tier_width[tier] = gui_ui_measure_text_width(
                app, full ? "HW Buffer:" : "HW:", status_font_size, 0) +
                device_buffer_caption_width[tier] + inner_gap;
            if (full) device_buffer_tier_width[tier] +=
                device_buffer_bar_width + inner_gap;
        }
    }
    const bool status_compact_labels =
        status_width < GUI_UI_STATUS_COMPACT_BREAKPOINT;
    const char *rf_buffer_label_full = "RF Buffer:";
    const char *audio_buffer_label_full = "Audio Buffer:";
    const char *samples_label_full = status_tiny ? "S:" : "Samples:";
    const char *frames_label_full = "Frames:";
    const char *missed_label_full = "Missed:";
    const char *errors_label_full = "Errors:";
    const char *rf_buffer_label_compact = "RF:";
    const char *audio_buffer_label_compact = "Aud:";
    const char *samples_label_compact = status_tiny ? "S:" : "Samp:";
    const char *frames_label_compact = "F:";
    const char *missed_label_compact = "M:";
    const char *errors_label_compact = "E:";
    const char *rf_buffer_label = status_compact_labels ? rf_buffer_label_compact : rf_buffer_label_full;
    const char *audio_buffer_label = status_compact_labels ? audio_buffer_label_compact : audio_buffer_label_full;
    const char *samples_label = status_compact_labels ? samples_label_compact : samples_label_full;
    const char *frames_label = status_compact_labels ? frames_label_compact : frames_label_full;
    const char *missed_label = status_compact_labels ? missed_label_compact : missed_label_full;
    const char *errors_label = status_compact_labels ? errors_label_compact : errors_label_full;
    update_status_free_space(app);

    bool show_record_indicator = (app->is_recording || m_recording) && !status_quarter_scale;
    if (show_record_indicator) {
        double rec_duration = m_recording ? (double)m_status.elapsed_ms/1000.0
                                         : GetTime() - app->recording_start_time;
        int rec_hours = (int)(rec_duration / 3600);
        int rec_mins  = ((int)(rec_duration / 60)) % 60;
        int rec_secs  = ((int)rec_duration) % 60;
        snprintf(status_record_timer_display, sizeof(status_record_timer_display),
                 "%02d:%02d:%02d", rec_hours, rec_mins, rec_secs);
    }

    const char *raw_status = rf_transport_banner ? rf_transport_banner_text :
        (raw_status_gate ? raw_status_gate :
         (app->is_capturing ? "Capturing..." : "Ready"));
    Color status_color = COLOR_TEXT_DIM;
    if (status_is_error) {
        status_color = COLOR_CLIP_RED;
    } else if (rf_transport_banner) {
        status_color = COLOR_METER_YELLOW;
    } else if (strstr(raw_status, "Requesting") != NULL ||
               strstr(raw_status, "Reconnecting") != NULL ||
               strstr(raw_status, "Waiting") != NULL ||
               strstr(raw_status, "Initializing") != NULL) {
        status_color = COLOR_METER_YELLOW;
    }
    char status_free_space_display_full[120] = "";
    char status_free_space_display_compact[120] = "";
    char status_free_space_display_short[120] = "";
    Color free_space_color = COLOR_TEXT_DIM;
    if (show_free_space) {
        if (s_status_free_space_valid) {
            char free_only_full[48];
            char free_only_compact[24];
            char runway_hms[24];
            format_status_free_space_label(free_only_full, sizeof(free_only_full), s_status_free_space_cached_bytes);
            format_status_free_space_compact_label(free_only_compact, sizeof(free_only_compact), s_status_free_space_cached_bytes);
            if (app->is_recording || m_recording) {
                bool runway_known = s_status_output_rate_bps > 0.0;
                if (runway_known) {
                    double runway_s = (double)s_status_free_space_cached_bytes / s_status_output_rate_bps;
                    format_status_runway_hhmmss(runway_hms, sizeof(runway_hms), runway_s);
                } else {
                    snprintf(runway_hms, sizeof(runway_hms), "--:--:--");
                }
                snprintf(status_free_space_display_full, sizeof(status_free_space_display_full),
                         "%s | Runway %s", free_only_full, runway_hms);
                if (status_tiny) {
                    snprintf(status_free_space_display_compact, sizeof(status_free_space_display_compact),
                             "Rwy %s", runway_hms);
                } else {
                    snprintf(status_free_space_display_compact, sizeof(status_free_space_display_compact),
                             "%s | Rwy %s", free_only_compact, runway_hms);
                }
                snprintf(status_free_space_display_short, sizeof(status_free_space_display_short),
                         "Rwy %s", runway_hms);
            } else {
                snprintf(status_free_space_display_full, sizeof(status_free_space_display_full),
                         "%s", free_only_full);
                snprintf(status_free_space_display_compact, sizeof(status_free_space_display_compact),
                         "%s", free_only_compact);
                snprintf(status_free_space_display_short, sizeof(status_free_space_display_short),
                         "%s", free_only_compact);
            }
            if (s_status_free_space_cached_bytes < STATUS_FREE_SPACE_LOW_BYTES) {
                free_space_color = COLOR_CLIP_RED;
            } else if (s_status_free_space_cached_bytes < STATUS_FREE_SPACE_WARN_BYTES) {
                free_space_color = COLOR_METER_YELLOW;
            }
        } else if (app->is_recording || m_recording) {
            snprintf(status_free_space_display_full, sizeof(status_free_space_display_full),
                     "Free: N/A | Runway --:--:--");
            if (status_tiny) {
                snprintf(status_free_space_display_compact, sizeof(status_free_space_display_compact),
                         "Rwy --:--:--");
            } else {
                snprintf(status_free_space_display_compact, sizeof(status_free_space_display_compact),
                         "N/A | Rwy --:--:--");
            }
            snprintf(status_free_space_display_short, sizeof(status_free_space_display_short),
                     "Rwy --:--:--");
        } else {
            snprintf(status_free_space_display_full, sizeof(status_free_space_display_full),
                     "Free: N/A");
            snprintf(status_free_space_display_compact, sizeof(status_free_space_display_compact),
                     "N/A");
            snprintf(status_free_space_display_short, sizeof(status_free_space_display_short),
                     "N/A");
        }
    }
    /* Budget bounded readout slots, not this frame's values. Crossing a
     * digit boundary must not move the status groups or the channel panels.
     * Longer values are visibly ellipsized inside the chosen fixed slot. */
    int status_free_space_tier = 0;
    char status_free_space_tier_text[3][120];
    int status_free_space_tier_width[3] = { 0, 0, 0 };
    snprintf(status_free_space_tier_text[0], sizeof(status_free_space_tier_text[0]),
             "%s", status_free_space_display_full);
    snprintf(status_free_space_tier_text[1], sizeof(status_free_space_tier_text[1]),
             "%s", status_free_space_display_compact);
    snprintf(status_free_space_tier_text[2], sizeof(status_free_space_tier_text[2]),
             "%s", status_free_space_display_short);
    const char *free_space_budget[3] = {
        "Free: 9999.99 GB", "9999.9G", "9999.9G"
    };
    if (app->is_recording) {
        free_space_budget[0] = "Free: 9999.99 GB | Runway 99:59:59";
        free_space_budget[1] = status_tiny ? "Rwy 99:59:59" :
                                           "9999.9G | Rwy 99:59:59";
        free_space_budget[2] = "Rwy 99:59:59";
    }
    if (show_free_space) {
        for (int tier = 0; tier < 3; tier++) {
            status_free_space_tier_width[tier] = gui_ui_measure_text_width(
                app, free_space_budget[tier], status_font_size, status_readout_font_id);
        }
    }

    uint32_t status_sample_rate_raw = atomic_load(&app->sample_rate);
    uint64_t status_samples_raw = m_status_valid ? m_status.samples_a : atomic_load(&app->samples_a);

    /* Tiered right-side value text: tier 0 is the full readout, higher tiers
     * shorten it under width pressure instead of hiding the readout. All
     * tiers use fixed representative widths, independent of the live value. */
    int status_sample_rate_tier = 0;
    int status_samples_tier = 0;
    char status_sample_rate_tier_text[3][32];
    int status_sample_rate_tier_width[3] = { 0, 0, 0 };
    char status_samples_tier_text[3][32];
    int status_samples_tier_width[3] = { 0, 0, 0 };
    const char *sample_rate_budget[3] = { "9999.9 MSPS", "9999 MSPS", "9999M" };
    const char *samples_budget[3] = { "999.99G", "999.9G", "999G" };
    for (int tier = 0; tier < 3; tier++) {
        format_status_sample_rate_tier(status_sample_rate_tier_text[tier],
                                       sizeof(status_sample_rate_tier_text[tier]),
                                       status_sample_rate_raw, tier);
        if (status_sample_rate_raw == 0) {
            snprintf(status_sample_rate_tier_text[tier],
                     sizeof(status_sample_rate_tier_text[tier]), "%s",
                     tier < 2 ? "-- MSPS" : "--");
        }
        status_sample_rate_tier_width[tier] = gui_ui_measure_text_width(
            app, sample_rate_budget[tier], status_font_size, status_readout_font_id);
        format_status_samples_tier(status_samples_tier_text[tier],
                                   sizeof(status_samples_tier_text[tier]),
                                   status_samples_raw, tier);
        status_samples_tier_width[tier] = gui_ui_measure_text_width(
            app, samples_budget[tier], status_font_size, status_readout_font_id);
    }

    int status_rf_pct = 0;
    int status_aud_pct = 0;
    uint32_t missed = m_status_valid ? gui_monitor_client_peer_missed() :
        (app->is_capturing ? atomic_load(&app->missed_frame_count) : 0);
    uint32_t errors = m_status_valid ? gui_monitor_client_peer_errors() :
        (app->is_capturing ? atomic_load(&app->error_count) : 0);
    const char *fault_captions[4] = { "", "M!", "E!", "M! E!" };
    int fault_caption_width[4] = { 0, 0, 0, 0 };
    for (int i = 1; i < 4; i++) {
        fault_caption_width[i] = gui_ui_measure_text_width(
            app, fault_captions[i], status_font_size, status_readout_font_id);
    }
    bool show_fault_summary = false;
    int fault_summary_flags = 0;
    /* Snapshot readouts once for this frame; their values do not affect the
     * width budget. Buffer fill and fault colors may still update normally. */
    {
        format_status_counter(status_frames_display, sizeof(status_frames_display),
                              (m_status_valid ? gui_monitor_client_peer_frames() : atomic_load(&app->frame_count)));
        format_status_counter(status_missed_display, sizeof(status_missed_display),
                              missed);
        format_status_counter(status_errors_display, sizeof(status_errors_display),
                              errors);
        size_t status_rf_head = atomic_load(&app->buffers.buffers[BUF_CAPTURE_RF].head);
        size_t status_rf_tail = atomic_load(&app->buffers.buffers[BUF_CAPTURE_RF].tail);
        size_t status_rf_size = app->buffers.buffers[BUF_CAPTURE_RF].buffer_size;
        status_rf_pct = status_rf_size > 0
            ? (int)(((status_rf_tail - status_rf_head) * 100) / status_rf_size) : 0;
        if(m_status_valid)status_rf_pct=(int)m_status.rf_fill_pct;
        snprintf(status_rf_buf_display, sizeof(status_rf_buf_display), "%d%%", status_rf_pct);
        size_t status_aud_head = atomic_load(&app->buffers.buffers[BUF_CAPTURE_AUDIO].head);
        size_t status_aud_tail = atomic_load(&app->buffers.buffers[BUF_CAPTURE_AUDIO].tail);
        size_t status_aud_size = app->buffers.buffers[BUF_CAPTURE_AUDIO].buffer_size;
        status_aud_pct = status_aud_size > 0
            ? (int)(((status_aud_tail - status_aud_head) * 100) / status_aud_size) : 0;
        if(m_status_valid)status_aud_pct=(int)m_status.aud_fill_pct;
        snprintf(status_aud_buf_display, sizeof(status_aud_buf_display), "%d%%", status_aud_pct);
    }

    int sync_label_width = gui_ui_measure_text_width(app, "Sync:", status_font_size, 0);
    int sync_value_width = gui_ui_measure_text_width(app, "OK", status_font_size, 0);
    {
        int sync_dash_value_width = gui_ui_measure_text_width(app, "--", status_font_size, 0);
        if (sync_dash_value_width > sync_value_width) sync_value_width = sync_dash_value_width;
    }
    int samples_label_width_full = gui_ui_measure_text_width(app, samples_label_full, status_font_size, 0);
    int frames_label_width_full = gui_ui_measure_text_width(app, frames_label_full, status_font_size, 0);
    int missed_label_width_full = gui_ui_measure_text_width(app, missed_label_full, status_font_size, 0);
    int errors_label_width_full = gui_ui_measure_text_width(app, errors_label_full, status_font_size, 0);
    int rf_label_width_full = gui_ui_measure_text_width(app, rf_buffer_label_full, status_font_size, 0);
    int audio_label_width_full = gui_ui_measure_text_width(app, audio_buffer_label_full, status_font_size, 0);
    int samples_label_width_compact = gui_ui_measure_text_width(app, samples_label_compact, status_font_size, 0);
    int frames_label_width_compact = gui_ui_measure_text_width(app, frames_label_compact, status_font_size, 0);
    int missed_label_width_compact = gui_ui_measure_text_width(app, missed_label_compact, status_font_size, 0);
    int errors_label_width_compact = gui_ui_measure_text_width(app, errors_label_compact, status_font_size, 0);
    int rf_label_width_compact = gui_ui_measure_text_width(app, rf_buffer_label_compact, status_font_size, 0);
    int audio_label_width_compact = gui_ui_measure_text_width(app, audio_buffer_label_compact, status_font_size, 0);
    int record_timer_text_width = 0;
    if (show_record_indicator) {
        // Inter has proportional digits: measuring "88:88:88" can be narrower
        // than the actual "00:00:42", causing the elapsed timer to be
        // ellipsized and appear to collide with the adjacent status message.
        // Reserve the widest six-digit clock, not a single digit pattern.
        char timer_probe[] = "00:00:00";
        for (char digit = '0'; digit <= '9'; digit++) {
            timer_probe[0] = timer_probe[1] = timer_probe[3] =
                timer_probe[4] = timer_probe[6] = timer_probe[7] = digit;
            int width = gui_ui_measure_text_width(app, timer_probe,
                status_font_size, status_readout_font_id);
            if (width > record_timer_text_width) record_timer_text_width = width;
        }
        // Account for three-digit hour counts and renderer rounding.
        int live_width = gui_ui_measure_text_width(app, status_record_timer_display,
            status_font_size, status_readout_font_id);
        if (live_width > record_timer_text_width) record_timer_text_width = live_width;
        record_timer_text_width += 8;
    }
    int frames_text_width = gui_ui_measure_text_width(app, "999M", status_font_size, status_readout_font_id);
    int missed_text_width = frames_text_width;
    int errors_text_width = frames_text_width;
    int rf_text_width = gui_ui_measure_text_width(app, "100%", status_font_size, status_readout_font_id);
    int aud_text_width = rf_text_width;
    int free_space_text_width = status_free_space_tier_width[status_free_space_tier];
    int free_space_width_budget = free_space_text_width;
    int free_space_min_width = status_tiny ? 22 : 48;
    int status_padding_h = 12;
    int status_content_width = status_width - status_padding_h * 2;
    if (status_content_width < 1) status_content_width = 1;
    int min_message_width = status_tiny ? 56 : 96;
    int min_message_floor = 24;
    int status_message_width_budget = 0;
    int status_error_width = status_is_error
        ? gui_ui_measure_text_width(app, raw_status, status_font_size, 0) : 0;
    /* Recompute from the same starting point every frame. Only viewport,
     * font and capture configuration can change the readout tiers. */
    int readout_level = 0;
    /* Allow enough passes so the full contraction chain can run (gaps ->
     * narrowed values/text -> last-resort hides) without stalling
     * midway under 1/4 and other constrained layouts. */
    for (int pass = 0; pass < 192; pass++) {
        // Keep the fault slot reserved while its individual counters are
        // hidden, so the first fault cannot move the neighboring readouts.
        show_fault_summary = !show_missed_count || !show_error_count;
        fault_summary_flags = (missed > 0 && !show_missed_count ? 1 : 0) |
                              (errors > 0 && !show_error_count ? 2 : 0);
        rf_buffer_label = status_compact_labels ? rf_buffer_label_compact : rf_buffer_label_full;
        audio_buffer_label = status_compact_labels ? audio_buffer_label_compact : audio_buffer_label_full;
        samples_label = status_compact_labels ? samples_label_compact : samples_label_full;
        frames_label = status_compact_labels ? frames_label_compact : frames_label_full;
        missed_label = status_compact_labels ? missed_label_compact : missed_label_full;
        errors_label = status_compact_labels ? errors_label_compact : errors_label_full;
        if (show_free_space) {
            free_space_text_width = status_free_space_tier_width[status_free_space_tier];
            if (free_space_width_budget <= 0 || free_space_width_budget > free_space_text_width) {
                free_space_width_budget = free_space_text_width;
            }
        } else {
            free_space_text_width = 0;
            free_space_width_budget = 0;
        }
        int samples_label_width = status_compact_labels ? samples_label_width_compact : samples_label_width_full;
        int frames_label_width = status_compact_labels ? frames_label_width_compact : frames_label_width_full;
        int missed_label_width = status_compact_labels ? missed_label_width_compact : missed_label_width_full;
        int errors_label_width = status_compact_labels ? errors_label_width_compact : errors_label_width_full;
        int rf_label_width = status_compact_labels ? rf_label_width_compact : rf_label_width_full;
        int audio_label_width = status_compact_labels ? audio_label_width_compact : audio_label_width_full;
        int samples_tier_width_now = status_samples_tier_width[status_samples_tier];
        int samples_value_effective = samples_value_width > samples_tier_width_now ? samples_value_width : samples_tier_width_now;
        int frames_value_effective = frames_value_width > frames_text_width ? frames_value_width : frames_text_width;
        int missed_value_effective = small_counter_width > missed_text_width ? small_counter_width : missed_text_width;
        int error_value_effective = small_counter_width > errors_text_width ? small_counter_width : errors_text_width;
        int rf_value_effective = buffer_value_width > rf_text_width ? buffer_value_width : rf_text_width;
        int aud_value_effective = buffer_value_width > aud_text_width ? buffer_value_width : aud_text_width;
        int sample_rate_tier_width_now = status_sample_rate_tier_width[status_sample_rate_tier];
        int sample_rate_group_width = sample_rate_value_width > sample_rate_tier_width_now
            ? sample_rate_value_width : sample_rate_tier_width_now;
        int sync_group_width = sync_label_width + status_counter_inner_gap + sync_value_width;
        int samples_group_width = samples_label_width + status_counter_inner_gap + samples_value_effective;
        int frame_group_width = frames_label_width + status_counter_inner_gap + frames_value_effective;
        int missed_group_width = missed_label_width + status_counter_inner_gap + missed_value_effective;
        int error_group_width = errors_label_width + status_counter_inner_gap + error_value_effective;
        int rf_group_width = rf_label_width + status_counter_inner_gap + rf_value_effective;
        int audio_group_width = audio_label_width + status_counter_inner_gap + aud_value_effective;
        int right_group_count = 0;
        int right_required_width = 0;
        if (show_sync_status) {
            right_required_width += sync_group_width;
            right_group_count++;
        }
        if (show_sample_rate) {
            right_required_width += sample_rate_group_width;
            right_group_count++;
        }
        if (show_samples_count) {
            right_required_width += samples_group_width;
            right_group_count++;
        }
        if (show_frame_count) {
            right_required_width += frame_group_width;
            right_group_count++;
        }
        if (show_missed_count) {
            right_required_width += missed_group_width;
            right_group_count++;
        }
        if (show_error_count) {
            right_required_width += error_group_width;
            right_group_count++;
        }
        if (show_fault_summary) {
            right_required_width += fault_caption_width[3];
            right_group_count++;
        }
        if (show_device_buffer_status) {
            right_required_width += device_buffer_tier_width[device_buffer_level];
            right_group_count++;
        }
        right_required_width += rf_group_width + audio_group_width;
        right_group_count += 2;
        if (right_group_count > 1) {
            right_required_width += status_right_gap * (right_group_count - 1);
        }
        // The minimal critical-error layout renders only the left message.
        // Invisible readouts must not shorten that message's width budget.
        if (status_is_error && status_minimal) right_required_width = 0;

        int fixed_left_width = 0;
        int fixed_left_items = 0;
        if (show_record_indicator) {
            fixed_left_width += 12 + record_timer_text_width;
            fixed_left_items += 2;
        }
        if (show_free_space) {
            int effective_free_space_width = free_space_text_width;
            if (free_space_width_budget < effective_free_space_width) {
                effective_free_space_width = free_space_width_budget;
            }
            fixed_left_width += effective_free_space_width;
            fixed_left_items++;
        }
        int left_gaps_reserved = 0;
        if (show_status_message) {
            left_gaps_reserved = fixed_left_items * status_left_gap;
        } else if (fixed_left_items > 1) {
            left_gaps_reserved = (fixed_left_items - 1) * status_left_gap;
        }
        if (pass == 0) {
            // Use reserved widths, never live counter text or previous-frame
            // geometry. Prefer a second row to hiding readouts for an error.
            int single_row_required_width = status_error_width + fixed_left_width +
                left_gaps_reserved + right_required_width + status_bar_gap * 2;
            status_two_rows = gui_ui_status_uses_two_rows(status_layout,
                status_is_error, status_content_width, single_row_required_width);
        }
        int outer_gaps = status_two_rows ? 0 : (status_bar_gap * 2);
        // With two rows the readouts no longer consume message-row width.
        int left_available = status_content_width - outer_gaps -
            (status_two_rows ? 0 : right_required_width);
        if (left_available < 0) left_available = 0;
        status_message_width_budget = show_status_message
            ? (left_available - fixed_left_width - left_gaps_reserved)
            : 0;

        int required_without_message = right_required_width + fixed_left_width +
                                       left_gaps_reserved + outer_gaps;
        int status_message_required = status_is_error && !status_two_rows && !status_minimal
            ? status_error_width : min_message_width;
        bool fits_current_budget = show_status_message
            ? (status_message_width_budget >= status_message_required)
            : (required_without_message <= status_content_width);
        // The second row must fit independently, even if the first already fits.
        if (status_two_rows && right_required_width > status_content_width) {
            fits_current_budget = false;
        }
        if (fits_current_budget) {
            break;
        }

        /* Shorten the optional hardware detail before it displaces any of
         * the established status fields. Each tier is budgeted above with
         * its label, reserved caption slot, meter and gaps. */
        if (show_device_buffer_status &&
            device_buffer_level < GUI_DEVICE_BUFFER_LAYOUT_TINY) {
            device_buffer_level++;
            continue;
        }

        /* Constrain from lowest-visibility impact first: close spacing, then
         * squeeze value widths/text budgets. Label style depends only on the
         * viewport, shared with channel spacing and sidebar width. */
        if (status_right_gap > 0) {
            status_right_gap -= 1;
            continue;
        }
        if (status_bar_gap > 0) {
            status_bar_gap -= 1;
            continue;
        }
        if (status_left_gap > 0) {
            status_left_gap -= 1;
            continue;
        }
        if (status_counter_inner_gap > 0) {
            status_counter_inner_gap -= 1;
            continue;
        }
        /* Shorten all small readouts together (level-based) instead of
         * collapsing one readout at a time: fewer discrete states, and the
         * row shortens in visible lockstep instead of one readout jumping
         * through all its tiers while the others stay long. */
        if (readout_level < 2) {
            readout_level++;
            status_sample_rate_tier = readout_level;
            status_samples_tier = readout_level;
            status_free_space_tier = readout_level;
            continue;
        }
        /* Do not contract a value slot below its tier's reserved text width. */
        if (sample_rate_value_width > 44 &&
            sample_rate_value_width > status_sample_rate_tier_width[status_sample_rate_tier]) {
            sample_rate_value_width -= 2;
            continue;
        }
        if (samples_value_width > 34 &&
            samples_value_width > status_samples_tier_width[status_samples_tier]) {
            samples_value_width -= 2;
            continue;
        }
        if (frames_value_width > 18 && frames_value_width > frames_text_width) {
            frames_value_width -= 1;
            continue;
        }
        if (small_counter_width > 18 &&
            (small_counter_width > missed_text_width ||
             small_counter_width > errors_text_width)) {
            small_counter_width -= 1;
            continue;
        }
        if (buffer_value_width > 18 &&
            (buffer_value_width > rf_text_width ||
             buffer_value_width > aud_text_width)) {
            buffer_value_width -= 1;
            continue;
        }
        if (show_free_space && free_space_width_budget > free_space_min_width) {
            free_space_width_budget -= 6;
            if (free_space_width_budget < free_space_min_width) {
                free_space_width_budget = free_space_min_width;
            }
            continue;
        }
        if (show_status_message && min_message_width > min_message_floor) {
            min_message_width -= 8;
            if (min_message_width < min_message_floor) {
                min_message_width = min_message_floor;
            }
            continue;
        }

        /* Last-resort fallback only after all dynamic contraction paths. */
        if (show_sample_rate) {
            show_sample_rate = false;
            continue;
        }
        if (show_sync_status) {
            show_sync_status = false;
            continue;
        }
        if (show_error_count) {
            show_error_count = false;
            continue;
        }
        if (show_missed_count) {
            show_missed_count = false;
            continue;
        }
        if (show_frame_count) {
            show_frame_count = false;
            continue;
        }
        if (show_free_space) {
            show_free_space = false;
            free_space_text_width = 0;
            free_space_width_budget = 0;
            continue;
        }
        if (show_status_message && !status_is_error) {
            show_status_message = false;
            status_message_width_budget = 0;
            continue;
        }
        // Keep the timer and fault/HW readouts at the minimum viewport by
        // spending the remaining outer padding only after other compaction.
        if (!(status_is_error && status_minimal) &&
            (show_device_buffer_status || show_fault_summary) && status_padding_h > 4) {
            status_padding_h -= 2;
            status_content_width = status_width - status_padding_h * 2;
            if (status_content_width < 1) status_content_width = 1;
            continue;
        }
        // As in the original tiny layout, prioritize fault details over the
        // cumulative sample count, but only when the measured row cannot fit.
        if (!(status_is_error && status_minimal) && show_device_buffer_status &&
            show_fault_summary && show_samples_count) {
            show_samples_count = false;
            continue;
        }
        break;
    }

    if (show_device_buffer_status) {
        status_device_buffer_view = device_buffer_views[device_buffer_level];
        gui_ui_ellipsize_text(app, status_device_buffer_view.caption,
            sizeof(status_device_buffer_view.caption), status_font_size, status_readout_font_id,
            device_buffer_caption_width[device_buffer_level]);
    }
    gui_device_buffer_layout_t device_buffer_layout =
        (gui_device_buffer_layout_t)device_buffer_level;
    bool device_buffer_tiny = device_buffer_layout == GUI_DEVICE_BUFFER_LAYOUT_TINY;

    /* Match the fixed rendered slots to the widths used by the budget. */
    if (sample_rate_value_width < status_sample_rate_tier_width[status_sample_rate_tier])
        sample_rate_value_width = status_sample_rate_tier_width[status_sample_rate_tier];
    if (samples_value_width < status_samples_tier_width[status_samples_tier])
        samples_value_width = status_samples_tier_width[status_samples_tier];
    if (frames_value_width < frames_text_width) frames_value_width = frames_text_width;
    if (small_counter_width < missed_text_width) small_counter_width = missed_text_width;
    if (buffer_value_width < rf_text_width) buffer_value_width = rf_text_width;

    /* Materialize the chosen tier text for rendering. Exact sample counts
     * and hardware details remain available from their status popups. */
    snprintf(status_sample_rate_display, sizeof(status_sample_rate_display), "%s",
             status_sample_rate_tier_text[status_sample_rate_tier]);
    snprintf(status_samples_display, sizeof(status_samples_display), "%s",
             status_samples_tier_text[status_samples_tier]);
    snprintf(status_free_space_display, sizeof(status_free_space_display), "%s",
             status_free_space_tier_text[show_free_space ? status_free_space_tier : 0]);
    if (!show_free_space) {
        status_free_space_display[0] = '\0';
    }
    if (show_status_message) {
        gui_ui_copy_text_prefix(status_message_display, sizeof(status_message_display), raw_status);
        if (status_message_width_budget < 24) {
            status_message_width_budget = 24;
        }
        gui_ui_ellipsize_text(app, status_message_display, sizeof(status_message_display),
                              status_font_size, 0, status_message_width_budget);
    }
    if (show_free_space && free_space_width_budget > 0) {
        gui_ui_ellipsize_text(app, status_free_space_display,
                              sizeof(status_free_space_display),
                              status_font_size, status_readout_font_id, free_space_width_budget);
    }
    gui_ui_ellipsize_text(app, status_sample_rate_display, sizeof(status_sample_rate_display),
                          status_font_size, status_readout_font_id, sample_rate_value_width);
    gui_ui_ellipsize_text(app, status_samples_display, sizeof(status_samples_display),
                          status_font_size, status_readout_font_id, samples_value_width);
    gui_ui_ellipsize_text(app, status_rf_buf_display, sizeof(status_rf_buf_display),
                          status_font_size, status_readout_font_id, buffer_value_width);
    gui_ui_ellipsize_text(app, status_aud_buf_display, sizeof(status_aud_buf_display),
                          status_font_size, status_readout_font_id, buffer_value_width);
    if (show_record_indicator) {
        gui_ui_ellipsize_text(app, status_record_timer_display,
                              sizeof(status_record_timer_display),
                              status_font_size, status_readout_font_id, record_timer_text_width);
    }
    // DdD aliases begin only when the entire sample-rate readout is hidden.
    // Pass the same-frame choice to the toolbar and return it for its dropdown;
    // do not use last frame's Clay elements or duplicate the status budget.
    bool ddd_compact_labels = !show_sample_rate || (status_is_error && status_minimal);
    render_toolbar(app, ddd_compact_labels);

    // Channels and status use the same deterministic choice in this frame.
    gui_ui_channel_spacing_t spacing = gui_ui_get_channel_spacing(
        status_compact_labels || status_tiny, gui_ui_get_render_scale().x);
    render_channels_panel(app, spacing, status_compact_labels || status_tiny);

    Clay_SizingAxis status_left_row_width = status_two_rows
        ? CLAY_SIZING_GROW(0)
        : CLAY_SIZING_FIT(0);
    Clay_SizingAxis status_right_row_width = status_two_rows
        ? CLAY_SIZING_GROW(0)
        : CLAY_SIZING_FIT(0);
    Clay_SizingAxis status_row_height = status_two_rows
        ? CLAY_SIZING_FIXED(22)
        : CLAY_SIZING_FIT(0);
    Clay_SizingAxis status_spacer_width = status_two_rows
        ? CLAY_SIZING_FIXED(0)
        : CLAY_SIZING_GROW(0);
    Clay_SizingAxis status_spacer_height = status_two_rows
        ? CLAY_SIZING_FIXED(0)
        : CLAY_SIZING_GROW(0);
    CLAY(CLAY_ID("StatusBar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(status_two_rows ? 52 : 28) },
            .layoutDirection = status_two_rows ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = status_two_rows ? CLAY_ALIGN_Y_TOP : CLAY_ALIGN_Y_CENTER },
            .padding = { status_padding_h, status_padding_h, status_two_rows ? 2 : 0, status_two_rows ? 2 : 0 },
            .childGap = status_two_rows ? 2 : status_bar_gap
        },
        .backgroundColor = to_clay_color(COLOR_TOOLBAR_BG)
    }) {
        CLAY(CLAY_ID("StatusLeft"), {
            .layout = {
                .sizing = { status_left_row_width, status_row_height },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = status_left_gap
            }
        }) {
            if (show_record_indicator) {
                CLAY(CLAY_ID("RecIndicator"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(12), CLAY_SIZING_FIXED(12) } },
                    .backgroundColor = to_clay_color(COLOR_CLIP_RED),
                    .cornerRadius = CLAY_CORNER_RADIUS(6)
                }) {}
                CLAY(CLAY_ID("RecordTimer"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(record_timer_text_width), CLAY_SIZING_FIT(0) } }
                }) {
                    CLAY_TEXT(make_string(status_record_timer_display),
                        CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
            }

            if (show_status_message) {
                CLAY(CLAY_ID("ConnectionStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(status_message_width_budget), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = status_counter_inner_gap
                    }
                }) {
                    CLAY_TEXT(make_string(status_message_display),
                        CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(status_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
            }


            if (show_free_space) {
                CLAY(CLAY_ID("FreeSpaceStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(free_space_width_budget), CLAY_SIZING_FIT(0) }
                    }
                }) {
                    CLAY_TEXT(make_string(status_free_space_display),
                        CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(free_space_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
            }
        }

        if (!status_two_rows) {
            CLAY(CLAY_ID("StatusSpacer"), {
                .layout = { .sizing = { status_spacer_width, status_spacer_height } }
            }) {}
        }

        if (!(status_is_error && status_minimal)) {
            CLAY(CLAY_ID("StatusRight"), {
                .layout = {
                    .sizing = { status_right_row_width, status_row_height },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childAlignment = {
                        .x = status_two_rows ? CLAY_ALIGN_X_RIGHT : CLAY_ALIGN_X_LEFT,
                        .y = CLAY_ALIGN_Y_CENTER
                    },
                    .childGap = status_right_gap
                }
            }) {
                if (show_sync_status) {
                    bool synced = m_status_valid ? m_status.stream_synced : atomic_load(&app->stream_synced);
                    Color sync_color = synced ? COLOR_SYNC_GREEN : COLOR_SYNC_RED;
                    CLAY(CLAY_ID("SyncStatus"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childGap = status_counter_inner_gap
                        }
                    }) {
                        CLAY_TEXT(CLAY_STRING("Sync:"),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        CLAY(CLAY_ID("SyncValue"), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(sync_value_width), CLAY_SIZING_FIT(0) } }
                        }) {
                            CLAY_TEXT(synced ? CLAY_STRING("OK") : CLAY_STRING("--"),
                                CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(sync_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }
                }

                if (show_sample_rate) {
                    CLAY(CLAY_ID("SampleRate"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(sample_rate_value_width), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_sample_rate_display),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }
                if (show_samples_count) {
                    /* Values cannot resize their reserved readout slots. */
                    CLAY(CLAY_ID("SamplesStatus"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childGap = status_counter_inner_gap
                        }
                    }) {
                        CLAY_TEXT(make_string(samples_label),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        CLAY(CLAY_ID("SamplesValue"), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(samples_value_width), CLAY_SIZING_FIT(0) } }
                        }) {
                            CLAY_TEXT(make_string(status_samples_display),
                                CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }
                    if (show_frame_count) {
                        CLAY(CLAY_ID("FrameStatus"), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                .childGap = status_counter_inner_gap
                            }
                        }) {
                            CLAY_TEXT(make_string(frames_label),
                                CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                            CLAY(CLAY_ID("FrameValue"), {
                                .layout = { .sizing = { CLAY_SIZING_FIXED(frames_value_width), CLAY_SIZING_FIT(0) } }
                            }) {
                                CLAY_TEXT(make_string(status_frames_display),
                                    CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                            }
                        }
                    }
                }

                if (show_missed_count) {
                    CLAY(CLAY_ID("MissedStatus"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childGap = status_counter_inner_gap
                        }
                    }) {
                        CLAY_TEXT(make_string(missed_label),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        CLAY(CLAY_ID("MissedValue"), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(small_counter_width), CLAY_SIZING_FIT(0) } }
                        }) {
                            CLAY_TEXT(make_string(status_missed_display),
                                CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(missed > 0 ? COLOR_CLIP_RED : COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }
                }

                if (show_error_count) {
                    CLAY(CLAY_ID("ErrorStatus"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childGap = status_counter_inner_gap
                        }
                    }) {
                        CLAY_TEXT(make_string(errors_label),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        CLAY(CLAY_ID("ErrorValue"), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(small_counter_width), CLAY_SIZING_FIT(0) } }
                        }) {
                            CLAY_TEXT(make_string(status_errors_display),
                                CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(errors > 0 ? COLOR_CLIP_RED : COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }
                }

                if (show_fault_summary) {
                    CLAY(CLAY_ID("StreamFaultStatus"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(fault_caption_width[3]), CLAY_SIZING_FIXED(status_font_size) } },
                        .backgroundColor = to_clay_color(
                            Clay_PointerOver(CLAY_ID("StreamFaultStatus"))
                                ? COLOR_BUTTON_HOVER : COLOR_TOOLBAR_BG)
                    }) {
                        CLAY_TEXT(make_string(fault_captions[fault_summary_flags]),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id,
                                .textColor = to_clay_color(COLOR_CLIP_RED), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }

                /* Device-side hardware-buffer health is separate from the host RF
                 * and audio rings below. Backends that cannot report it return no
                 * view, so this slot consumes no space for those devices. */
                if (show_device_buffer_status) {
                    const char *device_buffer_label =
                        device_buffer_layout == GUI_DEVICE_BUFFER_LAYOUT_FULL
                            ? "HW Buffer:"
                            : "HW:";
                    bool show_device_buffer_meter =
                        device_buffer_layout == GUI_DEVICE_BUFFER_LAYOUT_FULL;
                    int device_buffer_percent =
                        status_device_buffer_view.meter_percent;
                    if (device_buffer_percent < 0) device_buffer_percent = 0;
                    if (device_buffer_percent > 100) device_buffer_percent = 100;
                    int device_buffer_fill_width =
                        device_buffer_bar_width * device_buffer_percent / 100;
                    Color device_buffer_color = COLOR_SYNC_GREEN;
                    if (status_device_buffer_view.severity ==
                        GUI_DEVICE_BUFFER_ERROR) {
                        device_buffer_color = COLOR_CLIP_RED;
                    } else if (status_device_buffer_view.severity ==
                               GUI_DEVICE_BUFFER_WARNING) {
                        device_buffer_color = COLOR_METER_YELLOW;
                    }

                    CLAY(CLAY_ID("DeviceBufferStatus"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                            .childGap = device_buffer_tiny ? 2 : 4
                        },
                        .backgroundColor = to_clay_color(
                            Clay_PointerOver(CLAY_ID("DeviceBufferStatus"))
                                ? COLOR_BUTTON_HOVER : COLOR_TOOLBAR_BG)
                    }) {
                        CLAY_TEXT(make_string(device_buffer_label),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        if (show_device_buffer_meter) {
                        CLAY(CLAY_ID("DeviceBufferMeter"), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIXED(device_buffer_bar_width), CLAY_SIZING_FIXED(10) },
                                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                .childGap = 0
                            },
                            .backgroundColor = to_clay_color(COLOR_METER_BG),
                            .cornerRadius = CLAY_CORNER_RADIUS(2)
                        }) {
                            if (device_buffer_fill_width > 0) {
                                CLAY(CLAY_ID("DeviceBufferMeterFill"), {
                                    .layout = {
                                        .sizing = { CLAY_SIZING_FIXED(device_buffer_fill_width), CLAY_SIZING_GROW(0) }
                                    },
                                    .backgroundColor = to_clay_color(device_buffer_color),
                                    .cornerRadius = CLAY_CORNER_RADIUS(2)
                                }) {}
                            }
                        }
                        }
                        CLAY(CLAY_ID("DeviceBufferCaption"), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(device_buffer_caption_width[device_buffer_level]), CLAY_SIZING_FIT(0) } }
                        }) {
                            CLAY_TEXT(make_string(status_device_buffer_view.caption),
                                CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(device_buffer_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }
                }

                /* RF buffer readout formatted before the budget loop. */
                int rf_pct = status_rf_pct;
                CLAY(CLAY_ID("RFBufStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = status_counter_inner_gap
                    }
                }) {
                    CLAY_TEXT(make_string(rf_buffer_label),
                        CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    CLAY(CLAY_ID("RFBufValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(buffer_value_width), CLAY_SIZING_FIT(0) } }
                    }) {
                        Color rf_color = (rf_pct > 90) ? COLOR_CLIP_RED : (rf_pct > 75) ? COLOR_METER_YELLOW : COLOR_TEXT;
                        CLAY_TEXT(make_string(status_rf_buf_display),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(rf_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }

                /* Audio buffer readout formatted before the budget loop. */
                int aud_pct = status_aud_pct;
                CLAY(CLAY_ID("AudBufStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = status_counter_inner_gap
                    }
                }) {
                    CLAY_TEXT(make_string(audio_buffer_label),
                        CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    CLAY(CLAY_ID("AudBufValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(buffer_value_width), CLAY_SIZING_FIT(0) } }
                    }) {
                        Color aud_color = (aud_pct > 90) ? COLOR_CLIP_RED : (aud_pct > 75) ? COLOR_METER_YELLOW : COLOR_TEXT;
                        CLAY_TEXT(make_string(status_aud_buf_display),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = status_readout_font_id, .textColor = to_clay_color(aud_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }
            }
        }
    }
    return ddd_compact_labels;
}

static void render_ui_scale_hud(void)
{
    double remaining_s = s_ui_scale_hud_visible_until_s - GetTime();
    float opacity = gui_ui_scale_hud_opacity(remaining_s);
    if (opacity <= 0.0f) return;

    Color hud_background = COLOR_PANEL_BG;
    Color hud_border = COLOR_BUTTON_ACTIVE;
    Color hud_text = COLOR_TEXT;
    Color hud_hint = COLOR_TEXT_DIM;
    hud_background.a = (unsigned char)(232.0f * opacity);
    hud_border.a = (unsigned char)(220.0f * opacity);
    hud_text.a = (unsigned char)(255.0f * opacity);
    hud_hint.a = (unsigned char)(255.0f * opacity);

#if defined(__APPLE__)
    const Clay_String reset_hint = CLAY_STRING("Cmd+0 to reset to 100%");
#else
    const Clay_String reset_hint = CLAY_STRING("Ctrl+0 to reset to 100%");
#endif

    CLAY(CLAY_ID("UiScaleHud"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(224), CLAY_SIZING_FIT(0) },
            .padding = { 12, 12, 10, 10 },
            .childGap = 4,
            .childAlignment = { .x = CLAY_ALIGN_X_CENTER },
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .floating = {
            .offset = { .x = 0, .y = 10 },
            .zIndex = 1100,
            .parentId = CLAY_ID("Toolbar").id,
            .attachPoints = {
                .element = CLAY_ATTACH_POINT_CENTER_TOP,
                .parent = CLAY_ATTACH_POINT_CENTER_BOTTOM
            },
            .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
            .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID
        },
        .backgroundColor = to_clay_color(hud_background),
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = {
            .width = { 1, 1, 1, 1 },
            .color = to_clay_color(hud_border)
        }
    }) {
        CLAY_TEXT(make_string(s_ui_scale_hud_title),
            CLAY_TEXT_CONFIG({
                .fontSize = 24,
                .textColor = to_clay_color(hud_text),
                .wrapMode = CLAY_TEXT_WRAP_NONE
            }));
        CLAY_TEXT(reset_hint,
            CLAY_TEXT_CONFIG({
                .fontSize = 14,
                .textColor = to_clay_color(hud_hint),
                .wrapMode = CLAY_TEXT_WRAP_NONE
            }));
    }
}

/* Remote folder picker with server-absolute navigation.  The save directory
 * never restricts browsing: Linux root is '/', Windows root lists drives. */
static void render_monitor_folder_picker(gui_app_t *app){
    if(!s_monitor_folder_open||!(app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT)||!app->settings_panel_open)return;
    int n=gui_monitor_client_folder_count();
    char remote_path[FILE_PATH_MAX],remote_parent[FILE_PATH_MAX];
    bool recovered=false,computer=false,failed=false,more=false;
    if(gui_monitor_client_folder_state(remote_path,sizeof(remote_path),
          remote_parent,sizeof(remote_parent),&computer,&recovered,&failed,&more)){
        if(!failed){
            snprintf(s_monitor_folder_path,sizeof(s_monitor_folder_path),"%s",remote_path);
            snprintf(s_monitor_folder_parent,sizeof(s_monitor_folder_parent),"%s",remote_parent);
            s_monitor_folder_computer=computer;
            s_monitor_folder_recovered=recovered;
        }
        s_monitor_folder_error=failed;
        s_monitor_folder_more=more;
    }
    more=s_monitor_folder_more;
    int w=gui_ui_modal_max_extent(gui_ui_get_layout_width(),900);
    int h=gui_ui_modal_max_extent(gui_ui_get_layout_height(),690);
    if(!gui_ui_is_text_field_active(UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH))
        snprintf(s_monitor_folder_edit_path,sizeof(s_monitor_folder_edit_path),"%s",s_monitor_folder_path);
    /* More of the address stays visible than in the old filename-tail helper;
     * the full path is always available for editing by clicking the bar. */
    const char *address=s_monitor_folder_computer?"Server drives":s_monitor_folder_path;
    size_t addr_len=strlen(address);
    if(addr_len<=96){
        snprintf(s_monitor_folder_path_display,sizeof(s_monitor_folder_path_display),"%s",address);
    }else{
        size_t start=addr_len-93;
        while(start<addr_len&&((unsigned char)address[start]&0xc0)==0x80)start++;
        snprintf(s_monitor_folder_path_display,sizeof(s_monitor_folder_path_display),"...%s",address+start);
    }
    CLAY(CLAY_ID("RemoteBrowseShade"), {
        .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_GROW(0)}},
        .floating={.attachTo=CLAY_ATTACH_TO_ROOT,.zIndex=1500,
                   .attachPoints={.element=CLAY_ATTACH_POINT_LEFT_TOP,.parent=CLAY_ATTACH_POINT_LEFT_TOP}},
        .backgroundColor=(Clay_Color){0,0,0,190}
    }){}
    CLAY(CLAY_ID("RemoteBrowseDialog"), {
        .layout={.sizing={CLAY_SIZING_FIXED(w),CLAY_SIZING_FIXED(h)},
                 .layoutDirection=CLAY_TOP_TO_BOTTOM,.padding={20,20,16,16},.childGap=10},
        .floating={.attachTo=CLAY_ATTACH_TO_ROOT,.zIndex=1501,
                   .attachPoints={.element=CLAY_ATTACH_POINT_CENTER_CENTER,.parent=CLAY_ATTACH_POINT_CENTER_CENTER}},
        .backgroundColor=to_clay_color(COLOR_PANEL_BG),.cornerRadius=CLAY_CORNER_RADIUS(10)
    }){
        CLAY_TEXT(CLAY_STRING("Choose recording folder on server"),
            CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_TITLE,.textColor=to_clay_color(COLOR_TEXT)}));
        CLAY(CLAY_ID("RemoteBrowseCurrent"),{
            .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(48)},
                     .padding={10,10,8,8},.childAlignment={.y=CLAY_ALIGN_Y_CENTER}},
            .backgroundColor=(Clay_Color){24,26,32,255},.cornerRadius=CLAY_CORNER_RADIUS(5)
        }){
            if(gui_ui_is_text_field_active(UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH)){
                gui_ui_render_active_text(UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH,s_monitor_folder_edit_path,
                    FONT_SIZE_NORMAL,0,COLOR_TEXT);
            }else{
                CLAY_TEXT(make_string(s_monitor_folder_path_display),
                    CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,.textColor=to_clay_color(COLOR_TEXT)}));
            }
        }
        CLAY(CLAY_ID("RemoteBrowseNavigation"),{
            .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(36)},
                     .layoutDirection=CLAY_LEFT_TO_RIGHT,.childGap=8}
        }){
            CLAY(CLAY_ID("RemoteBrowseUp"),{
                .layout={.sizing={CLAY_SIZING_FIXED(60),CLAY_SIZING_FIXED(34)},
                         .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
                .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(5)
            }){CLAY_TEXT(CLAY_STRING("Up"),CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT)}));}
            CLAY(CLAY_ID("RemoteBrowseRoot"),{
                .layout={.sizing={CLAY_SIZING_FIXED(175),CLAY_SIZING_FIXED(34)},
                         .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
                .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(5)
            }){CLAY_TEXT(CLAY_STRING("Filesystem / Drives"),CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT)}));}
            CLAY(CLAY_ID("RemoteBrowseHome"),{
                .layout={.sizing={CLAY_SIZING_FIXED(75),CLAY_SIZING_FIXED(34)},
                         .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
                .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(5)
            }){CLAY_TEXT(CLAY_STRING("Home"),CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT)}));}
            CLAY(CLAY_ID("RemoteBrowseRecordings"),{
                .layout={.sizing={CLAY_SIZING_FIXED(175),CLAY_SIZING_FIXED(34)},
                         .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
                .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(5)
            }){CLAY_TEXT(CLAY_STRING("Current recording"),CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT)}));}
            CLAY(CLAY_ID("RemoteBrowseRefresh"),{
                .layout={.sizing={CLAY_SIZING_FIXED(80),CLAY_SIZING_FIXED(34)},
                         .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
                .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(5)
            }){CLAY_TEXT(CLAY_STRING("Refresh"),CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT)}));}
        }
        CLAY_TEXT(CLAY_STRING("Click the address above to type any server path; press Enter to open it."),
            CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT_DIM)}));
        if(s_monitor_folder_error){
            CLAY_TEXT(CLAY_STRING("Folder listing failed (access denied or connection lost). Use Up, Home, or Drives."),
                CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT_DIM)}));
        }else if(s_monitor_folder_recovered){
            CLAY_TEXT(CLAY_STRING("Previous folder was unavailable; showing the server filesystem root."),
                CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT_DIM)}));
        }
        CLAY(CLAY_ID("RemoteBrowseContents"),{
            .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_GROW(0)},
                     .layoutDirection=CLAY_TOP_TO_BOTTOM,.childGap=3,.padding={6,6,6,6}},
            .backgroundColor=(Clay_Color){24,26,32,255},.cornerRadius=CLAY_CORNER_RADIUS(5)
        }){
            if(n==-1)CLAY_TEXT(CLAY_STRING("Loading server folders..."),
                 CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,.textColor=to_clay_color(COLOR_TEXT_DIM)}));
            else if(n==0)CLAY_TEXT(CLAY_STRING("No subfolders (you may choose this folder)"),
                 CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,.textColor=to_clay_color(COLOR_TEXT_DIM)}));
            for(int i=0;i<n&&i<12;i++){
                if(!gui_monitor_client_folder_name(i,s_monitor_folder_labels[i],sizeof(s_monitor_folder_labels[i])))continue;
                CLAY(CLAY_IDI("RemoteBrowseItem",i),{
                    .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(25)},
                             .padding={10,8,0,0},.childAlignment={.y=CLAY_ALIGN_Y_CENTER}},
                    .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(3)
                }){
                    CLAY_TEXT(CLAY_STRING("[DIR]"),CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT_DIM)}));
                    CLAY_TEXT(make_string(s_monitor_folder_labels[i]),
                        CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_STATS,.textColor=to_clay_color(COLOR_TEXT)}));
                }
            }
        }
        CLAY(CLAY_ID("RemoteBrowsePager"),{
            .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(32)},
                     .layoutDirection=CLAY_LEFT_TO_RIGHT,.childGap=10}
        }){
            CLAY(CLAY_ID("RemoteBrowsePrev"),{
                .layout={.sizing={CLAY_SIZING_FIXED(100),CLAY_SIZING_FIXED(30)},
                         .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
                .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(4)
            }){CLAY_TEXT(CLAY_STRING("Previous"),CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,.textColor=to_clay_color(COLOR_TEXT)}));}
            CLAY(CLAY_ID("RemoteBrowseNext"),{
                .layout={.sizing={CLAY_SIZING_FIXED(100),CLAY_SIZING_FIXED(30)},
                         .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
                .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(4)
            }){CLAY_TEXT(more?CLAY_STRING("Next >"):CLAY_STRING("Last page"),
                CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,.textColor=to_clay_color(COLOR_TEXT)}));}
        }
        CLAY(CLAY_ID("RemoteBrowseActions"),{
            .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(36)},
                     .layoutDirection=CLAY_LEFT_TO_RIGHT,.childGap=14}
        }){
            CLAY(CLAY_ID("RemoteBrowseCancel"),{
                .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(36)},
                         .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
                .backgroundColor=to_clay_color(COLOR_BUTTON),.cornerRadius=CLAY_CORNER_RADIUS(5)
            }){CLAY_TEXT(CLAY_STRING("Cancel"),CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,.textColor=to_clay_color(COLOR_TEXT)}));}
            CLAY(CLAY_ID("RemoteBrowseSelect"),{
                .layout={.sizing={CLAY_SIZING_GROW(0),CLAY_SIZING_FIXED(36)},
                         .childAlignment={.x=CLAY_ALIGN_X_CENTER,.y=CLAY_ALIGN_Y_CENTER}},
                .backgroundColor=to_clay_color(COLOR_BUTTON_ACTIVE),.cornerRadius=CLAY_CORNER_RADIUS(5)
            }){CLAY_TEXT(CLAY_STRING("Use this folder"),CLAY_TEXT_CONFIG({.fontSize=FONT_SIZE_NORMAL,.textColor=to_clay_color(COLOR_TEXT)}));}
        }
    }
}


// Main layout function
void gui_render_layout(gui_app_t *app) {
    gui_ui_sync_capture_mode_state(app);
    bool ddd_compact_labels = false;
    // Root container
    CLAY(CLAY_ID("Root"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        // Apply cached hsdaoh status/errors at a low rate (2s)
        gui_capture_poll_hsdaoh_status(app);

        // Toolbar, main content and status share this frame's layout choices.
        ddd_compact_labels = render_main_panels(app);
    }

    // Settings panel overlay (if open)
    render_settings_panel(app);
    render_monitor_folder_picker(app);

    // Record-limit popup overlay (if open)
    if (!gui_ui_selected_device_is_playback(app)) {
        render_record_limit_window(app);
    }

    // Version info popup overlay (if open)
    render_version_info_window(app);
    // Metadata popup overlay (if open)
    render_metadata_window(app);

    // Device dropdown overlay (if open)
    if (gui_dropdown_is_open(DROPDOWN_DEVICE, 0) && app->device_count > 0) {
        int overlay_screen_width = gui_ui_get_layout_width();
        bool overlay_toolbar_two_rows = s_toolbar_uses_two_rows;
        bool overlay_toolbar_tiny = overlay_screen_width < 760;
        bool overlay_toolbar_ultra_narrow = overlay_screen_width < 900;
        bool overlay_toolbar_very_narrow = overlay_screen_width < 1020;
        int overlay_text_size = overlay_toolbar_very_narrow ? FONT_SIZE_DROPDOWN : FONT_SIZE_NORMAL;
        int overlay_padding = overlay_toolbar_very_narrow ? 6 : 10;
        int overlay_row_height = overlay_toolbar_tiny ? 24 : 28;
        int overlay_min_width = overlay_toolbar_tiny ? 100 : (overlay_toolbar_ultra_narrow ? 120 : (overlay_toolbar_very_narrow ? 138 : 160));
        int overlay_max_width = overlay_toolbar_tiny ? 190 : (overlay_toolbar_ultra_narrow ? 220 : (overlay_toolbar_very_narrow ? 250 : 320));
        int device_dropdown_overlay_width = overlay_min_width;
        Clay_ElementData device_dropdown_data = Clay_GetElementData(CLAY_ID("DeviceDropdown"));
        if (device_dropdown_data.found) {
            device_dropdown_overlay_width = gui_ui_clamp_int((int)roundf(device_dropdown_data.boundingBox.width),
                                                             overlay_min_width,
                                                             overlay_max_width);
        }
        for (int i = 0; i < app->device_count; i++) {
            bool ddd_option = app->devices[i].type == DEVICE_TYPE_DDD;
            int option_max_width = overlay_max_width;
            if (ddd_option && !ddd_compact_labels) {
                int left = device_dropdown_data.found
                    ? (int)ceilf(device_dropdown_data.boundingBox.x) : 0;
                option_max_width = overlay_screen_width - left - overlay_padding;
            }
            gui_ui_format_device_label(&app->devices[i],
                                        ddd_option ? ddd_compact_labels : overlay_toolbar_very_narrow,
                                        s_device_option_labels[i],
                                        sizeof(s_device_option_labels[i]));
            int option_width = gui_ui_measure_button_width(app,
                                                           s_device_option_labels[i],
                                                           overlay_text_size,
                                                           overlay_padding,
                                                           16,
                                                           overlay_min_width,
                                                           option_max_width);
            if (option_width > device_dropdown_overlay_width) {
                device_dropdown_overlay_width = option_width;
            }
        }
        for (int i = 0; i < app->device_count; i++) {
            gui_ui_ellipsize_text(app, s_device_option_labels[i],
                                  sizeof(s_device_option_labels[i]),
                                  overlay_text_size,
                                  0,
                                  device_dropdown_overlay_width - overlay_padding * 2);
        }
        CLAY(CLAY_ID("DeviceDropdownOverlay"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(device_dropdown_overlay_width), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .floating = {
                .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                .parentId = CLAY_ID("DeviceDropdown").id,
                .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM },
                .offset = { .x = 0, .y = overlay_toolbar_two_rows ? 44 : 0 }
            },
            .backgroundColor = to_clay_color(COLOR_PANEL_BG),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            for (int i = 0; i < app->device_count; i++) {
                bool opt_hover = Clay_PointerOver(CLAY_IDI("DeviceOption", i));
                Color item_color = gui_dropdown_option_color(i == app->selected_device, opt_hover);

                CLAY(CLAY_IDI("DeviceOption", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(overlay_row_height) },
                        .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                        .padding = { overlay_padding, overlay_padding, 0, 0 }
                    },
                    .backgroundColor = to_clay_color(item_color)
                }) {
                    CLAY_TEXT(make_string(s_device_option_labels[i]),
                        CLAY_TEXT_CONFIG({ .fontSize = overlay_text_size, .textColor = to_clay_color(COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
            }
        }
    }

    // Transient, pointer-transparent zoom feedback. It is attached to the
    // toolbar so its position follows both the one-row and wrapped layouts.
    render_ui_scale_hud();

    // Popup overlay (renders on top of everything)
    gui_popup_render();
}

#if defined(__ANDROID__)
/* Per-frame poll for async Android results (USB permission + file/folder
 * pickers). These run async so the render thread never blocks across a
 * system Activity transition (permission dialog / SAF picker), which would
 * destroy our EGL surface and hang the app on resume. */
static void gui_ui_poll_android_results(gui_app_t *app)
{
    extern int android_poll_picker_result(int *out_kind, int *out_ok, char *out_path, size_t out_path_len);
    extern int android_poll_usb_permission_result(int *out_granted);
    extern int android_permission_pending(void);
    extern int android_usb_has_fd(void);
    extern int android_usb_device_present(void);

    /* Re-enumerate the device list whenever the Java-reported physical USB
     * presence changes, so the MS2130 entry appears when plugged in and
     * disappears when unplugged (instead of being a permanent phantom that
     * shifted selection and broke the Test Signal device). Skip while
     * capturing or while a start/stop worker is in flight. */
    static int s_last_usb_present = -1;
    int usb_present_now = android_usb_device_present();
    if (usb_present_now != s_last_usb_present) {
        if (!app->is_capturing && !gui_app_capture_busy() && !gui_playback_is_starting(app)) {
            int prev_selected_type = -1;
            if (app->selected_device >= 0 && app->selected_device < app->device_count) {
                prev_selected_type = (int)app->devices[app->selected_device].type;
            }
            gui_app_enumerate_devices(app);
            /* Keep the user's selection on the same device type if possible
             * (e.g. stay on Test Signal when the MS2130 appears). */
            if (prev_selected_type >= 0) {
                for (int i = 0; i < app->device_count; i++) {
                    if ((int)app->devices[i].type == prev_selected_type) {
                        app->selected_device = i;
                        break;
                    }
                }
            }
            s_last_usb_present = usb_present_now;
        }
        /* else: retry on a later frame once capture/start/stop settles */
    }

    /* Picker results -> apply to settings. */
    int pkind = 0, pok = 0;
    char ppath[FILE_PATH_MAX];
    if (android_poll_picker_result(&pkind, &pok, ppath, sizeof(ppath))) {
        fprintf(stderr, "[GUI] Android picker result: kind=%d ok=%d path='%s'\n",
                pkind, pok, ppath);
        if (gui_playback_is_starting(app)) {
            gui_app_set_status(app, "File settings are locked while playback prepares");
        } else if (pkind == 1) { /* ANDROID_PICKER_KIND_OUTPUT_DIR */
            if (pok && ppath[0]) {
                snprintf(app->settings.output_path, sizeof(app->settings.output_path), "%s", ppath);
                gui_settings_save(&app->settings);
                char msg[256];
                snprintf(msg, sizeof(msg), "Output folder set: %s", ppath);
                gui_app_set_status(app, msg);
            } else if (pok) {
                /* ok but empty path: use the scoped-storage-exempt app dir. */
                const char *def = android_get_storage_path();
                if (def && def[0]) {
                    snprintf(app->settings.output_path, sizeof(app->settings.output_path), "%s", def);
                    gui_settings_save(&app->settings);
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Output folder set (app dir): %s", def);
                    gui_app_set_status(app, msg);
                } else {
                    gui_app_set_status(app, "No folder selected");
                }
            } else {
                gui_app_set_status(app, "No folder selected");
            }
        } else if (pkind == 2 || pkind == 3) { /* PLAYBACK_A / PLAYBACK_B */
            if (pok && ppath[0]) {
                char *dst = (pkind == 3) ? app->settings.playback_file_b : app->settings.playback_file_a;
                snprintf(dst, sizeof(app->settings.playback_file_a), "%s", ppath);
                gui_settings_save(&app->settings);
                gui_app_set_status(app, (pkind == 3) ? "Channel B playback file set" : "Channel A playback file set");
            } else {
                gui_app_set_status(app, "No file selected");
            }
        }
    }

    /* USB permission result -> complete the connect that the Connect button
     * (or auto-reconnect) launched asynchronously. Run the (potentially slow)
     * capture startup on a worker thread so the render loop stays alive. */
    if (android_permission_pending()) {
        int granted = 0;
        if (android_poll_usb_permission_result(&granted)) {
            if (gui_playback_is_starting(app) ||
                gui_ui_selected_device_is_simulated(app) ||
                gui_ui_selected_device_is_playback(app) ||
                gui_net_is_client(app)) {
                /* A USB response can arrive after the user has changed
                 * sources. Never start Test Signal, playback or a network
                 * client as a side effect of a stale USB permission result. */
            } else if (granted && android_usb_has_fd()) {
                gui_app_set_status(app, "USB permission granted, starting capture...");
                gui_app_start_capture_async(app);
            } else {
                gui_app_set_status(app, "USB permission denied or no USB capture device found");
                app->reconnect_pending = false;
                app->reconnect_attempts = 0;
            }
        }
    }
}
#endif

// Handle UI interactions
void gui_handle_interactions(gui_app_t *app) {
#if defined(__ANDROID__)
    if (gui_android_viewport_block_controls()) {
        gui_ui_set_click_consumed();
        return;
    }
#endif
#if defined(__APPLE__)
    if(gui_cxadc_macos_ui_handle(app))return;
#endif
    // Reset click consumed flag at start of each frame
    s_ui_consumed_click = false;
#if defined(__ANDROID__)
    gui_ui_poll_android_results(app);
#endif
    gui_ui_sync_capture_mode_state(app);
    if (gui_playback_is_starting(app)) {
        if (gui_dropdown_is_open(DROPDOWN_DEVICE, 0)) gui_dropdown_close_all();
        gui_ui_clear_text_edit();
    }
    gui_record_limit_runtime_tick(app);
    gui_ui_update_check_tick(app);
    // Re-default level-autostop Vpp when the active backend type changes so the
    // mV readout matches the selected hardware (hsdaoh uses its 1/2 Vpp jumper
    // memory; other backends use their verified default Vpp).
    gui_ui_level_autostop_reconcile_vpp(app);
    bool playback_mode = gui_ui_selected_device_is_playback(app);
    if (playback_mode) {
        s_record_limit_window_open = false;
        s_record_limit_timecode_edit = false;
    }
    if (!IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        s_playback_scrub_active = false;
    } else if (s_playback_scrub_active &&
               app->is_capturing &&
               playback_mode &&
               !s_record_limit_window_open &&
               !s_version_info_window_open &&
               !s_metadata_window_open) {
        if (!gui_ui_seek_playback_from_track(app, s_playback_scrub_track_index,
                                             gui_ui_get_mouse_position().x)) {
            s_playback_scrub_active = false;
        }
    }

    // Playback timeline mouse-wheel scrubbing (HARD DEV NOTE above the file
    // info helpers): wheel over a playback timeline row seeks that channel
    // by 1-10 s of stream time with burst acceleration. Wheel-up steps back,
    // wheel-down steps forward.
    {
        static double s_wheel_step_s = 1.0;
        static double s_wheel_last_time = -1000.0;
        if (playback_mode && app->is_capturing &&
            !s_record_limit_window_open && !s_version_info_window_open &&
            !s_metadata_window_open && !app->settings_panel_open) {
            float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) {
                int wheel_channel = -1;
                for (int ch = 0; ch < 2; ch++) {
                    if (Clay_PointerOver(CLAY_IDI("PlaybackTimelineRow", ch))) {
                        wheel_channel = ch;
                        break;
                    }
                }
                if (wheel_channel >= 0) {
                    double now = GetTime();
                    double gap = now - s_wheel_last_time;
                    s_wheel_last_time = now;
                    // Acceleration: notches within 400 ms ramp the step
                    // (+0.75 s per notch, 1 s .. 10 s); an idle gap resets to
                    // the 1 s base step.
                    if (gap > 0.4 || gap < 0.0) {
                        s_wheel_step_s = 1.0;
                    } else {
                        s_wheel_step_s += fabs((double)wheel) * 0.75;
                        if (s_wheel_step_s > 10.0) s_wheel_step_s = 10.0;
                    }
                    uint64_t total = 0;
                    double duration = 0.0;
                    if (gui_ui_playback_channel_timeline_info(app, wheel_channel,
                                                               &total, &duration, NULL, NULL) &&
                        total > 1 && duration > 0.0) {
                        double rate_hz = (double)total / duration;
                        uint64_t step = (uint64_t)(s_wheel_step_s * rate_hz);
                        if (step == 0) step = 1;
                        uint64_t pos = gui_playback_get_position_samples_channel(app, wheel_channel);
                        uint64_t target;
                        if (wheel > 0.0f) {
                            target = (pos > step) ? (pos - step) : 0;
                        } else {
                            target = (total - 1 > pos + step) ? (pos + step) : (total - 1);
                        }
                        gui_playback_seek_sample_channel(app, wheel_channel, target);
                    }
                }
            }
        }
    }

    if (s_record_limit_window_open && !s_record_limit_timecode_edit && IsKeyPressed(KEY_ESCAPE)) {
        s_record_limit_window_open = false;
    }
    if (s_version_info_window_open && IsKeyPressed(KEY_ESCAPE)) {
        // ESC closes the hold-press mode menu first; only a second ESC (or a
        // menu already closed) dismisses the window.
        if (s_net_mode_menu_open) {
            s_net_mode_menu_open = false;
            s_net_mode_press_armed = false;
            s_net_mode_hold_fired = false;
        } else {
            s_version_info_window_open = false;
            s_update_install_armed = false;
            s_net_mode_press_armed = false;
            s_net_mode_hold_fired = false;
        }
    }
    if (s_metadata_window_open && IsKeyPressed(KEY_ESCAPE)) {
        s_metadata_window_open = false;
    }


    // Level autostop fields are edited inside the record-limit (timer) window,
    // so keep processing their keystrokes even while that window is open.
    bool las_text_field_active = (s_active_text_field == UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL ||
                                  s_active_text_field == UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION);
    bool metadata_text_field_active =
        (s_active_text_field == UI_TEXT_FIELD_INGEST_PROJECT ||
         s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_ID ||
         s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_FORMAT ||
         s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_SIZE ||
         s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_SPEED ||
         s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_CONDITION ||
         s_active_text_field == UI_TEXT_FIELD_INGEST_OPERATOR ||
         s_active_text_field == UI_TEXT_FIELD_INGEST_LOCATION ||
         s_active_text_field == UI_TEXT_FIELD_INGEST_NOTES);
    // Network (Server/Client) fields live in the info ("About") window.
    bool net_text_field_active =
        (s_active_text_field == UI_TEXT_FIELD_NET_SERVER_PORT ||
         s_active_text_field == UI_TEXT_FIELD_NET_CLIENT_HOST ||
         s_active_text_field == UI_TEXT_FIELD_NET_CLIENT_PORT);
    if (las_text_field_active && s_record_limit_window_open) {
        gui_ui_handle_active_text_edit(app);
    } else if (net_text_field_active && s_version_info_window_open &&
               !s_record_limit_window_open && !s_metadata_window_open) {
        gui_ui_handle_active_text_edit(app);
        // Persist on each edit so the port/host survives a mode apply triggered
        // elsewhere; the string mirror is the source of truth while editing.
        gui_settings_save(&app->settings);
    } else if (metadata_text_field_active && s_metadata_window_open &&
               !s_record_limit_window_open && !s_version_info_window_open) {
        gui_ui_handle_active_text_edit(app);
    } else if ((!app->settings_panel_open && !s_metadata_window_open) ||
               s_record_limit_window_open || s_version_info_window_open) {
        gui_ui_clear_text_edit();
    } else {
        gui_ui_handle_active_text_edit(app);
    }

    if (s_record_limit_window_open && s_record_limit_timecode_edit) {
        gui_ui_clear_text_edit();
        s_record_limit_cursor_char = record_limit_nearest_digit_cursor_char(s_record_limit_cursor_char);
#if defined(__ANDROID__)
        {
            char android_text[128];
            size_t android_len = android_drain_text_input(android_text, sizeof(android_text));
            for (size_t i = 0; i < android_len; i++) {
                unsigned char ach = (unsigned char)android_text[i];
                if (ach == '\0') break;
                if (ach >= '0' && ach <= '9') {
                    s_record_limit_timecode_edit_buffer[s_record_limit_cursor_char] = (char)ach;
                    s_record_limit_cursor_char = record_limit_move_cursor_char(s_record_limit_cursor_char, +1);
                } else if (ach == ':' || ach == '/') {
                    if (s_record_limit_cursor_char <= 1) {
                        s_record_limit_cursor_char = 3;
                    } else if (s_record_limit_cursor_char <= 4) {
                        s_record_limit_cursor_char = 6;
                    }
                } else if (ach == '\b' || ach == 127) {
                    s_record_limit_cursor_char = record_limit_move_cursor_char(s_record_limit_cursor_char, -1);
                    s_record_limit_timecode_edit_buffer[s_record_limit_cursor_char] = '0';
                }
            }
        }
#endif

        int ch = GetCharPressed();
        while (ch > 0) {
            if (ch >= '0' && ch <= '9') {
                s_record_limit_timecode_edit_buffer[s_record_limit_cursor_char] = (char)ch;
                s_record_limit_cursor_char = record_limit_move_cursor_char(s_record_limit_cursor_char, +1);
            } else if (ch == ':' || ch == '/') {
                if (s_record_limit_cursor_char <= 1) {
                    s_record_limit_cursor_char = 3;
                } else if (s_record_limit_cursor_char <= 4) {
                    s_record_limit_cursor_char = 6;
                }
            }
            ch = GetCharPressed();
        }

        if (IsKeyPressed(KEY_LEFT)) {
            s_record_limit_cursor_char = record_limit_move_cursor_char(s_record_limit_cursor_char, -1);
        }
        if (IsKeyPressed(KEY_RIGHT)) {
            s_record_limit_cursor_char = record_limit_move_cursor_char(s_record_limit_cursor_char, +1);
        }
        if (IsKeyPressed(KEY_HOME)) {
            s_record_limit_cursor_char = 0;
        }
        if (IsKeyPressed(KEY_END)) {
            s_record_limit_cursor_char = 7;
        }

        if (IsKeyPressed(KEY_BACKSPACE)) {
            s_record_limit_backspace_repeat_at = GetTime() + 0.25;
            s_record_limit_cursor_char = record_limit_move_cursor_char(s_record_limit_cursor_char, -1);
            s_record_limit_timecode_edit_buffer[s_record_limit_cursor_char] = '0';
        } else if (IsKeyDown(KEY_BACKSPACE)) {
            double now = GetTime();
            if (now >= s_record_limit_backspace_repeat_at) {
                s_record_limit_backspace_repeat_at = now + 0.05;
                s_record_limit_cursor_char = record_limit_move_cursor_char(s_record_limit_cursor_char, -1);
                s_record_limit_timecode_edit_buffer[s_record_limit_cursor_char] = '0';
            }
        }
        if (IsKeyPressed(KEY_DELETE)) {
            s_record_limit_timecode_edit_buffer[s_record_limit_cursor_char] = '0';
        }

        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            uint32_t parsed = 0;
            if (parse_record_limit_timecode(s_record_limit_timecode_edit_buffer, &parsed) && parsed > 0) {
                s_record_limit_seconds = parsed;
                format_record_limit_timecode(s_record_limit_timecode, sizeof(s_record_limit_timecode), parsed);
                s_record_limit_timecode_edit = false;
                gui_record_limit_sync_settings(app);
                gui_record_limit_log_state(app, "Record timer updated");
            } else {
                gui_app_set_status(app, "Invalid record limit timecode");
            }
        } else if (IsKeyPressed(KEY_ESCAPE)) {
            s_record_limit_timecode_edit = false;
        }
    }

    // Deferred help popups: set by the capture thread, consumed here
    // at the top of gui_handle_interactions BEFORE
    // gui_popup_handle_interactions runs, so the popup is open by the
    // time the modal consumer runs.
    if (!gui_popup_is_open()) {
        if (atomic_exchange(&app->cxadc_perm_help_pending, false)) {
            gui_dropdown_close_all();
            gui_ui_clear_text_edit();
#if defined(_WIN32)
            // cxadc-win driver: the 10-bit toggle is a WMI config write
            // (CxadcWin_Config.EnableTenbit). Windows denies that write for
            // non-admin users; re-running elevated lets it through.
            gui_popup_info("CXADC 10-bit mode needs elevation",
                "10-bit mode requires a config write to the cxadc-win driver\n"
                "(WMI CxadcWin_Config), which Windows denied for this user.\n"
                "Capture fell back to 8-bit.\n\n"
                "Close and re-run MISRC GUI as Administrator\n"
                "(right-click -> Run as administrator), then re-enable 10-bit.\n\n"
                "8-bit capture needs no elevation and still works.");
#else
            gui_popup_info("CXADC 10-bit mode needs one-time setup",
                "10-bit mode requires write access to the cxadc sysfs parameters,\n"
                "which are root-only by default. Capture fell back to 8-bit.\n\n"
                "Run this one-time setup to enable 10-bit for non-root users:\n\n"
                "  sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*\n"
                "  sudo chmod g+w   /sys/class/cxadc/cxadc*/device/parameters/*\n"
                "  sudo usermod -aG video $USER   (then log out/in)\n\n"
                "8-bit capture needs no setup and still works.");
#endif
        } else if (atomic_exchange(&app->cxadc_open_perm_help_pending, false)) {
            gui_dropdown_close_all();
            gui_ui_clear_text_edit();
            gui_popup_info("CXADC card device permission denied",
                "The /dev/cxadcN device nodes are root-only by default,\n"
                "so capture could not open the card.\n\n"
                "Fix this with a one-time udev rule + group membership:\n\n"
                "  sudo chgrp video /dev/cxadc*\n"
                "  sudo chmod g+rw /dev/cxadc*\n"
                "  sudo usermod -aG video $USER   (then log out/in)\n\n"
                "Or add a udev rule so it persists across reboots:\n\n"
                "  echo 'KERNEL==\"cxadc*\", GROUP=\"video\", MODE=\"0660\"' | \\\n"
                "  sudo tee /etc/udev/rules.d/99-cxadc.rules\n"
                "  sudo udevadm control --reload-rules\n\n"
                "Then log out and back in so the video group takes effect.\n");
        }
    }

    // Handle popup interactions first (modal behavior)
    if (gui_popup_handle_interactions()) {
        s_ui_consumed_click = true;
        return;  // Popup consumed the interaction
    }

    // Handle clicks
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        if(s_monitor_folder_open&&(app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT)){
            if(gui_ui_is_text_field_active(UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH) &&
               !Clay_PointerOver(CLAY_ID("RemoteBrowseCurrent")) &&
               !Clay_PointerOver(CLAY_ID("RemoteBrowseSelect"))){
                /* Navigation cancels an unfinished address edit, not a save. */
                snprintf(s_monitor_folder_edit_path,sizeof(s_monitor_folder_edit_path),"%s",s_monitor_folder_path);
                gui_ui_clear_text_edit();
            }
            if(Clay_PointerOver(CLAY_ID("RemoteBrowseCurrent"))){
                gui_ui_begin_text_edit(app,UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH,
                    CLAY_ID("RemoteBrowseCurrent"),10.0f,10.0f);
            }else if(Clay_PointerOver(CLAY_ID("RemoteBrowseCancel")) ||
               Clay_PointerOver(CLAY_ID("RemoteBrowseShade"))){
                /* Close first so text-edit cleanup cannot navigate. */
                s_monitor_folder_open=false;
                if(gui_ui_is_text_field_active(UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH))
                    gui_ui_clear_text_edit();
            }else if(Clay_PointerOver(CLAY_ID("RemoteBrowseRoot"))){
                s_monitor_folder_page=0;
                gui_monitor_client_browse_folders("",0);
            }else if(Clay_PointerOver(CLAY_ID("RemoteBrowseHome"))){
                s_monitor_folder_page=0;
                gui_monitor_client_browse_folders("~",0);
            }else if(Clay_PointerOver(CLAY_ID("RemoteBrowseRecordings"))){
                s_monitor_folder_page=0;
                gui_monitor_client_browse_folders(app->settings.output_path,0);
            }else if(Clay_PointerOver(CLAY_ID("RemoteBrowseRefresh"))){
                gui_monitor_client_browse_folders(s_monitor_folder_path,(unsigned)s_monitor_folder_page);
            }else if(Clay_PointerOver(CLAY_ID("RemoteBrowseUp"))){
                s_monitor_folder_page=0;
                gui_monitor_client_browse_folders(s_monitor_folder_parent,0);
            }else if(Clay_PointerOver(CLAY_ID("RemoteBrowsePrev"))){
                if(s_monitor_folder_page>0){
                    --s_monitor_folder_page;
                    gui_monitor_client_browse_folders(s_monitor_folder_path,(unsigned)s_monitor_folder_page);
                }
            }else if(Clay_PointerOver(CLAY_ID("RemoteBrowseNext"))){
                if(s_monitor_folder_more){
                    ++s_monitor_folder_page;
                    gui_monitor_client_browse_folders(s_monitor_folder_path,(unsigned)s_monitor_folder_page);
                }
            }else if(Clay_PointerOver(CLAY_ID("RemoteBrowseSelect"))){
                if(gui_ui_is_text_field_active(UI_TEXT_FIELD_MONITOR_SERVER_BROWSER_PATH)){
                    gui_ui_clear_text_edit();
                    gui_ui_set_click_consumed();return;
                }
                /* Computer is virtual; cannot be used as a recording folder. */
                if(!s_monitor_folder_computer && !s_monitor_folder_error &&
                   gui_monitor_client_folder_count()>=0){
                    gui_monitor_client_select_output(s_monitor_folder_path);
                    gui_app_set_status(app,"Server Output folder change requested; awaiting server confirmation");
                    s_monitor_folder_open=false;
                }
            }else{
                int count=gui_monitor_client_folder_count();
                for(int i=0;i<count&&i<12;i++){
                    if(!Clay_PointerOver(CLAY_IDI("RemoteBrowseItem",i)))continue;
                    char name[256];
                    if(gui_monitor_client_folder_name(i,name,sizeof(name))){
                        char selected[FILE_PATH_MAX];
                        if(s_monitor_folder_computer){
                            snprintf(selected,sizeof(selected),"%s",name);
                        }else{
                            size_t len=strlen(s_monitor_folder_path);
                            bool trailing=len&&(s_monitor_folder_path[len-1]=='/'||s_monitor_folder_path[len-1]=='\\');
                            const char sep=(strchr(s_monitor_folder_path,'\\')||
                                           (len>=2&&s_monitor_folder_path[1]==':'))?'\\':'/';
                            int written=snprintf(selected,sizeof(selected),"%s%s%s",s_monitor_folder_path,
                                trailing?"":(sep=='\\'?"\\":"/"),name);
                            if(written<0||(size_t)written>=sizeof(selected))break;
                        }
                        s_monitor_folder_page=0;
                        gui_monitor_client_browse_folders(selected,0);
                    }
                    break;
                }
            }
            gui_ui_set_click_consumed();
            return;
        }
        // Version info popup modal interactions (consume before toolbar underneath)
        if (s_version_info_window_open) {
            // Net mode hold-press menu takes priority over everything else in
            // the modal while it is open: an item click switches directly, any
            // other click (toggle included) just closes the menu.
            if (s_net_mode_menu_open) {
                static const int menu_ids[4] = {
                    GUI_NET_MODE_LOCAL, GUI_NET_MODE_SERVER,
                    GUI_NET_MODE_CLIENT, GUI_NET_MODE_MONITOR_CLIENT
                };
                bool on_item = false;
                for (int i = 0; i < 4 && !on_item; i++) {
                    if (Clay_PointerOver(CLAY_IDI("VersionInfoNetModeMenuItem", i))) {
                        on_item = true;
                        s_net_mode_menu_open = false;
                        gui_ui_net_mode_apply(app, menu_ids[i]);
                    }
                }
                if (!on_item) s_net_mode_menu_open = false;
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoCheckUpdateButton"))) {
                gui_ui_process_update_check_result(app);
                if (atomic_load_explicit(&s_update_check_running, memory_order_acquire)) {
                    gui_app_set_status(app, "Update check already running");
                } else if (gui_ui_start_update_check(true)) {
                    gui_app_set_status(app, "Checking for updates...");
                } else {
                    gui_app_set_status(app, "Unable to start update check");
                }
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoReleasesPageButton"))) {
                gui_ui_process_update_check_result(app);
                if (atomic_load_explicit(&s_update_check_running, memory_order_acquire)) {
                    gui_app_set_status(app, "Please wait for update check to finish");
                    gui_ui_set_click_consumed();
                    return;
                }
                // Always the releases PAGE (never a per-platform asset URL
                // that can 404): manifest value when available, compiled
                // default otherwise.
                s_update_install_armed = false;
                const char *page_url = s_update_manifest_ok
                    ? s_update_manifest.releases_page
                    : GUI_UPDATER_RELEASES_PAGE_URL;
                char open_error[160] = {0};
                if (!gui_ui_open_url_external(page_url, open_error, sizeof(open_error))) {
                    char msg[240];
                    snprintf(msg, sizeof(msg), "Failed to open the releases page: %s",
                             open_error[0] ? open_error : "unknown error");
                    gui_app_set_status(app, msg);
                } else {
                    gui_app_set_status(app, "Opening the releases page");
                }
                gui_ui_set_click_consumed();
                return;
            }
            // Install Update: two-click confirm (first click arms, second
            // triggers). Desktop: full self-update on the worker thread -
            // download, archive the old binary (rename, never delete), move
            // the new one into place. Android: guided browser download of the
            // manifest-named APK; Android's installer then replaces the app
            // after the user confirms (the APK stays as the archive copy).
            if (Clay_PointerOver(CLAY_ID("VersionInfoInstallButton"))) {
                gui_ui_process_update_check_result(app);
                if (atomic_load_explicit(&s_update_check_running, memory_order_acquire)) {
                    gui_app_set_status(app, "Please wait for update check to finish");
                    gui_ui_set_click_consumed();
                    return;
                }
                if (app->is_capturing || app->is_recording) {
                    gui_app_set_status(app, "Stop capture/recording before installing an update");
                    gui_ui_set_click_consumed();
                    return;
                }
                if (!s_update_manifest_ok) {
                    s_update_install_armed = false;
                    gui_app_set_status(app, "Update manifest unavailable - use the Releases Page button");
                    gui_ui_set_click_consumed();
                    return;
                }
                // Target tag: the cached tag when it still reports a newer
                // release, otherwise resolve now via the manifest's lookup.
                char latest_tag[GUI_UPDATER_TAG_MAX] = {0};
                if (gui_updater_is_release_tag_safe(app->settings.update_last_release_tag) &&
                    gui_updater_compare_versions(MIRSC_TOOLS_VERSION,
                                                 app->settings.update_last_release_tag) < 0) {
                    snprintf(latest_tag, sizeof(latest_tag), "%s", app->settings.update_last_release_tag);
                } else {
                    char lookup_error[GUI_UPDATER_ERROR_MAX] = {0};
                    if (!gui_updater_fetch_latest_release_tag(s_update_manifest.tag_lookup_url,
                                                              latest_tag, sizeof(latest_tag),
                                                              lookup_error, sizeof(lookup_error))) {
                        gui_app_set_status(app, "Unable to resolve the latest release - run Check for Update");
                        gui_ui_set_click_consumed();
                        return;
                    }
                }
                if (!s_update_install_armed) {
                    // First click only ARMS the install (explicit confirmation).
                    s_update_install_armed = true;
                    char msg[GUI_UPDATER_STATUS_MAX];
                    snprintf(msg, sizeof(msg),
                             "Click Install again to confirm updating to %s "
                             "(the old version is archived, not deleted)",
                             latest_tag);
                    gui_app_set_status(app, msg);
                    gui_ui_set_click_consumed();
                    return;
                }
                s_update_install_armed = false;
                char asset_url[GUI_UPDATER_URL_MAX] = {0};
                char asset_name[192] = {0};
                if (!gui_updater_build_asset_url(&s_update_manifest, latest_tag,
                                                 asset_url, sizeof(asset_url),
                                                 asset_name, sizeof(asset_name))) {
                    gui_app_set_status(app, "No update asset mapping - use the Releases Page button");
                    gui_ui_set_click_consumed();
                    return;
                }
#if defined(__ANDROID__)
                (void)asset_name;
                char open_error[160] = {0};
                if (!gui_ui_open_url_external(asset_url, open_error, sizeof(open_error))) {
                    char msg[240];
                    snprintf(msg, sizeof(msg), "Failed to open the update download: %s",
                             open_error[0] ? open_error : "unknown error");
                    gui_app_set_status(app, msg);
                } else {
                    gui_app_set_status(app, "Downloading the update in the browser - install it when Android asks");
                }
                gui_ui_set_click_consumed();
                return;
#else
                gui_ui_update_check_task_t task = {0};
                task.install = true;
                snprintf(task.asset_url, sizeof(task.asset_url), "%s", asset_url);
                snprintf(task.asset_name, sizeof(task.asset_name), "%s", asset_name);
                snprintf(task.current_version, sizeof(task.current_version), "%s", MIRSC_TOOLS_VERSION);
                if (!gui_ui_start_update_task(&task)) {
                    gui_app_set_status(app, "Unable to start the update install");
                    gui_ui_set_click_consumed();
                    return;
                }
                gui_app_set_status(app, "Installing update: downloading and archiving the old version...");
                gui_ui_set_click_consumed();
                return;
#endif
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoCorePinningToggle"))) {
                app->settings.show_core_pinning_in_settings = !app->settings.show_core_pinning_in_settings;
                if (!app->settings.show_core_pinning_in_settings && s_active_text_field == UI_TEXT_FIELD_FLAC_AFFINITY) {
                    gui_ui_clear_text_edit();
                }
                gui_settings_save(&app->settings);
                gui_app_set_status(app, app->settings.show_core_pinning_in_settings
                    ? "Core pinning controls shown in Settings"
                    : "Core pinning controls hidden from Settings");
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoMemoryBudgetToggle"))) {
                // Cycle 1 -> 2 -> 4 -> 8 -> 16 -> 1 GB. Apply immediately when
                // idle by tearing down and re-initializing the buffer manager;
                // block the change while capturing/recording to avoid disrupting
                // the live data path.
                if (app->is_capturing || app->is_recording) {
                    gui_app_set_status(app, "Stop capture/recording to change memory budget");
                    gui_ui_set_click_consumed();
                    return;
                }
                static const uint32_t cycle[] = { 1, 2, 4, 8, 16 };
                uint32_t cur = app->settings.memory_budget_gb;
                if (cur < 1) cur = 1;
                if (cur > 16) cur = 16;
                size_t idx = 0;
                for (size_t i = 0; i < sizeof(cycle) / sizeof(cycle[0]); i++) {
                    if (cycle[i] == cur) { idx = i; break; }
                }
                uint32_t next = cycle[(idx + 1) % (sizeof(cycle) / sizeof(cycle[0]))];
                app->settings.memory_budget_gb = next;
                gui_settings_save(&app->settings);
                bufmgr_cleanup(&app->buffers);
                if (bufmgr_init_for_budget(&app->buffers, next) != 0) {
                    // Re-init failed: revert to the previous value so the UI
                    // label and the actual buffer sizes stay consistent.
                    app->settings.memory_budget_gb = cur;
                    gui_settings_save(&app->settings);
                    (void)bufmgr_init_for_budget(&app->buffers, cur);
                    gui_app_set_status(app, "Memory budget change failed (reverted)");
                } else {
                    char msg[80];
                    snprintf(msg, sizeof(msg), "Memory budget set to %u GB (applied)", (unsigned)next);
                    gui_app_set_status(app, msg);
                }
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoV4l2Toggle"))) {
                // Toggle V4L2/simple_capture device discovery and re-enumerate
                // so the device dropdown reflects the new setting immediately.
                app->settings.discover_simple_capture = !app->settings.discover_simple_capture;
                gui_settings_save(&app->settings);
                gui_app_enumerate_devices(app);
                gui_app_set_status(app, app->settings.discover_simple_capture
                    ? "V4L2 device discovery enabled"
                    : "V4L2 device discovery disabled");
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoMisrcAbSwapToggle"))) {
                bool ab_swap_cxadc = gui_ui_selected_device_is_cxadc(app, NULL);
#ifdef ENABLE_FX3
                bool ab_swap_fx3 = gui_ui_selected_device_is_fx3(app);
#else
                bool ab_swap_fx3 = false;
#endif
#ifdef ENABLE_DDD
                bool ab_swap_ddd = gui_ui_selected_device_is_ddd(app);
#else
                bool ab_swap_ddd = false;
#endif
                bool ab_swap_supported_backend = !(ab_swap_cxadc || ab_swap_fx3 || ab_swap_ddd);
                if (!ab_swap_supported_backend) {
                    gui_app_set_status(app, "MISRC V1.5/V2.5 A/B swap applies only to HSDAOH/Simple Capture");
                } else if (app->is_recording) {
                    gui_app_set_status(app, "MISRC V1.5/V2.5 A/B swap is locked while recording");
                } else {
                    app->settings.misrc_v15_v25_ab_swap = !app->settings.misrc_v15_v25_ab_swap;
                    gui_settings_save(&app->settings);
                    gui_app_set_status(app, app->settings.misrc_v15_v25_ab_swap
                        ? "MISRC V1.5/V2.5 A/B swap override enabled"
                        : "MISRC V1.5/V2.5 A/B swap override disabled");
                }
                gui_ui_set_click_consumed();
                return;
            }
            // Net mode toggle: a press only ARMS the hold decision (clicks fire
            // on press; acting here would also run the heavy apply inline).
            // Quick release cycles, >= 450 ms hold opens the direct-select
            // menu - both resolved per frame below the click gate.
            if (Clay_PointerOver(CLAY_ID("VersionInfoNetModeToggle"))) {
                if (gui_playback_is_starting(app)) {
                    gui_app_set_status(app, "Network mode is locked while playback prepares");
                    gui_ui_set_click_consumed();
                    return;
                }
                /* No busy gate: cycling during a running transition queues the
                 * newest mode (the transition worker chases it). */
                s_net_mode_press_armed = true;
                s_net_mode_hold_fired = false;
                s_net_mode_press_time = GetTime();
                gui_ui_set_click_consumed();
                return;
            }
            // Select a discovered server in either client mode.
            if (app->settings.net_mode == GUI_NET_MODE_CLIENT ||
                app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT) {
                int disc_n = gui_net_discovered_count();
                int disc_visible = disc_n;
                if (disc_visible > VERSION_INFO_NET_DISCOVERY_MAX_ROWS) {
                    disc_visible = VERSION_INFO_NET_DISCOVERY_MAX_ROWS;
                }
                for (int i = 0; i < disc_visible; i++) {
                    if (Clay_PointerOver(CLAY_IDI("VersionInfoNetDiscItem", i))) {
                        gui_ui_clear_text_edit();
                        gui_net_select_discovered(app, i);
                        gui_ui_set_click_consumed();
                        return;
                    }
                }
            }
            // Connect/Disconnect button: toggles the client worker while
            // staying in Client mode (discovery remains active when disconnected).
            if (Clay_PointerOver(CLAY_ID("VersionInfoNetConnectButton")) &&
                (app->settings.net_mode == GUI_NET_MODE_CLIENT ||
                 app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT)) {
                gui_ui_clear_text_edit();
                gui_settings_save(&app->settings);
                gui_net_client_toggle_connection(app);
                gui_ui_set_click_consumed();
                return;
            }
            // Click-to-edit the server port field.
            if (Clay_PointerOver(CLAY_ID("VersionInfoNetServerPortField")) &&
                app->settings.net_mode == GUI_NET_MODE_SERVER) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_NET_SERVER_PORT,
                                       CLAY_ID("VersionInfoNetServerPortField"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
                return;
            }
            // Click-to-edit the client host field.
            if (Clay_PointerOver(CLAY_ID("VersionInfoNetClientHostField")) &&
                (app->settings.net_mode == GUI_NET_MODE_CLIENT ||
                 app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT)) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_NET_CLIENT_HOST,
                                       CLAY_ID("VersionInfoNetClientHostField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            // Click-to-edit the client port field.
            if (Clay_PointerOver(CLAY_ID("VersionInfoNetClientPortField")) &&
                (app->settings.net_mode == GUI_NET_MODE_CLIENT ||
                 app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT)) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_NET_CLIENT_PORT,
                                       CLAY_ID("VersionInfoNetClientPortField"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoCloseButton"))) {
                s_version_info_window_open = false;
                s_update_install_armed = false;
                s_net_mode_menu_open = false;
                s_net_mode_press_armed = false;
                s_net_mode_hold_fired = false;
                // Commit any in-progress net field edit on close. Async: the
                // apply joins network threads with socket timeouts and must
                // not stall the close click. Same mode (port/host edit):
                // force the re-apply; a queued request would skip it.
                if (s_active_text_field == UI_TEXT_FIELD_NET_SERVER_PORT ||
                    s_active_text_field == UI_TEXT_FIELD_NET_CLIENT_HOST ||
                    s_active_text_field == UI_TEXT_FIELD_NET_CLIENT_PORT) {
                    gui_ui_clear_text_edit();
                    gui_settings_save(&app->settings);
                    (void)gui_net_request_apply_mode(app, app->settings.net_mode, true);
                }
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoWindow"))) {
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoBackdrop"))) {
                s_version_info_window_open = false;
                s_update_install_armed = false;
                s_net_mode_menu_open = false;
                s_net_mode_press_armed = false;
                s_net_mode_hold_fired = false;
                gui_ui_set_click_consumed();
                return;
            }
        }
        // Metadata popup modal interactions (consume before toolbar underneath)
        if (s_metadata_window_open) {
            if (Clay_PointerOver(CLAY_ID("MetadataCloseButton")) ||
                Clay_PointerOver(CLAY_ID("MetadataBackdrop"))) {
                s_metadata_window_open = false;
                if (s_active_text_field == UI_TEXT_FIELD_INGEST_PROJECT ||
                    s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_ID ||
                    s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_FORMAT ||
                    s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_SIZE ||
                    s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_SPEED ||
                    s_active_text_field == UI_TEXT_FIELD_INGEST_TAPE_CONDITION ||
                    s_active_text_field == UI_TEXT_FIELD_INGEST_OPERATOR ||
                    s_active_text_field == UI_TEXT_FIELD_INGEST_LOCATION ||
                    s_active_text_field == UI_TEXT_FIELD_INGEST_NOTES) {
                    gui_ui_clear_text_edit();
                }
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataProjectField"))) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_INGEST_PROJECT, CLAY_ID("MetadataProjectField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataTapeIdField"))) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_INGEST_TAPE_ID, CLAY_ID("MetadataTapeIdField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataTapeFormatField"))) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_INGEST_TAPE_FORMAT, CLAY_ID("MetadataTapeFormatField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataTapeSizeField"))) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_INGEST_TAPE_SIZE, CLAY_ID("MetadataTapeSizeField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataTapeSpeedField"))) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_INGEST_TAPE_SPEED, CLAY_ID("MetadataTapeSpeedField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataTapeConditionField"))) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_INGEST_TAPE_CONDITION, CLAY_ID("MetadataTapeConditionField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataOperatorField"))) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_INGEST_OPERATOR, CLAY_ID("MetadataOperatorField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataLocationField"))) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_INGEST_LOCATION, CLAY_ID("MetadataLocationField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataNotesField"))) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_INGEST_NOTES, CLAY_ID("MetadataNotesField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("MetadataWindow"))) {
                gui_ui_set_click_consumed();
                return;
            }
        }

        // Record-limit popup modal interactions (consume before toolbar underneath)
        if (s_record_limit_window_open) {
            if (Clay_PointerOver(CLAY_ID("RecordLimitCloseButton"))) {
                s_record_limit_window_open = false;
                s_record_limit_timecode_edit = false;
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("RecordLimitArmToggle"))) {
                if (s_record_limit_armed) {
                    s_record_limit_armed = false;
                    s_record_limit_deadline_active = false;
                    s_record_limit_deadline_s = 0.0;
                    gui_record_limit_sync_settings(app);
                    gui_record_limit_log_state(app, "Record timer disarmed");
                    gui_app_set_status(app, "Record time limit disarmed");
                } else {
                    if (s_record_limit_timecode_edit) {
                        uint32_t staged_seconds = 0;
                        if (parse_record_limit_timecode(s_record_limit_timecode_edit_buffer, &staged_seconds) && staged_seconds > 0) {
                            s_record_limit_seconds = staged_seconds;
                            format_record_limit_timecode(s_record_limit_timecode, sizeof(s_record_limit_timecode), staged_seconds);
                        }
                        s_record_limit_timecode_edit = false;
                    }
                    uint32_t parsed_seconds = 0;
                    bool timecode_valid = parse_record_limit_timecode(s_record_limit_timecode, &parsed_seconds) && parsed_seconds > 0;
                    if (!timecode_valid) {
                        gui_record_limit_sync_settings(app);
                        gui_app_set_status(app, "Invalid record limit timecode");
                    } else {
                        s_record_limit_seconds = parsed_seconds;
                        s_record_limit_armed = true;
                        gui_record_limit_sync_settings(app);
                        gui_record_limit_log_state(app, "Record timer armed");
                        if (app->is_recording) {
                            double now = GetTime();
                            double requested_deadline_s = app->recording_start_time + (double)s_record_limit_seconds;
                            if (!s_record_limit_deadline_active || requested_deadline_s > s_record_limit_deadline_s) {
                                if (requested_deadline_s > now) {
                                    s_record_limit_deadline_active = true;
                                    s_record_limit_deadline_s = requested_deadline_s;
                                    gui_app_set_status(app, "Record time limit armed");
                                } else {
                                    s_record_limit_deadline_active = false;
                                    s_record_limit_deadline_s = 0.0;
                                    gui_app_set_status(app, "Record limit shorter than elapsed; ignored");
                                }
                            } else {
                                gui_app_set_status(app, "Shorter record limit ignored while recording");
                            }
                        } else {
                            gui_app_set_status(app, "Record time limit armed");
                        }
                    }
                }
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("RecordLimitTimecodeField"))) {
                if (!s_record_limit_timecode_edit) {
                    record_limit_begin_timecode_edit();
                }
                record_limit_set_cursor_from_field_click(app);
                gui_ui_set_click_consumed();
                return;
            }
            // Level autostop controls render inside this window, so they must be
            // handled here (before the RecordLimitWindow catch-all below) or their
            // clicks are swallowed. They stay editable while recording, like the
            // timecode and the sibling Stop-on-Dropout toggle.
            if (Clay_PointerOver(CLAY_ID("LevelAutostopToggle"))) {
                s_record_limit_timecode_edit = false;
                app->settings.level_autostop_enabled = !app->settings.level_autostop_enabled;
                gui_settings_save(&app->settings);
                if (!gui_ui_text_field_can_edit(app, s_active_text_field)) {
                    gui_ui_clear_text_edit();
                }
                gui_app_set_status(app, app->settings.level_autostop_enabled
                    ? "Level autostop enabled"
                    : "Level autostop disabled");
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("LevelAutostopLevelField"))) {
                s_record_limit_timecode_edit = false;
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL, CLAY_ID("LevelAutostopLevelField"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("LevelAutostopDurationField"))) {
                s_record_limit_timecode_edit = false;
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION, CLAY_ID("LevelAutostopDurationField"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
                return;
            }
            // Vpp (ADC range) control lives on the waveform monitor's Layout
            // dropdown (channel A) — see handle_layout_dropdown.
            if (Clay_PointerOver(CLAY_ID("RecordLimitWindow"))) {
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("RecordLimitBackdrop"))) {
                s_record_limit_window_open = false;
                s_record_limit_timecode_edit = false;
                gui_ui_set_click_consumed();
                return;
            }
        }

        if (Clay_PointerOver(CLAY_ID("RecordLimitButton"))) {
            if (playback_mode) {
                if (app->is_capturing && gui_playback_is_running(app)) {
                    bool loop_enabled = gui_playback_get_loop(app);
                    gui_playback_set_loop(app, !loop_enabled);
                    gui_app_set_status(app, !loop_enabled ? "Playback loop enabled" : "Playback loop disabled");
                }
                gui_ui_set_click_consumed();
                return;
            }
            s_record_limit_window_open = !s_record_limit_window_open;
            if (!s_record_limit_window_open) {
                s_record_limit_timecode_edit = false;
            }
            gui_ui_set_click_consumed();
            return;
        }
        if (Clay_PointerOver(CLAY_ID("VersionIconButton"))) {
            s_version_info_window_open = !s_version_info_window_open;
            if (s_version_info_window_open) {
                s_metadata_window_open = false;
            } else {
                s_update_install_armed = false;
                s_net_mode_menu_open = false;
                s_net_mode_press_armed = false;
                s_net_mode_hold_fired = false;
            }
            gui_ui_set_click_consumed();
            return;
        }
        if (Clay_PointerOver(CLAY_ID("StreamFaultStatus"))) {
            char details[128];
            snprintf(details, sizeof(details), "Missed frames: %" PRIuFAST32 "\nErrors: %" PRIuFAST32,
                     atomic_load(&app->missed_frame_count), atomic_load(&app->error_count));
            gui_dropdown_close_all();
            gui_ui_clear_text_edit();
            gui_popup_info("Stream faults (snapshot)", details);
            gui_ui_set_click_consumed();
            return;
        }
        if (Clay_PointerOver(CLAY_ID("SamplesStatus"))) {
            char details[96];
            snprintf(details, sizeof(details), "Samples: %" PRIuFAST64,
                     atomic_load(&app->samples_a));
            gui_dropdown_close_all();
            gui_ui_clear_text_edit();
            gui_popup_info("Samples (snapshot)", details);
            gui_ui_set_click_consumed();
            return;
        }
        for (int channel = 0; channel < 2; channel++) {
            if (s_record_details_view[channel].visible &&
                Clay_PointerOver(CLAY_IDI("RecordingDetailsButton", channel))) {
                const gui_record_details_view_t *detail = &s_record_details_view[channel];
                char message[480];
                char buffer_section[200];
                if (detail->buffer_known) {
                    snprintf(buffer_section, sizeof(buffer_section),
                             "Record buffer %c: %u%%\nWrite waits: %" PRIu64
                             "\nDropped blocks: %" PRIu64,
                             channel == 0 ? 'A' : 'B', detail->buffer_fill_pct,
                             detail->write_waits, detail->dropped_blocks);
                } else {
                    snprintf(buffer_section, sizeof(buffer_section),
                             "Record buffer: unavailable");
                }
                char send_section[96] = "";
                if (detail->local_save && channel == 0 && detail->send_queue_capacity > 0) {
                    snprintf(send_section, sizeof(send_section),
                             "\nLocal Save queue: %.1f / %.1f MiB",
                             (double)detail->send_queue_bytes / 1048576.0,
                             (double)detail->send_queue_capacity / 1048576.0);
                }
                snprintf(message, sizeof(message),
                         "%s\n%s\n%s%s%s%s%s\n\n%s%s",
                         detail->recording ? "Recording in progress" : "Last recording",
                         stat_rec_duration[channel], stat_rec_raw[channel],
                         stat_rec_flac[channel][0] ? "\n" : "",
                         stat_rec_flac[channel],
                         stat_rec_ratio[channel][0] ? "\n" : "",
                         stat_rec_ratio[channel], buffer_section, send_section);
                gui_dropdown_close_all();
                gui_ui_clear_text_edit();
                gui_popup_info("Recording details (snapshot)", message);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_IDI("StatClip", channel))) {
                char details[160];
                uint_fast32_t positive = channel == 0 ? atomic_load(&app->clip_count_a_pos)
                                                     : atomic_load(&app->clip_count_b_pos);
                uint_fast32_t negative = channel == 0 ? atomic_load(&app->clip_count_a_neg)
                                                     : atomic_load(&app->clip_count_b_neg);
                snprintf(details, sizeof(details),
                         "Channel %c\nPositive clips: %" PRIuFAST32 "\nNegative clips: %" PRIuFAST32 "\n\nK/M/G indicate thousands/millions/billions.",
                         channel == 0 ? 'A' : 'B', positive, negative);
                gui_dropdown_close_all();
                gui_ui_clear_text_edit();
                gui_popup_info("Clipping counts (snapshot)", details);
                gui_ui_set_click_consumed();
                return;
            }
        }
        if (Clay_PointerOver(CLAY_ID("DeviceBufferStatus"))) {
            gui_device_buffer_view_t view;
            if (gui_ui_get_device_buffer_view(app, GUI_DEVICE_BUFFER_LAYOUT_FULL,
                                              &view) && view.details[0]) {
                gui_dropdown_close_all();
                gui_ui_clear_text_edit();
                gui_popup_info("Hardware buffer (snapshot)", view.details);
            }
            gui_ui_set_click_consumed();
            return;
        }
        if (Clay_PointerOver(CLAY_ID("MetadataIconButton"))) {
            s_metadata_window_open = !s_metadata_window_open;
            if (s_metadata_window_open) {
                s_version_info_window_open = false;
                s_update_install_armed = false;
            }
            gui_ui_set_click_consumed();
            return;
        }
        if (app->is_capturing &&
            gui_ui_selected_device_is_playback(app) &&
            Clay_PointerOver(CLAY_IDI("PlaybackTimelineTrack", 0))) {
            s_playback_scrub_track_index = 0;
            if (gui_ui_seek_playback_from_track(app, s_playback_scrub_track_index,
                                                gui_ui_get_mouse_position().x)) {
                s_playback_scrub_active = true;
            } else {
                s_playback_scrub_active = false;
                gui_app_set_status(app, gui_ui_playback_channel_timeline_info(app, 0, NULL, NULL, NULL, NULL)
                    ? "CH A length is unknown; seeking unavailable"
                    : "No playback file loaded for CH A");
            }
            gui_ui_set_click_consumed();
            return;
        }
        if (app->is_capturing &&
            gui_ui_selected_device_is_playback(app) &&
            Clay_PointerOver(CLAY_IDI("PlaybackTimelineTrack", 1))) {
            s_playback_scrub_track_index = 1;
            if (gui_ui_seek_playback_from_track(app, s_playback_scrub_track_index,
                                                gui_ui_get_mouse_position().x)) {
                s_playback_scrub_active = true;
            } else {
                s_playback_scrub_active = false;
                gui_app_set_status(app, gui_ui_playback_channel_timeline_info(app, 1, NULL, NULL, NULL, NULL)
                    ? "CH B length is unknown; seeking unavailable"
                    : "No playback file loaded for CH B");
            }
            gui_ui_set_click_consumed();
            return;
        }
        Vector2 click_pos = gui_ui_get_mouse_position();
        bool mode_toggle_hit = Clay_PointerOver(CLAY_ID("CaptureModeToggle"));
        bool mode_toggle_cxadc_clockgen = false;
        bool mode_toggle_is_cxadc = gui_ui_selected_device_is_cxadc(app, &mode_toggle_cxadc_clockgen);
        bool mode_toggle_is_cxadc_misrc_clockgen = gui_ui_selected_device_is_cxadc_misrc_clockgen(app);
        bool mode_toggle_is_misrc_clockgen = gui_ui_selected_device_is_misrc_clockgen(app);
#ifdef ENABLE_FX3
        bool mode_toggle_is_fx3 = gui_ui_selected_device_is_fx3(app);
#else
        bool mode_toggle_is_fx3 = false;
#endif
#ifdef ENABLE_DDD
        bool mode_toggle_is_ddd = gui_ui_selected_device_is_ddd(app);
#else
        bool mode_toggle_is_ddd = false;
#endif
        if (mode_toggle_hit) {
            TraceLog(LOG_INFO,
                     "MODE CLICK TRACE: x=%.1f y=%.1f recording=%d capturing=%d user=%s runtime=%s settings=%s",
                     click_pos.x, click_pos.y,
                     app->is_recording ? 1 : 0,
                     app->is_capturing ? 1 : 0,
                     gui_ui_capture_mode_name(app->user_capture_mode_misrc),
                     gui_ui_capture_mode_name(app->capture_mode_runtime_misrc),
                     gui_ui_capture_mode_name(app->settings.misrc_mode));
        }
        // Check connect button
        if (Clay_PointerOver(CLAY_ID("ConnectButton"))) {
            if (gui_playback_is_starting(app)) {
                gui_app_stop_capture(app);
                gui_ui_set_click_consumed();
                return;
            }
            bool control_capturing = app->is_capturing;
            if (gui_net_is_client(app)) {
                control_capturing = gui_net_client_peer_capturing(app);
            }
            if (control_capturing) {
                // Refuse to disconnect while recording: tearing capture down
                // mid-recording races the async record finalize thread (which
                // is still joining the writer threads draining BUF_RECORD_A/B)
                // and can corrupt/truncate the capture file. Force the user to
                // stop recording first via the Record button, which finalizes
                // cleanly before this control becomes usable again. Mirrors the
                // "settings locked while recording" pattern at gui_ui_settings_locked.
                if (app->is_recording) {
                    gui_app_set_status(app, "Stop recording before disconnecting (protects the capture file)");
                    gui_ui_set_click_consumed();
                    return;
                }
#if defined(__ANDROID__)
                /* Async stop: hsdaoh_stop_stream/close on the wrapped fd can
                 * hang joining libusb/libuvc threads; never block the render
                 * thread on it. */
                gui_app_stop_capture_async(app);
#else
                gui_app_stop_capture(app);
#endif
                app->reconnect_pending = false;
                app->reconnect_attempts = 0;
            } else if (gui_ui_selected_device_is_playback(app) && !gui_net_is_client(app)) {
                // File preparation is asynchronous; the per-frame poll commits it.
                (void)gui_app_start_capture(app);
                gui_dropdown_close_all();
                gui_ui_clear_text_edit();
                gui_ui_set_click_consumed();
                return;
            } else {
#if defined(__ANDROID__)
                /* Network clients do NOT own local capture hardware.  The
                 * previous Android-only path unconditionally requested USB
                 * permission, so Connect failed in Monitor & Remote mode if
                 * the phone had no USB device (as intended). Keep the remote
                 * start/stop commands distinct from local USB capture. */
                if (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT ||
                    app->settings.net_mode == GUI_NET_MODE_CLIENT) {
                    bool peer_connected = app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT
                        ? gui_monitor_client_connected() : gui_net_active(app);
                    if (peer_connected) {
                        /* This only queues a command to the server; it never
                         * opens local USB (see gui_app_start_capture). */
                        if (gui_app_start_capture(app) == 0) {
                            app->reconnect_pending = false;
                            app->reconnect_attempts = 0;
                        }
                    } else if (!gui_net_client_connection_running(app)) {
                        /* The network connection itself was disconnected:
                         * re-start its worker instead of asking for USB. */
                        gui_net_client_toggle_connection(app);
                        gui_app_set_status(app, "Connecting to remote Server...");
                    } else {
                        gui_app_set_status(app, "Waiting for remote Server connection...");
                    }
                } else if (gui_ui_selected_device_is_simulated(app)) {
                    /* Software generator: use the existing non-blocking
                     * capture worker directly. No USB hardware or Android
                     * USB permission is needed for Test Signal. */
                    gui_app_start_capture_async(app);
                    app->reconnect_pending = false;
                    app->reconnect_attempts = 0;
                } else {
                    /* Local USB hardware only: async Android permission.
                     * Never block the render thread on the system dialog. */
                    extern int android_usb_has_fd(void);
                    extern int android_request_usb_permission_async(void);
                    extern int android_permission_pending(void);
                    extern void android_usb_clear_fd(void);
                    if (android_usb_has_fd()) {
                        gui_app_start_capture_async(app);
                        app->reconnect_pending = false;
                        app->reconnect_attempts = 0;
                    } else if (!android_permission_pending()) {
                        android_usb_clear_fd();
                        if (android_request_usb_permission_async()) {
                            gui_app_set_status(app, "Requesting USB permission...");
                        } else {
                            gui_app_set_status(app, "USB permission request failed to start");
                        }
                    }
                }
#else
                if (gui_app_start_capture(app) == 0) {
                    app->reconnect_pending = false;
                    app->reconnect_attempts = 0;
                }
#endif
            }
        }
        if (mode_toggle_hit) {
            if (gui_playback_is_starting(app)) {
                gui_app_set_status(app, "Capture mode is locked while playback prepares");
            } else if (mode_toggle_is_misrc_clockgen) {
                gui_app_set_status(app, "MISRC Clockgen mode is selected from the device list");
            } else if (mode_toggle_is_cxadc) {
                if (mode_toggle_cxadc_clockgen) {
                    gui_app_set_status(app, mode_toggle_is_cxadc_misrc_clockgen
                        ? "MISRC Clockgen mode is selected from the device list"
                        : "CXADC Clockgen mode is selected from the device list");
                } else {
                    gui_app_set_status(app, "CXADC mode is selected from the device list");
                }
            } else if (mode_toggle_is_fx3) {
                gui_app_set_status(app, "FX3ADC backend selected; MISRC/HSDAOH mode not applicable");
            } else if (mode_toggle_is_ddd) {
                gui_app_set_status(app, "DdD backend selected; MISRC/HSDAOH mode not applicable");
            } else if (gui_net_is_client(app)) {
                gui_app_set_status(app, "Client mode mirrors server capture mode; change mode on server");
            } else if (app->is_recording || gui_record_is_finalizing()) {
                // Mode switch is locked while recording is active AND while a
                // recording finalize is still in flight. gui_record_stop() sets
                // is_recording=false before the async finalize thread completes,
                // so checking is_recording alone would re-enable the toggle
                // mid-finalize — switching backend at that point would race the
                // writer threads draining BUF_RECORD_A/B. Mirrors the Disconnect
                // lockout and the "settings locked while recording" pattern.
                TraceLog(LOG_INFO,
                         "MODE TRACE: source=CaptureModeToggle blocked current=%s recording=%d finalizing=%d",
                         gui_ui_capture_mode_name(s_capture_mode_state_misrc),
                         app->is_recording ? 1 : 0,
                         gui_record_is_finalizing() ? 1 : 0);
                gui_app_set_status(app, app->is_recording
                    ? "Capture mode is locked while recording is active"
                    : "Capture mode is locked while recording finalizes");
            } else {
                gui_ui_set_capture_mode_state(app, !s_capture_mode_state_misrc);
                gui_settings_save(&app->settings);
                if (app->is_capturing) {
                    gui_app_set_status(app, s_capture_mode_state_misrc
                        ? "Mode set to MISRC (raw/parser backend on reconnect)"
                        : "Mode set to HSDAOH (upstream backend on reconnect)");
                } else {
                    gui_app_set_status(app, s_capture_mode_state_misrc
                        ? "Mode set to MISRC (raw/parser backend)"
                        : "Mode set to HSDAOH (upstream backend)");
                }
            }
            gui_ui_set_click_consumed();
            return;
        }

        if (app->settings_panel_open &&
            (Clay_PointerOver(CLAY_ID("SettingsBackdrop")) || Clay_PointerOver(CLAY_ID("SettingsCloseButton")))) {
            app->settings_panel_open = false;
            gui_ui_clear_text_edit();
            gui_settings_save(&app->settings);
            if (!gui_ui_text_field_can_edit(app, s_active_text_field)) {
                gui_ui_clear_text_edit();
            }
            return;
        }

        if (app->settings_panel_open &&
            gui_ui_settings_locked(app) &&
            Clay_PointerOver(CLAY_ID("SettingsPanel"))) {
            gui_ui_clear_text_edit();
            gui_app_set_status(app, gui_playback_is_starting(app)
                ? "Settings are locked while playback prepares"
                : "Settings are locked while recording is active");
            gui_ui_set_click_consumed();
            return;
        }
        if (Clay_PointerOver(CLAY_ID("StopOnDropoutToggle"))) {
            app->settings.stop_on_dropout = !app->settings.stop_on_dropout;
            gui_settings_save(&app->settings);
            gui_app_set_status(app, app->settings.stop_on_dropout
                ? "Stop on dropout enabled"
                : "Stop on dropout disabled");
        }

        // Audio playback monitoring toggle
        if (Clay_PointerOver(CLAY_ID("AudioPlaybackToggle"))) {
            bool enable = !app->settings.audio_monitor_playback;
            gui_audio_set_playback_enabled(app, enable);
            bool mon_cxadc_mode = gui_ui_selected_device_is_cxadc(app, NULL);
            if (mon_cxadc_mode) {
                gui_app_set_status(app, enable
                    ? "CXADC audio monitoring enabled"
                    : "CXADC audio monitoring disabled");
            } else {
                gui_app_set_status(app, enable
                    ? "Audio monitoring enabled"
                    : "Audio monitoring disabled");
            }
        }

        // Audio channel select toggle
        if (Clay_PointerOver(CLAY_ID("AudioChannelToggle"))) {
            app->settings.audio_monitor_ch34 = !app->settings.audio_monitor_ch34;
            gui_settings_save(&app->settings);
#if defined(_WIN32)
            bool mon_cxadc_mode = gui_ui_selected_device_is_cxadc(app, NULL);
            if (mon_cxadc_mode) {
                gui_app_set_status(app, app->settings.audio_monitor_ch34
                    ? "CXADC monitor source: headswitch (CH3)"
                    : "CXADC monitor source: audio pair (CH1/2)");
            } else {
                gui_app_set_status(app, app->settings.audio_monitor_ch34
                    ? "Audio monitor source: CH3/4"
                    : "Audio monitor source: CH1/2");
            }
#else
            gui_app_set_status(app, app->settings.audio_monitor_ch34
                ? "Audio monitor source: CH3/4"
                : "Audio monitor source: CH1/2");
#endif
        }

        // Check record button - UI indicates record-write to disk finialization
        // Mitigate app appearing hung
        if (Clay_PointerOver(CLAY_ID("RecordButton"))) {
            if (app->settings.net_mode != GUI_NET_MODE_MONITOR_CLIENT &&
                gui_ui_selected_device_is_playback(app)) {
                if (app->is_capturing && gui_playback_is_running(app)) {
                    gui_playback_toggle_pause(app);
                    if (gui_playback_get_state(app) == PLAYBACK_STATE_PAUSED) {
                        gui_app_set_status(app, "Playback paused");
                    } else {
                        gui_app_set_status(app, "Playback resumed");
                    }
                }
                gui_ui_set_click_consumed();
                return;
            }
            if (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT) {
                if (!gui_monitor_client_connected()) {
                    gui_app_set_status(app,"Monitor & Remote: connect to Server before recording");
                } else if (gui_monitor_client_peer_state() == 2) {
                    gui_net_client_request_record(app,false);
                    gui_app_set_status(app,"Server: stop recording requested");
                } else if (gui_monitor_client_peer_state() == 1) {
                    gui_net_client_request_record(app,true);
                    gui_app_set_status(app,"Server: start recording requested (server disk)");
                } else {
                    gui_app_set_status(app,"Server capture is not running");
                }
                gui_ui_set_click_consumed();
                return;
            }
            if (app->is_capturing) {
                if (gui_record_is_finalizing()) {
                    gui_app_set_status(app, "Finalizing previous recording...");
                } else if (app->is_recording) {
                    gui_app_stop_recording(app);
                } else {
                    gui_app_start_recording(app);
                }
            }
        }

        // Check settings button
        if (Clay_PointerOver(CLAY_ID("SettingsButton"))) {
            app->settings_panel_open = !app->settings_panel_open;
            gui_ui_clear_text_edit();
            gui_settings_save(&app->settings);
        }


        // Clip reset buttons (per-channel stats)
        if (Clay_PointerOver(CLAY_IDI("ResetClipBtn", 0))) {
            if(app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT)
                gui_monitor_client_reset_clips(0);
            else {
                atomic_store(&app->clip_count_a_pos, 0);
                atomic_store(&app->clip_count_a_neg, 0);
            }
        }
        if (Clay_PointerOver(CLAY_IDI("ResetClipBtn", 1))) {
            if(app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT)
                gui_monitor_client_reset_clips(1);
            else {
                atomic_store(&app->clip_count_b_pos, 0);
                atomic_store(&app->clip_count_b_neg, 0);
            }
        }
        if (gui_ui_selected_device_is_cxadc(app, NULL)) {
            const int dc_step = 1;
            if (app->selected_device != s_cxadc_dc_anchor_device_index) {
                s_cxadc_dc_anchor_device_index = app->selected_device;
                memset(s_cxadc_dc_anchor_valid, 0, sizeof(s_cxadc_dc_anchor_valid));
                memset(s_cxadc_dc_anchor_raw, 0, sizeof(s_cxadc_dc_anchor_raw));
                memset(s_cxadc_dc_relative, 0, sizeof(s_cxadc_dc_relative));
            }
            for (int ch = 0; ch < 2; ch++) {
                bool dc_down = Clay_PointerOver(CLAY_IDI("DcOffsetDown", ch));
                bool dc_up = Clay_PointerOver(CLAY_IDI("DcOffsetUp", ch));
                if (!dc_down && !dc_up) continue;
                int card_idx = -1;
                if (!gui_ui_map_cxadc_channel_to_card(app, ch, &card_idx)) {
                    gui_app_set_status(app, "CXADC card not available for this channel");
                    gui_ui_set_click_consumed();
                    return;
                }
                if (card_idx < 0 || card_idx > 1) {
                    gui_app_set_status(app, "Invalid CXADC card index");
                    gui_ui_set_click_consumed();
                    return;
                }

                int current_raw = 0;
                if (gui_cxadc_get_center_offset(card_idx, &current_raw) != 0) {
                    if (errno == EACCES || errno == EPERM) {
#if defined(_WIN32)
                        gui_app_set_status(app, "CXADC DC-offset write denied - re-run as administrator");
#else
                        gui_app_set_status(app, "CXADC permission denied: run sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*");
#endif
                    } else {
                        gui_app_set_status(app, "Failed to read CXADC DC offset");
                    }
                    gui_ui_set_click_consumed();
                    return;
                }

                if (!s_cxadc_dc_anchor_valid[card_idx]) {
                    s_cxadc_dc_anchor_raw[card_idx] = current_raw;
                    s_cxadc_dc_relative[card_idx] = 0;
                    s_cxadc_dc_anchor_valid[card_idx] = true;
                }
                int delta = dc_down ? -dc_step : dc_step;
                int target_relative = s_cxadc_dc_relative[card_idx] + delta;
                int target_raw = s_cxadc_dc_anchor_raw[card_idx] + target_relative;
                const int cxadc_dc_raw_max = gui_cxadc_center_offset_max();
                if (target_raw < 0) target_raw = 0;
                if (target_raw > cxadc_dc_raw_max) target_raw = cxadc_dc_raw_max;
                target_relative = target_raw - s_cxadc_dc_anchor_raw[card_idx];

                if (gui_cxadc_set_center_offset(card_idx, target_raw) == 0) {
                    s_cxadc_dc_relative[card_idx] = target_relative;
                    char msg[128];
                    snprintf(msg, sizeof(msg), "CH %c CXADC%d DC: %+d (raw %d)", (ch == 0) ? 'A' : 'B', card_idx, target_relative, target_raw);
                    gui_app_set_status(app, msg);
                } else {
                    if (errno == EACCES || errno == EPERM) {
#if defined(_WIN32)
                        gui_app_set_status(app, "CXADC DC-offset write denied - re-run as administrator");
#else
                        gui_app_set_status(app, "CXADC permission denied: run sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*");
#endif
                    } else {
                        gui_app_set_status(app, "Failed to update CXADC DC offset");
                    }
                }
                gui_ui_set_click_consumed();
                return;
            }
        }

        // Settings panel interactions
        if (app->settings_panel_open) {
            bool settings_cxadc_has_channel_b = false;
            bool settings_cxadc_mode = gui_ui_selected_device_is_cxadc(app, &settings_cxadc_has_channel_b);
#ifdef ENABLE_DDD
            bool settings_ddd_mode = gui_ui_selected_device_is_ddd(app);
            bool settings_ddd_v1_mode = gui_ui_selected_device_is_ddd_v1(app);
#else
            bool settings_ddd_mode = false;
            bool settings_ddd_v1_mode = false;
#endif
#ifdef ENABLE_FX3
            bool settings_fx3_mode = gui_ui_selected_device_is_fx3(app);
#else
            bool settings_fx3_mode = false;
#endif
            bool settings_b_disabled = settings_ddd_mode || settings_fx3_mode || (settings_cxadc_mode && !settings_cxadc_has_channel_b);
            bool settings_b_controls_disabled = settings_b_disabled || !app->settings.capture_b;
            float settings_non_cxadc_base_rate_a_khz = 40000.0f;
#ifdef ENABLE_DDD
            if (settings_ddd_v1_mode) {
                settings_non_cxadc_base_rate_a_khz =
                    (float)ddd_sample_rate_khz(app->settings.ddd_decimation);
            }
#endif
            float settings_base_rate_a_khz = settings_cxadc_mode
                ? gui_ui_cxadc_hw_rate_khz(app, 0)
                : settings_non_cxadc_base_rate_a_khz;
            float settings_base_rate_b_khz = settings_cxadc_mode ? gui_ui_cxadc_hw_rate_khz(app, settings_cxadc_has_channel_b ? 1 : 0) : 40000.0f;
            if (Clay_PointerOver(CLAY_ID("SettingsBackdrop")) || Clay_PointerOver(CLAY_ID("SettingsCloseButton"))) {
                app->settings_panel_open = false;
                gui_ui_clear_text_edit();
                gui_settings_save(&app->settings);
                if (!gui_ui_text_field_can_edit(app, s_active_text_field)) {
                    gui_ui_clear_text_edit();
                }
                return;
            }

            if (Clay_PointerOver(CLAY_ID("ToggleCaptureA"))) {
                app->settings.capture_a = !app->settings.capture_a;
                gui_settings_save(&app->settings);
                if (!gui_ui_text_field_can_edit(app, s_active_text_field)) {
                    gui_ui_clear_text_edit();
                }
            }
            if (Clay_PointerOver(CLAY_ID("ToggleCaptureB"))) {
                if (settings_b_disabled) {
                    if (settings_ddd_mode) {
                        gui_app_set_status(app, "DdD is single-channel; channel B has no signal source");
                    } else if (settings_fx3_mode) {
                        gui_app_set_status(app, "FX3ADC is single-channel; channel B has no signal source");
                    } else if (settings_cxadc_mode) {
                        gui_app_set_status(app, "Single-card CXADC has no RF channel B source");
                    }
                } else {
                    app->settings.capture_b = !app->settings.capture_b;
                    if (!app->settings.capture_b) {
                        app->settings.enable_resample_b = false;
                        app->settings.resample_rate_b = settings_base_rate_b_khz;
                    }
                    gui_settings_save(&app->settings);
                }
            }
            if (Clay_PointerOver(CLAY_ID("ToggleUseFlac"))) {
                app->settings.use_flac = !app->settings.use_flac;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleOverwrite"))) {
                app->settings.overwrite_files = !app->settings.overwrite_files;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleFlacVerify"))) {
                app->settings.flac_verification = !app->settings.flac_verification;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacLevelMinus"))) {
                if (app->settings.flac_level > 0) app->settings.flac_level--;
                if (app->settings.flac_level >= 1 && app->settings.flac_level <= 3) {
                    // Levels 1-3 give poor compression ratios for archival RF.
                    // Warn that 6-8 should be used unless there are explicit
                    // resource constraints (slow CPU, single-core, etc).
                    gui_dropdown_close_all();
                    gui_ui_clear_text_edit();
                    gui_popup_info("FLAC level is low (1-3)",
                        "FLAC compression level is now in the 1-3 range.\n\n"
                        "Levels 6-8 are recommended for archival RF capture\n"
                        "(best compression ratio, smallest files).\n\n"
                        "Only use level 1-3 if you have explicit resource\n"
                        "constraints (slow CPU, single-core, limited I/O).\n\n"
                        "You can raise it with the + button.");
                }
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacLevelPlus"))) {
                if (app->settings.flac_level < 8) app->settings.flac_level++;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacThreadsMinus"))) {
                if (app->settings.flac_threads > 0) app->settings.flac_threads--;
                if (app->settings.flac_threads == 0) {
                    // 0 means "auto" to the encoder, which can under-use
                    // available cores. Warn immediately so the user knows
                    // to pick an explicit thread count for best throughput.
                    gui_dropdown_close_all();
                    gui_ui_clear_text_edit();
                    gui_popup_info("FLAC threads set to auto (0)",
                        "FLAC encoder threads is now 0 (auto).\n\n"
                        "Auto may not use all available CPU cores efficiently.\n"
                        "For best encode throughput on multi-core systems, set an\n"
                        "explicit thread count (e.g. 4, 6, or 8).\n\n"
                        "You can raise it back with the + button.");
                }
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacThreadsPlus"))) {
                if (app->settings.flac_threads < 64) app->settings.flac_threads++;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleFlacAffinity"))) {
                if (app->settings.show_core_pinning_in_settings &&
                    gui_ui_flac_affinity_supported() &&
                    app->settings.use_flac) {
                    app->settings.flac_affinity_enabled = !app->settings.flac_affinity_enabled;
                    gui_settings_save(&app->settings);
                    if (!gui_ui_text_field_can_edit(app, s_active_text_field)) {
                        gui_ui_clear_text_edit();
                    }
                } else if (!gui_ui_flac_affinity_supported()) {
                    gui_app_set_status(app, "FLAC affinity is Linux-only");
                }
            }
            if (app->settings_panel_open &&
                Clay_PointerOver(CLAY_ID("FlacAffinityListField")) &&
                app->settings.show_core_pinning_in_settings &&
                !gui_ui_click_consumed()) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_FLAC_AFFINITY, CLAY_ID("FlacAffinityListField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
            }
            if (!settings_ddd_v1_mode &&
                Clay_PointerOver(CLAY_ID("ToggleResampleA"))) {
                bool enable = !app->settings.enable_resample_a;
                app->settings.enable_resample_a = enable;
                if (!enable) {
                    app->settings.resample_rate_a = settings_base_rate_a_khz;
                }
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ResampleRateABox"))) {
#ifdef ENABLE_DDD
                if (settings_ddd_v1_mode) {
                    ddd_v1_rate_plan_t current_plan;
                    if (app->is_capturing) {
                        gui_app_set_status(app,
                            "Stop capture before changing the DdD RF rate");
                    } else if (!gui_ui_ddd_v1_rate_plan(
                                   app, &current_plan)) {
                        gui_app_set_status(app,
                            "Invalid DdD RF rate settings");
                    } else {
                        float next_rate_khz = cycle_resample_khz(
                            (float)current_plan.output_rate_khz,
                            (float)ddd_sample_rate_khz(
                                DDD_DECIMATION_FULL_RATE));
                        if (!gui_ui_set_ddd_v1_output_rate(
                                app, (uint32_t)lroundf(next_rate_khz))) {
                            gui_app_set_status(app,
                                "Invalid DdD RF rate selection");
                        }
                    }
                } else
#endif
                {
                    if (settings_cxadc_mode) {
                        gui_ui_cxadc_cycle_rate(app, 0);
                    } else {
                        app->settings.resample_rate_a = cycle_resample_khz(app->settings.resample_rate_a, settings_base_rate_a_khz);
                        gui_settings_save(&app->settings);
                        gui_ui_warn_low_rate(app, app->settings.resample_rate_a);
                    }
                }
            }
            if (Clay_PointerOver(CLAY_ID("ToggleResampleB"))) {
                if (settings_b_controls_disabled) {
                    if (settings_ddd_mode) {
                        gui_app_set_status(app, "DdD is single-channel; channel B resample not applicable");
                    } else if (settings_fx3_mode) {
                        gui_app_set_status(app, "FX3ADC is single-channel; channel B resample not applicable");
                    } else if (settings_cxadc_mode) {
                        gui_app_set_status(app, "Single-card CXADC has no RF channel B source");
                    } else if (!app->settings.capture_b) {
                        gui_app_set_status(app, "Enable RF channel B to edit CH B resample settings");
                    }
                } else {
                    bool enable = !app->settings.enable_resample_b;
                    app->settings.enable_resample_b = enable;
                    if (!enable) {
                        app->settings.resample_rate_b = settings_base_rate_b_khz;
                    }
                    gui_settings_save(&app->settings);
                }
            }
            if (Clay_PointerOver(CLAY_ID("ResampleRateBBox"))) {
                if (settings_b_controls_disabled) {
                    if (settings_ddd_mode) {
                        gui_app_set_status(app, "DdD is single-channel; channel B resample not applicable");
                    } else if (settings_fx3_mode) {
                        gui_app_set_status(app, "FX3ADC is single-channel; channel B resample not applicable");
                    } else if (settings_cxadc_mode) {
                        gui_app_set_status(app, "Single-card CXADC has no RF channel B source");
                    } else if (!app->settings.capture_b) {
                        gui_app_set_status(app, "Enable RF channel B to edit CH B resample settings");
                    }
                } else {
                    if (settings_cxadc_mode) {
                        int bcard = settings_cxadc_has_channel_b ? 1 : 0;
                        gui_ui_cxadc_cycle_rate(app, bcard);
                    } else {
                        app->settings.resample_rate_b = cycle_resample_khz(app->settings.resample_rate_b, settings_base_rate_b_khz);
                        gui_settings_save(&app->settings);
                        gui_ui_warn_low_rate(app, app->settings.resample_rate_b);
                    }
                }
            }

            // SDR controls (shown for any SDR backend; today only RTL-SDR).
#ifdef ENABLE_RTLSDR
            if (gui_ui_selected_device_is_sdr(app)) {
                if (Clay_PointerOver(CLAY_ID("RtlsdrFreqField")) && !gui_ui_click_consumed()) {
                    gui_ui_begin_text_edit(app, UI_TEXT_FIELD_RTLSDR_FREQ, CLAY_ID("RtlsdrFreqField"), 8.0f, 8.0f);
                    gui_ui_set_click_consumed();
                }
                if (Clay_PointerOver(CLAY_ID("RtlsdrSampleRateBox"))) {
                    // Cycle through known-stable RTL sample rates (Hz).
                    static const uint32_t rtlsdr_rates[] = { 250000, 1024000, 1200000, 1536000, 2048000, 2400000 };
                    static const int n = (int)(sizeof(rtlsdr_rates)/sizeof(rtlsdr_rates[0]));
                    uint32_t cur = app->settings.rtlsdr_sample_rate_hz;
                    int idx = 0;
                    for (int i = 0; i < n; i++) { if (rtlsdr_rates[i] == cur) { idx = i; break; } }
                    app->settings.rtlsdr_sample_rate_hz = rtlsdr_rates[(idx + 1) % n];
                    gui_settings_save(&app->settings);
                    char msg[80];
                    snprintf(msg, sizeof(msg), "SDR sample rate set to %.2f MSPS (applies on next capture start)",
                             (double)app->settings.rtlsdr_sample_rate_hz / 1.0e6);
                    gui_app_set_status(app, msg);
                }
                if (Clay_PointerOver(CLAY_ID("RtlsdrGainModeToggle"))) {
                    // 0=auto,1=manual
                    app->settings.rtlsdr_gain_mode = app->settings.rtlsdr_gain_mode ? 0 : 1;
                    gui_settings_save(&app->settings);
                    gui_app_set_status(app, app->settings.rtlsdr_gain_mode
                        ? "SDR gain mode: manual"
                        : "SDR gain mode: auto");
                }
                if (Clay_PointerOver(CLAY_ID("RtlsdrGainMinus"))) {
                    if (app->settings.rtlsdr_gain_tenths_db > 0) app->settings.rtlsdr_gain_tenths_db -= 10;
                    gui_settings_save(&app->settings);
                }
                if (Clay_PointerOver(CLAY_ID("RtlsdrGainPlus"))) {
                    if (app->settings.rtlsdr_gain_tenths_db < 600) app->settings.rtlsdr_gain_tenths_db += 10;
                    gui_settings_save(&app->settings);
                }
                if (Clay_PointerOver(CLAY_ID("RtlsdrAgcToggle"))) {
                    app->settings.rtlsdr_agc = !app->settings.rtlsdr_agc;
                    gui_settings_save(&app->settings);
                    gui_app_set_status(app, app->settings.rtlsdr_agc
                        ? "SDR AGC enabled"
                        : "SDR AGC disabled");
                }
                if (Clay_PointerOver(CLAY_ID("RtlsdrOffsetToggle"))) {
                    app->settings.rtlsdr_offset_corr = !app->settings.rtlsdr_offset_corr;
                    gui_settings_save(&app->settings);
                    gui_app_set_status(app, app->settings.rtlsdr_offset_corr
                        ? "SDR offset tuning enabled"
                        : "SDR offset tuning disabled");
                }
            }
#endif // ENABLE_RTLSDR

            // Auto naming controls
            if (Clay_PointerOver(CLAY_ID("ToggleAutoNames"))) {
                app->settings.auto_names_enabled = !app->settings.auto_names_enabled;
                if (!app->settings.output_base_name[0]) {
                    snprintf(app->settings.output_base_name, sizeof(app->settings.output_base_name), "%s", "capture");
                }
                gui_settings_save(&app->settings);
                if (!gui_ui_text_field_can_edit(app, s_active_text_field)) {
                    gui_ui_clear_text_edit();
                }
            }
            if (app->settings_panel_open &&
                Clay_PointerOver(CLAY_ID("OutputBaseNameField")) &&
                !gui_ui_click_consumed())
            {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_OUTPUT_BASE_NAME, CLAY_ID("OutputBaseNameField"), 8.0f, 8.0f);

                gui_ui_set_click_consumed();
            }

            if (Clay_PointerOver(CLAY_ID("AppendTimestampToggle")) && app->settings.auto_names_enabled) {
                app->settings.append_timestamp_on_capture_start = !app->settings.append_timestamp_on_capture_start;
                gui_settings_save(&app->settings);
                gui_ui_set_click_consumed();
            }

            if (app->settings_panel_open &&
                (app->settings.net_mode != GUI_NET_MODE_MONITOR_CLIENT ||
                 !app->settings.monitor_local_save_enabled) &&
                Clay_PointerOver(CLAY_ID("OutputPathBox")) &&
                !gui_ui_click_consumed())
            {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_OUTPUT_PATH, CLAY_ID("OutputPathBox"), 10.0f, 10.0f);

                gui_ui_set_click_consumed();
            }


             // RF bit depth selection (cycle)
            // If user switches to RAW and a channel was set to 12-bit, treat it as 16-bit.
            if (!app->settings.use_flac) {
                if (app->settings.rf_bits_a == 12) app->settings.rf_bits_a = 16;
                if (app->settings.rf_bits_b == 12) app->settings.rf_bits_b = 16;
            }

            if (Clay_PointerOver(CLAY_ID("RfBitsABox"))) {
                if (settings_cxadc_mode) {
                    gui_ui_toggle_cxadc_bit_mode(app, 0);
                } else {
                    uint8_t b = app->settings.rf_bits_a;
                    if (app->settings.use_flac) {
                        // 8 -> 12 -> 16 -> 8
                        b = (b == 8) ? 12 : (b == 12) ? 16 : 8;
                    } else {
                        // RAW: 8 <-> 16
                        b = (b == 8) ? 16 : 8;
                    }
                    app->settings.rf_bits_a = b;
                    gui_settings_save(&app->settings);
                }
            }
            if (Clay_PointerOver(CLAY_ID("RfBitsBBox"))) {
                if (settings_cxadc_mode && settings_b_disabled) {
                    gui_app_set_status(app, "Single-card CXADC has no RF channel B source");
                } else if (settings_cxadc_mode) {
                    gui_ui_toggle_cxadc_bit_mode(app, 1);
                } else if (settings_b_controls_disabled) {
                    if (settings_ddd_mode) {
                        gui_app_set_status(app, "DdD is single-channel; channel B bit depth not applicable");
                    } else if (settings_fx3_mode) {
                        gui_app_set_status(app, "FX3ADC is single-channel; channel B bit depth not applicable");
                    } else if (!app->settings.capture_b) {
                        gui_app_set_status(app, "Enable RF channel B to edit CH B bit depth");
                    }
                } else {
                    uint8_t b = app->settings.rf_bits_b;
                    if (app->settings.use_flac) {
                        b = (b == 8) ? 12 : (b == 12) ? 16 : 8;
                    } else {
                        b = (b == 8) ? 16 : 8;
                    }
                    app->settings.rf_bits_b = b;
                    gui_settings_save(&app->settings);
                }
            }
            if (Clay_PointerOver(CLAY_ID("RfTagAField")) && app->settings.auto_names_enabled) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_RF_TAG_A, CLAY_ID("RfTagAField"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
            }

            if (Clay_PointerOver(CLAY_ID("AudioTag4chField")) && app->settings.auto_names_enabled) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_AUDIO_TAG_4CH, CLAY_ID("AudioTag4chField"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("AudioTag2ch12Field")) && app->settings.auto_names_enabled) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_AUDIO_TAG_12, CLAY_ID("AudioTag2ch12Field"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("AudioTag2ch34Field")) && app->settings.auto_names_enabled) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_AUDIO_TAG_34, CLAY_ID("AudioTag2ch34Field"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("RfTagBField")) && app->settings.auto_names_enabled && !settings_b_controls_disabled) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_RF_TAG_B, CLAY_ID("RfTagBField"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
            }

            if (Clay_PointerOver(CLAY_ID("ToggleAudio2ch12"))) {
                app->settings.enable_audio_2ch_12 = !app->settings.enable_audio_2ch_12;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleAudio4ch"))) {
                app->settings.enable_audio_4ch = !app->settings.enable_audio_4ch;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleAudio2ch34"))) {
                app->settings.enable_audio_2ch_34 = !app->settings.enable_audio_2ch_34;
                gui_settings_save(&app->settings);
            }

            for (int i = 0; i < 4; i++) {
                if (Clay_PointerOver(CLAY_IDI("ToggleAudio1ch", i))) {
                    app->settings.enable_audio_1ch[i] = !app->settings.enable_audio_1ch[i];
                    gui_settings_save(&app->settings);
                }
                if (Clay_PointerOver(CLAY_IDI("Audio1chLabelField", i)) && app->settings.auto_names_enabled) {
                    ui_text_field_t field = (ui_text_field_t)(UI_TEXT_FIELD_AUDIO_LABEL_1 + i);
                    gui_ui_begin_text_edit(app, field, CLAY_IDI("Audio1chLabelField", i), 6.0f, 6.0f);
                    gui_ui_set_click_consumed();
                }
            }

            if (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT &&
                Clay_PointerOver(CLAY_ID("MonitorLocalSaveToggle")) &&
                !gui_ui_click_consumed()) {
                /* Never change destination mode during an active remote recording. */
                if(gui_monitor_client_peer_state()==2)
                    gui_app_set_status(app,"Stop recording before switching Local Save");
                else {
                    app->settings.monitor_local_save_enabled = !app->settings.monitor_local_save_enabled;
                    // The other path field is now hidden, so close any active
                    // text edit before switching the visible destination.
                    gui_ui_clear_text_edit();
                    gui_settings_save(&app->settings);
                    gui_app_set_status(app,app->settings.monitor_local_save_enabled ?
                        "Local Save ON: next recording goes to this computer" :
                        "Local Save OFF: next recording goes to the Server");
                }
                gui_ui_set_click_consumed();return;
            }
            if (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT &&
                app->settings.monitor_local_save_enabled &&
                Clay_PointerOver(CLAY_ID("MonitorLocalSavePathBox")) &&
                !gui_ui_click_consumed()) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_MONITOR_LOCAL_SAVE_PATH,
                                       CLAY_ID("MonitorLocalSavePathBox"), 10.0f, 10.0f);
                gui_ui_set_click_consumed();
            }
            if (app->settings.net_mode == GUI_NET_MODE_MONITOR_CLIENT &&
                app->settings.monitor_local_save_enabled &&
                Clay_PointerOver(CLAY_ID("MonitorLocalSaveChooseFolder"))) {
                gui_settings_t tmp=app->settings;
                // Picker returns its chosen path via tmp.output_path. Start
                // empty so selecting a folder equal to the Output folder is
                // not incorrectly treated as a cancelled picker operation.
                tmp.output_path[0]='\0';
                /* Picker operates on the client computer, NOT server paths. */
                if (gui_settings_choose_output_folder(&tmp)) {
                    snprintf(app->settings.monitor_local_save_path,
                             sizeof(app->settings.monitor_local_save_path),
                             "%s",tmp.output_path);
                    gui_settings_save(&app->settings);
                    gui_app_set_status(app,"Monitor & Remote local recording folder saved");
                } else gui_app_set_status(app,"Folder selection cancelled or unavailable; click local path to type it");
                gui_ui_set_click_consumed();
                return;
            }
            if ((app->settings.net_mode != GUI_NET_MODE_MONITOR_CLIENT ||
                 !app->settings.monitor_local_save_enabled) &&
                Clay_PointerOver(CLAY_ID("ChooseOutputFolderButton"))) {
                if(app->settings.net_mode==GUI_NET_MODE_MONITOR_CLIENT){
                    if(!gui_monitor_client_connected())
                        gui_app_set_status(app,"Connect to Server before browsing its folders");
                    else{
                        s_monitor_folder_open=true;
                        s_monitor_folder_page=0;
                        s_monitor_folder_error=false;s_monitor_folder_computer=false;
                        s_monitor_folder_recovered=false;s_monitor_folder_more=false;
                        snprintf(s_monitor_folder_path,sizeof(s_monitor_folder_path),"%s",app->settings.output_path);
                        gui_monitor_client_browse_folders(s_monitor_folder_path,0);
                    }
                    gui_ui_set_click_consumed();return;
                }
#if defined(__ANDROID__)
                /* Async SAF picker: launch and return; the per-frame poll in
                 * gui_handle_interactions applies the result. Never block the
                 * render thread across the picker Activity transition. */
                extern int android_pick_output_folder_async(void);
                extern int android_picker_active(void);
                if (!android_picker_active()) {
                    if (android_pick_output_folder_async()) {
                        gui_app_set_status(app, "Waiting for folder selection...");
                    } else {
                        gui_app_set_status(app, "Folder picker unavailable");
                    }
                }
                gui_ui_set_click_consumed();
#else
                // Best-effort folder picker (platform-specific).
                if (gui_settings_choose_output_folder(&app->settings)) {
                    gui_settings_save(&app->settings);
                } else {
                    gui_app_set_status(app, "No folder selected (or folder picker unavailable)");
                }
#endif
            }

        // Playback file selection buttons
            if (Clay_PointerOver(CLAY_ID("PlaybackFileBrowseA"))) {
#if defined(__ANDROID__)
                extern int android_pick_playback_file_async(int channel);
                extern int android_picker_active(void);
                if (!android_picker_active()) {
                    if (android_pick_playback_file_async(0)) {
                        gui_app_set_status(app, "Waiting for CH A playback file...");
                    } else {
                        gui_app_set_status(app, "File picker unavailable");
                    }
                }
                gui_ui_set_click_consumed();
#else
                if (gui_settings_choose_playback_file(&app->settings, 0)) {
                    gui_settings_save(&app->settings);
                } else {
                    gui_app_set_status(app, "No file selected (or file picker unavailable)");
                }
                gui_ui_set_click_consumed();
#endif
            }
            if (Clay_PointerOver(CLAY_ID("PlaybackFileBrowseB"))) {
#if defined(__ANDROID__)
                extern int android_pick_playback_file_async(int channel);
                extern int android_picker_active(void);
                if (!android_picker_active()) {
                    if (android_pick_playback_file_async(1)) {
                        gui_app_set_status(app, "Waiting for CH B playback file...");
                    } else {
                        gui_app_set_status(app, "File picker unavailable");
                    }
                }
                gui_ui_set_click_consumed();
#else
                if (gui_settings_choose_playback_file(&app->settings, 1)) {
                    gui_settings_save(&app->settings);
                } else {
                    gui_app_set_status(app, "No file selected (or file picker unavailable)");
                }
                gui_ui_set_click_consumed();
#endif
            }
            if (Clay_PointerOver(CLAY_ID("PlaybackFileClearA"))) {
                app->settings.playback_file_a[0] = '\0';
                gui_settings_save(&app->settings);
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("PlaybackFileClearB"))) {
                app->settings.playback_file_b[0] = '\0';
                gui_settings_save(&app->settings);
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("PlaybackFileClearAudio"))) {
                app->settings.playback_audio_file[0] = '\0';
                gui_settings_save(&app->settings);
                gui_ui_set_click_consumed();
            }
        }

        // Note: CVBS enable/disable is now handled automatically when selecting
        // CVBS view via ensure_cvbs_enabled_for_channel() in gui_dropdown.c

        // Handle all dropdown interactions via centralized handler
        if (!s_ui_consumed_click && gui_playback_is_starting(app) &&
            Clay_PointerOver(CLAY_ID("DeviceDropdown"))) {
            gui_app_set_status(app, "Device selection is locked while playback prepares");
            s_ui_consumed_click = true;
        } else if (!s_ui_consumed_click && gui_dropdown_handle_click(app)) {
            s_ui_consumed_click = true;
        }
    }

    // Net mode hold-press resolution: a press on the Mode toggle (armed in the
    // version-window click gate above) becomes either a quick-click cycle
    // (released under 450 ms while still over the toggle) or opens the
    // direct-select menu (held 450 ms). Runs outside the press gate so a
    // press spanning many frames is detected.
    if (s_net_mode_press_armed) {
        bool over_toggle = s_version_info_window_open &&
            Clay_PointerOver(CLAY_ID("VersionInfoNetModeToggle"));
        if (!IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            bool quick_click = !s_net_mode_hold_fired && over_toggle;
            s_net_mode_press_armed = false;
            s_net_mode_hold_fired = false;
            if (quick_click) {
                gui_ui_net_mode_apply(app, gui_ui_net_mode_cycle_next(app->settings.net_mode));
            }
        } else if (!s_net_mode_hold_fired && over_toggle &&
                   (GetTime() - s_net_mode_press_time) >= 0.45) {
            s_net_mode_hold_fired = true;
            s_net_mode_menu_open = true;
            gui_dropdown_close_all();
        }
    }
}
