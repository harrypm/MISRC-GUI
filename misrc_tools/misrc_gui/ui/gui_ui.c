/*
 * MISRC GUI - UI Layout Implementation
 * Clay-based declarative UI layout (Clay v0.14 API)
 */

#include "gui_ui.h"
#include "gui_dropdown.h"
#include "gui_popup.h"
#include "../visualization/gui_fft.h"
#include "../signal/gui_cvbs.h"
#include "../visualization/gui_panel.h"
#include "../input/gui_playback.h"
#include "../output/gui_audio.h"
#include "../output/gui_record.h"
#include "../input/gui_capture.h" // Support hsdoah-rp2350 Error & stats
#include "version.h"
#include "../visualization/gui_custom_elements.h"
#include "../../common/buffer_manager.h"
#include <clay.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#ifndef MIRSC_TOOLS_VERSION
#define MIRSC_TOOLS_VERSION "dev"
#endif
#ifndef MIRSC_TOOLS_COPYRIGHT
#define MIRSC_TOOLS_COPYRIGHT "licensed under GNU GPL v3 or later, (c) 2023-2026 Harry Munday, AlessandroAU, Stefan O, Vrunk11, machcnz"
#endif

// Track if UI consumed the current frame's click (prevents click-through)
static bool s_ui_consumed_click = false;
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

#ifdef ENABLE_DDD
// DdD is single-channel (channel A only); channel B has no signal source.
static bool gui_ui_selected_device_is_ddd(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    return app->devices[app->selected_device].type == DEVICE_TYPE_DDD;
}
#endif

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
    UI_TEXT_FIELD_INGEST_NOTES
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

    Vector2 mouse = GetMousePosition();
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

// Format helpers - use separate buffers to avoid overwriting
static char temp_buf1[64];
static char device_dropdown_buf[64];

// Per-channel stat buffers (separate for A and B to avoid overwrite)
static char stat_a_peak_pos[16];
static char stat_a_peak_neg[16];
static char stat_a_clip_pos[16];
static char stat_a_clip_neg[16];
static char stat_a_errors[16];
static char stat_b_peak_pos[16];
static char stat_b_peak_neg[16];
static char stat_b_clip_pos[16];
static char stat_b_clip_neg[16];
static char stat_b_errors[16];
static char stat_rec_raw[2][32];
static char stat_rec_flac[2][32];
static char stat_rec_ratio[2][24];
static char stat_rec_duration[2][24];

// Playback file display buffers
static char playback_file_a_display[64];
static char playback_file_b_display[64];

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

static void gui_ui_sync_capture_mode_state(gui_app_t *app) {
    if (!app) return;
    if (!s_capture_mode_state_initialized) {
        s_capture_mode_state_misrc = app->settings.misrc_mode;
        s_capture_mode_state_initialized = true;
        gui_ui_trace_capture_mode_state(app, "ui_init_from_settings", true);
    }
#ifdef ENABLE_DDD
    bool ddd_mode = gui_ui_selected_device_is_ddd(app);
#else
    bool ddd_mode = false;
#endif
    bool cxadc_mode = gui_ui_selected_device_is_cxadc(app, NULL);
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
        bool cxadc_has_channel_b = false;
        if (app->selected_device >= 0 && app->selected_device < app->device_count) {
            cxadc_has_channel_b = (app->devices[app->selected_device].index > 1);
        }
        // Single-card CXADC has no RF-B source.
        if (!cxadc_has_channel_b && app->settings.capture_b) {
            app->settings.capture_b = false;
            cxadc_settings_changed = true;
        }
        if (app->settings.rf_bits_a != 8) {
            app->settings.rf_bits_a = 8;
            cxadc_settings_changed = true;
        }
        if (app->settings.rf_bits_b != 8) {
            app->settings.rf_bits_b = 8;
            cxadc_settings_changed = true;
        }
        if (cxadc_settings_changed) {
            gui_settings_save(&app->settings);
        }
    }
#ifdef ENABLE_DDD
    if (ddd_mode) {
        // DdD is single-channel: force channel A on, channel B off. Channel B
        // has no signal source (the 32-bit packed B field is always 0), so
        // recording it would produce a silent empty file.
        bool ddd_settings_changed = false;
        if (!app->settings.capture_a) {
            app->settings.capture_a = true;
            ddd_settings_changed = true;
        }
        if (app->settings.capture_b) {
            app->settings.capture_b = false;
            ddd_settings_changed = true;
        }
        if (ddd_settings_changed) {
            gui_settings_save(&app->settings);
        }
    }
#endif
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



static float cycle_resample_khz(float current_khz) {
    // User-facing presets (stored as kHz), including 40 MSPS passthrough base.
    static const float presets_khz[] = { 5000.0f, 10000.0f, 14300.0f, 17900.0f, 20000.0f, 40000.0f };
    const int n = (int)(sizeof(presets_khz) / sizeof(presets_khz[0]));

    // Find nearest preset (within 1 kHz), otherwise start from first.
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (fabsf(current_khz - presets_khz[i]) < 1.0f) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return presets_khz[0];
    return presets_khz[(idx + 1) % n];
}

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

    Vector2 mouse = GetMousePosition();
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
    bool settings_cxadc_has_channel_b = false;
    bool settings_cxadc_mode = gui_ui_selected_device_is_cxadc(app, &settings_cxadc_has_channel_b);
#ifdef ENABLE_DDD
    bool settings_ddd_mode = gui_ui_selected_device_is_ddd(app);
#else
    bool settings_ddd_mode = false;
#endif
    // DdD is single-channel and single-card CXADC has no RF-B source.
    // In both cases RF-B controls are disabled (grayed out).
    bool settings_b_disabled = settings_ddd_mode || (settings_cxadc_mode && !settings_cxadc_has_channel_b);
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
            .sizing = { CLAY_SIZING_FIT(.min = 760, .max = 1080), CLAY_SIZING_FIT(.min = 520, .max = 780) },
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
                .horizontal = false,
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
                        Color rf_bits_a_bg = settings_cxadc_mode ? ui_disabled_color(COLOR_BUTTON) : COLOR_BUTTON;
                        Color rf_bits_a_fg = settings_cxadc_mode ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
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
                        Color rf_bits_b_bg = (settings_cxadc_mode || settings_b_controls_disabled) ? ui_disabled_color(COLOR_BUTTON) : COLOR_BUTTON;
                        Color rf_bits_b_fg = (settings_cxadc_mode || settings_b_controls_disabled) ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
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

                    // Resample section
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

                    CLAY(CLAY_ID("ToggleRowResampleB"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        Color resample_b_toggle_bg = settings_b_controls_disabled
                            ? ui_disabled_color(COLOR_BUTTON)
                            : (app->settings.enable_resample_b ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
                        Color resample_b_toggle_fg = settings_b_controls_disabled ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT;
                        CLAY(CLAY_ID("ToggleResampleB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(resample_b_toggle_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_resample_b ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(resample_b_toggle_fg) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Resample B"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(resample_b_toggle_fg) }));

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
            .sizing = { CLAY_SIZING_FIT(.min = 420, .max = 420), CLAY_SIZING_FIT(.min = 235, .max = 440) },
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
                .childGap = 12
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
        case DEVICE_TYPE_FX3:            return "FX3";
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

    static char vi_version[64];
    static char vi_state[24];
    static char vi_device[96];
    static char vi_rate[32];

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
            .sizing = { CLAY_SIZING_FIT(.min = 380, .max = 460), CLAY_SIZING_FIT(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 16, 16, 16, 16 },
            .childGap = 10
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER }
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
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 10 }
        }) {
            CLAY(CLAY_ID("VersionInfoVersionLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Version:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(vi_version),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
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

        // Copyright
        CLAY_TEXT(CLAY_STRING(MIRSC_TOOLS_COPYRIGHT),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
    }
}
// Metadata popup (opened by clicking the toolbar scroll badge)
static void render_metadata_window(gui_app_t *app)
{
    if (!s_metadata_window_open) return;

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
            .sizing = { CLAY_SIZING_FIT(.min = 640, .max = 840), CLAY_SIZING_FIT(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 16, 16, 16, 16 },
            .childGap = 10
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER }
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

// Render the toolbar
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
    CLAY(CLAY_ID("Toolbar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(48) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .padding = { 8, 8, 8, 8 },
            .childGap = 12
        },
        .backgroundColor = to_clay_color(COLOR_TOOLBAR_BG)
    }) {
        // Version/status icon (fixed 32x32, no variable-width text)
        CLAY(CLAY_ID("VersionIconButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(32), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(COLOR_BUTTON),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY(CLAY_ID("VersionIcon"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(20), CLAY_SIZING_FIXED(20) } },
                .custom = { .customData = &s_version_icon_element }
            }) {}
        }
        CLAY(CLAY_ID("MetadataIconButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(32), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(COLOR_BUTTON),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY(CLAY_ID("MetadataIcon"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(18), CLAY_SIZING_FIXED(18) } },
                .custom = { .customData = &s_metadata_icon_element }
            }) {}
        }

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer1"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(8), CLAY_SIZING_GROW(0) } }
        }) {}

        // Device label
        CLAY_TEXT(CLAY_STRING("Device:"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        // Device dropdown button
        bool device_dropdown_open = gui_dropdown_is_open(DROPDOWN_DEVICE, 0);
        Color dropdown_color = device_dropdown_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        CLAY(CLAY_ID("DeviceDropdown"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(250), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                .padding = { 10, 10, 0, 0 }
            },
            .backgroundColor = to_clay_color(dropdown_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            const char *device_name = app->device_count > 0 ?
                app->devices[app->selected_device].name : "No devices";
            snprintf(device_dropdown_buf, sizeof(device_dropdown_buf), "%s", device_name);
            CLAY_TEXT(make_string(device_dropdown_buf),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // Connect/Disconnect button (next to device dropdown)
        Color connect_color = app->is_capturing ? COLOR_CLIP_RED : COLOR_SYNC_GREEN;
        CLAY(CLAY_ID("ConnectButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(connect_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(app->is_capturing ? CLAY_STRING("Disconnect") : CLAY_STRING("Connect"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = { 255, 255, 255, 255 } }));
        }
        // Capture mode toggle (MISRC default: swapped A/B; HSDAOH: normal A/B).
        // For non-hsdaoh USB backends (CXADC, FX3, DdD) the MISRC/HSDAOH A/B-swap
        // concept does not apply, so the toggle shows the backend name as the
        // mode label and is disabled — this avoids confusion about which
        // driver/interface is active.
        bool cxadc_clockgen_mode = false;
        bool cxadc_mode = gui_ui_selected_device_is_cxadc(app, &cxadc_clockgen_mode);
#ifdef ENABLE_FX3
        bool fx3_mode = gui_ui_selected_device_is_fx3(app);
#else
        bool fx3_mode = false;
#endif
#ifdef ENABLE_DDD
        bool ddd_mode = gui_ui_selected_device_is_ddd(app);
#else
        bool ddd_mode = false;
#endif
        bool mode_source_runtime = app->is_recording;
        bool mode_misrc = mode_source_runtime ? app->capture_mode_runtime_misrc
                                              : app->user_capture_mode_misrc;
        if (cxadc_mode || ddd_mode) {
            mode_misrc = false;
        }
        gui_ui_trace_capture_mode_render(app, mode_misrc, mode_source_runtime);
        // Toggle is only clickable for hsdaoh/simple_capture backends where
        // the MISRC/HSDAOH A/B-swap is meaningful.
        bool mode_change_allowed = !app->is_recording && !cxadc_mode && !fx3_mode && !ddd_mode;
        Color mode_bg = mode_misrc ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        if (!mode_change_allowed) {
            mode_bg = ui_disabled_color(mode_bg);
        }
        Color mode_fg = mode_change_allowed ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
        CLAY(CLAY_ID("CaptureModeToggle"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(185), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(mode_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            const char *mode_label = NULL;
            if (cxadc_mode) {
                mode_label = cxadc_clockgen_mode ? "Mode: CXADC Clockgen" : "Mode: CXADC";
            } else if (fx3_mode) {
                mode_label = "Mode: FX3";
            } else if (ddd_mode) {
                mode_label = "Mode: DdD";
            } else {
                mode_label = mode_misrc ? "Mode: MISRC" : "Mode: HSDAOH";
            }
            CLAY_TEXT(make_string(mode_label),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(mode_fg) }));
        }

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer2"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
        }) {}

        // Audio playback monitoring toggle
        Color mon_bg = app->settings.audio_monitor_playback ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        CLAY(CLAY_ID("AudioPlaybackToggle"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(32) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = to_clay_color(mon_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(CLAY_STRING("Audio Mon"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }
        
        // Audio channel select (CH1/2 vs CH3/4)
        Color ch_bg = app->settings.audio_monitor_ch34 ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        CLAY(CLAY_ID("AudioChannelToggle"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(32) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = to_clay_color(ch_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(app->settings.audio_monitor_ch34 ? CLAY_STRING("CH3/4") : CLAY_STRING("CH1/2"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }


        // 4 channel horizontal audio meters (compact for toolbar)
        CLAY(CLAY_ID("AudioLevelBars"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(240), CLAY_SIZING_FIXED(32) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 4, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .padding = { 4, 4, 4, 4 } },
            .backgroundColor = to_clay_color((Color){25,25,30,255}),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            // 4 horizontal meters in a row with labels
            for (int i = 0; i < 4; i++) {
                uint32_t p = atomic_load(&app->audio_peak[i]);
                float frac = (p > 0) ? (float)p / 8388607.0f : 0.0f;
                if (frac > 1.0f) frac = 1.0f;

                const int meter_w = 50;
                int fill_w = (int)(frac * (float)meter_w);
                if (fill_w < 0) fill_w = 0;
                if (fill_w > meter_w) fill_w = meter_w;

                // Color thresholds
                Color bar_col = (frac > 0.95f) ? COLOR_CLIP_RED : (frac > 0.75f) ? COLOR_METER_YELLOW : COLOR_SYNC_GREEN;

                // Column: channel label above meter
                CLAY(CLAY_IDI("AudioMeterCol", i), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(54), CLAY_SIZING_FIXED(24) }, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 1, .childAlignment = { .x = CLAY_ALIGN_X_CENTER } }
                }) {
                    // Channel label (CH1-CH4)
                    snprintf(audio_ch_label[i], sizeof(audio_ch_label[i]), "CH%d", i + 1);
                    CLAY(CLAY_IDI("AudioChLabel", i), { .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) } } }) {
                        CLAY_TEXT(make_string(audio_ch_label[i]),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    }

                    // Horizontal meter bar container
                    CLAY(CLAY_IDI("AudioMeter", i), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(meter_w), CLAY_SIZING_FIXED(8) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 0 },
                        .backgroundColor = to_clay_color((Color){40,40,48,255}),
                        .cornerRadius = CLAY_CORNER_RADIUS(2)
                    }) {
                        // Fill bar (left side)
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
        // Record button
        Color record_color = app->is_recording ? COLOR_CLIP_RED : COLOR_BUTTON;
        if (!app->is_capturing) record_color = (Color){ 50, 50, 55, 255 };
        CLAY(CLAY_ID("RecordButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(record_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            Color text_color = app->is_capturing ? COLOR_TEXT : COLOR_TEXT_DIM;
            CLAY_TEXT(app->is_recording ? CLAY_STRING("Stop Rec") : CLAY_STRING("Record"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(text_color) }));
        }
        // Record limit button (clock icon)
        Color limit_button_color = s_record_limit_window_open
            ? COLOR_BUTTON_ACTIVE
            : (s_record_limit_armed ? COLOR_SYNC_GREEN : COLOR_BUTTON);
        CLAY(CLAY_ID("RecordLimitButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(32), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(limit_button_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY(CLAY_ID("RecordLimitIcon"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(18), CLAY_SIZING_FIXED(18) } },
                .custom = { .customData = &s_record_limit_icon_element }
            }) {}
        }

        // Settings button
        CLAY(CLAY_ID("SettingsButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(32), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(app->settings_panel_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            // Font-independent settings icon (rendered as a custom Clay element)
            CLAY(CLAY_ID("SettingsIcon"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(18), CLAY_SIZING_FIXED(18) } },
                .custom = { .customData = &s_settings_icon_element }
            }) {}
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
    // Get per-channel stats
    uint32_t clip_pos, clip_neg, errors;
    float peak_pos, peak_neg;
    char *buf_peak_pos, *buf_peak_neg, *buf_clip_pos, *buf_clip_neg, *buf_errors;
    char *buf_rec_raw, *buf_rec_flac, *buf_rec_ratio, *buf_rec_duration;
    Color channel_value_color = (channel == 0) ? COLOR_CHANNEL_A : COLOR_CHANNEL_B;

    if (channel == 0) {
        clip_pos = atomic_load(&app->clip_count_a_pos);
        clip_neg = atomic_load(&app->clip_count_a_neg);
        errors = atomic_load(&app->error_count_a);
        peak_pos = app->vu_a.peak_pos;
        peak_neg = app->vu_a.peak_neg;
        buf_peak_pos = stat_a_peak_pos;
        buf_peak_neg = stat_a_peak_neg;
        buf_clip_pos = stat_a_clip_pos;
        buf_clip_neg = stat_a_clip_neg;
        buf_errors = stat_a_errors;
        buf_rec_raw = stat_rec_raw[0];
        buf_rec_flac = stat_rec_flac[0];
        buf_rec_ratio = stat_rec_ratio[0];
        buf_rec_duration = stat_rec_duration[0];
    } else {
        clip_pos = atomic_load(&app->clip_count_b_pos);
        clip_neg = atomic_load(&app->clip_count_b_neg);
        errors = atomic_load(&app->error_count_b);
        peak_pos = app->vu_b.peak_pos;
        peak_neg = app->vu_b.peak_neg;
        buf_peak_pos = stat_b_peak_pos;
        buf_peak_neg = stat_b_peak_neg;
        buf_clip_pos = stat_b_clip_pos;
        buf_clip_neg = stat_b_clip_neg;
        buf_errors = stat_b_errors;
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
    snprintf(buf_errors, 16, "%u", errors);

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

        // Errors row
        CLAY(CLAY_IDI("StatErrors", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblErrors", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Errors:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(buf_errors),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(errors > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
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
                CLAY(CLAY_IDI("RecRawRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                    CLAY_TEXT(make_string(buf_rec_raw),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(channel_value_color) }));
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

// Render the channels panel - each channel has VU meter + waveform + stats grouped together
static void render_channels_panel(gui_app_t *app) {
#ifdef ENABLE_DDD
    // DdD is single-channel: hide the Channel B row entirely so channel A
    // expands to fill the preview area. Channel B has no signal source (the
    // 32-bit packed B field is always 0), so showing it would just display a
    // flat zero line and dead stats.
    bool ddd_single_channel = gui_ui_selected_device_is_ddd(app);
#else
    bool ddd_single_channel = false;
#endif

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
        // Channel A row: VU meter + waveform + stats
        CLAY(CLAY_ID("ChannelARow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
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
        // Hidden entirely for DdD (single-channel device) — channel A expands
        // to fill the full preview height instead.
        if (!ddd_single_channel) {
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
    update_status_free_space(app);
    CLAY(CLAY_ID("StatusBar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .padding = { 12, 12, 0, 0 },
            .childGap = 20
        },
        .backgroundColor = to_clay_color(COLOR_TOOLBAR_BG)
    }) {
        // Left side: Recording indicators / Status message
        CLAY(CLAY_ID("StatusLeft"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 12
            }
        }) {
            if (app->is_recording) {
                CLAY(CLAY_ID("RecIndicator"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(12), CLAY_SIZING_FIXED(12) } },
                    .backgroundColor = to_clay_color(COLOR_CLIP_RED),
                    .cornerRadius = CLAY_CORNER_RADIUS(6)
                }) {}

                double duration = GetTime() - app->recording_start_time;
                int hours = (int)(duration / 3600);
                int mins = ((int)(duration / 60)) % 60;
                int secs = ((int)duration) % 60;
                snprintf(temp_buf1, sizeof(temp_buf1), "%02d:%02d:%02d", hours, mins, secs);
                CLAY_TEXT(make_string(temp_buf1),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
            } else {
                snprintf(temp_buf1, sizeof(temp_buf1), "%s", app->status_message);
                CLAY_TEXT(make_string(temp_buf1),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }

            Color free_space_color = COLOR_TEXT_DIM;
            if (s_status_free_space_valid) {
                format_status_free_space_label(status_free_space_display, sizeof(status_free_space_display),
                                               s_status_free_space_cached_bytes);
                if (app->is_recording && s_status_output_rate_bps > 0.0) {
                    char free_only[48];
                    char runway_hms[24];
                    double runway_s = (double)s_status_free_space_cached_bytes / s_status_output_rate_bps;
                    format_status_free_space_label(free_only, sizeof(free_only), s_status_free_space_cached_bytes);
                    format_status_runway_hhmmss(runway_hms, sizeof(runway_hms), runway_s);
                    snprintf(status_free_space_display, sizeof(status_free_space_display),
                             "%s | Runway %s @ %.1f MB/s",
                             free_only, runway_hms, s_status_output_rate_bps / (1024.0 * 1024.0));
                }
                if (s_status_free_space_cached_bytes < STATUS_FREE_SPACE_LOW_BYTES) {
                    free_space_color = COLOR_CLIP_RED;
                } else if (s_status_free_space_cached_bytes < STATUS_FREE_SPACE_WARN_BYTES) {
                    free_space_color = COLOR_METER_YELLOW;
                }
            } else {
                snprintf(status_free_space_display, sizeof(status_free_space_display), "Free: N/A");
            }

            CLAY(CLAY_ID("FreeSpaceStatus"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) }
                }
            }) {
                CLAY_TEXT(make_string(status_free_space_display),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(free_space_color) }));
            }
        }

        // Spacer to push right side to the right
        CLAY(CLAY_ID("StatusSpacer"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
        }) {}

        // Right side: Connection stats
        CLAY(CLAY_ID("StatusRight"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 16
            }
        }) {
            // Sync status indicator
            bool synced = atomic_load(&app->stream_synced);
            Color sync_color = synced ? COLOR_SYNC_GREEN : COLOR_SYNC_RED;
            CLAY(CLAY_ID("SyncStatus"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 4
                }
            }) {
                CLAY_TEXT(CLAY_STRING("Sync:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                CLAY_TEXT(synced ? CLAY_STRING("OK") : CLAY_STRING("--"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(sync_color) }));
            }

            // Sample rate (placed next to Sync status)
            {
                uint32_t srate = atomic_load(&app->sample_rate);
                if (srate > 0) {
                    format_live_msps_label(status_sample_rate_display, sizeof(status_sample_rate_display), srate);
                    CLAY(CLAY_ID("SampleRate"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_sample_rate_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    }
                }
            }

            

            // Unified Samples (same for both channels)
            {
                uint64_t samples_status = atomic_load(&app->samples_a);
                if (samples_status >= 1000000000ULL) {
                    snprintf(status_samples_display, sizeof(status_samples_display), "%.2fG", (double)samples_status / 1000000000.0);
                } else if (samples_status >= 1000000ULL) {
                    snprintf(status_samples_display, sizeof(status_samples_display), "%.2fM", (double)samples_status / 1000000.0);
                } else if (samples_status >= 1000ULL) {
                    snprintf(status_samples_display, sizeof(status_samples_display), "%.1fK", (double)samples_status / 1000.0);
                } else {
                    snprintf(status_samples_display, sizeof(status_samples_display), "%llu", (unsigned long long)samples_status);
                }

                CLAY(CLAY_ID("SamplesStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 4
                    }
                }) {
                    CLAY_TEXT(CLAY_STRING("Samples:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    CLAY(CLAY_ID("SamplesValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(60), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_samples_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                }

                // Frames count placed next to Samples
                uint32_t frames = atomic_load(&app->frame_count);
                snprintf(status_frames_display, sizeof(status_frames_display), "%u", frames);
                CLAY(CLAY_ID("FrameStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 4
                    }
                }) {
                    CLAY_TEXT(CLAY_STRING("Frames:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    CLAY(CLAY_ID("FrameValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(50), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_frames_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                }
            }

            // Missed frames count
            uint32_t missed = app->is_capturing ? atomic_load(&app->missed_frame_count) : 0;
            //if (missed > 0) {
                snprintf(status_missed_display, sizeof(status_missed_display), "%u", missed);
                CLAY(CLAY_ID("MissedStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 4
                    }
                }) {
                    CLAY_TEXT(CLAY_STRING("Missed:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    CLAY(CLAY_ID("MissedValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(20), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_missed_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(missed > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
                    }
                }
            //}

            // Total errors (single combined counter)
            uint32_t errors = app->is_capturing ? atomic_load(&app->error_count) : 0;
            //if (errors > 0) {
                snprintf(status_errors_display, sizeof(status_errors_display), "%u", errors);
                CLAY(CLAY_ID("ErrorStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 4
                    }
                }) {
                    CLAY_TEXT(CLAY_STRING("Errors:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    CLAY(CLAY_ID("ErrorValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(20), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_errors_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(errors > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
                    }
                }
            //}

            // RF Buffer usage
            size_t rf_head = atomic_load(&app->buffers.buffers[BUF_CAPTURE_RF].head);
            size_t rf_tail = atomic_load(&app->buffers.buffers[BUF_CAPTURE_RF].tail);
            size_t rf_size = app->buffers.buffers[BUF_CAPTURE_RF].buffer_size;
            size_t rf_used = rf_tail - rf_head;  // Simple subtraction - no wrap handling
            int rf_pct = rf_size > 0 ? (int)((rf_used * 100) / rf_size) : 0;
            snprintf(status_rf_buf_display, sizeof(status_rf_buf_display), "%d%%", rf_pct);
            CLAY(CLAY_ID("RFBufStatus"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 4
                }
            }) {
                CLAY_TEXT(CLAY_STRING("RF Buffer:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                CLAY(CLAY_ID("RFBufValue"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(35), CLAY_SIZING_FIT(0) } }
                }) {
                    Color rf_color = (rf_pct > 90) ? COLOR_CLIP_RED : (rf_pct > 75) ? COLOR_METER_YELLOW : COLOR_TEXT;
                    CLAY_TEXT(make_string(status_rf_buf_display),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(rf_color) }));
                }
            }

            // Audio Buffer usage
            size_t aud_head = atomic_load(&app->buffers.buffers[BUF_CAPTURE_AUDIO].head);
            size_t aud_tail = atomic_load(&app->buffers.buffers[BUF_CAPTURE_AUDIO].tail);
            size_t aud_size = app->buffers.buffers[BUF_CAPTURE_AUDIO].buffer_size;
            size_t aud_used = aud_tail - aud_head;  // Simple subtraction - no wrap handling  
            int aud_pct = aud_size > 0 ? (int)((aud_used * 100) / aud_size) : 0;
            snprintf(status_aud_buf_display, sizeof(status_aud_buf_display), "%d%%", aud_pct);
            CLAY(CLAY_ID("AudBufStatus"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 4
                }
            }) {
                CLAY_TEXT(CLAY_STRING("Audio Buffer:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                CLAY(CLAY_ID("AudBufValue"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(35), CLAY_SIZING_FIT(0) } }
                }) {
                    Color aud_color = (aud_pct > 90) ? COLOR_CLIP_RED : (aud_pct > 75) ? COLOR_METER_YELLOW : COLOR_TEXT;
                    CLAY_TEXT(make_string(status_aud_buf_display),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(aud_color) }));
                }
            }
        }
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
    render_record_limit_window(app);

    // Version info popup overlay (if open)
    render_version_info_window(app);
    // Metadata popup overlay (if open)
    render_metadata_window(app);

    // Device dropdown overlay (if open)
    if (gui_dropdown_is_open(DROPDOWN_DEVICE, 0) && app->device_count > 0) {
        CLAY(CLAY_ID("DeviceDropdownOverlay"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(250), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .floating = {
                .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                .parentId = CLAY_ID("DeviceDropdown").id,
                .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
            },
            .backgroundColor = to_clay_color(COLOR_PANEL_BG),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            for (int i = 0; i < app->device_count; i++) {
                bool opt_hover = Clay_PointerOver(CLAY_IDI("DeviceOption", i));
                Color item_color = gui_dropdown_option_color(i == app->selected_device, opt_hover);

                CLAY(CLAY_IDI("DeviceOption", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) },
                        .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                        .padding = { 10, 10, 0, 0 }
                    },
                    .backgroundColor = to_clay_color(item_color)
                }) {
                    // Use device name directly - it's already in persistent storage
                    CLAY_TEXT(make_string(app->devices[i].name),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }
        }
    }

    // Popup overlay (renders on top of everything)
    gui_popup_render();
}

// Handle UI interactions
void gui_handle_interactions(gui_app_t *app) {
    // Reset click consumed flag at start of each frame
    s_ui_consumed_click = false;
    gui_ui_sync_capture_mode_state(app);
    gui_record_limit_runtime_tick(app);

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
    if (las_text_field_active && s_record_limit_window_open) {
        gui_ui_handle_active_text_edit(app);
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
            if (Clay_PointerOver(CLAY_ID("VersionInfoCloseButton"))) {
                s_version_info_window_open = false;
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
        Vector2 click_pos = GetMousePosition();
        bool mode_toggle_hit = Clay_PointerOver(CLAY_ID("CaptureModeToggle"));
        bool mode_toggle_cxadc_clockgen = false;
        bool mode_toggle_is_cxadc = gui_ui_selected_device_is_cxadc(app, &mode_toggle_cxadc_clockgen);
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
            if (app->is_capturing) {
                gui_app_stop_capture(app);
                app->reconnect_pending = false;
                app->reconnect_attempts = 0;
            } else {
                if (gui_app_start_capture(app) == 0) {
                    app->reconnect_pending = false;
                    app->reconnect_attempts = 0;
                }
            }
        }
        if (mode_toggle_hit) {
            if (mode_toggle_is_cxadc) {
                gui_app_set_status(app, mode_toggle_cxadc_clockgen
                    ? "CXADC Clockgen mode is fixed by detected card count"
                    : "CXADC mode is fixed by detected card count");
            } else if (mode_toggle_is_fx3) {
                gui_app_set_status(app, "FX3 backend selected; MISRC/HSDAOH mode not applicable");
            } else if (mode_toggle_is_ddd) {
                gui_app_set_status(app, "DdD backend selected; MISRC/HSDAOH mode not applicable");
            } else if (app->is_recording) {
                TraceLog(LOG_INFO,
                         "MODE TRACE: source=CaptureModeToggle blocked current=%s recording=1",
                         gui_ui_capture_mode_name(s_capture_mode_state_misrc));
                gui_app_set_status(app, "Capture mode is locked while recording is active");
            } else {
                gui_ui_set_capture_mode_state(app, !s_capture_mode_state_misrc);
                gui_settings_save(&app->settings);
                gui_app_set_status(app, s_capture_mode_state_misrc
                    ? "Capture mode set to MISRC (A/B swapped)"
                    : "Capture mode set to HSDAOH (A/B normal)");
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
            gui_audio_set_playback_enabled(app, !app->settings.audio_monitor_playback);
        }
        
        // Audio channel select toggle (CH1/2 vs CH3/4)
        if (Clay_PointerOver(CLAY_ID("AudioChannelToggle"))) {
            app->settings.audio_monitor_ch34 = !app->settings.audio_monitor_ch34;
            gui_settings_save(&app->settings);
        }

        // Check record button
        if (Clay_PointerOver(CLAY_ID("RecordButton")) && app->is_capturing) {
            if (app->is_recording) {
                gui_app_stop_recording(app);
            } else {
                gui_app_start_recording(app);
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

        // Settings panel interactions
        if (app->settings_panel_open) {
            bool settings_cxadc_has_channel_b = false;
            bool settings_cxadc_mode = gui_ui_selected_device_is_cxadc(app, &settings_cxadc_has_channel_b);
#ifdef ENABLE_DDD
            bool settings_ddd_mode = gui_ui_selected_device_is_ddd(app);
#else
            bool settings_ddd_mode = false;
#endif
            bool settings_b_disabled = settings_ddd_mode || (settings_cxadc_mode && !settings_cxadc_has_channel_b);
            bool settings_b_controls_disabled = settings_b_disabled || !app->settings.capture_b;
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
                    } else if (settings_cxadc_mode) {
                        gui_app_set_status(app, "Single-card CXADC has no RF channel B source");
                    }
                } else {
                    app->settings.capture_b = !app->settings.capture_b;
                    if (!app->settings.capture_b) {
                        app->settings.enable_resample_b = false;
                        app->settings.resample_rate_b = 40000.0f;
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
            if (Clay_PointerOver(CLAY_ID("ToggleResampleA"))) {
                bool enable = !app->settings.enable_resample_a;
                app->settings.enable_resample_a = enable;
                if (!enable) {
                    app->settings.resample_rate_a = 40000.0f;
                }
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ResampleRateABox"))) {
                app->settings.resample_rate_a = cycle_resample_khz(app->settings.resample_rate_a);
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleResampleB"))) {
                if (settings_b_controls_disabled) {
                    if (settings_ddd_mode) {
                        gui_app_set_status(app, "DdD is single-channel; channel B resample not applicable");
                    } else if (settings_cxadc_mode) {
                        gui_app_set_status(app, "Single-card CXADC has no RF channel B source");
                    } else if (!app->settings.capture_b) {
                        gui_app_set_status(app, "Enable RF channel B to edit CH B resample settings");
                    }
                } else {
                    bool enable = !app->settings.enable_resample_b;
                    app->settings.enable_resample_b = enable;
                    if (!enable) {
                        app->settings.resample_rate_b = 40000.0f;
                    }
                    gui_settings_save(&app->settings);
                }
            }
            if (Clay_PointerOver(CLAY_ID("ResampleRateBBox"))) {
                if (settings_b_controls_disabled) {
                    if (settings_ddd_mode) {
                        gui_app_set_status(app, "DdD is single-channel; channel B resample not applicable");
                    } else if (settings_cxadc_mode) {
                        gui_app_set_status(app, "Single-card CXADC has no RF channel B source");
                    } else if (!app->settings.capture_b) {
                        gui_app_set_status(app, "Enable RF channel B to edit CH B resample settings");
                    }
                } else {
                    app->settings.resample_rate_b = cycle_resample_khz(app->settings.resample_rate_b);
                    gui_settings_save(&app->settings);
                }
            }

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
                    gui_app_set_status(app, "CXADC RF is fixed at 8-bit 40MSPS");
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
                if (settings_cxadc_mode) {
                    gui_app_set_status(app, "CXADC RF is fixed at 8-bit 40MSPS");
                } else if (settings_b_controls_disabled) {
                    if (settings_ddd_mode) {
                        gui_app_set_status(app, "DdD is single-channel; channel B bit depth not applicable");
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
                // Best-effort folder picker (platform-specific).
                if (gui_settings_choose_output_folder(&app->settings)) {
                    gui_settings_save(&app->settings);
                } else {
                    gui_app_set_status(app, "No folder selected (or folder picker unavailable)");
                }
            }

        // Playback file selection buttons
            if (Clay_PointerOver(CLAY_ID("PlaybackFileBrowseA"))) {
                if (gui_settings_choose_playback_file(&app->settings, 0)) {
                    gui_settings_save(&app->settings);
                } else {
                    gui_app_set_status(app, "No file selected (or file picker unavailable)");
                }
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("PlaybackFileBrowseB"))) {
                if (gui_settings_choose_playback_file(&app->settings, 1)) {
                    gui_settings_save(&app->settings);
                } else {
                    gui_app_set_status(app, "No file selected (or file picker unavailable)");
                }
                gui_ui_set_click_consumed();
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
