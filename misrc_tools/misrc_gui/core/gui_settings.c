/*
 * MISRC GUI - Settings Persistence
 * 16/02/25 - Remediate Win settings not saving - %appdata%
 * Handles loading/saving settings to JSON file and provides defaults
 */

#include "../core/gui_app.h"
#include "../ui/gui_ui_scale.h"
#include "../../common/file_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#else
#include <direct.h>
#define COBJMACROS
#define INITGUID
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <shlobj.h>
#include <knownfolders.h>
#include <shobjidl.h> 
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif
// CPU core detection for the FLAC thread default. get_num_cores() is
// defined in the CLI lib (misrc_capture_cli.a, compiled from
// misrc_capture.c which includes numcores.h) and linked into the GUI
// binary, so declare it extern here instead of including the header
// (which would cause a multiple-definition link error).
// Stock threads = min(8, available cores) so an 8+-core system gets 8,
// a smaller system gets its core count, and a detection failure falls
// back to 0 (auto).
#if !defined(__ANDROID__)
extern uint32_t get_num_cores(void);
#endif

// Reject oversized paths instead of silently opening a different filename.
static bool gui_settings_copy(char *dst, size_t cap, const char *src) {
    size_t len = strlen(src);
    if (len >= cap) return false;
    memcpy(dst, src, len + 1);
    return true;
}

static bool gui_settings_format(char *dst, size_t cap, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(dst, cap, fmt, args);
    va_end(args);
    if (len < 0 || (size_t)len >= cap) {
        dst[0] = '\0';
        return false;
    }
    return true;
}

static bool gui_settings_separator(char c) {
#if defined(_WIN32) || defined(_WIN64)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static bool gui_settings_path_is_dir(const char *path) {
    bool is_dir = false;
    return path && path[0] && file_path_info_utf8(path, NULL, &is_dir) && is_dir;
}

static bool gui_settings_make_dir_if_needed(const char *path) {
    if (!path || !path[0]) return false;
    if (gui_settings_path_is_dir(path)) return true;
    if (file_mkdir_utf8(path, 0700) == 0) return true;
    return errno == EEXIST && gui_settings_path_is_dir(path);
}

static bool gui_settings_ensure_parent_dirs(const char *file_path) {
    char path_copy[FILE_PATH_MAX];
    if (!file_path || !file_path[0] ||
        !gui_settings_copy(path_copy, sizeof(path_copy), file_path)) return false;
    size_t start = 1;
#if defined(_WIN32) || defined(_WIN64)
    // A UNC server/share root already exists; do not try mkdir on its server.
    if (gui_settings_separator(path_copy[0]) && gui_settings_separator(path_copy[1])) {
        start = 2;
        while (path_copy[start] && !gui_settings_separator(path_copy[start])) start++;
        if (path_copy[start]) start++;
        while (path_copy[start] && !gui_settings_separator(path_copy[start])) start++;
        if (path_copy[start]) start++;
    }
#endif
    for (size_t i = start; path_copy[i]; i++) {
        char c = path_copy[i];
        if (!gui_settings_separator(c)) continue;
#if defined(_WIN32) || defined(_WIN64)
        if (i == 2 && path_copy[1] == ':') continue;
#endif
        path_copy[i] = '\0';
        bool ok = gui_settings_make_dir_if_needed(path_copy);
        path_copy[i] = c;
        if (!ok) return false;
    }
    return true;
}

// Optional override path for the settings file, set via --config <path> on
// the GUI command line. When non-NULL, gui_settings_load/save use this path
// instead of the platform-default location. This makes automated GUI testing
// easy: launch the GUI with a pre-written config (e.g. Server mode + port)
// without touching the user's real settings.
static const char *s_override_settings_path = NULL;

void gui_settings_set_override_path(const char *path) {
    s_override_settings_path = (path && path[0]) ? path : NULL;
}

bool gui_settings_override_active(void) {
    return s_override_settings_path != NULL;
}

// Settings file location
static const char* get_settings_file_path(void) {
    static char settings_path[FILE_PATH_MAX];
    static bool initialized = false;
    if (s_override_settings_path) return s_override_settings_path;
    if (initialized) return settings_path;
    char *base = NULL;
#if defined(__ANDROID__)
    extern const char *android_get_storage_path(void);
    const char *ext = android_get_storage_path();
    gui_settings_format(settings_path, sizeof(settings_path), "%s/misrc_gui_settings.json",
                        ext && ext[0] ? ext : "/sdcard/Android/data/dev.misrc.gui/files");
#elif defined(_WIN32) || defined(_WIN64)
    base = file_getenv_utf8("APPDATA");
    if (!base || !base[0]) { free(base); base = file_getenv_utf8("LOCALAPPDATA"); }
    if (base && base[0]) {
        gui_settings_format(settings_path, sizeof(settings_path), "%s/MISRC/misrc_gui_settings.json", base);
    } else {
        free(base);
        base = file_getenv_utf8("USERPROFILE");
        if (base && base[0])
            gui_settings_format(settings_path, sizeof(settings_path), "%s/AppData/Roaming/MISRC/misrc_gui_settings.json", base);
        else
            strcpy(settings_path, "./misrc_gui_settings.json");
    }
#else
    const char *suffix = "/.config/misrc_gui_settings.json";
#ifdef __APPLE__
    suffix = "/Library/Preferences/com.misrc.gui.json";
#else
    base = file_getenv_utf8("XDG_CONFIG_HOME");
    if (base && base[0]) suffix = "/misrc_gui_settings.json";
#endif
    if (!base || !base[0]) { free(base); base = file_getenv_utf8("HOME"); }
    if (base && base[0])
        gui_settings_format(settings_path, sizeof(settings_path), "%s%s", base, suffix);
    else
        strcpy(settings_path, "./misrc_gui_settings.json");
#endif
    free(base);
    initialized = true;
    return settings_path;
}

#if defined(_WIN32) || defined(_WIN64)
static bool gui_settings_from_wide(char *dst, size_t cap, const wchar_t *src) {
    int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, NULL, 0, NULL, NULL);
    return need > 0 && (size_t)need <= cap &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, dst, (int)cap, NULL, NULL) == need;
}
#elif !defined(__ANDROID__) && !defined(__APPLE__)
// user-dirs.dirs contains shell-style quoted paths, but no shell is executed.
static void gui_settings_xdg_desktop(char *dst, size_t cap, const char *home) {
    char config[FILE_PATH_MAX];
    char *xdg = file_getenv_utf8("XDG_CONFIG_HOME");
    bool ok = xdg && xdg[0]
        ? gui_settings_format(config, sizeof(config), "%s/user-dirs.dirs", xdg)
        : gui_settings_format(config, sizeof(config), "%s/.config/user-dirs.dirs", home);
    free(xdg);
    FILE *f = ok ? file_open_read_utf8(config) : NULL;
    if (!f) return;
    char line[FILE_PATH_MAX * 2];
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        const char key[] = "XDG_DESKTOP_DIR=\"";
        if (strncmp(p, key, sizeof(key) - 1)) continue;
        p += sizeof(key) - 1;
        char path[FILE_PATH_MAX];
        size_t len = 0;
        if (!strncmp(p, "$HOME", 5) && (p[5] == '/' || p[5] == '"')) {
            len = strlen(home);
            if (len >= sizeof(path)) break;
            memcpy(path, home, len);
            p += 5;
        }
        while (*p && *p != '"' && len + 1 < sizeof(path)) {
            if (*p == '\\' && (p[1] == '\\' || p[1] == '"' || p[1] == '$' || p[1] == '`')) p++;
            path[len++] = *p++;
        }
        path[len] = '\0';
        if (*p == '"' && gui_settings_path_is_dir(path)) gui_settings_copy(dst, cap, path);
        break;
    }
    fclose(f);
}
#endif

const char* gui_settings_get_desktop_path(void) {
    static char desktop_path[FILE_PATH_MAX];
    static bool initialized = false;
    if (initialized) return desktop_path;
#if defined(__ANDROID__)
    extern const char *android_get_storage_path(void);
    const char *ext = android_get_storage_path();
    gui_settings_format(desktop_path, sizeof(desktop_path), "%s",
                        ext && ext[0] ? ext : "/sdcard/Android/data/dev.misrc.gui/files");
#elif defined(_WIN32) || defined(_WIN64)
    PWSTR path = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL, &path))) {
        gui_settings_from_wide(desktop_path, sizeof(desktop_path), path);
        CoTaskMemFree(path);
    }
    if (!gui_settings_path_is_dir(desktop_path)) {
        char *home = file_getenv_utf8("USERPROFILE");
        gui_settings_copy(desktop_path, sizeof(desktop_path), home ? home : ".");
        free(home);
    }
#else
    char *home = file_getenv_utf8("HOME");
    if (home && home[0]) {
        gui_settings_format(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
        if (!gui_settings_path_is_dir(desktop_path)) gui_settings_copy(desktop_path, sizeof(desktop_path), home);
#ifndef __APPLE__
        gui_settings_xdg_desktop(desktop_path, sizeof(desktop_path), home);
#endif
    }
    free(home);
#endif
    if (!desktop_path[0]) strcpy(desktop_path, ".");
    initialized = true;
    return desktop_path;
}

static uint8_t clamp_rf_bits_flac(uint8_t bits) {
    if (bits == 8 || bits == 12 || bits == 16) return bits;
    return 16;
}

static uint8_t rf_bits_for_raw(uint8_t requested) {
    // RAW supports 8/16 only; treat 12 as 16.
    return (requested == 8) ? 8 : 16;
}

// File extension for a converted RAW RF capture (settings-load preview):
// 8-bit -> .s8, 16-bit -> .s16, because the converting writer stores signed
// samples (ld-decode .s8/.s16 convention). Direct native CXADC passthrough
// (unsigned .u8/.u16) needs device knowledge, so it is decided by
// gui_record.c's predicate at record start (gui_record_apply_auto_names
// recomputes the name there); this load-time preview cannot see the device.
static const char *raw_ext_for_bits(uint8_t bits) {
    return (bits == 8) ? "s8" : "s16";
}

static void format_msps_from_khz(char *dst, size_t dst_len, float khz) {
    if (!dst || dst_len == 0) return;
    uint32_t khz_u = (uint32_t)(khz + 0.5f);
    if ((khz_u % 1000U) == 0U) {
        snprintf(dst, dst_len, "%umsps", khz_u / 1000U);
    } else {
        snprintf(dst, dst_len, "%.1fmsps", (double)khz / 1000.0);
    }
}

static void sanitize_tag(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;

    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || (unsigned char)c >= 0x80 || c == '.' || c == '_' || c == '-') {
            dst[j++] = c;
        } else if (c == ' ' || c == '\t') {
            dst[j++] = '-';
        }
    }
    // A short tag buffer must not retain half of the final UTF-8 character.
    if (j > 0 && (unsigned char)dst[j - 1] >= 0x80) {
        size_t start = j - 1;
        while (start > 0 && ((unsigned char)dst[start] & 0xc0) == 0x80) start--;
        unsigned char lead = (unsigned char)dst[start];
        size_t count = lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (j - start < count) j = start;
    }
    dst[j] = '\0';

    while (j > 0 && dst[j - 1] == '-') {
        dst[--j] = '\0';
    }
}

// Keep derived filenames in sync with current auto-naming settings.
// This refresh does NOT append capture-start timestamp (that is applied when recording starts).
static void gui_settings_refresh_auto_names(gui_settings_t *settings) {
    if (!settings) return;
    if (!settings->auto_names_enabled) return;

    const char *base = settings->output_base_name[0] ? settings->output_base_name : "capture";

    if (settings->use_flac) {
        uint8_t bits_a = clamp_rf_bits_flac(settings->rf_bits_a);
        uint8_t bits_b = clamp_rf_bits_flac(settings->rf_bits_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (settings->enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), settings->resample_rate_a);
        if (settings->enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), settings->resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), settings->rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), settings->rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            gui_settings_format(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_a, (unsigned)bits_a, rate_tag_a);
        } else if (rf_tag_a[0]) {
            gui_settings_format(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_a, (unsigned)bits_a);
        } else if (rate_tag_a[0]) {
            gui_settings_format(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.flac", base, (unsigned)bits_a, rate_tag_a);
        } else {
            gui_settings_format(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.flac", base, (unsigned)bits_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            gui_settings_format(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_b, (unsigned)bits_b, rate_tag_b);
        } else if (rf_tag_b[0]) {
            gui_settings_format(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_b, (unsigned)bits_b);
        } else if (rate_tag_b[0]) {
            gui_settings_format(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.flac", base, (unsigned)bits_b, rate_tag_b);
        } else {
            gui_settings_format(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.flac", base, (unsigned)bits_b);
        }
    } else {
        // RAW: 8/16 only. Extension is per-channel (.u8 / .u16) so the
        // filename advertises the on-disk sample width.
        uint8_t bits_a = rf_bits_for_raw(settings->rf_bits_a);
        uint8_t bits_b = rf_bits_for_raw(settings->rf_bits_b);
        const char *ext_a = raw_ext_for_bits(bits_a);
        const char *ext_b = raw_ext_for_bits(bits_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (settings->enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), settings->resample_rate_a);
        if (settings->enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), settings->resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), settings->rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), settings->rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            gui_settings_format(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.%s", base, rf_tag_a, (unsigned)bits_a, rate_tag_a, ext_a);
        } else if (rf_tag_a[0]) {
            gui_settings_format(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.%s", base, rf_tag_a, (unsigned)bits_a, ext_a);
        } else if (rate_tag_a[0]) {
            gui_settings_format(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.%s", base, (unsigned)bits_a, rate_tag_a, ext_a);
        } else {
            gui_settings_format(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.%s", base, (unsigned)bits_a, ext_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            gui_settings_format(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.%s", base, rf_tag_b, (unsigned)bits_b, rate_tag_b, ext_b);
        } else if (rf_tag_b[0]) {
            gui_settings_format(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.%s", base, rf_tag_b, (unsigned)bits_b, ext_b);
        } else if (rate_tag_b[0]) {
            gui_settings_format(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.%s", base, (unsigned)bits_b, rate_tag_b, ext_b);
        } else {
            gui_settings_format(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.%s", base, (unsigned)bits_b, ext_b);
        }
    }

    char audio_tag_4ch[40] = {0};
    char audio_tag_12[40] = {0};
    char audio_tag_34[40] = {0};
    sanitize_tag(audio_tag_4ch, sizeof(audio_tag_4ch), settings->audio_output_tags[0]);
    sanitize_tag(audio_tag_12, sizeof(audio_tag_12), settings->audio_output_tags[1]);
    sanitize_tag(audio_tag_34, sizeof(audio_tag_34), settings->audio_output_tags[2]);

    if (audio_tag_4ch[0]) {
        gui_settings_format(settings->audio_4ch_filename, MAX_FILENAME_LEN, "%s_%s_quad_4ch.wav", base, audio_tag_4ch);
    } else {
        gui_settings_format(settings->audio_4ch_filename, MAX_FILENAME_LEN, "%s_quad_4ch.wav", base);
    }
    if (audio_tag_12[0]) {
        gui_settings_format(settings->audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch1_ch2.wav", base, audio_tag_12);
    } else {
        gui_settings_format(settings->audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_stereo_ch1_ch2.wav", base);
    }
    if (audio_tag_34[0]) {
        gui_settings_format(settings->audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch3_ch4.wav", base, audio_tag_34);
    } else {
        gui_settings_format(settings->audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_stereo_ch3_ch4.wav", base);
    }

    for (int i = 0; i < 4; i++) {
        char tag[40];
        sanitize_tag(tag, sizeof(tag), settings->audio_1ch_labels[i]);
        if (tag[0]) {
            gui_settings_format(settings->audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_%s_audio_ch%d.wav", base, tag, i + 1);
        } else {
            gui_settings_format(settings->audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_audio_ch%d.wav", base, i + 1);
        }
    }
}
void gui_settings_init_defaults(gui_settings_t *settings) {
    if (!settings) return;
    
    // Zero out the structure
    memset(settings, 0, sizeof(gui_settings_t));
    
    // Basic settings
    settings->device_index = 0;
    settings->capture_a = true;
    settings->capture_b = true;
    
    // Set default save path to Desktop
    gui_settings_copy(settings->output_path, sizeof(settings->output_path), gui_settings_get_desktop_path());
    
    // Auto naming defaults
    settings->auto_names_enabled = true;
    strcpy(settings->output_base_name, "capture");

    // Timestamp behavior
    settings->append_timestamp_on_capture_start = false;

    // Duration limits: removed from UI, force to 0 (unlimited)
    settings->capture_limit_seconds = 0;
    settings->record_limit_seconds = 0;

    // Default filenames (used when auto naming is disabled)
    strcpy(settings->output_filename_a, "rfA_capture.flac");
    strcpy(settings->output_filename_b, "rfB_capture.flac");
    strcpy(settings->aux_filename, "aux_data.bin");
    strcpy(settings->raw_filename, "raw_data.bin");
    strcpy(settings->audio_4ch_filename, "quad_4ch.wav");
    strcpy(settings->audio_2ch_12_filename, "stereo_ch1_ch2.wav");
    strcpy(settings->audio_2ch_34_filename, "stereo_ch3_ch4.wav");

    // RF bit depth defaults (per-channel)
    // Shared preview budget default, scaled to the machine so the RF preview
    // can sustain realtime decode: measured steady-state DSP cost is ~1.1
    // logical cores per active worker at 20 MSPS (gui_rf_preview_test --bench)
    // and the channels' workers share one FIFO-serialized budget. 12+ cores
    // -> 300%, 6-11 -> 200%, else 100%. The Budget button overrides per session.
    settings->rf_preview_budget_percent = 100;
#if !defined(__ANDROID__)
    {
        uint32_t cores = get_num_cores();
        if (cores >= 12) settings->rf_preview_budget_percent = 300;
        else if (cores >= 6) settings->rf_preview_budget_percent = 200;
    }
#endif
    settings->rf_bits_a = 16;
    settings->rf_bits_b = 16;
    settings->cxadc_tenbit_mode_card[0] = false;
    settings->cxadc_tenbit_mode_card[1] = false;
    
    // Individual channel filenames
    for (int i = 0; i < 4; i++) {
        snprintf(settings->audio_1ch_filenames[i], MAX_FILENAME_LEN, "audio_ch%d.wav", i + 1);
    }

    // Per-channel audio labels (optional, used for auto naming)
    for (int i = 0; i < 4; i++) {
        settings->audio_1ch_labels[i][0] = '\0';
    }
    // Optional tags for non-mono audio outputs
    for (int i = 0; i < 3; i++) {
        settings->audio_output_tags[i][0] = '\0';
    }
    settings->ingest_project[0] = '\0';
    settings->ingest_tape_id[0] = '\0';
    settings->ingest_tape_format[0] = '\0';
    settings->ingest_tape_size[0] = '\0';
    settings->ingest_tape_speed[0] = '\0';
    settings->ingest_tape_condition[0] = '\0';
    settings->ingest_operator[0] = '\0';
    settings->ingest_location[0] = '\0';
    settings->ingest_notes[0] = '\0';
    // Optional per-channel RF tags
    for (int i = 0; i < 2; i++) {
        settings->rf_channel_tags[i][0] = '\0';
    }
    
    // Capture control defaults
    settings->sample_count = 0; // Infinite
    strcpy(settings->capture_time, ""); // Empty = infinite
    settings->overwrite_files = false;
    
    // Processing options
    settings->pad_lower_bits = false;
    settings->show_peak_levels = true;
    settings->suppress_clip_a = false;
    settings->suppress_clip_b = false;
    settings->reduce_8bit_a = false;
    settings->reduce_8bit_b = false;
    
    // Resampling defaults
    settings->enable_resample_a = false;
    settings->enable_resample_b = false;
    settings->resample_rate_a = 4000.0f;  // 4 MHz
    settings->resample_rate_b = 4000.0f;  // 4 MHz
    settings->resample_quality_a = 3;     // High quality
    settings->resample_quality_b = 3;     // High quality
    settings->resample_gain_a = 0.0f;     // No gain
    settings->resample_gain_b = 0.0f;     // No gain
#ifdef ENABLE_DDD
    settings->ddd_decimation = DDD_DECIMATION_FULL_RATE;
#endif
    
    // FLAC defaults
    settings->use_flac = true;
    settings->flac_12bit = false;
    settings->flac_level = 8;             // Max compression (best ratio for archival RF)
    settings->flac_verification = false;  // Faster
    // Stock FLAC encoder threads: min(8, available CPU cores). An 8+-core
    // system gets 8 (the recommended discrete count for archival RF); a
    // smaller system gets its actual core count; a detection failure
    // falls back to 0 (auto) so the encoder picks something sane.
    // Android doesn't link the CLI lib (where get_num_cores lives), so it
    // uses 0 (auto) there.
#if !defined(__ANDROID__)
    {
        uint32_t cores = get_num_cores();
        if (cores >= 8) {
            settings->flac_threads = 8;
        } else if (cores > 0) {
            settings->flac_threads = cores;
        } else {
            settings->flac_threads = 0;  // auto fallback
        }
    }
#else
    settings->flac_threads = 0;  // auto (Android: no CLI lib / get_num_cores)
#endif
    settings->flac_affinity_enabled = false;
    settings->flac_affinity_cpu_list[0] = '\0';
    
    // Audio output defaults
    settings->enable_audio_4ch = false;
    settings->enable_audio_2ch_12 = false;
    settings->enable_audio_2ch_34 = false;
    for (int i = 0; i < 4; i++) {
        settings->enable_audio_1ch[i] = false;
    }

    // Audio monitoring defaults
    settings->audio_monitor_playback = false;
    settings->audio_monitor_ch34 = false;  // Default to CH1/2
    settings->misrc_mode = true;           // Default to MISRC mode (A/B swapped)
    settings->misrc_v15_v25_ab_swap = false;
    settings->stop_on_dropout = false;

    // Level autostop defaults (tape-end detection). Disabled by default.
    // Stock config: trigger level 0.2 (normalized 0.1-0.8), sustain 30 seconds.
    // Vpp defaults to 2.0 (hsdaoh/CXADC/DdD); FX3 overrides to 1.0 on device
    // change. level_autostop_vpp_hsdaoh remembers the hsdaoh 1/2 Vpp jumper.
    settings->level_autostop_enabled = false;
    strcpy(settings->level_autostop_level_str, "0.2");
    strcpy(settings->level_autostop_duration_str, "30");
    settings->level_autostop_vpp = 2.0f;
    settings->level_autostop_vpp_hsdaoh = 2.0f;
    
    // Display settings
    settings->show_grid = true;
    settings->time_scale = 1.0f;
    settings->amplitude_scale = 1.0f;
    settings->ui_scale_percent = GUI_UI_SCALE_DEFAULT_PERCENT;
    settings->waveform_scale_mode = 0;  // Basic (0.X, majors only +-0.5/0) by default

    // V4L2/simple_capture device discovery is opt-in (disabled by default).
    settings->discover_simple_capture = false;
    // Core-pinning controls are hidden by default; users can enable from info page.
    settings->show_core_pinning_in_settings = false;
    // Max total buffer RAM budget (1-16 GB). Default 4 GB mirrors the older
    // code's ~4 GB target. Clamped on load; applied at buffer-manager init.
    settings->memory_budget_gb = 4;
    settings->update_last_check_unix_s = 0;
    settings->update_last_release_tag[0] = '\0';
    settings->update_available_cached = false;

    // RTL-SDR defaults (only relevant when an RTL-SDR device is selected)
    settings->rtlsdr_freq_hz = 100000000ULL;   // 100.0 MHz (FM broadcast band)
    settings->rtlsdr_gain_mode = 0;             // 0 = auto, 1 = manual
    settings->rtlsdr_gain_tenths_db = 0;        // manual gain (tenths dB); unused in auto mode
    settings->rtlsdr_sample_rate_hz = 2400000U;  // 2.4 MSPS (highest widely-stable)
    settings->rtlsdr_agc = true;                // RTL2832 AGC on
    settings->rtlsdr_offset_corr = false;       // RTL offset tuning correction off

    // Demod view defaults (device-agnostic; apply to the Demod panel)
    settings->demod_mode = 0;                   // 0=WFM,1=NFM,2=AM,3=USB,4=LSB
    settings->demod_bandwidth_hz = 0;           // 0 = use mode default bandwidth
    settings->demod_squelch = 0.0f;             // 0.0 = squelch open
    settings->demod_volume = 1.0f;              // 1.0 = unity gain
    settings->demod_output_pair = 0;            // 0 = CH1/2, 1 = CH3/4

    // Server/Client networking defaults (cxadc_vhs_server-style). Stock mode
    // is Local (no networking). Default port 8080 mirrors the reference server.
    settings->net_mode = 0;                     // 0=Local, 1=Server, 2=Client
    settings->net_server_port = 8080;
    snprintf(settings->net_server_port_str, sizeof(settings->net_server_port_str), "%u",
             (unsigned)settings->net_server_port);
    settings->net_client_host[0] = '\0';
    settings->net_client_port = 8080;
    snprintf(settings->net_client_port_str, sizeof(settings->net_client_port_str), "%u",
             (unsigned)settings->net_client_port);
}

static void gui_settings_write_string(FILE *f, const char *key, const char *value, bool comma) {
    fprintf(f, "  \"%s\": \"", key);
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fprintf(f, "\"%s\n", comma ? "," : "");
}

// Simple JSON-like format for settings
// 16.02.25 - Remediate Win save
void gui_settings_save(const gui_settings_t *settings) {
    if (!settings) return;
    
    const char* path = get_settings_file_path();
    if (!gui_settings_ensure_parent_dirs(path)) {
        return;
    }
    FILE *f = file_open_utf8(path, "w");
    if (!f) {
        return;
    }
    
    fprintf(f, "{\n  \"settings_format_version\": 2,\n");
    fprintf(f, "  \"device_index\": %d,\n", settings->device_index);
    gui_settings_write_string(f, "output_path", settings->output_path, true);
    fprintf(f, "  \"auto_names_enabled\": %s,\n", settings->auto_names_enabled ? "true" : "false");
    gui_settings_write_string(f, "output_base_name", settings->output_base_name, true);
    fprintf(f, "  \"append_timestamp_on_capture_start\": %s,\n", settings->append_timestamp_on_capture_start ? "true" : "false");
    fprintf(f, "  \"rf_preview_budget_percent\": %d,\n", settings->rf_preview_budget_percent);
    fprintf(f, "  \"rf_bits_a\": %u,\n", (unsigned)settings->rf_bits_a);
    fprintf(f, "  \"rf_bits_b\": %u,\n", (unsigned)settings->rf_bits_b);
    fprintf(f, "  \"cxadc_tenbit_mode_a\": %s,\n", settings->cxadc_tenbit_mode_card[0] ? "true" : "false");
    fprintf(f, "  \"cxadc_tenbit_mode_b\": %s,\n", settings->cxadc_tenbit_mode_card[1] ? "true" : "false");
    gui_settings_write_string(f, "rf_tag_a", settings->rf_channel_tags[0], true);
    gui_settings_write_string(f, "rf_tag_b", settings->rf_channel_tags[1], true);
    gui_settings_write_string(f, "output_filename_a", settings->output_filename_a, true);
    gui_settings_write_string(f, "output_filename_b", settings->output_filename_b, true);
    fprintf(f, "  \"capture_a\": %s,\n", settings->capture_a ? "true" : "false");
    fprintf(f, "  \"capture_b\": %s,\n", settings->capture_b ? "true" : "false");
    fprintf(f, "  \"sample_count\": %llu,\n", (unsigned long long)settings->sample_count);
    gui_settings_write_string(f, "capture_time", settings->capture_time, true);
    fprintf(f, "  \"overwrite_files\": %s,\n", settings->overwrite_files ? "true" : "false");
    gui_settings_write_string(f, "aux_filename", settings->aux_filename, true);
    gui_settings_write_string(f, "raw_filename", settings->raw_filename, true);

    // Audio filenames + enables (mirror CLI options)
    gui_settings_write_string(f, "audio_4ch_filename", settings->audio_4ch_filename, true);
    gui_settings_write_string(f, "audio_2ch_12_filename", settings->audio_2ch_12_filename, true);
    gui_settings_write_string(f, "audio_2ch_34_filename", settings->audio_2ch_34_filename, true);
    gui_settings_write_string(f, "audio_1ch_1_filename", settings->audio_1ch_filenames[0], true);
    gui_settings_write_string(f, "audio_1ch_2_filename", settings->audio_1ch_filenames[1], true);
    gui_settings_write_string(f, "audio_1ch_3_filename", settings->audio_1ch_filenames[2], true);
    gui_settings_write_string(f, "audio_1ch_4_filename", settings->audio_1ch_filenames[3], true);
    gui_settings_write_string(f, "audio_1ch_1_label", settings->audio_1ch_labels[0], true);
    gui_settings_write_string(f, "audio_1ch_2_label", settings->audio_1ch_labels[1], true);
    gui_settings_write_string(f, "audio_1ch_3_label", settings->audio_1ch_labels[2], true);
    gui_settings_write_string(f, "audio_1ch_4_label", settings->audio_1ch_labels[3], true);
    gui_settings_write_string(f, "audio_tag_4ch", settings->audio_output_tags[0], true);
    gui_settings_write_string(f, "audio_tag_2ch_12", settings->audio_output_tags[1], true);
    gui_settings_write_string(f, "audio_tag_2ch_34", settings->audio_output_tags[2], true);

    fprintf(f, "  \"enable_audio_4ch\": %s,\n", settings->enable_audio_4ch ? "true" : "false");
    fprintf(f, "  \"enable_audio_2ch_12\": %s,\n", settings->enable_audio_2ch_12 ? "true" : "false");
    fprintf(f, "  \"enable_audio_2ch_34\": %s,\n", settings->enable_audio_2ch_34 ? "true" : "false");
    fprintf(f, "  \"audio_monitor_playback\": %s,\n", settings->audio_monitor_playback ? "true" : "false");
    fprintf(f, "  \"audio_monitor_ch34\": %s,\n", settings->audio_monitor_ch34 ? "true" : "false");
    fprintf(f, "  \"misrc_mode\": %s,\n", settings->misrc_mode ? "true" : "false");
    fprintf(f, "  \"misrc_v15_v25_ab_swap\": %s,\n", settings->misrc_v15_v25_ab_swap ? "true" : "false");
    fprintf(f, "  \"stop_on_dropout\": %s,\n", settings->stop_on_dropout ? "true" : "false");
    fprintf(f, "  \"level_autostop_enabled\": %s,\n", settings->level_autostop_enabled ? "true" : "false");
    gui_settings_write_string(f, "level_autostop_level_str", settings->level_autostop_level_str, true);
    gui_settings_write_string(f, "level_autostop_duration_str", settings->level_autostop_duration_str, true);
    fprintf(f, "  \"level_autostop_vpp\": %.3f,\n", settings->level_autostop_vpp);
    fprintf(f, "  \"level_autostop_vpp_hsdaoh\": %.3f,\n", settings->level_autostop_vpp_hsdaoh);
    gui_settings_write_string(f, "ingest_project", settings->ingest_project, true);
    gui_settings_write_string(f, "ingest_tape_id", settings->ingest_tape_id, true);
    gui_settings_write_string(f, "ingest_tape_format", settings->ingest_tape_format, true);
    gui_settings_write_string(f, "ingest_tape_size", settings->ingest_tape_size, true);
    gui_settings_write_string(f, "ingest_tape_speed", settings->ingest_tape_speed, true);
    gui_settings_write_string(f, "ingest_tape_condition", settings->ingest_tape_condition, true);
    gui_settings_write_string(f, "ingest_operator", settings->ingest_operator, true);
    gui_settings_write_string(f, "ingest_location", settings->ingest_location, true);
    gui_settings_write_string(f, "ingest_notes", settings->ingest_notes, true);
    fprintf(f, "  \"enable_audio_1ch_1\": %s,\n", settings->enable_audio_1ch[0] ? "true" : "false");
    fprintf(f, "  \"enable_audio_1ch_2\": %s,\n", settings->enable_audio_1ch[1] ? "true" : "false");
    fprintf(f, "  \"enable_audio_1ch_3\": %s,\n", settings->enable_audio_1ch[2] ? "true" : "false");
    fprintf(f, "  \"enable_audio_1ch_4\": %s,\n", settings->enable_audio_1ch[3] ? "true" : "false");

    fprintf(f, "  \"pad_lower_bits\": %s,\n", settings->pad_lower_bits ? "true" : "false");
    fprintf(f, "  \"show_peak_levels\": %s,\n", settings->show_peak_levels ? "true" : "false");
    fprintf(f, "  \"suppress_clip_a\": %s,\n", settings->suppress_clip_a ? "true" : "false");
    fprintf(f, "  \"suppress_clip_b\": %s,\n", settings->suppress_clip_b ? "true" : "false");
    fprintf(f, "  \"reduce_8bit_a\": %s,\n", settings->reduce_8bit_a ? "true" : "false");
    fprintf(f, "  \"reduce_8bit_b\": %s,\n", settings->reduce_8bit_b ? "true" : "false");
    fprintf(f, "  \"enable_resample_a\": %s,\n", settings->enable_resample_a ? "true" : "false");
    fprintf(f, "  \"enable_resample_b\": %s,\n", settings->enable_resample_b ? "true" : "false");
    fprintf(f, "  \"resample_rate_a\": %.1f,\n", settings->resample_rate_a);
    fprintf(f, "  \"resample_rate_b\": %.1f,\n", settings->resample_rate_b);
    fprintf(f, "  \"resample_quality_a\": %d,\n", settings->resample_quality_a);
    fprintf(f, "  \"resample_quality_b\": %d,\n", settings->resample_quality_b);
    fprintf(f, "  \"resample_gain_a\": %.1f,\n", settings->resample_gain_a);
    fprintf(f, "  \"resample_gain_b\": %.1f,\n", settings->resample_gain_b);
#ifdef ENABLE_DDD
    fprintf(f, "  \"ddd_decimation\": %u,\n", (unsigned)settings->ddd_decimation);
#endif
    fprintf(f, "  \"use_flac\": %s,\n", settings->use_flac ? "true" : "false");
    fprintf(f, "  \"flac_12bit\": %s,\n", settings->flac_12bit ? "true" : "false");
    fprintf(f, "  \"flac_level\": %d,\n", settings->flac_level);
    fprintf(f, "  \"flac_verification\": %s,\n", settings->flac_verification ? "true" : "false");
    fprintf(f, "  \"flac_threads\": %d,\n", settings->flac_threads);
    fprintf(f, "  \"flac_affinity_enabled\": %s,\n", settings->flac_affinity_enabled ? "true" : "false");
    gui_settings_write_string(f, "flac_affinity_cpu_list", settings->flac_affinity_cpu_list, true);
    fprintf(f, "  \"waveform_scale_mode\": %d,\n", settings->waveform_scale_mode);
    fprintf(f, "  \"enable_resample_a\": %s,\n", settings->enable_resample_a ? "true" : "false");
    fprintf(f, "  \"enable_resample_b\": %s,\n", settings->enable_resample_b ? "true" : "false");
    fprintf(f, "  \"resample_rate_a\": %.1f,\n", settings->resample_rate_a);
    fprintf(f, "  \"resample_rate_b\": %.1f,\n", settings->resample_rate_b);
    fprintf(f, "  \"resample_quality_a\": %d,\n", settings->resample_quality_a);
    fprintf(f, "  \"resample_quality_b\": %d,\n", settings->resample_quality_b);
    fprintf(f, "  \"resample_gain_a\": %.1f,\n", settings->resample_gain_a);
    fprintf(f, "  \"resample_gain_b\": %.1f,\n", settings->resample_gain_b);
    fprintf(f, "  \"overwrite_files\": %s,\n", settings->overwrite_files ? "true" : "false");
    fprintf(f, "  \"show_grid\": %s,\n", settings->show_grid ? "true" : "false");
    fprintf(f, "  \"time_scale\": %.2f,\n", settings->time_scale);
    fprintf(f, "  \"amplitude_scale\": %.2f,\n", settings->amplitude_scale);
    fprintf(f, "  \"ui_scale_percent\": %d,\n", settings->ui_scale_percent);
    fprintf(f, "  \"discover_simple_capture\": %s,\n", settings->discover_simple_capture ? "true" : "false");
    fprintf(f, "  \"show_core_pinning_in_settings\": %s,\n", settings->show_core_pinning_in_settings ? "true" : "false");
    fprintf(f, "  \"memory_budget_gb\": %u,\n", (unsigned)settings->memory_budget_gb);
    fprintf(f, "  \"update_last_check_unix_s\": %llu,\n", (unsigned long long)settings->update_last_check_unix_s);
    gui_settings_write_string(f, "update_last_release_tag", settings->update_last_release_tag, true);
    fprintf(f, "  \"update_available_cached\": %s,\n", settings->update_available_cached ? "true" : "false");
    fprintf(f, "  \"rtlsdr_freq_hz\": %llu,\n", (unsigned long long)settings->rtlsdr_freq_hz);
    fprintf(f, "  \"rtlsdr_gain_mode\": %d,\n", settings->rtlsdr_gain_mode);
    fprintf(f, "  \"rtlsdr_gain_tenths_db\": %d,\n", settings->rtlsdr_gain_tenths_db);
    fprintf(f, "  \"rtlsdr_sample_rate_hz\": %u,\n", (unsigned)settings->rtlsdr_sample_rate_hz);
    fprintf(f, "  \"rtlsdr_agc\": %s,\n", settings->rtlsdr_agc ? "true" : "false");
    fprintf(f, "  \"rtlsdr_offset_corr\": %s,\n", settings->rtlsdr_offset_corr ? "true" : "false");
    fprintf(f, "  \"demod_mode\": %d,\n", settings->demod_mode);
    fprintf(f, "  \"demod_bandwidth_hz\": %d,\n", settings->demod_bandwidth_hz);
    fprintf(f, "  \"demod_squelch\": %.3f,\n", settings->demod_squelch);
    fprintf(f, "  \"demod_volume\": %.3f,\n", settings->demod_volume);
    fprintf(f, "  \"demod_output_pair\": %d,\n", settings->demod_output_pair);
    // Server/Client networking (cxadc_vhs_server-style peer mode).
    fprintf(f, "  \"net_mode\": %d,\n", settings->net_mode);
    fprintf(f, "  \"net_server_port\": %u,\n", (unsigned)settings->net_server_port);
    gui_settings_write_string(f, "net_server_port_str", settings->net_server_port_str, true);
    gui_settings_write_string(f, "net_client_host", settings->net_client_host, true);
    fprintf(f, "  \"net_client_port\": %u,\n", (unsigned)settings->net_client_port);
    gui_settings_write_string(f, "net_client_port_str", settings->net_client_port_str, true);
    gui_settings_write_string(f, "playback_file_a", settings->playback_file_a, true);
    gui_settings_write_string(f, "playback_file_b", settings->playback_file_b, false);
    fprintf(f, "}\n");
    
    fclose(f);
}

static int gui_settings_hex4(const char *p) {
    int value = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)p[i];
        int n = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (n < 0) return -1;
        value = (value << 4) | n;
    }
    return value;
}

static const char *gui_settings_value_start(const char *p, const char *key, bool escaped) {
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '{' || *p == ',') p++;
        if (*p++ != '"') return NULL;
        const char *name = p;
        while (*p && *p != '"') p++;
        if (!*p) return NULL;
        bool match = strlen(key) == (size_t)(p - name) && !memcmp(name, key, (size_t)(p - name));
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p++ != ':') return NULL;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (match) return p;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (escaped && *p == '\\' && p[1]) p++;
                p++;
            }
            if (!*p) return NULL;
            p++;
        }
        while (*p && *p != ',' && *p != '}') p++;
        if (*p == '}') return NULL;
    }
    return NULL;
}

static bool s_settings_value_too_long = false;

// Version 1 wrote Windows backslashes literally. Only decode escapes after
// an explicit version marker; otherwise C:\tapes\new would change meaning.
static char* find_value(const char* content, const char* key) {
    static char value[FILE_PATH_MAX];
    s_settings_value_too_long = false;
    const char *version = gui_settings_value_start(content, "settings_format_version", true);
    bool escaped = version && strtol(version, NULL, 10) >= 2;
    const char *pos = gui_settings_value_start(content, key, escaped);
    if (!pos) return NULL;
    size_t len = 0;
    if (*pos == '"') {
        pos++;
        while (*pos && *pos != '"') {
            unsigned char c = (unsigned char)*pos++;
            if (escaped && c == '\\') {
                c = (unsigned char)*pos++;
                switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (strlen(pos) < 4) return NULL;
                    int hi = gui_settings_hex4(pos);
                    if (hi < 0) return NULL;
                    pos += 4;
                    unsigned cp = (unsigned)hi;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (strlen(pos) < 6 || pos[0] != '\\' || pos[1] != 'u') return NULL;
                        int lo = gui_settings_hex4(pos + 2);
                        if (lo < 0xdc00 || lo > 0xdfff) return NULL;
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (unsigned)(lo - 0xdc00);
                        pos += 6;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) return NULL;
                    if (!cp) return NULL; // A native path cannot contain NUL.
                    unsigned count = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
                    if (len + count >= sizeof(value)) goto too_long;
                    if (count == 1) value[len++] = (char)cp;
                    else {
                        value[len++] = (char)((count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0) | (cp >> (6 * (count - 1))));
                        for (unsigned shift = count - 1; shift > 0; shift--)
                            value[len++] = (char)(0x80 | ((cp >> (6 * (shift - 1))) & 0x3f));
                    }
                    continue;
                }
                default: return NULL;
                }
            }
            if (len + 1 >= sizeof(value)) goto too_long;
            value[len++] = (char)c;
        }
        if (*pos != '"') return NULL;
    } else {
        while (*pos && *pos != ',' && *pos != '\n' && *pos != '}') {
            if (len + 1 >= sizeof(value)) return NULL;
            value[len++] = *pos++;
        }
        while (len && (value[len - 1] == ' ' || value[len - 1] == '\t' || value[len - 1] == '\r')) len--;
    }
    value[len] = '\0';
#if defined(_WIN32) || defined(_WIN64)
    // Older native folder pickers saved ACP bytes. Keep existing UTF-8 intact,
    // and migrate only legacy strings that cannot be decoded as UTF-8.
    if (!escaped && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0)) {
        wchar_t wide[FILE_PATH_MAX];
        if (!MultiByteToWideChar(CP_ACP, 0, value, -1, wide, FILE_PATH_MAX) ||
            !gui_settings_from_wide(value, sizeof(value), wide)) goto too_long;
    }
#endif
    return value;
too_long:
    s_settings_value_too_long = true;
    value[0] = '\0';
    return value;
}

// A specified but oversized value must not silently select the default file.
// Pickers use gui_settings_copy directly, retaining the previous selection.
static bool gui_settings_load_string(char *dst, size_t cap, const char *src) {
    if (!s_settings_value_too_long && gui_settings_copy(dst, cap, src)) return true;
    dst[0] = '\0';
    return false;
}

#if !defined(_WIN32) && !defined(_WIN64) && !defined(__ANDROID__)
// Pickers append one newline. Read their complete output so embedded/trailing
// newlines in a POSIX filename survive; reject overlong output, never a prefix.
static bool gui_settings_read_picker(FILE *fp, char *dst, size_t cap) {
    size_t len = 0;
    bool overflow = false;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (!c) overflow = true;
        if (len < cap) dst[len++] = (char)c;
        else overflow = true;
    }
    if (ferror(fp) || overflow) return false;
    if (len && dst[len - 1] == '\n') len--;
    if (!len || len >= cap) return false;
    dst[len] = '\0';
    return true;
}
#endif

// Backward-compat migration:
// Earlier builds could permanently prefix output_base_name with a timestamp like:
// - "yy.mm.dd_hh.mm.ss_<name>"
// - "yyyy.mm.dd_hh.mm.ss_<name>"
// That is no longer desired (timestamp is now appended at capture start only for derived filenames).
static void strip_timestamp_prefix_inplace(char *s) {
    if (!s) return;

    // yy.mm.dd_hh.mm.ss_
    const size_t len_yy = 18;
    // yyyy.mm.dd_hh.mm.ss_
    const size_t len_yyyy = 20;

    size_t n = strlen(s);
    if (n <= len_yy) return;

    bool match_yy = (n > len_yy) &&
        (s[2] == '.' && s[5] == '.' && s[8] == '_' && s[11] == '.' && s[14] == '.' && s[17] == '_');

    bool match_yyyy = (n > len_yyyy) &&
        (s[4] == '.' && s[7] == '.' && s[10] == '_' && s[13] == '.' && s[16] == '.' && s[19] == '_');

    if (match_yyyy) {
        memmove(s, s + len_yyyy, n - len_yyyy + 1);
    } else if (match_yy) {
        memmove(s, s + len_yy, n - len_yy + 1);
    }
}

static bool gui_settings_pick_path(char *picked, size_t cap, bool folder) {
#if defined(__ANDROID__)
    (void)picked; (void)cap; (void)folder;
    return false; // Android uses the async SAF picker.
#elif defined(_WIN32) || defined(_WIN64)
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *pfd = NULL;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IFileOpenDialog, (void **)&pfd))) {
        if (folder) {
            DWORD options = 0;
            IFileOpenDialog_GetOptions(pfd, &options);
            IFileOpenDialog_SetOptions(pfd, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        } else {
            COMDLG_FILTERSPEC filter = { L"FLAC files", L"*.flac" };
            IFileOpenDialog_SetFileTypes(pfd, 1, &filter);
        }
        IFileOpenDialog_SetTitle(pfd, folder ? L"Select output folder for MISRC captures" : L"Select FLAC playback file");
        if (SUCCEEDED(IFileOpenDialog_Show(pfd, NULL))) {
            IShellItem *psi = NULL;
            if (SUCCEEDED(IFileOpenDialog_GetResult(pfd, &psi))) {
                PWSTR wpath = NULL;
                if (SUCCEEDED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &wpath))) {
                    ok = gui_settings_from_wide(picked, cap, wpath);
                    CoTaskMemFree(wpath);
                }
                IShellItem_Release(psi);
            }
        }
        IFileOpenDialog_Release(pfd);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok;
#else
#ifdef __APPLE__
    const char *cmd = folder
        ? "osascript -e 'POSIX path of (choose folder with prompt \"Select output folder for MISRC captures\")'"
        : "osascript -e 'POSIX path of (choose file with prompt \"Select FLAC playback file\")'";
#else
    const char *cmd = folder
        ? "sh -c 'if command -v zenity >/dev/null 2>&1; then "
          "zenity --file-selection --directory --title=\"Select output folder for MISRC captures\"; "
          "elif command -v kdialog >/dev/null 2>&1; then "
          "kdialog --getexistingdirectory \"$HOME\" \"Select output folder for MISRC captures\"; fi'"
        : "sh -c 'if command -v zenity >/dev/null 2>&1; then "
          "zenity --file-selection --title=\"Select FLAC playback file\" --file-filter=\"*.flac\"; "
          "elif command -v kdialog >/dev/null 2>&1; then "
          "kdialog --getopenfilename \"$HOME\" \"*.flac|FLAC files (*.flac)\"; fi'";
#endif
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    bool ok = gui_settings_read_picker(fp, picked, cap);
    return pclose(fp) == 0 && ok;
#endif
}

bool gui_settings_choose_output_folder(gui_settings_t *settings) {
    if (!settings) return false;
    char picked[FILE_PATH_MAX];
    if (!gui_settings_pick_path(picked, sizeof(picked), true)) return false;
    // Keep native root and literal POSIX backslash characters unchanged.
    if (!strcmp(settings->output_path, picked)) return false;
    return gui_settings_copy(settings->output_path, sizeof(settings->output_path), picked);
}

bool gui_settings_choose_playback_file(gui_settings_t *settings, int channel) {
    if (!settings || (channel != 0 && channel != 1)) return false;
    char picked[FILE_PATH_MAX];
    if (!gui_settings_pick_path(picked, sizeof(picked), false)) return false;
    char *dst = channel == 0 ? settings->playback_file_a : settings->playback_file_b;
    if (!strcmp(dst, picked)) return false;
    return gui_settings_copy(dst, sizeof(settings->playback_file_a), picked);
}

// Normalize level_autostop_level_str on load: migrate legacy integer percent
// (1-99, e.g. "33") to the normalized 0.X fraction (0.1-0.8), and clamp any
// value into the valid 0.1-0.8 range. Reformat canonically with up to 2
// decimals and no trailing zeros (0.4 stays "0.4", 0.45 stays "0.45",
// legacy "33" -> "0.33").
static void gui_settings_normalize_level_autostop_level(char *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    char *end = NULL;
    double v = strtod(buf, &end);
    bool has_dot = (strchr(buf, '.') != NULL);
    // Legacy integer percent (no decimal point, 1-99) -> fraction.
    if (!has_dot && end != buf && *end == '\0') {
        long pct = strtol(buf, NULL, 10);
        if (pct >= 1 && pct <= 99) {
            v = (double)pct / 100.0;
        }
    }
    // Clamp to valid 0.1-0.8 range.
    if (v < 0.1) v = 0.1;
    if (v > 0.8) v = 0.8;
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%.2f", v);
    // Strip trailing zeros and a trailing dot.
    size_t len = strlen(tmp);
    while (len > 0 && tmp[len - 1] == '0') { tmp[--len] = '\0'; }
    if (len > 0 && tmp[len - 1] == '.') { tmp[--len] = '\0'; }
    if (tmp[0] == '\0') snprintf(tmp, sizeof(tmp), "0.1");
    snprintf(buf, cap, "%s", tmp);
}

void gui_settings_load(gui_settings_t *settings) {
    if (!settings) return;
    
    // Start with defaults
    gui_settings_init_defaults(settings);
    
    const char* path = get_settings_file_path();
    FILE* f = file_open_utf8(path, "rb");
    if (!f) return; // No settings file, use defaults
    
    // Read entire file
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 262144) { // Sanity check
        fclose(f);
        return;
    }
    
    char* content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return;
    }
    
    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);

    // Editors may prefix either legacy or current settings with a UTF-8 BOM.
    if (read_size >= 3 && !memcmp(content, "\xef\xbb\xbf", 3)) {
        read_size -= 3;
        memmove(content, content + 3, read_size + 1);
    }
    
    // Parse values
    char* value;
    
    if ((value = find_value(content, "device_index")) != NULL) {
        settings->device_index = atoi(value);
    }
    
    if ((value = find_value(content, "output_path")) != NULL) {
        gui_settings_load_string(settings->output_path, sizeof(settings->output_path), value);
    }
    
    // Auto naming
    if ((value = find_value(content, "auto_names_enabled")) != NULL) {
        settings->auto_names_enabled = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "output_base_name")) != NULL) {
        if (!gui_settings_load_string(settings->output_base_name, sizeof(settings->output_base_name), value))
            settings->output_path[0] = '\0';
        strip_timestamp_prefix_inplace(settings->output_base_name);
    }
    if ((value = find_value(content, "append_timestamp_on_capture_start")) != NULL) {
        settings->append_timestamp_on_capture_start = (strcmp(value, "true") == 0);
    }
    // Duration limits: feature removed, ignore saved values and force to 0
    settings->capture_limit_seconds = 0;
    settings->record_limit_seconds = 0;
    if ((value = find_value(content, "rf_preview_budget_percent")) != NULL) {
        int budget = atoi(value);
        switch (budget) {
            case 50: case 75: case 100: case 150: case 200: case 300:
                settings->rf_preview_budget_percent = budget;
                break;
            default:
                break; // invalid saved value: keep the machine-scaled default
        }
    }
    if ((value = find_value(content, "rf_bits_a")) != NULL) {
        settings->rf_bits_a = (uint8_t)atoi(value);
    }
    if ((value = find_value(content, "rf_bits_b")) != NULL) {
        settings->rf_bits_b = (uint8_t)atoi(value);
    }
    bool cxadc_mode_legacy_loaded = false;
    if ((value = find_value(content, "cxadc_tenbit_mode")) != NULL) {
        bool mode = (strcmp(value, "true") == 0);
        settings->cxadc_tenbit_mode_card[0] = mode;
        settings->cxadc_tenbit_mode_card[1] = mode;
        cxadc_mode_legacy_loaded = true;
    }
    if ((value = find_value(content, "cxadc_tenbit_mode_a")) != NULL) {
        settings->cxadc_tenbit_mode_card[0] = (strcmp(value, "true") == 0);
        cxadc_mode_legacy_loaded = true;
    }
    if ((value = find_value(content, "cxadc_tenbit_mode_b")) != NULL) {
        settings->cxadc_tenbit_mode_card[1] = (strcmp(value, "true") == 0);
        cxadc_mode_legacy_loaded = true;
    }
    if (!cxadc_mode_legacy_loaded) {
        settings->cxadc_tenbit_mode_card[0] = false;
        settings->cxadc_tenbit_mode_card[1] = false;
    }
    if ((value = find_value(content, "rf_tag_a")) != NULL) {
        if (!gui_settings_load_string(settings->rf_channel_tags[0], sizeof(settings->rf_channel_tags[0]), value))
            settings->output_path[0] = '\0';
    }
    if ((value = find_value(content, "rf_tag_b")) != NULL) {
        if (!gui_settings_load_string(settings->rf_channel_tags[1], sizeof(settings->rf_channel_tags[1]), value))
            settings->output_path[0] = '\0';
    }

    if ((value = find_value(content, "output_filename_a")) != NULL) {
        gui_settings_load_string(settings->output_filename_a, sizeof(settings->output_filename_a), value);
    }

    if ((value = find_value(content, "output_filename_b")) != NULL) {
        gui_settings_load_string(settings->output_filename_b, sizeof(settings->output_filename_b), value);
    }
    
    if ((value = find_value(content, "capture_a")) != NULL) {
        settings->capture_a = (strcmp(value, "true") == 0);
    }
    
    if ((value = find_value(content, "capture_b")) != NULL) {
        settings->capture_b = (strcmp(value, "true") == 0);
    }
    
    if ((value = find_value(content, "use_flac")) != NULL) {
        settings->use_flac = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "flac_12bit")) != NULL) {
        settings->flac_12bit = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "flac_verification")) != NULL) {
        settings->flac_verification = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "flac_threads")) != NULL) {
        settings->flac_threads = atoi(value);
    }
    if ((value = find_value(content, "flac_affinity_enabled")) != NULL) {
        settings->flac_affinity_enabled = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "flac_affinity_cpu_list")) != NULL) {
        gui_settings_load_string(settings->flac_affinity_cpu_list, sizeof(settings->flac_affinity_cpu_list), value);
    }

    if ((value = find_value(content, "flac_level")) != NULL) {
        settings->flac_level = atoi(value);
    }

    if ((value = find_value(content, "enable_resample_a")) != NULL) {
        settings->enable_resample_a = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "enable_resample_b")) != NULL) {
        settings->enable_resample_b = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "resample_rate_a")) != NULL) {
        settings->resample_rate_a = (float)atof(value);
    }

    if ((value = find_value(content, "resample_rate_b")) != NULL) {
        settings->resample_rate_b = (float)atof(value);
    }

    if ((value = find_value(content, "resample_quality_a")) != NULL) {
        settings->resample_quality_a = atoi(value);
    }

    if ((value = find_value(content, "resample_quality_b")) != NULL) {
        settings->resample_quality_b = atoi(value);
    }

    if ((value = find_value(content, "resample_gain_a")) != NULL) {
        settings->resample_gain_a = (float)atof(value);
    }

#ifdef ENABLE_DDD
    if ((value = find_value(content, "ddd_decimation")) != NULL) {
        uint8_t factor = (uint8_t)atoi(value);
        if (ddd_decimation_is_supported(factor)) {
            settings->ddd_decimation = factor;
        }
    }
#endif

    if ((value = find_value(content, "resample_gain_b")) != NULL) {
        settings->resample_gain_b = (float)atof(value);
    }

    if ((value = find_value(content, "overwrite_files")) != NULL) {
        settings->overwrite_files = (strcmp(value, "true") == 0);
    }
    
    if ((value = find_value(content, "show_grid")) != NULL) {
        settings->show_grid = (strcmp(value, "true") == 0);
    }
    
    if ((value = find_value(content, "time_scale")) != NULL) {
        settings->time_scale = (float)atof(value);
    }
    
    if ((value = find_value(content, "amplitude_scale")) != NULL) {
        settings->amplitude_scale = (float)atof(value);
    }
    if ((value = find_value(content, "ui_scale_percent")) != NULL) {
        settings->ui_scale_percent = gui_ui_scale_parse_percent(value);
    }
    if ((value = find_value(content, "waveform_scale_mode")) != NULL) {
        int mode = atoi(value);
        if (mode < 0 || mode > 4) mode = 0;
        settings->waveform_scale_mode = mode;
    } else if ((value = find_value(content, "waveform_scale_mv")) != NULL) {
        // Backward compat: migrate the old bool (false->0 Basic, true->4 2Vpp).
        settings->waveform_scale_mode = (strcmp(value, "true") == 0) ? 4 : 0;
    }
    if ((value = find_value(content, "discover_simple_capture")) != NULL) {
        settings->discover_simple_capture = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "show_core_pinning_in_settings")) != NULL) {
        settings->show_core_pinning_in_settings = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "memory_budget_gb")) != NULL) {
        long gb = atol(value);
        if (gb < 1) gb = 4;
        if (gb > 16) gb = 16;
        settings->memory_budget_gb = (uint32_t)gb;
    }
    if ((value = find_value(content, "update_last_check_unix_s")) != NULL) {
        settings->update_last_check_unix_s = (uint64_t)strtoull(value, NULL, 10);
    }
    if ((value = find_value(content, "update_last_release_tag")) != NULL) {
        gui_settings_load_string(settings->update_last_release_tag, sizeof(settings->update_last_release_tag), value);
    }
    if ((value = find_value(content, "update_available_cached")) != NULL) {
        settings->update_available_cached = (strcmp(value, "true") == 0);
    }

    if ((value = find_value(content, "reduce_8bit_a")) != NULL) {
        settings->reduce_8bit_a = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "reduce_8bit_b")) != NULL) {
        settings->reduce_8bit_b = (strcmp(value, "true") == 0);
    }

    // Audio filenames + enables
    if ((value = find_value(content, "audio_4ch_filename")) != NULL) {
        gui_settings_load_string(settings->audio_4ch_filename, sizeof(settings->audio_4ch_filename), value);
    }
    if ((value = find_value(content, "audio_2ch_12_filename")) != NULL) {
        gui_settings_load_string(settings->audio_2ch_12_filename, sizeof(settings->audio_2ch_12_filename), value);
    }
    if ((value = find_value(content, "audio_2ch_34_filename")) != NULL) {
        gui_settings_load_string(settings->audio_2ch_34_filename, sizeof(settings->audio_2ch_34_filename), value);
    }
    if ((value = find_value(content, "audio_1ch_1_filename")) != NULL) {
        gui_settings_load_string(settings->audio_1ch_filenames[0], sizeof(settings->audio_1ch_filenames[0]), value);
    }
    if ((value = find_value(content, "audio_1ch_2_filename")) != NULL) {
        gui_settings_load_string(settings->audio_1ch_filenames[1], sizeof(settings->audio_1ch_filenames[1]), value);
    }
    if ((value = find_value(content, "audio_1ch_3_filename")) != NULL) {
        gui_settings_load_string(settings->audio_1ch_filenames[2], sizeof(settings->audio_1ch_filenames[2]), value);
    }
    if ((value = find_value(content, "audio_1ch_4_filename")) != NULL) {
        gui_settings_load_string(settings->audio_1ch_filenames[3], sizeof(settings->audio_1ch_filenames[3]), value);
    }

    // Per-channel audio labels (optional)
    if ((value = find_value(content, "audio_1ch_1_label")) != NULL) {
        if (!gui_settings_load_string(settings->audio_1ch_labels[0], sizeof(settings->audio_1ch_labels[0]), value))
            settings->output_path[0] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_2_label")) != NULL) {
        if (!gui_settings_load_string(settings->audio_1ch_labels[1], sizeof(settings->audio_1ch_labels[1]), value))
            settings->output_path[0] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_3_label")) != NULL) {
        if (!gui_settings_load_string(settings->audio_1ch_labels[2], sizeof(settings->audio_1ch_labels[2]), value))
            settings->output_path[0] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_4_label")) != NULL) {
        if (!gui_settings_load_string(settings->audio_1ch_labels[3], sizeof(settings->audio_1ch_labels[3]), value))
            settings->output_path[0] = '\0';
    }
    if ((value = find_value(content, "audio_tag_4ch")) != NULL) {
        if (!gui_settings_load_string(settings->audio_output_tags[0], sizeof(settings->audio_output_tags[0]), value))
            settings->output_path[0] = '\0';
    }
    if ((value = find_value(content, "audio_tag_2ch_12")) != NULL) {
        if (!gui_settings_load_string(settings->audio_output_tags[1], sizeof(settings->audio_output_tags[1]), value))
            settings->output_path[0] = '\0';
    }
    if ((value = find_value(content, "audio_tag_2ch_34")) != NULL) {
        if (!gui_settings_load_string(settings->audio_output_tags[2], sizeof(settings->audio_output_tags[2]), value))
            settings->output_path[0] = '\0';
    }

    if ((value = find_value(content, "enable_audio_4ch")) != NULL) {
        settings->enable_audio_4ch = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_2ch_12")) != NULL) {
        settings->enable_audio_2ch_12 = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_2ch_34")) != NULL) {
        settings->enable_audio_2ch_34 = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "audio_monitor_playback")) != NULL) {
        settings->audio_monitor_playback = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "audio_monitor_ch34")) != NULL) {
        settings->audio_monitor_ch34 = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "misrc_mode")) != NULL) {
        settings->misrc_mode = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "misrc_v15_v25_ab_swap")) != NULL) {
        settings->misrc_v15_v25_ab_swap = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "stop_on_dropout")) != NULL) {
        settings->stop_on_dropout = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "level_autostop_enabled")) != NULL) {
        settings->level_autostop_enabled = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "level_autostop_level_str")) != NULL) {
        gui_settings_load_string(settings->level_autostop_level_str, sizeof(settings->level_autostop_level_str), value);
        // Migrate legacy integer percent (1-99) -> normalized 0.X (0.1-0.8)
        // and clamp into range. Idempotent for already-0.X saves.
        gui_settings_normalize_level_autostop_level(settings->level_autostop_level_str,
                                                    sizeof(settings->level_autostop_level_str));
    }
    if ((value = find_value(content, "level_autostop_duration_str")) != NULL) {
        gui_settings_load_string(settings->level_autostop_duration_str, sizeof(settings->level_autostop_duration_str), value);
    }
    if ((value = find_value(content, "level_autostop_vpp")) != NULL) {
        settings->level_autostop_vpp = (float)atof(value);
        if (!(settings->level_autostop_vpp > 0.1f)) settings->level_autostop_vpp = 2.0f;
    }
    if ((value = find_value(content, "level_autostop_vpp_hsdaoh")) != NULL) {
        settings->level_autostop_vpp_hsdaoh = (float)atof(value);
        if (!(settings->level_autostop_vpp_hsdaoh > 0.1f)) settings->level_autostop_vpp_hsdaoh = 2.0f;
    }
    if ((value = find_value(content, "ingest_project")) != NULL) {
        gui_settings_load_string(settings->ingest_project, sizeof(settings->ingest_project), value);
    }
    if ((value = find_value(content, "ingest_tape_id")) != NULL) {
        gui_settings_load_string(settings->ingest_tape_id, sizeof(settings->ingest_tape_id), value);
    }
    if ((value = find_value(content, "ingest_tape_format")) != NULL) {
        gui_settings_load_string(settings->ingest_tape_format, sizeof(settings->ingest_tape_format), value);
    }
    if ((value = find_value(content, "ingest_tape_size")) != NULL) {
        gui_settings_load_string(settings->ingest_tape_size, sizeof(settings->ingest_tape_size), value);
    }
    if ((value = find_value(content, "ingest_tape_speed")) != NULL) {
        gui_settings_load_string(settings->ingest_tape_speed, sizeof(settings->ingest_tape_speed), value);
    }
    if ((value = find_value(content, "ingest_tape_condition")) != NULL) {
        gui_settings_load_string(settings->ingest_tape_condition, sizeof(settings->ingest_tape_condition), value);
    }
    if ((value = find_value(content, "ingest_operator")) != NULL) {
        gui_settings_load_string(settings->ingest_operator, sizeof(settings->ingest_operator), value);
    }
    if ((value = find_value(content, "ingest_location")) != NULL) {
        gui_settings_load_string(settings->ingest_location, sizeof(settings->ingest_location), value);
    }
    if ((value = find_value(content, "ingest_notes")) != NULL) {
        gui_settings_load_string(settings->ingest_notes, sizeof(settings->ingest_notes), value);
    }
    if ((value = find_value(content, "enable_audio_1ch_1")) != NULL) {
        settings->enable_audio_1ch[0] = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_1ch_2")) != NULL) {
        settings->enable_audio_1ch[1] = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_1ch_3")) != NULL) {
        settings->enable_audio_1ch[2] = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "enable_audio_1ch_4")) != NULL) {
        settings->enable_audio_1ch[3] = (strcmp(value, "true") == 0);
    }

    // Playback files
    if ((value = find_value(content, "playback_file_a")) != NULL) {
        gui_settings_load_string(settings->playback_file_a, sizeof(settings->playback_file_a), value);
    }
    if ((value = find_value(content, "playback_file_b")) != NULL) {
        gui_settings_load_string(settings->playback_file_b, sizeof(settings->playback_file_b), value);
    }

    // RTL-SDR settings
    if ((value = find_value(content, "rtlsdr_freq_hz")) != NULL) {
        settings->rtlsdr_freq_hz = (uint64_t)strtoull(value, NULL, 10);
    }
    if ((value = find_value(content, "rtlsdr_gain_mode")) != NULL) {
        settings->rtlsdr_gain_mode = atoi(value);
    }
    if ((value = find_value(content, "rtlsdr_gain_tenths_db")) != NULL) {
        settings->rtlsdr_gain_tenths_db = atoi(value);
    }
    if ((value = find_value(content, "rtlsdr_sample_rate_hz")) != NULL) {
        settings->rtlsdr_sample_rate_hz = (uint32_t)strtoul(value, NULL, 10);
    }
    if ((value = find_value(content, "rtlsdr_agc")) != NULL) {
        settings->rtlsdr_agc = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "rtlsdr_offset_corr")) != NULL) {
        settings->rtlsdr_offset_corr = (strcmp(value, "true") == 0);
    }
    // Demod view settings
    if ((value = find_value(content, "demod_mode")) != NULL) {
        settings->demod_mode = atoi(value);
    }
    if ((value = find_value(content, "demod_bandwidth_hz")) != NULL) {
        settings->demod_bandwidth_hz = atoi(value);
    }
    if ((value = find_value(content, "demod_squelch")) != NULL) {
        settings->demod_squelch = (float)atof(value);
    }
    if ((value = find_value(content, "demod_volume")) != NULL) {
        settings->demod_volume = (float)atof(value);
    }
    if ((value = find_value(content, "demod_output_pair")) != NULL) {
        settings->demod_output_pair = atoi(value);
    }

    // Server/Client networking (cxadc_vhs_server-style peer mode).
    if ((value = find_value(content, "net_mode")) != NULL) {
        int mode = atoi(value);
        if (mode < 0 || mode > 2) mode = 0;
        settings->net_mode = mode;
    }
    if ((value = find_value(content, "net_server_port")) != NULL) {
        long p = atol(value);
        if (p < 1 || p > 65535) p = 8080;
        settings->net_server_port = (uint16_t)p;
    }
    if ((value = find_value(content, "net_server_port_str")) != NULL) {
        gui_settings_load_string(settings->net_server_port_str, sizeof(settings->net_server_port_str), value);
    }
    if ((value = find_value(content, "net_client_host")) != NULL) {
        gui_settings_load_string(settings->net_client_host, sizeof(settings->net_client_host), value);
    }
    if ((value = find_value(content, "net_client_port")) != NULL) {
        long p = atol(value);
        if (p < 1 || p > 65535) p = 8080;
        settings->net_client_port = (uint16_t)p;
    }
    if ((value = find_value(content, "net_client_port_str")) != NULL) {
        gui_settings_load_string(settings->net_client_port_str, sizeof(settings->net_client_port_str), value);
    }
    // Re-sync port string mirrors if the numeric port was loaded but the
    // string mirror was absent/empty (older settings files).
    if (settings->net_server_port_str[0] == '\0') {
        snprintf(settings->net_server_port_str, sizeof(settings->net_server_port_str), "%u",
                 (unsigned)settings->net_server_port);
    }
    if (settings->net_client_port_str[0] == '\0') {
        snprintf(settings->net_client_port_str, sizeof(settings->net_client_port_str), "%u",
                 (unsigned)settings->net_client_port);
    }

    // Backward-compat migration:
    // - If rf_bits_* not present, derive from legacy flags.
    if (settings->rf_bits_a != 8 && settings->rf_bits_a != 12 && settings->rf_bits_a != 16) {
        settings->rf_bits_a = settings->reduce_8bit_a ? 8 : (settings->use_flac && settings->flac_12bit ? 12 : 16);
    }
    if (settings->rf_bits_b != 8 && settings->rf_bits_b != 12 && settings->rf_bits_b != 16) {
        settings->rf_bits_b = settings->reduce_8bit_b ? 8 : (settings->use_flac && settings->flac_12bit ? 12 : 16);
    }

    // Default auto naming to ON if missing.
    // (If the key is not present, defaults already set it true.)
    if (settings->output_base_name[0] == '\0') {
        strcpy(settings->output_base_name, "capture");
    }

    // Keep auto-derived names in sync on startup (RF prefixes are conditional on empty RF tags).
    gui_settings_refresh_auto_names(settings);

    free(content);
}
