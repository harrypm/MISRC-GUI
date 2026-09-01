/*
 * MISRC GUI - UI Layout Implementation
 * Clay-based declarative UI layout (Clay v0.14 API)
 */

#include "gui_ui.h"
#include "gui_dropdown.h"
#include "gui_popup.h"
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
#include "version.h"
#include "../visualization/gui_custom_elements.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"
#include <clay.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
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
    Vector2 position = GetMousePosition();
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

static float gui_ui_cxadc_base_rate_khz(const gui_app_t *app, int card_idx)
{
    if (!app) return 40000.0f;
    if (card_idx < 0 || card_idx > 1) card_idx = 0;
    return app->settings.cxadc_tenbit_mode_card[card_idx] ? 20000.0f : 40000.0f;
}
static uint8_t gui_ui_cxadc_rf_bits(const gui_app_t *app, int card_idx)
{
    if (!app) return 8;
    if (card_idx < 0 || card_idx > 1) card_idx = 0;
    return app->settings.cxadc_tenbit_mode_card[card_idx] ? 16 : 8;
}
static void gui_ui_toggle_cxadc_bit_mode(gui_app_t *app, int card_idx)
{
    if (!app) return;
    if (card_idx < 0 || card_idx > 1) card_idx = 0;
    app->settings.cxadc_tenbit_mode_card[card_idx] = !app->settings.cxadc_tenbit_mode_card[card_idx];
    uint8_t cxadc_bits = gui_ui_cxadc_rf_bits(app, card_idx);
    float cxadc_base_rate_khz = gui_ui_cxadc_base_rate_khz(app, card_idx);
    if (card_idx == 0) {
        app->settings.rf_bits_a = cxadc_bits;
        if (!app->settings.enable_resample_a || app->settings.resample_rate_a > cxadc_base_rate_khz) {
            app->settings.resample_rate_a = cxadc_base_rate_khz;
        }
    } else {
        app->settings.rf_bits_b = cxadc_bits;
        if (!app->settings.enable_resample_b || app->settings.resample_rate_b > cxadc_base_rate_khz) {
            app->settings.resample_rate_b = cxadc_base_rate_khz;
        }
    }
    gui_settings_save(&app->settings);
    const char *card_label = (card_idx == 0) ? "A" : "B";
    bool enabled = app->settings.cxadc_tenbit_mode_card[card_idx];
    if (app->is_capturing) {
        gui_app_set_status(app, enabled
            ? ((card_idx == 0) ? "CXADC card A 10-bit mode enabled (applies on next capture start)"
                               : "CXADC card B 10-bit mode enabled (applies on next capture start)")
            : ((card_idx == 0) ? "CXADC card A 8-bit mode enabled (applies on next capture start)"
                               : "CXADC card B 8-bit mode enabled (applies on next capture start)"));
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "CXADC card %s %s (%s base)",
                 card_label,
                 enabled ? "10-bit mode enabled" : "8-bit mode enabled",
                 enabled ? "20 MSPS" : "40 MSPS");
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
    UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL,    // Level autostop threshold percent
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
// Update checker (GitHub release tag lookup).
#define GUI_UI_UPDATE_CHECK_INTERVAL_SECONDS (7ULL * 24ULL * 60ULL * 60ULL)
#define GUI_UI_RELEASES_LATEST_URL "https://github.com/harrypm/MISRC-GUI/releases/latest"
#define GUI_UI_RELEASES_DOWNLOAD_BASE_URL "https://github.com/harrypm/MISRC-GUI/releases/download"
#define VERSION_INFO_NET_DISCOVERY_MAX_ROWS 6

typedef struct {
    bool manual;
} gui_ui_update_check_task_t;

static thrd_t s_update_check_thread;
static bool s_update_check_thread_started = false;
static atomic_bool s_update_check_running = false;
static atomic_bool s_update_check_result_pending = false;
static bool s_update_check_result_manual = false;
static bool s_update_check_result_success = false;
static char s_update_check_result_tag[64] = {0};
static char s_update_check_result_error[160] = {0};

static void gui_ui_trim_ascii_whitespace_inplace(char *s)
{
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

static bool gui_ui_ci_starts_with(const char *value, const char *prefix)
{
    if (!value || !prefix) return false;
    while (*prefix) {
        if (!*value) return false;
        if (tolower((unsigned char)*value) != tolower((unsigned char)*prefix)) {
            return false;
        }
        value++;
        prefix++;
    }
    return true;
}

static bool gui_ui_extract_release_tag_from_url(const char *url, char *tag_out, size_t tag_out_len)
{
    if (!url || !tag_out || tag_out_len == 0) return false;
    tag_out[0] = '\0';
    const char *marker = "/releases/tag/";
    const char *start = strstr(url, marker);
    if (!start) return false;
    start += strlen(marker);
    if (*start == '\0') return false;

    size_t i = 0;
    while (start[i] &&
           !isspace((unsigned char)start[i]) &&
           start[i] != '?' &&
           start[i] != '#' &&
           start[i] != '/') {
        if (i + 1 >= tag_out_len) break;
        tag_out[i] = start[i];
        i++;
    }
    tag_out[i] = '\0';
    return (i > 0);
}

static bool gui_ui_parse_semver_triplet(const char *version, int *major_out, int *minor_out, int *patch_out)
{
    if (!version || !major_out || !minor_out || !patch_out) return false;
    const char *p = version;
    if (*p == 'v' || *p == 'V') p++;
    if (!isdigit((unsigned char)*p)) return false;

    char *end = NULL;
    long major = strtol(p, &end, 10);
    if (!end || end == p || *end != '.') return false;
    p = end + 1;
    if (!isdigit((unsigned char)*p)) return false;

    long minor = strtol(p, &end, 10);
    if (!end || end == p || *end != '.') return false;
    p = end + 1;
    if (!isdigit((unsigned char)*p)) return false;

    long patch = strtol(p, &end, 10);
    if (!end || end == p) return false;

    *major_out = (int)major;
    *minor_out = (int)minor;
    *patch_out = (int)patch;
    return true;
}

static bool gui_ui_is_release_tag_safe(const char *tag)
{
    if (!tag || !tag[0]) return false;
    const unsigned char *p = (const unsigned char *)tag;
    while (*p) {
        if (!(isalnum(*p) || *p == '.' || *p == '-' || *p == '_')) {
            return false;
        }
        p++;
    }
    return true;
}

static int gui_ui_compare_versions(const char *current_version, const char *latest_version)
{
    int cmaj = 0, cmin = 0, cpat = 0;
    int lmaj = 0, lmin = 0, lpat = 0;
    bool current_ok = gui_ui_parse_semver_triplet(current_version, &cmaj, &cmin, &cpat);
    bool latest_ok = gui_ui_parse_semver_triplet(latest_version, &lmaj, &lmin, &lpat);
    if (current_ok && latest_ok) {
        if (cmaj != lmaj) return (cmaj < lmaj) ? -1 : 1;
        if (cmin != lmin) return (cmin < lmin) ? -1 : 1;
        if (cpat != lpat) return (cpat < lpat) ? -1 : 1;
        return 0;
    }
    int text_cmp = strcmp(current_version ? current_version : "",
                          latest_version ? latest_version : "");
    if (text_cmp < 0) return -1;
    if (text_cmp > 0) return 1;
    return 0;
}

static bool gui_ui_fetch_latest_release_tag(char *tag_out, size_t tag_out_len, char *error_out, size_t error_out_len)
{
    if (!tag_out || tag_out_len == 0 || !error_out || error_out_len == 0) return false;
    tag_out[0] = '\0';
    error_out[0] = '\0';

#if defined(_WIN32)
    const char *cmd = "curl.exe -fsSI --max-time 10 " GUI_UI_RELEASES_LATEST_URL;
    FILE *fp = _popen(cmd, "r");
#else
    const char *cmd = "curl -fsSI --max-time 10 " GUI_UI_RELEASES_LATEST_URL;
    FILE *fp = popen(cmd, "r");
#endif
    if (!fp) {
        snprintf(error_out, error_out_len, "unable to launch curl");
        return false;
    }

    char line[512];
    char location[512] = {0};
    while (fgets(line, sizeof(line), fp)) {
        gui_ui_trim_ascii_whitespace_inplace(line);
        if (line[0] == '\0') continue;
        if (gui_ui_ci_starts_with(line, "location:")) {
            const char *value = line + 9;
            while (*value && isspace((unsigned char)*value)) value++;
            snprintf(location, sizeof(location), "%s", value);
        }
    }

#if defined(_WIN32)
    int rc = _pclose(fp);
#else
    int rc = pclose(fp);
#endif

    if (location[0] == '\0') {
        if (rc != 0) {
            snprintf(error_out, error_out_len, "curl request failed");
        } else {
            snprintf(error_out, error_out_len, "latest release redirect header missing");
        }
        return false;
    }

    if (!gui_ui_extract_release_tag_from_url(location, tag_out, tag_out_len)) {
        snprintf(error_out, error_out_len, "unable to parse release tag");
        return false;
    }
    return true;
}

static bool gui_ui_build_release_asset_filename_for_platform(const char *release_tag,
                                                             char *filename_out,
                                                             size_t filename_out_len)
{
    if (!release_tag || !filename_out || filename_out_len == 0) return false;
    filename_out[0] = '\0';
    if (!gui_ui_is_release_tag_safe(release_tag)) return false;

#if defined(__ANDROID__)
    const char *pattern = "Android_MISRC_%s_arm64.apk";
#elif defined(__APPLE__)
    const char *pattern = "macOS_MISRC_%s_universal.dmg";
#elif defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
    const char *pattern = "Windows_MISRC_%s_arm64.zip";
#else
    const char *pattern = "Windows_MISRC_%s_x86.zip";
#endif
#elif defined(__linux__)
#if defined(__aarch64__) || defined(__arm64__)
    const char *pattern = "Linux_MISRC_%s_arm64.zip";
#else
    const char *pattern = "Linux_MISRC_%s_x86.zip";
#endif
#else
    return false;
#endif

    int written = snprintf(filename_out, filename_out_len, pattern, release_tag);
    return (written > 0) && ((size_t)written < filename_out_len);
}

static bool gui_ui_build_release_asset_url_for_platform(const char *release_tag,
                                                        char *url_out,
                                                        size_t url_out_len,
                                                        char *asset_out,
                                                        size_t asset_out_len)
{
    if (!url_out || url_out_len == 0) return false;
    url_out[0] = '\0';
    if (asset_out && asset_out_len > 0) asset_out[0] = '\0';

    char asset[160];
    if (!gui_ui_build_release_asset_filename_for_platform(release_tag, asset, sizeof(asset))) {
        return false;
    }
    int written = snprintf(url_out,
                           url_out_len,
                           GUI_UI_RELEASES_DOWNLOAD_BASE_URL "/%s/%s",
                           release_tag,
                           asset);
    if (written <= 0 || (size_t)written >= url_out_len) {
        return false;
    }
    if (asset_out && asset_out_len > 0) {
        snprintf(asset_out, asset_out_len, "%s", asset);
    }
    return true;
}

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

    char latest_tag[64] = {0};
    char error_text[160] = {0};
    bool ok = gui_ui_fetch_latest_release_tag(latest_tag, sizeof(latest_tag), error_text, sizeof(error_text));

    s_update_check_result_manual = task.manual;
    s_update_check_result_success = ok;
    if (ok) {
        snprintf(s_update_check_result_tag, sizeof(s_update_check_result_tag), "%s", latest_tag);
        s_update_check_result_error[0] = '\0';
    } else {
        s_update_check_result_tag[0] = '\0';
        snprintf(s_update_check_result_error, sizeof(s_update_check_result_error), "%s",
                 error_text[0] ? error_text : "unknown error");
    }

    atomic_store_explicit(&s_update_check_result_pending, true, memory_order_release);
    atomic_store_explicit(&s_update_check_running, false, memory_order_release);
    return 0;
}

static bool gui_ui_start_update_check(bool manual)
{
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
    task->manual = manual;

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

    if (s_update_check_result_success) {
        snprintf(app->settings.update_last_release_tag,
                 sizeof(app->settings.update_last_release_tag),
                 "%s",
                 s_update_check_result_tag);
        int cmp = gui_ui_compare_versions(MIRSC_TOOLS_VERSION, app->settings.update_last_release_tag);
        app->settings.update_available_cached = (cmp < 0);
        gui_settings_save(&app->settings);

        if (s_update_check_result_manual) {
            char msg[196];
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
        Vector2 m = MeasureTextEx(font, glyph, (float)font_size, 0.0f);
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
                                                  double *out_duration_seconds)
{
    if (out_total_samples) *out_total_samples = 0;
    if (out_duration_seconds) *out_duration_seconds = 0.0;
    if (!app) return false;

    playback_file_info_t info = {0};
    (void)((channel_index == 0)
        ? gui_playback_get_file_info_a(app, &info)
        : gui_playback_get_file_info_b(app, &info));

    if (info.total_samples == 0) return false;
    double duration_seconds = info.duration_seconds;
    if (!(duration_seconds > 0.0) || !isfinite(duration_seconds)) {
        return false;
    }

    if (out_total_samples) *out_total_samples = info.total_samples;
    if (out_duration_seconds) *out_duration_seconds = duration_seconds;
    return true;
}
static void gui_ui_format_playback_timeline(char *dst, size_t dst_len, int *out_fill_w, bool *out_has_file,
                                            uint64_t current_sample, uint64_t total_samples,
                                            double total_duration_seconds, int track_width_px)
{
    if (!dst || dst_len == 0) return;
    if (out_fill_w) *out_fill_w = 0;
    if (out_has_file) *out_has_file = false;
    if (total_samples == 0 || track_width_px <= 0 ||
        !(total_duration_seconds > 0.0) || !isfinite(total_duration_seconds)) {
        snprintf(dst, dst_len, "--:--:--/--:--:--");
        return;
    }
    if (out_has_file) *out_has_file = true;
    uint64_t channel_sample = current_sample;
    if (channel_sample >= total_samples) {
        channel_sample %= total_samples;
    }
    double t = (double)channel_sample / (double)total_samples;
    double playback_pos_s = t * total_duration_seconds;
    double playback_total_s = total_duration_seconds;
    char pos_tc[16];
    char total_tc[16];
    format_playback_timecode(pos_tc, sizeof(pos_tc), playback_pos_s);
    format_playback_timecode(total_tc, sizeof(total_tc), playback_total_s);
    snprintf(dst, dst_len, "%s/%s", pos_tc, total_tc);
    int fill_w = (int)round(t * (double)track_width_px);
    if (fill_w < 0) fill_w = 0;
    if (fill_w > track_width_px) fill_w = track_width_px;
    if (out_fill_w) *out_fill_w = fill_w;
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
    if (!gui_ui_playback_channel_timeline_info(app, track_index, &channel_total_samples, &channel_duration_seconds)) {
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

// Playback file display buffers
static char playback_file_a_display[64];
static char playback_file_b_display[64];
static char playback_timeline_display_a[48];
static char playback_timeline_display_b[48];
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
static char status_message_display[192];
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
/* Persistent small-readout shortening level (0 full .. 2 shortest) for the
 * status bar, kept across frames with hysteresis: pressure raises it
 * immediately, ample slack lowers it one step, so ticking counters at a
 * boundary width cannot flap the small readouts between full and short
 * forms every frame. */
static int s_status_readout_level = 0;
/* Persisted compact-label state for the status bar right side, with the
 * same hysteresis as the readout level: the flip is earned by measured
 * pressure (never a hard width breakpoint) and only relaxes when the full
 * labels measurably fit again with slack to spare. */
static bool s_status_labels_compact = false;
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
    bool cxadc_has_channel_b = false;
    if (cxadc_mode && app->selected_device >= 0 && app->selected_device < app->device_count) {
        cxadc_has_channel_b = (app->devices[app->selected_device].index > 1);
    }
    bool single_channel_device = ddd_mode || fx3_mode || (cxadc_mode && !cxadc_has_channel_b);
    bool expected_mode = s_capture_mode_state_misrc;
    if (cxadc_mode) {
        expected_mode = false;
    }
    bool mismatch_user = (app->user_capture_mode_misrc != expected_mode);
    bool mismatch_settings = (!cxadc_mode && app->settings.misrc_mode != expected_mode);
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
    if (!cxadc_mode) {
        app->settings.misrc_mode = expected_mode;
    }
    if (!app->is_recording) {
        app->capture_mode_runtime_misrc = expected_mode;
    }
    if (cxadc_mode) {
        bool cxadc_settings_changed = false;
        uint8_t cxadc_rf_bits_a = gui_ui_cxadc_rf_bits(app, 0);
        uint8_t cxadc_rf_bits_b = gui_ui_cxadc_rf_bits(app, cxadc_has_channel_b ? 1 : 0);
        float cxadc_base_rate_a_khz = gui_ui_cxadc_base_rate_khz(app, 0);
        float cxadc_base_rate_b_khz = gui_ui_cxadc_base_rate_khz(app, cxadc_has_channel_b ? 1 : 0);
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
        if (!app->settings.enable_resample_a) {
            if (fabsf(app->settings.resample_rate_a - cxadc_base_rate_a_khz) > 0.5f) {
                app->settings.resample_rate_a = cxadc_base_rate_a_khz;
                cxadc_settings_changed = true;
            }
        } else if (app->settings.resample_rate_a > cxadc_base_rate_a_khz) {
            app->settings.resample_rate_a = cxadc_base_rate_a_khz;
            cxadc_settings_changed = true;
        }
        if (!app->settings.enable_resample_b) {
            if (fabsf(app->settings.resample_rate_b - cxadc_base_rate_b_khz) > 0.5f) {
                app->settings.resample_rate_b = cxadc_base_rate_b_khz;
                cxadc_settings_changed = true;
            }
        } else if (app->settings.resample_rate_b > cxadc_base_rate_b_khz) {
            app->settings.resample_rate_b = cxadc_base_rate_b_khz;
            cxadc_settings_changed = true;
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

    uint64_t free_bytes = 0;
    if (gui_record_get_output_free_space_bytes(app, &free_bytes)) {
        s_status_free_space_cached_bytes = free_bytes;
        s_status_free_space_valid = true;
    } else {
        s_status_free_space_valid = false;
    }

    if (!app->is_recording) {
        s_status_output_last_bytes = 0;
        s_status_output_last_sample_s = 0.0;
        s_status_output_rate_bps = 0.0;
        return;
    }

    uint64_t output_bytes = gui_ui_recording_output_total_bytes(app);
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
    Vector2 m = MeasureTextEx(font, safe_text, (float)font_size, 0.0f);
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
    Vector2 m = MeasureTextEx(font, safe_text, (float)font_size, 0.0f);
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
    if (MeasureTextEx(font, text, (float)font_size, 0.0f).x <=
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
        if (MeasureTextEx(font, text, (float)font_size, 0.0f).x <=
            (float)max_text_width) {
            return;
        }
        text[cut] = '\0';
    }
    snprintf(text, text_capacity, "...");
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
    // User-facing presets (stored as kHz), including 40 MSPS passthrough base.
    static const float presets_khz[] = { 5000.0f, 10000.0f, 14300.0f, 17900.0f, 20000.0f, 40000.0f };
    const int n = (int)(sizeof(presets_khz) / sizeof(presets_khz[0]));
    if (max_khz < 5000.0f) max_khz = 5000.0f;

    float allowed[n];
    int allowed_count = 0;
    for (int i = 0; i < n; i++) {
        if (presets_khz[i] <= max_khz + 0.5f) {
            allowed[allowed_count++] = presets_khz[i];
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
    return app && app->is_recording;
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
            return true;
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
        // Integer percent only.
        return (ch >= '0' && ch <= '9');
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
    if (ch < 32 || ch >= 127) {
        return false;
    }
    if (field == UI_TEXT_FIELD_OUTPUT_PATH) {
        return !(ch == '*' || ch == '?' || ch == '\"' || ch == '<' || ch == '>' || ch == '|');
    }
    return !(ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '\"' || ch == '<' || ch == '>' || ch == '|');
}

static void gui_ui_text_field_font(ui_text_field_t field, int *font_size, int *font_id)
{
    int size = FONT_SIZE_NORMAL;
    int id = 0;
    switch (field) {
        case UI_TEXT_FIELD_OUTPUT_BASE_NAME:
        case UI_TEXT_FIELD_OUTPUT_PATH:
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

static float gui_ui_text_char_width_px(const gui_app_t *app, int font_id, int font_size, unsigned char ch)
{
    Font font = gui_ui_text_get_font(app, font_id);
    char glyph[2] = { (char)ch, '\0' };
    Vector2 m = MeasureTextEx(font, glyph, (float)font_size, 0.0f);
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
    for (int i = 0; i < (int)len; i++) {
        unsigned char ch = (unsigned char)text[i];
        float w = gui_ui_text_char_width_px(app, font_id, font_size, ch);
        if (local_x < x + (w * 0.5f)) {
            return i;
        }
        x += w;
    }
    return (int)len;
}

static void gui_ui_text_clamp_state(const char *dst)
{
    size_t len = dst ? strlen(dst) : 0;
    if (s_active_text_cursor < 0) s_active_text_cursor = 0;
    if ((size_t)s_active_text_cursor > len) s_active_text_cursor = (int)len;
    if (s_active_text_selection_anchor >= 0) {
        if (s_active_text_selection_anchor < 0) s_active_text_selection_anchor = 0;
        if ((size_t)s_active_text_selection_anchor > len) s_active_text_selection_anchor = (int)len;
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
    if (!dst || cap == 0) return false;
    size_t len = strlen(dst);
    if (len + 1 >= cap) return false;
    if (s_active_text_cursor < 0) s_active_text_cursor = 0;
    if ((size_t)s_active_text_cursor > len) s_active_text_cursor = (int)len;
    memmove(dst + s_active_text_cursor + 1,
            dst + s_active_text_cursor,
            len - (size_t)s_active_text_cursor + 1);
    dst[s_active_text_cursor] = (char)ch;
    s_active_text_cursor++;
    return true;
}

static bool gui_ui_text_insert_filtered(ui_text_field_t field, char *dst, size_t cap, const char *src)
{
    if (!dst || !src) return false;
    bool changed = false;
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        int ch = (int)(*p);
        if (!gui_ui_text_field_char_allowed(field, ch)) continue;
        if (!gui_ui_text_insert_char(dst, cap, ch)) break;
        changed = true;
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
    int anchor = s_active_text_selection_anchor;
    if (anchor < 0) anchor = cursor;
    if (anchor > len) anchor = len;
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
    if ((size_t)*cursor > len) *cursor = (int)len;
    memmove(dst + *cursor - 1, dst + *cursor, len - (size_t)(*cursor) + 1);
    (*cursor)--;
    return true;
}

static bool gui_ui_text_delete(char *dst, int *cursor)
{
    if (!dst || !cursor) return false;
    size_t len = strlen(dst);
    if ((size_t)*cursor >= len) return false;
    memmove(dst + *cursor, dst + *cursor + 1, len - (size_t)(*cursor));
    return true;
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
        if (gui_ui_text_delete_selection(dst)) changed = true;
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
            for (size_t i = 0; i < android_len; i++) {
                unsigned char ach = (unsigned char)android_text[i];
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
                if (gui_ui_text_delete_selection(dst)) changed = true;
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
            if (gui_ui_text_delete_selection(dst)) changed = true;
            if (gui_ui_text_insert_char(dst, cap, ch)) changed = true;
        }
        ch = GetCharPressed();
    }

    if (IsKeyPressed(KEY_LEFT)) {
        gui_ui_text_set_cursor_position(dst, s_active_text_cursor - 1, shift_down);
    }
    if (IsKeyPressed(KEY_RIGHT)) {
        gui_ui_text_set_cursor_position(dst, s_active_text_cursor + 1, shift_down);
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
    if (changed) {
        gui_settings_save(&app->settings);
    }

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
        gui_settings_save(&app->settings);
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

// Output path display + choose button
CLAY(CLAY_ID("SettingsOutputPath"), {
    .layout = {
        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
        .layoutDirection = CLAY_TOP_TO_BOTTOM,
        .childGap = 6
    }
}) {
    CLAY_TEXT(CLAY_STRING("Output folder:"),
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

                            // Rate selector (kHz stored; display MSPS)
                            format_msps_label(settings_resample_a_display, sizeof(settings_resample_a_display), app->settings.resample_rate_a);
                            Color rate_bg = !app->settings.enable_resample_a ? ui_disabled_color(COLOR_BUTTON) : COLOR_BUTTON;
                            Color rate_fg = !app->settings.enable_resample_a ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
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

                        format_msps_label(settings_resample_b_display, sizeof(settings_resample_b_display), app->settings.resample_rate_b);
                        Color rate_bg = (settings_b_controls_disabled || !app->settings.enable_resample_b) ? ui_disabled_color(COLOR_BUTTON) : COLOR_BUTTON;
                        Color rate_fg = (settings_b_controls_disabled || !app->settings.enable_resample_b) ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
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
                    CLAY_TEXT(CLAY_STRING("Playback files (FLAC):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    // Channel A playback file
                    CLAY(CLAY_ID("PlaybackFileARow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 6 } }) {
                        CLAY(CLAY_ID("PlaybackFileBrowseA"), { .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("Ch A..."), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        // Show filename or "(none)"
                        const char *file_a = app->settings.playback_file_a[0] ? app->settings.playback_file_a : "(none)";
                        // Truncate long paths for display
                        size_t len_a = strlen(file_a);
                        if (len_a > 30) {
                            snprintf(playback_file_a_display, sizeof(playback_file_a_display), "...%s", file_a + len_a - 27);
                        } else {
                            snprintf(playback_file_a_display, sizeof(playback_file_a_display), "%s", file_a);
                        }
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
                        size_t len_b = strlen(file_b);
                        if (len_b > 30) {
                            snprintf(playback_file_b_display, sizeof(playback_file_b_display), "...%s", file_b + len_b - 27);
                        } else {
                            snprintf(playback_file_b_display, sizeof(playback_file_b_display), "%s", file_b);
                        }
                        CLAY(CLAY_ID("PlaybackFileBPath"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(playback_file_b_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(app->settings.playback_file_b[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                        }
                        CLAY(CLAY_ID("PlaybackFileClearB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
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

    int record_limit_max_width = gui_ui_modal_max_extent(gui_ui_get_layout_width(), 420);
    int record_limit_max_height = gui_ui_modal_max_extent(gui_ui_get_layout_height(), 440);
    int record_limit_min_width = gui_ui_clamp_int(record_limit_max_width, 1, 420);
    int record_limit_min_height = gui_ui_clamp_int(record_limit_max_height, 1, 235);

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
                CLAY_SIZING_FIT(.min = record_limit_min_width, .max = record_limit_max_width),
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
            .horizontal = true,
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
        Vector2 record_limit_timecode_text_size = MeasureTextEx(record_limit_font,
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

        // Level autostop (tape-end detection): enable/disable + level box + duration box.
        // Lives in the timer window alongside the record time limit. Independent from
        // the digital dropout (frame error/missed frame) logic in the main settings.
        CLAY_TEXT(CLAY_STRING("Level autostop (tape end):"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        CLAY(CLAY_ID("LevelAutostopRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 10
            }
        }) {
            Color las_bg = app->settings.level_autostop_enabled ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
            CLAY(CLAY_ID("LevelAutostopToggle"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(las_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(app->settings.level_autostop_enabled ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }

            // Level percent box (click to edit)
            Color lvl_box_bg = app->settings.level_autostop_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
            Color lvl_box_fg = app->settings.level_autostop_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
            CLAY(CLAY_ID("LevelAutostopLevelField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(56), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 6, 6, 0, 0 }
                },
                .backgroundColor = to_clay_color(lvl_box_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *lvl = app->settings.level_autostop_level_str[0] ? app->settings.level_autostop_level_str : "33";
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL) && app->settings.level_autostop_enabled) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL, lvl, FONT_SIZE_STATS, 1, lvl_box_fg);
                } else {
                    CLAY_TEXT(make_string(lvl), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(lvl_box_fg) }));
                }
            }
            CLAY_TEXT(CLAY_STRING("% level"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

            CLAY(CLAY_ID("LevelAutostopSpacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } } }) { }

            // Duration seconds box (click to edit)
            Color dur_box_bg = app->settings.level_autostop_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
            Color dur_box_fg = app->settings.level_autostop_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
            CLAY(CLAY_ID("LevelAutostopDurationField"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(64), CLAY_SIZING_FIXED(32) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER },
                    .padding = { 6, 6, 0, 0 }
                },
                .backgroundColor = to_clay_color(dur_box_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                const char *dur = app->settings.level_autostop_duration_str[0] ? app->settings.level_autostop_duration_str : "5.0";
                if (gui_ui_is_text_field_active(UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION) && app->settings.level_autostop_enabled) {
                    gui_ui_render_active_text(UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION, dur, FONT_SIZE_STATS, 1, dur_box_fg);
                } else {
                    CLAY_TEXT(make_string(dur), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(dur_box_fg) }));
                }
            }
            CLAY_TEXT(CLAY_STRING("s below"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
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
    if (app->is_recording)      { state_label = "Recording";  state_col = COLOR_CLIP_RED;   }
    else if (app->is_capturing) { state_label = "Capturing";  state_col = COLOR_SYNC_GREEN; }
    else                        { state_label = "Idle";       state_col = COLOR_TEXT_DIM;   }
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
    Color update_status_color = COLOR_TEXT_DIM;
    if (update_check_running) {
        snprintf(vi_update_status, sizeof(vi_update_status), "Checking...");
    } else if (app->settings.update_last_release_tag[0]) {
        int update_cmp = gui_ui_compare_versions(MIRSC_TOOLS_VERSION, app->settings.update_last_release_tag);
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
    Color update_download_btn_bg = update_check_running ? ui_disabled_color(COLOR_BUTTON)
                                                        : COLOR_BUTTON;
    Color update_download_btn_fg = update_check_running ? ui_disabled_color(COLOR_TEXT)
                                                        : COLOR_TEXT;
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
            CLAY(CLAY_ID("VersionInfoDownloadButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(24) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(update_download_btn_bg),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("Download Latest"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(update_download_btn_fg) }));
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
        CLAY_TEXT(CLAY_STRING("Network (Server/Client):"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_HEADING, .textColor = to_clay_color(COLOR_TEXT) }));

        {
            int mode = app->settings.net_mode;
            if (mode < GUI_NET_MODE_LOCAL || mode > GUI_NET_MODE_CLIENT) mode = GUI_NET_MODE_LOCAL;
            const char *mode_label = gui_net_mode_name(mode);
            Color mode_bg = (mode == GUI_NET_MODE_LOCAL) ? COLOR_BUTTON : COLOR_BUTTON_ACTIVE;
            CLAY(CLAY_ID("VersionInfoNetModeRow"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
            }) {
                CLAY(CLAY_ID("VersionInfoNetModeLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Mode:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
                CLAY(CLAY_ID("VersionInfoNetModeToggle"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(28) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(mode_bg),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    CLAY_TEXT(make_string(mode_label),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                }
                CLAY_TEXT(CLAY_STRING("click to cycle Local / Server / Client"),
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

        // Client: discovered-server list (primary) + compact manual host:port fallback.
        if (app->settings.net_mode == GUI_NET_MODE_CLIENT) {
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
                    CLAY_TEXT(CLAY_STRING("click a server to connect (LAN only, no auth)"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
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
                Color connect_bg = net_connected ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                const char *connect_label = net_connected ? "Disconnect" : "Connect";
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
                CLAY_TEXT(CLAY_STRING("toggle client connection"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
        }

        // Network status line.
        {
            static char net_status[160];
            gui_net_status_string(app, net_status, sizeof(net_status));
            CLAY(CLAY_ID("VersionInfoNetStatusRow"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
            }) {
                CLAY(CLAY_ID("VersionInfoNetStatusLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Status:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }
                CLAY_TEXT(make_string(net_status),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
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
#if defined(_WIN32)
        bool cxadc_win_audio_map = cxadc_mode;
#else
        (void)cxadc_mode;
        bool cxadc_win_audio_map = false;
#endif
        const char *audio_ch_toggle_label = NULL;
        if (cxadc_win_audio_map) {
            audio_ch_toggle_label = app->settings.audio_monitor_ch34
                ? (toolbar_ultra_narrow ? "HSW" : "HSW CH3")
                : (toolbar_ultra_narrow ? "A1/2" : "AUD 1/2");
        } else {
            audio_ch_toggle_label = app->settings.audio_monitor_ch34
                ? (toolbar_ultra_narrow ? "3/4" : "CH3/4")
                : (toolbar_ultra_narrow ? "1/2" : "CH1/2");
        }
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

static void render_toolbar_connection_group(gui_app_t *app,
                                            bool toolbar_tiny,
                                            bool toolbar_very_narrow,
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
        bool control_capturing = app->is_capturing;
        if (gui_net_is_client(app)) {
            control_capturing = gui_net_client_peer_capturing(app);
        }
        Color connect_color = control_capturing ? COLOR_CLIP_RED : COLOR_SYNC_GREEN;
        /* Pick the longest connect/disconnect label that measurably fits the
         * fixed-width button (same measured-fit rule as the Audio Mon
         * label). Tier width steps alone missed the ultra-narrow profile,
         * which shrank the button without tripping the very_narrow flag and
         * left the full-size text overflowing a 66px button. */
        const char *connect_long = control_capturing ? "Disconnect" : "Connect";
        const char *connect_med = control_capturing ? "Disc" : "Conn";
        const char *connect_short = control_capturing ? "Dis" : "Con";
        const char *connect_label;
        if (gui_ui_measure_text_width(app, connect_long, toolbar_text_size, 0) <= connect_button_width - 4) {
            connect_label = connect_long;
        } else if (gui_ui_measure_text_width(app, connect_med, toolbar_text_size, 0) <= connect_button_width - 4) {
            connect_label = connect_med;
        } else {
            connect_label = connect_short;
        }
        (void)toolbar_tiny;
        (void)toolbar_very_narrow;
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
            .backgroundColor = to_clay_color(mode_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(make_string(mode_label),
                CLAY_TEXT_CONFIG({
                    .fontSize = toolbar_text_size,
                    .textColor = to_clay_color(mode_fg),
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
                                         int mode_width)
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
        p->connect_width + mode_width + p->gap +
        p->audio_mon_width + p->audio_ch_width + p->bars_panel_width +
        (p->gap * 2) +
        p->record_width + (p->icon_button_size * 2) +
        ((child_count - 1) * p->gap) + 8;
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

static void render_toolbar(gui_app_t *app) {
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
    const char *device_name = app->device_count > 0
        ? app->devices[app->selected_device].name
        : "No devices";
    snprintf(device_dropdown_buf, sizeof(device_dropdown_buf), "%s", device_name);
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
            int tier_required = gui_ui_toolbar_required_width(&p, dev_label_w, p.dd_min_width, mode_w);
            if (tier == GUI_UI_TOOLBAR_TIER_TINY) {
                tiny_required_width = tier_required;
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
    int device_dropdown_width = gui_ui_measure_button_width(app,
                                                            device_dropdown_buf,
                                                            toolbar_text_size,
                                                            dropdown_padding,
                                                            16,
                                                            device_dropdown_min_width,
                                                            device_dropdown_max_width);
    device_dropdown_width = device_dropdown_min_width +
        gui_ui_clamp_int(toolbar_slack, 0, device_dropdown_width - device_dropdown_min_width);
    gui_ui_ellipsize_text(app,
                          device_dropdown_buf,
                          sizeof(device_dropdown_buf),
                          toolbar_text_size,
                          0,
                          device_dropdown_width - (dropdown_padding * 2) - 16);
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
        // Version/status icon (fixed, compact-hidden in tiny layouts)
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
        Color dropdown_color = device_dropdown_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
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
                    .textColor = to_clay_color(COLOR_TEXT),
                    .wrapMode = CLAY_TEXT_WRAP_NONE
                }));
        }

        if (!toolbar_two_rows) {
            render_toolbar_connection_group(app,
                                            toolbar_tiny,
                                            toolbar_very_narrow,
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
        bool playback_mode = gui_ui_selected_device_is_playback(app);
        bool playback_running = playback_mode && gui_playback_is_running(app);
        playback_state_t playback_state = playback_running
            ? gui_playback_get_state(app)
            : PLAYBACK_STATE_STOPPED;
        bool playback_paused = (playback_state == PLAYBACK_STATE_PAUSED);

        // Record button (capture) / Play-Pause button (playback mode)
        bool record_finalizing = gui_record_is_finalizing();
        Color record_color = record_finalizing ? (Color){184, 118, 20, 255} : (app->is_recording ? COLOR_CLIP_RED : COLOR_BUTTON);
        const char *record_label = record_finalizing ? "Finalize" : (app->is_recording ? "Stop Rec" : "Record");
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
            } else if (app->is_recording) {
                record_display_label = "Stop";
            } else {
                record_display_label = "Rec";
            }
        }
        if (!app->is_capturing) record_color = (Color){ 50, 50, 55, 255 };
        CLAY(CLAY_ID("RecordButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(record_button_width), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(record_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            Color text_color = app->is_capturing ? COLOR_TEXT : COLOR_TEXT_DIM;
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
                render_toolbar_connection_group(app,
                                                toolbar_tiny,
                                                toolbar_very_narrow,
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
    .childGap = 4 \
}

// Fixed width for labels to ensure alignment
#define LABEL_WIDTH 50

// Render per-channel stats panel (trigger controls moved to waveform panel overlay)
static void render_channel_stats(gui_app_t *app, int channel) {
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
        clip_pos = atomic_load(&app->clip_count_a_pos);
        clip_neg = atomic_load(&app->clip_count_a_neg);
        peak_pos = app->vu_a.peak_pos;
        peak_neg = app->vu_a.peak_neg;
        buf_peak_pos = stat_a_peak_pos;
        buf_peak_neg = stat_a_peak_neg;
        buf_clip_pos = stat_a_clip_pos;
        buf_clip_neg = stat_a_clip_neg;
        buf_rec_raw = stat_rec_raw[0];
        buf_rec_flac = stat_rec_flac[0];
        buf_rec_ratio = stat_rec_ratio[0];
        buf_rec_duration = stat_rec_duration[0];
    } else {
        clip_pos = atomic_load(&app->clip_count_b_pos);
        clip_neg = atomic_load(&app->clip_count_b_neg);
        peak_pos = app->vu_b.peak_pos;
        peak_neg = app->vu_b.peak_neg;
        buf_peak_pos = stat_b_peak_pos;
        buf_peak_neg = stat_b_peak_neg;
        buf_clip_pos = stat_b_clip_pos;
        buf_clip_neg = stat_b_clip_neg;
        buf_rec_raw = stat_rec_raw[1];
        buf_rec_flac = stat_rec_flac[1];
        buf_rec_ratio = stat_rec_ratio[1];
        buf_rec_duration = stat_rec_duration[1];
    }

    // Format stats (peak/clip/errors)
    snprintf(buf_peak_pos, 16, "+%.0f%%", peak_pos * 100.0f);
    snprintf(buf_peak_neg, 16, "-%.0f%%", peak_neg * 100.0f);
    snprintf(buf_clip_pos, 16, "+%u", clip_pos);
    snprintf(buf_clip_neg, 16, "-%u", clip_neg);

    CLAY(CLAY_IDI("StatsPanel", channel), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(185), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 6, 6, 4, 4 },
            .childGap = 2
        },
        .backgroundColor = to_clay_color((Color){ 35, 35, 42, 255 })
    }) {
        // Channel label
        // CLAY_TEXT(channel == 0 ? CLAY_STRING("Channel A") : CLAY_STRING("Channel B"),
        //     CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS_LABEL, .textColor = to_clay_color(channel_color) }));

        // Samples row removed (shown in status bar)

        // Peak row (shows both + and -)
        CLAY(CLAY_IDI("StatPeak", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblPeak", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Peak:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(buf_peak_pos),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(peak_pos > 0.95f ? COLOR_CLIP_RED : COLOR_TEXT) }));
            CLAY_TEXT(make_string(buf_peak_neg),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(peak_neg > 0.95f ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }

        // Clip row (shows both + and -)
        CLAY(CLAY_IDI("StatClip", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblClip", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Clip:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(buf_clip_pos),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(clip_pos > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
            CLAY_TEXT(make_string(buf_clip_neg),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(clip_neg > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }

        // Reset button row (kept separate so it scales better)
        CLAY(CLAY_IDI("ResetClipRow", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblClipReset", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
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
                CLAY(CLAY_IDI("LblDcOffset", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
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
            uint64_t raw_bytes = (channel == 0)
                                    ? atomic_load(&app->recording_raw_a)
                                    : atomic_load(&app->recording_raw_b);
            uint64_t comp_bytes = (channel == 0)
                                    ? atomic_load(&app->recording_compressed_a)
                                    : atomic_load(&app->recording_compressed_b);
            bool show_record_stats = app->is_capturing && (app->is_recording || raw_bytes > 0 || comp_bytes > 0 || app->last_recording_duration_s > 0.0);
            if (show_record_stats) {
                double shown_duration = app->is_recording ? (GetTime() - app->recording_start_time) : app->last_recording_duration_s;
                int d_hours = (int)(shown_duration / 3600.0);
                int d_mins = ((int)(shown_duration / 60.0)) % 60;
                int d_secs = ((int)(shown_duration)) % 60;

                snprintf(buf_rec_duration, 24, "Dur: %02d:%02d:%02d", d_hours, d_mins, d_secs);
                if (raw_bytes >= 1073741824ULL) {
                    double raw_gb = (double)raw_bytes / (1024.0 * 1024.0 * 1024.0);
                    snprintf(buf_rec_raw, 32, "RAW: %.2f GB", raw_gb);
                } else {
                    double raw_mb = (double)raw_bytes / (1024.0 * 1024.0);
                    snprintf(buf_rec_raw, 32, "RAW: %.1f MB", raw_mb);
                }
                if (comp_bytes > 0 || app->settings.use_flac) {
                    double ratio = (comp_bytes > 0) ? ((double)raw_bytes / (double)comp_bytes) : 0.0;
                    if (comp_bytes >= 1073741824ULL) {
                        double comp_gb = (double)comp_bytes / (1024.0 * 1024.0 * 1024.0);
                        snprintf(buf_rec_flac, 32, "FLAC: %.2f GB", comp_gb);
                    } else {
                        double comp_mb = (double)comp_bytes / (1024.0 * 1024.0);
                        snprintf(buf_rec_flac, 32, "FLAC: %.1f MB", comp_mb);
                    }
                    snprintf(buf_rec_ratio, 24, "Ratio: %.1fx", ratio);
                } else {
                    buf_rec_flac[0] = '\0';
                    buf_rec_ratio[0] = '\0';
                }
                CLAY(CLAY_IDI("RecDurationRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                    CLAY_TEXT(make_string(buf_rec_duration),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(channel_value_color) }));
                }
                if (!quarter_scale_layout) {
                    CLAY(CLAY_IDI("RecRawRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                        CLAY_TEXT(make_string(buf_rec_raw),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(channel_value_color) }));
                    }
                }
                if (buf_rec_flac[0]) {
                    CLAY(CLAY_IDI("RecFlacRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                        CLAY_TEXT(make_string(buf_rec_flac),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(channel_value_color) }));
                    }
                }
                if (buf_rec_ratio[0]) {
                    CLAY(CLAY_IDI("RecRatioRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                        CLAY_TEXT(make_string(buf_rec_ratio),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(channel_value_color) }));
                    }
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

        // Layout row (Single/Split toggle)
        CLAY(CLAY_IDI("LayoutRow", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblLayout", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Layout:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }

            const char *layout_name = panel_split ? "Split" : "Single";
            bool layout_dropdown_open = gui_dropdown_is_open(DROPDOWN_LAYOUT, channel);
            CLAY(CLAY_IDI("LayoutBtn", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIXED(18) },
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
            CLAY(CLAY_IDI("LayoutOpts", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .floating = {
                    .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                    .parentId = CLAY_IDI("LayoutBtn", channel).id,
                    .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
                },
                .backgroundColor = to_clay_color(COLOR_PANEL_BG),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
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
            CLAY(CLAY_IDI("LblLeft", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(panel_split ? CLAY_STRING("Left:") : CLAY_STRING("View:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }

            const char *left_name = panel_view_type_name((panel_view_type_t)left_view);
            bool left_dropdown_open = gui_dropdown_is_open(DROPDOWN_LEFT_VIEW, channel);
            CLAY(CLAY_IDI("LeftViewBtn", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIXED(18) },
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
            CLAY(CLAY_IDI("LeftViewOpts", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .floating = {
                    .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                    .parentId = CLAY_IDI("LeftViewBtn", channel).id,
                    .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
                },
                .backgroundColor = to_clay_color(COLOR_PANEL_BG),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
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
                CLAY(CLAY_IDI("LblRight", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Right:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }

                const char *right_name = panel_view_type_name((panel_view_type_t)right_view);
                bool right_dropdown_open = gui_dropdown_is_open(DROPDOWN_RIGHT_VIEW, channel);
                CLAY(CLAY_IDI("RightViewBtn", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIXED(18) },
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
                CLAY(CLAY_IDI("RightViewOpts", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    },
                    .floating = {
                        .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                        .parentId = CLAY_IDI("RightViewBtn", channel).id,
                        .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
                    },
                    .backgroundColor = to_clay_color(COLOR_PANEL_BG),
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
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

// Render one playback timeline row (used for Channel A and Channel B in playback mode).
static void render_playback_timeline_row(int channel_index, const char *timeline_text, int track_width_px, int fill_w, bool enabled)
{
    Color timeline_text_color = enabled ? COLOR_TEXT : COLOR_TEXT_DIM;
    Color timeline_track_color = enabled ? (Color){45, 45, 52, 255} : (Color){33, 33, 38, 255};
    Color timeline_fill_color = enabled ? COLOR_SYNC_GREEN : COLOR_TEXT_DIM;
    int screen_width = gui_ui_get_layout_width();
    int left_pad_width = screen_width < 900 ? 60 : 74;
    int label_width = screen_width < 900 ? 120 : 150;
    int right_pad_width = screen_width < 900 ? 0 : 189;
    CLAY(CLAY_IDI("PlaybackTimelineRow", channel_index), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = 4
        }
    }) {
        CLAY(CLAY_IDI("PlaybackTimelineLeftPad", channel_index), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(left_pad_width), CLAY_SIZING_GROW(0) } }
        }) {}

        CLAY(CLAY_IDI("PlaybackTimeline", channel_index), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 8
            }
        }) {
            CLAY(CLAY_IDI("PlaybackTimelineLabel", channel_index), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(label_width), CLAY_SIZING_FIT(0) } }
            }) {
                CLAY_TEXT(make_string(timeline_text),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(timeline_text_color) }));
            }

            CLAY(CLAY_IDI("PlaybackTimelineTrack", channel_index), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(track_width_px), CLAY_SIZING_FIXED(10) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
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
                }
            }
        }

        CLAY(CLAY_IDI("PlaybackTimelineRightPad", channel_index), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(right_pad_width), CLAY_SIZING_GROW(0) } }
        }) {}
    }
}

// Render the channels panel - each channel has VU meter + waveform + stats grouped together
static void render_channels_panel(gui_app_t *app) {
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
    // DdD and FX3 are single-channel: hide the Channel B row and apply a
    // natural fill layout for Channel A height (no reserved spacer/dead area).
    bool single_channel_preview = ddd_single_channel || fx3_single_channel;
    Clay_SizingAxis channel_a_height = CLAY_SIZING_GROW(0);
    bool playback_mode = gui_ui_selected_device_is_playback(app);

    // Setup custom element data for this frame
    s_vu_a_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER;
    s_vu_a_element.customData.vu_meter.meter = &app->vu_a;
    s_vu_a_element.customData.vu_meter.label = "CH A";
    s_vu_a_element.customData.vu_meter.is_clipping_pos = atomic_load(&app->clip_count_a_pos) > 0;
    s_vu_a_element.customData.vu_meter.is_clipping_neg = atomic_load(&app->clip_count_a_neg) > 0;
    s_vu_a_element.customData.vu_meter.channel_color = COLOR_CHANNEL_A;

    s_osc_a_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_CHANNEL_PANEL;
    s_osc_a_element.customData.channel_panel.app = app;
    s_osc_a_element.customData.channel_panel.channel = 0;

    s_vu_b_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER;
    s_vu_b_element.customData.vu_meter.meter = &app->vu_b;
    s_vu_b_element.customData.vu_meter.label = "CH B";
    s_vu_b_element.customData.vu_meter.is_clipping_pos = atomic_load(&app->clip_count_b_pos) > 0;
    s_vu_b_element.customData.vu_meter.is_clipping_neg = atomic_load(&app->clip_count_b_neg) > 0;
    s_vu_b_element.customData.vu_meter.channel_color = COLOR_CHANNEL_B;

    s_osc_b_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_CHANNEL_PANEL;
    s_osc_b_element.customData.channel_panel.app = app;
    s_osc_b_element.customData.channel_panel.channel = 1;

    s_settings_icon_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_SETTINGS_ICON;

    CLAY(CLAY_ID("ChannelsPanel"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 4, 4, 4, 4 },
            .childGap = 8
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG)
    }) {
        int screen_width = gui_ui_get_layout_width();
        int playback_track_width_px = screen_width < 900 ? 180 : (screen_width < 1150 ? 240 : 300);
        int playback_fill_w_a = 0;
        int playback_fill_w_b = 0;
        bool playback_has_file_a = false;
        bool playback_has_file_b = false;
        snprintf(playback_timeline_display_a, sizeof(playback_timeline_display_a), "--:--:--/--:--:--");
        snprintf(playback_timeline_display_b, sizeof(playback_timeline_display_b), "--:--:--/--:--:--");
        if (playback_mode) {
            uint64_t current_sample_a = gui_playback_get_position_samples_channel(app, 0);
            uint64_t current_sample_b = gui_playback_get_position_samples_channel(app, 1);
            uint64_t total_samples_a = 0;
            uint64_t total_samples_b = 0;
            double duration_seconds_a = 0.0;
            double duration_seconds_b = 0.0;
            (void)gui_ui_playback_channel_timeline_info(app, 0, &total_samples_a, &duration_seconds_a);
            (void)gui_ui_playback_channel_timeline_info(app, 1, &total_samples_b, &duration_seconds_b);
            gui_ui_format_playback_timeline(playback_timeline_display_a, sizeof(playback_timeline_display_a),
                                            &playback_fill_w_a, &playback_has_file_a,
                                            current_sample_a, total_samples_a, duration_seconds_a, playback_track_width_px);
            gui_ui_format_playback_timeline(playback_timeline_display_b, sizeof(playback_timeline_display_b),
                                            &playback_fill_w_b, &playback_has_file_b,
                                            current_sample_b, total_samples_b, duration_seconds_b, playback_track_width_px);

            // Playback scrub row aligned with Channel A preview.
            render_playback_timeline_row(0, playback_timeline_display_a, playback_track_width_px, playback_fill_w_a, playback_has_file_a);
        }
        // Channel A row: VU meter + waveform + stats
        CLAY(CLAY_ID("ChannelARow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), channel_a_height },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 4
            }
        }) {
            // VU meter A - custom element
            CLAY(CLAY_ID("VUMeterA"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_vu_a_element }
            }) {}

            // Oscilloscope canvas A - custom element
            CLAY(CLAY_ID("OscilloscopeCanvasA"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_osc_a_element }
            }) {}

            // Stats panel A
            render_channel_stats(app, 0);
        }

        // Channel B row: VU meter + waveform + stats
        // Hidden entirely for single-channel devices (DdD/FX3).
        if (!single_channel_preview) {
            if (playback_mode) {
                // Second playback scrub row aligned with Channel B preview.
                render_playback_timeline_row(1, playback_timeline_display_b, playback_track_width_px, playback_fill_w_b, playback_has_file_b);
            }
            CLAY(CLAY_ID("ChannelBRow"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 4
                }
            }) {
                // VU meter B - custom element
                CLAY(CLAY_ID("VUMeterB"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_GROW(0) } },
                    .custom = { .customData = &s_vu_b_element }
                }) {}

                // Oscilloscope canvas B - custom element
                CLAY(CLAY_ID("OscilloscopeCanvasB"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                    .custom = { .customData = &s_osc_b_element }
                }) {}

                // Stats panel B
                render_channel_stats(app, 1);
            }
        }
    }
}

// Render status bar
static void render_status_bar(gui_app_t *app) {
    int status_width = gui_ui_get_layout_width();
    int status_height = gui_ui_get_layout_height();
    gui_ui_status_layout_mode_t status_layout =
        gui_ui_get_status_layout_mode(status_width, status_height,
                                      app->is_recording);
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
    bool status_compact = status_layout != GUI_UI_STATUS_LAYOUT_FULL_SINGLE;
    bool device_buffer_tiny = status_tiny || status_minimal ||
        status_width < GUI_UI_STATUS_RECORDING_NARROW_BREAKPOINT;
    gui_device_buffer_layout_t device_buffer_layout = device_buffer_tiny
        ? GUI_DEVICE_BUFFER_LAYOUT_TINY
        : (status_compact
            ? GUI_DEVICE_BUFFER_LAYOUT_COMPACT
            : GUI_DEVICE_BUFFER_LAYOUT_FULL);
    bool show_device_buffer_status = gui_ui_get_device_buffer_view(
        app, device_buffer_layout, &status_device_buffer_view);
    // Detect an error/denied/failed or critical capture-stop status so the bar can
    // yield space to it: when an error is being shown, hide the free-space
    // readout (and widen the message budget) so the actual error text isn't
    // truncated behind the free-space label. This is what makes CXADC/USB
    // permission failures readable in the bottom bar instead of "CXADC permi...".
    const char *raw_status_gate = (app->status_message[0] != '\0') ? app->status_message : NULL;
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
    // Long error messages get an explicit second line whenever there is enough
    // height. Normal recording status is redundant with the timer, so omit it
    // at every width to leave room for free-space/runway information.
    bool status_two_rows = gui_ui_status_uses_two_rows(status_layout,
                                                       status_is_error);
    bool show_status_message = status_is_error ||
        (!status_minimal && !app->is_recording);
    /* Keep free-space visible by default and let the dynamic budget contract
     * gaps/widths first; only hide it as a last resort. Error status still
     * reserves the left-side for readable failure text. */
    bool show_free_space = !status_is_error;
    /* Full-quality starting values: the budget loop contracts them only
     * under measured pressure, so the bar keeps full labels, spacing, and
     * containers whenever the real space allows it. Only the absolute tiny
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
    bool status_compact_labels = s_status_labels_compact;
    bool status_labels_flipped = false;
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
    update_status_free_space(app);

    bool show_record_indicator = app->is_recording && !status_quarter_scale;
    if (show_record_indicator) {
        double rec_duration = GetTime() - app->recording_start_time;
        int rec_hours = (int)(rec_duration / 3600);
        int rec_mins  = ((int)(rec_duration / 60)) % 60;
        int rec_secs  = ((int)rec_duration) % 60;
        snprintf(status_record_timer_display, sizeof(status_record_timer_display),
                 "%02d:%02d:%02d", rec_hours, rec_mins, rec_secs);
    }

    const char *raw_status = (app->status_message[0] != '\0')
        ? app->status_message
        : (app->is_capturing ? "Capturing..." : "Ready");
    Color status_color = COLOR_TEXT_DIM;
    if (status_is_error) {
        status_color = COLOR_CLIP_RED;
    } else if (strstr(raw_status, "Requesting") != NULL ||
               strstr(raw_status, "Reconnecting") != NULL ||
               strstr(raw_status, "Waiting") != NULL ||
               strstr(raw_status, "Initializing") != NULL) {
        status_color = COLOR_METER_YELLOW;
    }
    /* Natural rendered width of the status message. The message is a FIT(0)
     * element: it only ever occupies its actual text width, so the budget
     * must not reserve the full min_message_width for short statuses like
     * "Ready" — doing so forced the small readouts to shorten while dead
     * space was still available on the bar. */
    int status_message_natural_width = show_status_message
        ? gui_ui_measure_text_width(app, raw_status, status_font_size, 0)
        : 0;

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
            if (app->is_recording) {
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
        } else if (app->is_recording) {
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
    /* Tiered free-space text: tier 0 full, tier 1 compact, tier 2 drops the
     * byte count while recording (runway only). All tiers are measured
     * up-front so the budget loop can switch tiers without re-measuring. */
    int status_free_space_tier = 0;
    char status_free_space_tier_text[3][120];
    int status_free_space_tier_width[3] = { 0, 0, 0 };
    snprintf(status_free_space_tier_text[0], sizeof(status_free_space_tier_text[0]),
             "%s", status_free_space_display_full);
    snprintf(status_free_space_tier_text[1], sizeof(status_free_space_tier_text[1]),
             "%s", status_free_space_display_compact);
    snprintf(status_free_space_tier_text[2], sizeof(status_free_space_tier_text[2]),
             "%s", status_free_space_display_short);
    if (show_free_space) {
        for (int tier = 0; tier < 3; tier++) {
            status_free_space_tier_width[tier] = gui_ui_measure_text_width(
                app, status_free_space_tier_text[tier], status_font_size, 1);
        }
    }

    uint32_t status_sample_rate_raw = atomic_load(&app->sample_rate);
    show_sample_rate = show_sample_rate && status_sample_rate_raw > 0;
    uint64_t status_samples_raw = atomic_load(&app->samples_a);

    /* Tiered right-side value text: tier 0 is the full readout, higher tiers
     * shorten it under width pressure instead of hiding the readout. All
     * tiers are formatted and measured up-front. */
    int status_sample_rate_tier = 0;
    int status_samples_tier = 0;
    char status_sample_rate_tier_text[3][32];
    int status_sample_rate_tier_width[3] = { 0, 0, 0 };
    char status_samples_tier_text[3][32];
    int status_samples_tier_width[3] = { 0, 0, 0 };
    for (int tier = 0; tier < 3; tier++) {
        format_status_sample_rate_tier(status_sample_rate_tier_text[tier],
                                       sizeof(status_sample_rate_tier_text[tier]),
                                       status_sample_rate_raw, tier);
        status_sample_rate_tier_width[tier] = gui_ui_measure_text_width(
            app, status_sample_rate_tier_text[tier], status_font_size, 1);
        format_status_samples_tier(status_samples_tier_text[tier],
                                   sizeof(status_samples_tier_text[tier]),
                                   status_samples_raw, tier);
        status_samples_tier_width[tier] = gui_ui_measure_text_width(
            app, status_samples_tier_text[tier], status_font_size, 1);
    }

    int status_rf_pct = 0;
    int status_aud_pct = 0;
    /* Format the fixed-form right-side readouts up-front so the width budget
     * can use their real rendered text widths. These were previously formatted
     * inline at draw time while the budget assumed small fixed container
     * widths, so the drawn right edge extended past the accounted width and
     * the last readouts were pushed off the window instead of the layout
     * adapting. */
    {
        format_status_counter(status_frames_display, sizeof(status_frames_display),
                              atomic_load(&app->frame_count));
        format_status_counter(status_missed_display, sizeof(status_missed_display),
                              app->is_capturing ? atomic_load(&app->missed_frame_count) : 0);
        format_status_counter(status_errors_display, sizeof(status_errors_display),
                              app->is_capturing ? atomic_load(&app->error_count) : 0);
        size_t status_rf_head = atomic_load(&app->buffers.buffers[BUF_CAPTURE_RF].head);
        size_t status_rf_tail = atomic_load(&app->buffers.buffers[BUF_CAPTURE_RF].tail);
        size_t status_rf_size = app->buffers.buffers[BUF_CAPTURE_RF].buffer_size;
        status_rf_pct = status_rf_size > 0
            ? (int)(((status_rf_tail - status_rf_head) * 100) / status_rf_size) : 0;
        snprintf(status_rf_buf_display, sizeof(status_rf_buf_display), "%d%%", status_rf_pct);
        size_t status_aud_head = atomic_load(&app->buffers.buffers[BUF_CAPTURE_AUDIO].head);
        size_t status_aud_tail = atomic_load(&app->buffers.buffers[BUF_CAPTURE_AUDIO].tail);
        size_t status_aud_size = app->buffers.buffers[BUF_CAPTURE_AUDIO].buffer_size;
        status_aud_pct = status_aud_size > 0
            ? (int)(((status_aud_tail - status_aud_head) * 100) / status_aud_size) : 0;
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
    int record_timer_text_width = show_record_indicator
        ? gui_ui_measure_text_width(app, status_record_timer_display, status_font_size, 1)
        : 0;
    /* Real rendered width of each right-side value (font 1). Value containers
     * use FIT(min) sizing, so their laid-out width is the larger of the stable
     * minimum and this text width; budgeting anything less lets the drawn
     * right edge escape the accounted width and get clipped off-window.
     * Sample-rate and samples widths come from the tier arrays above. */
    int frames_text_width = gui_ui_measure_text_width(app, status_frames_display, status_font_size, 1);
    int missed_text_width = gui_ui_measure_text_width(app, status_missed_display, status_font_size, 1);
    int errors_text_width = gui_ui_measure_text_width(app, status_errors_display, status_font_size, 1);
    int rf_text_width = gui_ui_measure_text_width(app, status_rf_buf_display, status_font_size, 1);
    int aud_text_width = gui_ui_measure_text_width(app, status_aud_buf_display, status_font_size, 1);
    int free_space_text_width = status_free_space_tier_width[status_free_space_tier];
    int free_space_width_budget = free_space_text_width;
    int free_space_min_width = status_tiny ? 22 : 48;
    int status_content_width = status_width - 24; /* horizontal padding */
    if (status_content_width < 1) status_content_width = 1;
    int min_message_width = status_tiny ? 56 : 96;
    int min_message_floor = 24;
    int status_message_width_budget = 0;
    /* Small-readout hysteresis: start the budget at the level kept from the
     * previous frame instead of recomputing it from scratch, so ticking
     * counters at a boundary width cannot flap the small readouts between
     * full and short forms frame-to-frame. */
    int readout_level_start = s_status_readout_level;
    if (readout_level_start < 0) readout_level_start = 0;
    if (readout_level_start > 2) readout_level_start = 2;
    int readout_level = readout_level_start;
    status_sample_rate_tier = readout_level;
    status_samples_tier = readout_level;
    status_free_space_tier = readout_level;
    bool labels_compact_start = s_status_labels_compact;
    int status_fit_slack = 0;
    bool status_fit_first_pass = false;
    /* Allow enough passes so the full dynamic chain can run (gaps -> compact
     * labels -> narrowed values/text -> last-resort hides) without stalling
     * midway under 1/4 and other constrained layouts. */
    for (int pass = 0; pass < 192; pass++) {
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
        right_required_width += samples_group_width;
        right_group_count++;
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
        right_required_width += rf_group_width + audio_group_width;
        right_group_count += 2;
        if (right_group_count > 1) {
            right_required_width += status_right_gap * (right_group_count - 1);
        }

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
        int outer_gaps = status_two_rows ? 0 : (status_bar_gap * 2);
        int left_available = status_content_width - right_required_width - outer_gaps;
        if (left_available < 0) left_available = 0;
        status_message_width_budget = show_status_message
            ? (left_available - fixed_left_width - left_gaps_reserved)
            : 0;

        int required_without_message = right_required_width + fixed_left_width +
                                       left_gaps_reserved + outer_gaps;
        /* The message only claims what it actually needs: its natural width
         * when that is below the readable minimum, otherwise the minimum
         * (long/error text yields down to the minimum before the readouts
         * do). This keeps every counter at its full readout whenever the
         * real space on the bar allows it. */
        int status_message_required = status_message_natural_width < min_message_width
            ? status_message_natural_width
            : min_message_width;
        bool fits_current_budget = show_status_message
            ? (status_message_width_budget >= status_message_required)
            : (required_without_message <= status_content_width);
        if (fits_current_budget) {
            status_fit_slack = show_status_message
                ? status_message_width_budget - status_message_required
                : status_content_width - required_without_message;
            status_fit_first_pass = (pass == 0);
            break;
        }

        /* Constrain from lowest-visibility impact first: close spacing, switch
         * to compact labels, then squeeze value widths/text budgets. */
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
        if (!status_compact_labels) {
            status_compact_labels = true;
            status_labels_flipped = true;
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
        /* Value containers are FIT(min), so shrinking a minimum below the
         * current text width cannot free any space; skip those no-op steps. */
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
        break;
    }

    /* Persist the readout level with hysteresis: keep any pressure-raised
     * level immediately, but relax at most one step and only when the bar
     * fit on the first pass with ample slack. The extra margin must exceed
     * the width the lower (longer) tier adds, otherwise the next frame
     * seeds the longer text, no longer fits, and the level flips right
     * back — a two-frame flicker band while scaling. With the margin, the
     * dead band is wider than any counter tick or tier step, so the small
     * readouts hold steady at boundary widths. */
    bool readout_level_relaxed = false;
    if (readout_level > readout_level_start) {
        s_status_readout_level = readout_level;
    } else if (readout_level_start > 0 && status_fit_first_pass &&
               status_fit_slack >= 48) {
        int readout_level_lower = readout_level_start - 1;
        int level_step_width = 0;
        int rate_w_prev = status_sample_rate_tier_width[readout_level_lower];
        int rate_w_cur = status_sample_rate_tier_width[readout_level_start];
        int rate_eff_prev = sample_rate_value_width > rate_w_prev ? sample_rate_value_width : rate_w_prev;
        int rate_eff_cur = sample_rate_value_width > rate_w_cur ? sample_rate_value_width : rate_w_cur;
        if (show_sample_rate && rate_eff_prev > rate_eff_cur) {
            level_step_width += rate_eff_prev - rate_eff_cur;
        }
        int samp_w_prev = status_samples_tier_width[readout_level_lower];
        int samp_w_cur = status_samples_tier_width[readout_level_start];
        int samp_eff_prev = samples_value_width > samp_w_prev ? samples_value_width : samp_w_prev;
        int samp_eff_cur = samples_value_width > samp_w_cur ? samples_value_width : samp_w_cur;
        if (samp_eff_prev > samp_eff_cur) {
            level_step_width += samp_eff_prev - samp_eff_cur;
        }
        if (show_free_space) {
            int free_w_prev = status_free_space_tier_width[readout_level_lower];
            int free_w_cur = status_free_space_tier_width[readout_level_start];
            if (free_w_prev > free_w_cur) {
                level_step_width += free_w_prev - free_w_cur;
            }
        }
        if (status_fit_slack - level_step_width >= 48) {
            s_status_readout_level = readout_level_lower;
            readout_level_relaxed = true;
        }
    }
    /* Compact-label flip uses the same hysteresis: keep a pressure flip,
     * and relax at most one state per frame (relaxing both the labels and
     * the level in one frame could overshoot the slack and flap back). The
     * margin is the labels' own measured width step, so the longer forms
     * are guaranteed to fit before the state relaxes. */
    if (status_labels_flipped) {
        s_status_labels_compact = true;
    } else if (labels_compact_start && !readout_level_relaxed &&
               status_fit_first_pass && status_fit_slack >= 24) {
        int labels_step_width = 0;
        if (samples_label_width_full > samples_label_width_compact) {
            labels_step_width += samples_label_width_full - samples_label_width_compact;
        }
        if (show_frame_count && frames_label_width_full > frames_label_width_compact) {
            labels_step_width += frames_label_width_full - frames_label_width_compact;
        }
        if (show_missed_count && missed_label_width_full > missed_label_width_compact) {
            labels_step_width += missed_label_width_full - missed_label_width_compact;
        }
        if (show_error_count && errors_label_width_full > errors_label_width_compact) {
            labels_step_width += errors_label_width_full - errors_label_width_compact;
        }
        if (rf_label_width_full > rf_label_width_compact) {
            labels_step_width += rf_label_width_full - rf_label_width_compact;
        }
        if (audio_label_width_full > audio_label_width_compact) {
            labels_step_width += audio_label_width_full - audio_label_width_compact;
        }
        if (status_fit_slack - labels_step_width >= 24) {
            s_status_labels_compact = false;
        }
    }

    /* Materialize the chosen tier text for rendering. */
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
        snprintf(status_message_display, sizeof(status_message_display), "%s", raw_status);
        if (status_message_width_budget < 24) {
            status_message_width_budget = 24;
        }
        gui_ui_ellipsize_text(app, status_message_display, sizeof(status_message_display),
                              status_font_size, 0, status_message_width_budget);
    }
    if (show_free_space && free_space_width_budget > 0 &&
        free_space_width_budget < free_space_text_width) {
        gui_ui_ellipsize_text(app, status_free_space_display,
                              sizeof(status_free_space_display),
                              status_font_size, 1, free_space_width_budget);
    }
    CLAY(CLAY_ID("StatusBar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(status_two_rows ? 52 : 28) },
            .layoutDirection = status_two_rows ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = status_two_rows ? CLAY_ALIGN_Y_TOP : CLAY_ALIGN_Y_CENTER },
            .padding = { 12, 12, status_two_rows ? 2 : 0, status_two_rows ? 2 : 0 },
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
                CLAY_TEXT(make_string(status_record_timer_display),
                    CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }

            if (show_status_message) {
                CLAY(CLAY_ID("ConnectionStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
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
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) }
                    }
                }) {
                    CLAY_TEXT(make_string(status_free_space_display),
                        CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(free_space_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
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
                    bool synced = atomic_load(&app->stream_synced);
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
                        CLAY_TEXT(synced ? CLAY_STRING("OK") : CLAY_STRING("--"),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(sync_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }

                if (show_sample_rate) {
                    CLAY(CLAY_ID("SampleRate"), {
                        .layout = { .sizing = { CLAY_SIZING_FIT(sample_rate_value_width), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_sample_rate_display),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }
                {
                    /* samples display formatted before the budget loop so its
                     * rendered width is accounted; reused verbatim here. */
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
                            .layout = { .sizing = { CLAY_SIZING_FIT(samples_value_width), CLAY_SIZING_FIT(0) } }
                        }) {
                            CLAY_TEXT(make_string(status_samples_display),
                                CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
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
                                .layout = { .sizing = { CLAY_SIZING_FIT(frames_value_width), CLAY_SIZING_FIT(0) } }
                            }) {
                                CLAY_TEXT(make_string(status_frames_display),
                                    CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                            }
                        }
                    }
                }

                uint32_t missed = app->is_capturing ? atomic_load(&app->missed_frame_count) : 0;
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
                            .layout = { .sizing = { CLAY_SIZING_FIT(small_counter_width), CLAY_SIZING_FIT(0) } }
                        }) {
                            CLAY_TEXT(make_string(status_missed_display),
                                CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(missed > 0 ? COLOR_CLIP_RED : COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
                    }
                }

                uint32_t errors = app->is_capturing ? atomic_load(&app->error_count) : 0;
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
                            .layout = { .sizing = { CLAY_SIZING_FIT(small_counter_width), CLAY_SIZING_FIT(0) } }
                        }) {
                            CLAY_TEXT(make_string(status_errors_display),
                                CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(errors > 0 ? COLOR_CLIP_RED : COLOR_TEXT), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        }
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
                    int device_buffer_bar_width = device_buffer_tiny
                        ? 20
                        : (status_compact ? 36 : 48);
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
                        }
                    }) {
                        CLAY_TEXT(make_string(device_buffer_label),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .textColor = to_clay_color(COLOR_TEXT_DIM), .wrapMode = CLAY_TEXT_WRAP_NONE }));
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
                        CLAY_TEXT(make_string(status_device_buffer_view.caption),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(device_buffer_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
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
                        .layout = { .sizing = { CLAY_SIZING_FIT(buffer_value_width), CLAY_SIZING_FIT(0) } }
                    }) {
                        Color rf_color = (rf_pct > 90) ? COLOR_CLIP_RED : (rf_pct > 75) ? COLOR_METER_YELLOW : COLOR_TEXT;
                        CLAY_TEXT(make_string(status_rf_buf_display),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(rf_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
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
                        .layout = { .sizing = { CLAY_SIZING_FIT(buffer_value_width), CLAY_SIZING_FIT(0) } }
                    }) {
                        Color aud_color = (aud_pct > 90) ? COLOR_CLIP_RED : (aud_pct > 75) ? COLOR_METER_YELLOW : COLOR_TEXT;
                        CLAY_TEXT(make_string(status_aud_buf_display),
                            CLAY_TEXT_CONFIG({ .fontSize = status_font_size, .fontId = 1, .textColor = to_clay_color(aud_color), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }
            }
        }
    }
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

// Main layout function
void gui_render_layout(gui_app_t *app) {
    gui_ui_sync_capture_mode_state(app);
    // Root container
    CLAY(CLAY_ID("Root"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        // Apply cached hsdaoh status/errors at a low rate (2s)
        gui_capture_poll_hsdaoh_status(app);

        // Toolbar
        render_toolbar(app);

        // Main content area - channels panel now includes per-channel stats with trigger controls
        render_channels_panel(app);

        // Status bar
        render_status_bar(app);
    }

    // Settings panel overlay (if open)
    render_settings_panel(app);

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
            int option_width = gui_ui_measure_button_width(app,
                                                           app->devices[i].name,
                                                           overlay_text_size,
                                                           overlay_padding,
                                                           16,
                                                           overlay_min_width,
                                                           overlay_max_width);
            if (option_width > device_dropdown_overlay_width) {
                device_dropdown_overlay_width = option_width;
            }
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
                    // Use device name directly - it's already in persistent storage
                    CLAY_TEXT(make_string(app->devices[i].name),
                        CLAY_TEXT_CONFIG({ .fontSize = overlay_text_size, .textColor = to_clay_color(COLOR_TEXT) }));
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
        if (!app->is_capturing && !gui_app_capture_busy()) {
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
    char ppath[512];
    if (android_poll_picker_result(&pkind, &pok, ppath, sizeof(ppath))) {
        fprintf(stderr, "[GUI] Android picker result: kind=%d ok=%d path='%s'\n",
                pkind, pok, ppath);
        if (pkind == 1) { /* ANDROID_PICKER_KIND_OUTPUT_DIR */
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
            if (granted && android_usb_has_fd()) {
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
    // Reset click consumed flag at start of each frame
    s_ui_consumed_click = false;
#if defined(__ANDROID__)
    gui_ui_poll_android_results(app);
#endif
    gui_ui_sync_capture_mode_state(app);
    gui_record_limit_runtime_tick(app);
    gui_ui_update_check_tick(app);
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

    if (s_record_limit_window_open && !s_record_limit_timecode_edit && IsKeyPressed(KEY_ESCAPE)) {
        s_record_limit_window_open = false;
    }
    if (s_version_info_window_open && IsKeyPressed(KEY_ESCAPE)) {
        s_version_info_window_open = false;
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

    // Handle popup interactions first (modal behavior)
    if (gui_popup_handle_interactions()) {
        s_ui_consumed_click = true;
        return;  // Popup consumed the interaction
    }

    // Handle clicks
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        // Version info popup modal interactions (consume before toolbar underneath)
        if (s_version_info_window_open) {
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
            if (Clay_PointerOver(CLAY_ID("VersionInfoDownloadButton"))) {
                gui_ui_process_update_check_result(app);
                if (atomic_load_explicit(&s_update_check_running, memory_order_acquire)) {
                    gui_app_set_status(app, "Please wait for update check to finish");
                    gui_ui_set_click_consumed();
                    return;
                }

                char latest_tag[64] = {0};
                char lookup_error[160] = {0};
                bool used_cached_tag = false;
                bool have_latest_tag = gui_ui_fetch_latest_release_tag(latest_tag,
                                                                       sizeof(latest_tag),
                                                                       lookup_error,
                                                                       sizeof(lookup_error));
                if (have_latest_tag) {
                    snprintf(app->settings.update_last_release_tag,
                             sizeof(app->settings.update_last_release_tag),
                             "%s",
                             latest_tag);
                    app->settings.update_last_check_unix_s = (uint64_t)time(NULL);
                    int cmp = gui_ui_compare_versions(MIRSC_TOOLS_VERSION, latest_tag);
                    app->settings.update_available_cached = (cmp < 0);
                    gui_settings_save(&app->settings);
                } else if (gui_ui_is_release_tag_safe(app->settings.update_last_release_tag)) {
                    snprintf(latest_tag, sizeof(latest_tag), "%s", app->settings.update_last_release_tag);
                    used_cached_tag = true;
                    have_latest_tag = true;
                }

                if (!have_latest_tag) {
                    char msg[220];
                    snprintf(msg,
                             sizeof(msg),
                             "Unable to resolve latest release: %s",
                             lookup_error[0] ? lookup_error : "unknown error");
                    gui_app_set_status(app, msg);
                    gui_ui_set_click_consumed();
                    return;
                }

                char download_url[512] = {0};
                char asset_name[160] = {0};
                if (!gui_ui_build_release_asset_url_for_platform(latest_tag,
                                                                 download_url,
                                                                 sizeof(download_url),
                                                                 asset_name,
                                                                 sizeof(asset_name))) {
                    gui_app_set_status(app, "No direct download mapping for this platform");
                    gui_ui_set_click_consumed();
                    return;
                }

                char open_error[160] = {0};
                if (!gui_ui_open_url_external(download_url, open_error, sizeof(open_error))) {
                    char msg[240];
                    snprintf(msg,
                             sizeof(msg),
                             "Failed to open download URL: %s",
                             open_error[0] ? open_error : "unknown error");
                    gui_app_set_status(app, msg);
                    gui_ui_set_click_consumed();
                    return;
                }

                if (used_cached_tag) {
                    char msg[220];
                    snprintf(msg, sizeof(msg), "Opening cached download: %s", asset_name);
                    gui_app_set_status(app, msg);
                } else {
                    char msg[220];
                    snprintf(msg, sizeof(msg), "Opening latest download: %s", asset_name);
                    gui_app_set_status(app, msg);
                }
                gui_ui_set_click_consumed();
                return;
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
            // Network (Server/Client) mode cycle: Local -> Server -> Client -> Local.
            if (Clay_PointerOver(CLAY_ID("VersionInfoNetModeToggle"))) {
                int cur = app->settings.net_mode;
                if (cur < GUI_NET_MODE_LOCAL || cur > GUI_NET_MODE_CLIENT) cur = GUI_NET_MODE_LOCAL;
                int next = (cur == GUI_NET_MODE_LOCAL) ? GUI_NET_MODE_SERVER
                         : (cur == GUI_NET_MODE_SERVER) ? GUI_NET_MODE_CLIENT
                         : GUI_NET_MODE_LOCAL;
                app->settings.net_mode = next;
                gui_ui_clear_text_edit();
                gui_settings_save(&app->settings);
                (void)gui_net_apply_mode(app);
                // Returning to Local restores the local hardware device list
                // (client mode had replaced it with the server's mirrored list).
                if (next == GUI_NET_MODE_LOCAL) {
                    gui_app_enumerate_devices(app);
                }
                gui_ui_set_click_consumed();
                return;
            }
            // Click a discovered server to connect to it (client mode).
            if (app->settings.net_mode == GUI_NET_MODE_CLIENT) {
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
                app->settings.net_mode == GUI_NET_MODE_CLIENT) {
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
                app->settings.net_mode == GUI_NET_MODE_CLIENT) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_NET_CLIENT_HOST,
                                       CLAY_ID("VersionInfoNetClientHostField"), 8.0f, 8.0f);
                gui_ui_set_click_consumed();
                return;
            }
            // Click-to-edit the client port field.
            if (Clay_PointerOver(CLAY_ID("VersionInfoNetClientPortField")) &&
                app->settings.net_mode == GUI_NET_MODE_CLIENT) {
                gui_ui_begin_text_edit(app, UI_TEXT_FIELD_NET_CLIENT_PORT,
                                       CLAY_ID("VersionInfoNetClientPortField"), 6.0f, 6.0f);
                gui_ui_set_click_consumed();
                return;
            }
            if (Clay_PointerOver(CLAY_ID("VersionInfoCloseButton"))) {
                s_version_info_window_open = false;
                // Commit any in-progress net field edit on close.
                if (s_active_text_field == UI_TEXT_FIELD_NET_SERVER_PORT ||
                    s_active_text_field == UI_TEXT_FIELD_NET_CLIENT_HOST ||
                    s_active_text_field == UI_TEXT_FIELD_NET_CLIENT_PORT) {
                    gui_ui_clear_text_edit();
                    gui_settings_save(&app->settings);
                    (void)gui_net_apply_mode(app);
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
            }
            gui_ui_set_click_consumed();
            return;
        }
        if (Clay_PointerOver(CLAY_ID("MetadataIconButton"))) {
            s_metadata_window_open = !s_metadata_window_open;
            if (s_metadata_window_open) {
                s_version_info_window_open = false;
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
                gui_app_set_status(app, "No playback file loaded for CH A");
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
                gui_app_set_status(app, "No playback file loaded for CH B");
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
            bool control_capturing = app->is_capturing;
            if (gui_net_is_client(app)) {
                control_capturing = gui_net_client_peer_capturing(app);
            }
            if (control_capturing) {
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
            } else {
#if defined(__ANDROID__)
                /* Async USB permission: never block the render thread on the
                 * system permission dialog (a separate Activity that destroys
                 * our EGL surface mid-block). If the fd is already granted,
                 * start capture now; otherwise launch the async permission
                 * request and let the per-frame poll complete the connect. */
                extern int android_usb_has_fd(void);
                extern int android_request_usb_permission_async(void);
                extern int android_permission_pending(void);
                extern void android_usb_clear_fd(void);
                if (android_usb_has_fd()) {
                    /* fd already granted: start capture on a worker thread so
                     * the render loop stays responsive during hsdaoh_open. */
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
#else
                if (gui_app_start_capture(app) == 0) {
                    app->reconnect_pending = false;
                    app->reconnect_attempts = 0;
                }
#endif
            }
        }
        if (mode_toggle_hit) {
            if (mode_toggle_is_misrc_clockgen) {
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
            } else if (app->is_recording) {
                TraceLog(LOG_INFO,
                         "MODE TRACE: source=CaptureModeToggle blocked current=%s recording=1",
                         gui_ui_capture_mode_name(s_capture_mode_state_misrc));
                gui_app_set_status(app, "Capture mode is locked while recording is active");
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
            gui_app_set_status(app, "Settings are locked while recording is active");
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
            if (gui_ui_selected_device_is_playback(app)) {
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
            atomic_store(&app->clip_count_a_pos, 0);
            atomic_store(&app->clip_count_a_neg, 0);
        }
        if (Clay_PointerOver(CLAY_IDI("ResetClipBtn", 1))) {
            atomic_store(&app->clip_count_b_pos, 0);
            atomic_store(&app->clip_count_b_neg, 0);
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
                        gui_app_set_status(app, "CXADC permission denied: run sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*");
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
                if (target_raw < 0) target_raw = 0;
                if (target_raw > 255) target_raw = 255;
                target_relative = target_raw - s_cxadc_dc_anchor_raw[card_idx];

                if (gui_cxadc_set_center_offset(card_idx, target_raw) == 0) {
                    s_cxadc_dc_relative[card_idx] = target_relative;
                    char msg[128];
                    snprintf(msg, sizeof(msg), "CH %c CXADC%d DC: %+d (raw %d)", (ch == 0) ? 'A' : 'B', card_idx, target_relative, target_raw);
                    gui_app_set_status(app, msg);
                } else {
                    if (errno == EACCES || errno == EPERM) {
                        gui_app_set_status(app, "CXADC permission denied: run sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*");
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
                ? gui_ui_cxadc_base_rate_khz(app, 0)
                : settings_non_cxadc_base_rate_a_khz;
            float settings_base_rate_b_khz = settings_cxadc_mode ? gui_ui_cxadc_base_rate_khz(app, settings_cxadc_has_channel_b ? 1 : 0) : 40000.0f;
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
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacLevelPlus"))) {
                if (app->settings.flac_level < 8) app->settings.flac_level++;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacThreadsMinus"))) {
                if (app->settings.flac_threads > 0) app->settings.flac_threads--;
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
                    app->settings.resample_rate_a = cycle_resample_khz(app->settings.resample_rate_a, settings_base_rate_a_khz);
                    gui_settings_save(&app->settings);
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
                    app->settings.resample_rate_b = cycle_resample_khz(app->settings.resample_rate_b, settings_base_rate_b_khz);
                    gui_settings_save(&app->settings);
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

            if (Clay_PointerOver(CLAY_ID("ChooseOutputFolderButton"))) {
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
        }

        // Note: CVBS enable/disable is now handled automatically when selecting
        // CVBS view via ensure_cvbs_enabled_for_channel() in gui_dropdown.c

        // Handle all dropdown interactions via centralized handler
        if (!s_ui_consumed_click && gui_dropdown_handle_click(app)) {
            s_ui_consumed_click = true;
        }
    }
}
