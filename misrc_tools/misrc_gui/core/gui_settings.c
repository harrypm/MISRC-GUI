/*
 * MISRC GUI - Settings Persistence
 * 16/02/25 - Remediate Win settings not saving - %appdata%
 * Handles loading/saving settings to JSON file and provides defaults
 */

#include "../core/gui_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#else
#include <direct.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

static bool gui_settings_path_is_dir(const char *path) {
    struct stat st;
    if (!path || !path[0]) return false;
    if (stat(path, &st) != 0) return false;
#if defined(_WIN32) || defined(_WIN64)
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

static bool gui_settings_make_dir_if_needed(const char *path) {
    if (!path || !path[0]) return false;
    if (gui_settings_path_is_dir(path)) return true;
#if defined(_WIN32) || defined(_WIN64)
    if (_mkdir(path) == 0) return true;
#else
    if (mkdir(path, 0700) == 0) return true;
#endif
    if (errno == EEXIST) return gui_settings_path_is_dir(path);
    return false;
}

// Best-effort recursive mkdir for a file path's parent directories.
static bool gui_settings_ensure_parent_dirs(const char *file_path) {
    if (!file_path || !file_path[0]) return false;

    char path_copy[512];
    strncpy(path_copy, file_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    size_t len = strlen(path_copy);
    if (len == 0) return false;

    for (size_t i = 0; i < len; i++) {
        char c = path_copy[i];
        if (c != '/' && c != '\\') continue;
        if (i == 0) continue;
#if defined(_WIN32) || defined(_WIN64)
        // Skip "C:\" root separator.
        if (i == 2 && path_copy[1] == ':') continue;
#endif
        path_copy[i] = '\0';
        if (!gui_settings_make_dir_if_needed(path_copy)) return false;
        path_copy[i] = c;
    }

    return true;
}

// Settings file location
static const char* get_settings_file_path(void) {
    static char settings_path[512];
    static bool initialized = false;
    
    if (!initialized) {
#if defined(__APPLE__)
        // Use ~/Library/Preferences on macOS
        const char* home = getenv("HOME");
        if (home) {
            snprintf(settings_path, sizeof(settings_path),
                    "%s/Library/Preferences/com.misrc.gui.json", home);
        } else {
            strcpy(settings_path, "./misrc_gui_settings.json");
        }
#elif defined(_WIN32) || defined(_WIN64)
        // Use %APPDATA% on Windows, then LOCALAPPDATA/USERPROFILE fallbacks.
        const char* appdata = getenv("APPDATA");
        if (!appdata || !appdata[0]) {
            appdata = getenv("LOCALAPPDATA");
        }
        if (appdata && appdata[0]) {
            snprintf(settings_path, sizeof(settings_path),
                    "%s\\MISRC\\misrc_gui_settings.json", appdata);
        } else {
            const char* userprofile = getenv("USERPROFILE");
            if (userprofile && userprofile[0]) {
                snprintf(settings_path, sizeof(settings_path),
                        "%s\\AppData\\Roaming\\MISRC\\misrc_gui_settings.json", userprofile);
            } else {
                strcpy(settings_path, "./misrc_gui_settings.json");
            }
        }
#else
        // Use XDG config directory on Linux/BSD.
        const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
        if (xdg_config_home && xdg_config_home[0]) {
            snprintf(settings_path, sizeof(settings_path),
                    "%s/misrc_gui_settings.json", xdg_config_home);
        } else {
            const char* home = getenv("HOME");
            if (home && home[0]) {
                snprintf(settings_path, sizeof(settings_path),
                        "%s/.config/misrc_gui_settings.json", home);
            } else {
                strcpy(settings_path, "./misrc_gui_settings.json");
            }
        }
#endif
        initialized = true;
    }
    
    return settings_path;
}

const char* gui_settings_get_desktop_path(void) {
    static char desktop_path[512];
    static bool initialized = false;
    
    if (!initialized) {
#if defined(__APPLE__)
        const char* home = getenv("HOME");
        if (home) {
            snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
        } else {
            strcpy(desktop_path, ".");
        }
#elif defined(_WIN32) || defined(_WIN64)
        const char* userprofile = getenv("USERPROFILE");
        if (userprofile) {
            snprintf(desktop_path, sizeof(desktop_path), "%s\\Desktop", userprofile);
        } else {
            strcpy(desktop_path, ".");
        }
#else
        const char* home = getenv("HOME");
        if (home) {
            snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
        } else {
            strcpy(desktop_path, ".");
        }
#endif
        initialized = true;
    }
    
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
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            dst[j++] = c;
        } else if (c == ' ' || c == '\t') {
            dst[j++] = '-';
        }
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
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_a, (unsigned)bits_a, rate_tag_a);
        } else if (rf_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_a, (unsigned)bits_a);
        } else if (rate_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.flac", base, (unsigned)bits_a, rate_tag_a);
        } else {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.flac", base, (unsigned)bits_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_b, (unsigned)bits_b, rate_tag_b);
        } else if (rf_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_b, (unsigned)bits_b);
        } else if (rate_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.flac", base, (unsigned)bits_b, rate_tag_b);
        } else {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.flac", base, (unsigned)bits_b);
        }
    } else {
        uint8_t bits_a = rf_bits_for_raw(settings->rf_bits_a);
        uint8_t bits_b = rf_bits_for_raw(settings->rf_bits_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (settings->enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), settings->resample_rate_a);
        if (settings->enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), settings->resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), settings->rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), settings->rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.raw", base, rf_tag_a, (unsigned)bits_a, rate_tag_a);
        } else if (rf_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.raw", base, rf_tag_a, (unsigned)bits_a);
        } else if (rate_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.raw", base, (unsigned)bits_a, rate_tag_a);
        } else {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.raw", base, (unsigned)bits_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.raw", base, rf_tag_b, (unsigned)bits_b, rate_tag_b);
        } else if (rf_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.raw", base, rf_tag_b, (unsigned)bits_b);
        } else if (rate_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.raw", base, (unsigned)bits_b, rate_tag_b);
        } else {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.raw", base, (unsigned)bits_b);
        }
    }

    char audio_tag_4ch[40] = {0};
    char audio_tag_12[40] = {0};
    char audio_tag_34[40] = {0};
    sanitize_tag(audio_tag_4ch, sizeof(audio_tag_4ch), settings->audio_output_tags[0]);
    sanitize_tag(audio_tag_12, sizeof(audio_tag_12), settings->audio_output_tags[1]);
    sanitize_tag(audio_tag_34, sizeof(audio_tag_34), settings->audio_output_tags[2]);

    if (audio_tag_4ch[0]) {
        snprintf(settings->audio_4ch_filename, MAX_FILENAME_LEN, "%s_%s_quad_4ch.wav", base, audio_tag_4ch);
    } else {
        snprintf(settings->audio_4ch_filename, MAX_FILENAME_LEN, "%s_quad_4ch.wav", base);
    }
    if (audio_tag_12[0]) {
        snprintf(settings->audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch1_ch2.wav", base, audio_tag_12);
    } else {
        snprintf(settings->audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_stereo_ch1_ch2.wav", base);
    }
    if (audio_tag_34[0]) {
        snprintf(settings->audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch3_ch4.wav", base, audio_tag_34);
    } else {
        snprintf(settings->audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_stereo_ch3_ch4.wav", base);
    }

    for (int i = 0; i < 4; i++) {
        char tag[40];
        sanitize_tag(tag, sizeof(tag), settings->audio_1ch_labels[i]);
        if (tag[0]) {
            snprintf(settings->audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_%s_audio_ch%d.wav", base, tag, i + 1);
        } else {
            snprintf(settings->audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_audio_ch%d.wav", base, i + 1);
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
    strncpy(settings->output_path, gui_settings_get_desktop_path(), MAX_FILENAME_LEN - 1);
    settings->output_path[MAX_FILENAME_LEN - 1] = '\0';
    
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
    settings->rf_bits_a = 16;
    settings->rf_bits_b = 16;
    
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
    
    // FLAC defaults
    settings->use_flac = true;
    settings->flac_12bit = false;
    settings->flac_level = 4;             // Balanced compression
    settings->flac_verification = false;  // Faster
    settings->flac_threads = 0;           // Auto
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
    settings->stop_on_dropout = false;

    // Level autostop defaults (tape-end detection). Disabled by default.
    // Defaults mirror PR #11: 33% threshold, 5.0s sustain.
    settings->level_autostop_enabled = false;
    strcpy(settings->level_autostop_level_str, "33");
    strcpy(settings->level_autostop_duration_str, "5.0");
    
    // Display settings
    settings->show_grid = true;
    settings->time_scale = 1.0f;
    settings->amplitude_scale = 1.0f;

    // Keep derived filenames coherent with default auto-naming state.
    gui_settings_refresh_auto_names(settings);
}

// Simple JSON-like format for settings
// 16.02.25 - Remediate Win save
void gui_settings_save(const gui_settings_t *settings) {
    if (!settings) return;
    
    const char* path = get_settings_file_path();
    if (!gui_settings_ensure_parent_dirs(path)) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    
    fprintf(f, "{\n");
    fprintf(f, "  \"device_index\": %d,\n", settings->device_index);
    fprintf(f, "  \"output_path\": \"%s\",\n", settings->output_path);
    fprintf(f, "  \"auto_names_enabled\": %s,\n", settings->auto_names_enabled ? "true" : "false");
    fprintf(f, "  \"output_base_name\": \"%s\",\n", settings->output_base_name);
    fprintf(f, "  \"append_timestamp_on_capture_start\": %s,\n", settings->append_timestamp_on_capture_start ? "true" : "false");
    fprintf(f, "  \"rf_bits_a\": %u,\n", (unsigned)settings->rf_bits_a);
    fprintf(f, "  \"rf_bits_b\": %u,\n", (unsigned)settings->rf_bits_b);
    fprintf(f, "  \"rf_tag_a\": \"%s\",\n", settings->rf_channel_tags[0]);
    fprintf(f, "  \"rf_tag_b\": \"%s\",\n", settings->rf_channel_tags[1]);
    fprintf(f, "  \"output_filename_a\": \"%s\",\n", settings->output_filename_a);
    fprintf(f, "  \"output_filename_b\": \"%s\",\n", settings->output_filename_b);
    fprintf(f, "  \"capture_a\": %s,\n", settings->capture_a ? "true" : "false");
    fprintf(f, "  \"capture_b\": %s,\n", settings->capture_b ? "true" : "false");
    fprintf(f, "  \"sample_count\": %llu,\n", (unsigned long long)settings->sample_count);
    fprintf(f, "  \"capture_time\": \"%s\",\n", settings->capture_time);
    fprintf(f, "  \"overwrite_files\": %s,\n", settings->overwrite_files ? "true" : "false");
    fprintf(f, "  \"aux_filename\": \"%s\",\n", settings->aux_filename);
    fprintf(f, "  \"raw_filename\": \"%s\",\n", settings->raw_filename);

    // Audio filenames + enables (mirror CLI options)
    fprintf(f, "  \"audio_4ch_filename\": \"%s\",\n", settings->audio_4ch_filename);
    fprintf(f, "  \"audio_2ch_12_filename\": \"%s\",\n", settings->audio_2ch_12_filename);
    fprintf(f, "  \"audio_2ch_34_filename\": \"%s\",\n", settings->audio_2ch_34_filename);
    fprintf(f, "  \"audio_1ch_1_filename\": \"%s\",\n", settings->audio_1ch_filenames[0]);
    fprintf(f, "  \"audio_1ch_2_filename\": \"%s\",\n", settings->audio_1ch_filenames[1]);
    fprintf(f, "  \"audio_1ch_3_filename\": \"%s\",\n", settings->audio_1ch_filenames[2]);
    fprintf(f, "  \"audio_1ch_4_filename\": \"%s\",\n", settings->audio_1ch_filenames[3]);
    fprintf(f, "  \"audio_1ch_1_label\": \"%s\",\n", settings->audio_1ch_labels[0]);
    fprintf(f, "  \"audio_1ch_2_label\": \"%s\",\n", settings->audio_1ch_labels[1]);
    fprintf(f, "  \"audio_1ch_3_label\": \"%s\",\n", settings->audio_1ch_labels[2]);
    fprintf(f, "  \"audio_1ch_4_label\": \"%s\",\n", settings->audio_1ch_labels[3]);
    fprintf(f, "  \"audio_tag_4ch\": \"%s\",\n", settings->audio_output_tags[0]);
    fprintf(f, "  \"audio_tag_2ch_12\": \"%s\",\n", settings->audio_output_tags[1]);
    fprintf(f, "  \"audio_tag_2ch_34\": \"%s\",\n", settings->audio_output_tags[2]);

    fprintf(f, "  \"enable_audio_4ch\": %s,\n", settings->enable_audio_4ch ? "true" : "false");
    fprintf(f, "  \"enable_audio_2ch_12\": %s,\n", settings->enable_audio_2ch_12 ? "true" : "false");
    fprintf(f, "  \"enable_audio_2ch_34\": %s,\n", settings->enable_audio_2ch_34 ? "true" : "false");
    fprintf(f, "  \"audio_monitor_playback\": %s,\n", settings->audio_monitor_playback ? "true" : "false");
    fprintf(f, "  \"audio_monitor_ch34\": %s,\n", settings->audio_monitor_ch34 ? "true" : "false");
    fprintf(f, "  \"misrc_mode\": %s,\n", settings->misrc_mode ? "true" : "false");
    fprintf(f, "  \"stop_on_dropout\": %s,\n", settings->stop_on_dropout ? "true" : "false");
    fprintf(f, "  \"level_autostop_enabled\": %s,\n", settings->level_autostop_enabled ? "true" : "false");
    fprintf(f, "  \"level_autostop_level_str\": \"%s\",\n", settings->level_autostop_level_str);
    fprintf(f, "  \"level_autostop_duration_str\": \"%s\",\n", settings->level_autostop_duration_str);
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
    fprintf(f, "  \"use_flac\": %s,\n", settings->use_flac ? "true" : "false");
    fprintf(f, "  \"flac_12bit\": %s,\n", settings->flac_12bit ? "true" : "false");
    fprintf(f, "  \"flac_level\": %d,\n", settings->flac_level);
    fprintf(f, "  \"flac_verification\": %s,\n", settings->flac_verification ? "true" : "false");
    fprintf(f, "  \"flac_threads\": %d,\n", settings->flac_threads);
    fprintf(f, "  \"flac_affinity_enabled\": %s,\n", settings->flac_affinity_enabled ? "true" : "false");
    fprintf(f, "  \"flac_affinity_cpu_list\": \"%s\",\n", settings->flac_affinity_cpu_list);
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
    fprintf(f, "  \"playback_file_a\": \"%s\",\n", settings->playback_file_a);
    fprintf(f, "  \"playback_file_b\": \"%s\"\n", settings->playback_file_b);
    fprintf(f, "}\n");
    
    fclose(f);
}

// Simple parser for our JSON-like format
static char* find_value(const char* content, const char* key) {
    static char value[512];
    char search_key[256];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    char* pos = strstr(content, search_key);
    if (!pos) return NULL;
    
    pos += strlen(search_key);
    while (*pos == ' ' || *pos == '\t') pos++; // Skip whitespace
    
    if (*pos == '"') {
        // String value
        pos++;
        char* end = strchr(pos, '"');
        if (!end) return NULL;
        
        size_t len = end - pos;
        if (len >= sizeof(value)) len = sizeof(value) - 1;
        strncpy(value, pos, len);
        value[len] = '\0';
        return value;
    } else {
        // Number or boolean
        char* end = strpbrk(pos, ",\n}");
        if (!end) return NULL;
        
        size_t len = end - pos;
        if (len >= sizeof(value)) len = sizeof(value) - 1;
        strncpy(value, pos, len);
        value[len] = '\0';
        
        // Trim trailing whitespace
        while (len > 0 && (value[len-1] == ' ' || value[len-1] == '\t')) {
            value[--len] = '\0';
        }
        
        return value;
    }
}

// Helpers
static void trim_newlines(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

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

// macOS folder picker using osascript. Returns true if output_path changed.
bool gui_settings_choose_output_folder(gui_settings_t *settings) {
    if (!settings) return false;
    char picked[512] = {0};

#ifdef __APPLE__
    // Use AppleScript choose folder dialog and return POSIX path.
    // Note: This will prompt the user and block until a selection is made.
    const char *cmd = "osascript -e 'POSIX path of (choose folder with prompt \"Select output folder for MISRC captures\")'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
#elif defined(_WIN32) || defined(_WIN64)
    // PowerShell folder picker (requires Windows Forms)
    const char *cmd =
        "powershell -NoProfile -Command "
        "\"Add-Type -AssemblyName System.Windows.Forms; "
        "$f=New-Object System.Windows.Forms.FolderBrowserDialog; "
        "if($f.ShowDialog() -eq 'OK'){ $f.SelectedPath }\"";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
#else
    // Linux/BSD: try zenity first, then kdialog.
    const char *cmd =
        "sh -c '"
        "if command -v zenity >/dev/null 2>&1; then "
        "zenity --file-selection --directory --title=\"Select output folder for MISRC captures\"; "
        "elif command -v kdialog >/dev/null 2>&1; then "
        "kdialog --getexistingdirectory \"$HOME\" \"Select output folder for MISRC captures\"; "
        "fi'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
#endif

    trim_newlines(picked);
    if (picked[0] == '\0') return false;

    // Remove trailing slash/backslash
    size_t len = strlen(picked);
    while (len > 1 && (picked[len - 1] == '/' || picked[len - 1] == '\\')) {
        picked[--len] = '\0';
    }

    if (strncmp(settings->output_path, picked, MAX_FILENAME_LEN) == 0) {
        return false; // no change
    }

    strncpy(settings->output_path, picked, MAX_FILENAME_LEN - 1);
    settings->output_path[MAX_FILENAME_LEN - 1] = '\0';
    return true;
}

// Cross-platform (best-effort) playback file picker.
bool gui_settings_choose_playback_file(gui_settings_t *settings, int channel) {
    if (!settings) return false;
    if (channel != 0 && channel != 1) return false;

    char picked[512] = {0};

#if defined(__APPLE__)
    // choose file, return POSIX path
    const char *cmd = "osascript -e 'POSIX path of (choose file with prompt \"Select FLAC playback file\")'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
    trim_newlines(picked);
#elif defined(_WIN32) || defined(_WIN64)
    // PowerShell OpenFileDialog (requires Windows Forms)
    const char *cmd =
        "powershell -NoProfile -Command "
        "\"Add-Type -AssemblyName System.Windows.Forms; "
        "$f=New-Object System.Windows.Forms.OpenFileDialog; "
        "$f.Filter='FLAC files (*.flac)|*.flac|All files (*.*)|*.*'; "
        "if($f.ShowDialog() -eq 'OK'){ $f.FileName }\"";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
    trim_newlines(picked);
#else
    // Linux/BSD: try zenity first, then kdialog.
    const char *cmd =
        "sh -c '"
        "if command -v zenity >/dev/null 2>&1; then "
        "zenity --file-selection --title=\"Select FLAC playback file\" --file-filter=\"*.flac\"; "
        "elif command -v kdialog >/dev/null 2>&1; then "
        "kdialog --getopenfilename \"$HOME\" \"*.flac|FLAC files (*.flac)\"; "
        "fi'";
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    if (!fgets(picked, sizeof(picked), fp)) {
        pclose(fp);
        return false;
    }
    (void)pclose(fp);
    trim_newlines(picked);
#endif

    if (picked[0] == '\0') return false;

    char *dst = (channel == 0) ? settings->playback_file_a : settings->playback_file_b;
    if (strncmp(dst, picked, MAX_FILENAME_LEN) == 0) {
        return false;
    }

    strncpy(dst, picked, MAX_FILENAME_LEN - 1);
    dst[MAX_FILENAME_LEN - 1] = '\0';
    return true;
}

void gui_settings_load(gui_settings_t *settings) {
    if (!settings) return;
    
    // Start with defaults
    gui_settings_init_defaults(settings);
    
    const char* path = get_settings_file_path();
    FILE* f = fopen(path, "r");
    if (!f) return; // No settings file, use defaults
    
    // Read entire file
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 32768) { // Sanity check
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
    
    // Parse values
    char* value;
    
    if ((value = find_value(content, "device_index")) != NULL) {
        settings->device_index = atoi(value);
    }
    
    if ((value = find_value(content, "output_path")) != NULL) {
        strncpy(settings->output_path, value, MAX_FILENAME_LEN - 1);
        settings->output_path[MAX_FILENAME_LEN - 1] = '\0';
    }
    
    // Auto naming
    if ((value = find_value(content, "auto_names_enabled")) != NULL) {
        settings->auto_names_enabled = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "output_base_name")) != NULL) {
        strncpy(settings->output_base_name, value, MAX_FILENAME_LEN - 1);
        settings->output_base_name[MAX_FILENAME_LEN - 1] = '\0';
        strip_timestamp_prefix_inplace(settings->output_base_name);
    }
    if ((value = find_value(content, "append_timestamp_on_capture_start")) != NULL) {
        settings->append_timestamp_on_capture_start = (strcmp(value, "true") == 0);
    }
    // Duration limits: feature removed, ignore saved values and force to 0
    settings->capture_limit_seconds = 0;
    settings->record_limit_seconds = 0;
    if ((value = find_value(content, "rf_bits_a")) != NULL) {
        settings->rf_bits_a = (uint8_t)atoi(value);
    }
    if ((value = find_value(content, "rf_bits_b")) != NULL) {
        settings->rf_bits_b = (uint8_t)atoi(value);
    }
    if ((value = find_value(content, "rf_tag_a")) != NULL) {
        strncpy(settings->rf_channel_tags[0], value, sizeof(settings->rf_channel_tags[0]) - 1);
        settings->rf_channel_tags[0][sizeof(settings->rf_channel_tags[0]) - 1] = '\0';
    }
    if ((value = find_value(content, "rf_tag_b")) != NULL) {
        strncpy(settings->rf_channel_tags[1], value, sizeof(settings->rf_channel_tags[1]) - 1);
        settings->rf_channel_tags[1][sizeof(settings->rf_channel_tags[1]) - 1] = '\0';
    }

    if ((value = find_value(content, "output_filename_a")) != NULL) {
        strncpy(settings->output_filename_a, value, MAX_FILENAME_LEN - 1);
        settings->output_filename_a[MAX_FILENAME_LEN - 1] = '\0';
    }

    if ((value = find_value(content, "output_filename_b")) != NULL) {
        strncpy(settings->output_filename_b, value, MAX_FILENAME_LEN - 1);
        settings->output_filename_b[MAX_FILENAME_LEN - 1] = '\0';
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
        strncpy(settings->flac_affinity_cpu_list, value, sizeof(settings->flac_affinity_cpu_list) - 1);
        settings->flac_affinity_cpu_list[sizeof(settings->flac_affinity_cpu_list) - 1] = '\0';
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

    if ((value = find_value(content, "reduce_8bit_a")) != NULL) {
        settings->reduce_8bit_a = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "reduce_8bit_b")) != NULL) {
        settings->reduce_8bit_b = (strcmp(value, "true") == 0);
    }

    // Audio filenames + enables
    if ((value = find_value(content, "audio_4ch_filename")) != NULL) {
        strncpy(settings->audio_4ch_filename, value, MAX_FILENAME_LEN - 1);
        settings->audio_4ch_filename[MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_2ch_12_filename")) != NULL) {
        strncpy(settings->audio_2ch_12_filename, value, MAX_FILENAME_LEN - 1);
        settings->audio_2ch_12_filename[MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_2ch_34_filename")) != NULL) {
        strncpy(settings->audio_2ch_34_filename, value, MAX_FILENAME_LEN - 1);
        settings->audio_2ch_34_filename[MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_1_filename")) != NULL) {
        strncpy(settings->audio_1ch_filenames[0], value, MAX_FILENAME_LEN - 1);
        settings->audio_1ch_filenames[0][MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_2_filename")) != NULL) {
        strncpy(settings->audio_1ch_filenames[1], value, MAX_FILENAME_LEN - 1);
        settings->audio_1ch_filenames[1][MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_3_filename")) != NULL) {
        strncpy(settings->audio_1ch_filenames[2], value, MAX_FILENAME_LEN - 1);
        settings->audio_1ch_filenames[2][MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_4_filename")) != NULL) {
        strncpy(settings->audio_1ch_filenames[3], value, MAX_FILENAME_LEN - 1);
        settings->audio_1ch_filenames[3][MAX_FILENAME_LEN - 1] = '\0';
    }

    // Per-channel audio labels (optional)
    if ((value = find_value(content, "audio_1ch_1_label")) != NULL) {
        strncpy(settings->audio_1ch_labels[0], value, sizeof(settings->audio_1ch_labels[0]) - 1);
        settings->audio_1ch_labels[0][sizeof(settings->audio_1ch_labels[0]) - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_2_label")) != NULL) {
        strncpy(settings->audio_1ch_labels[1], value, sizeof(settings->audio_1ch_labels[1]) - 1);
        settings->audio_1ch_labels[1][sizeof(settings->audio_1ch_labels[1]) - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_3_label")) != NULL) {
        strncpy(settings->audio_1ch_labels[2], value, sizeof(settings->audio_1ch_labels[2]) - 1);
        settings->audio_1ch_labels[2][sizeof(settings->audio_1ch_labels[2]) - 1] = '\0';
    }
    if ((value = find_value(content, "audio_1ch_4_label")) != NULL) {
        strncpy(settings->audio_1ch_labels[3], value, sizeof(settings->audio_1ch_labels[3]) - 1);
        settings->audio_1ch_labels[3][sizeof(settings->audio_1ch_labels[3]) - 1] = '\0';
    }
    if ((value = find_value(content, "audio_tag_4ch")) != NULL) {
        strncpy(settings->audio_output_tags[0], value, sizeof(settings->audio_output_tags[0]) - 1);
        settings->audio_output_tags[0][sizeof(settings->audio_output_tags[0]) - 1] = '\0';
    }
    if ((value = find_value(content, "audio_tag_2ch_12")) != NULL) {
        strncpy(settings->audio_output_tags[1], value, sizeof(settings->audio_output_tags[1]) - 1);
        settings->audio_output_tags[1][sizeof(settings->audio_output_tags[1]) - 1] = '\0';
    }
    if ((value = find_value(content, "audio_tag_2ch_34")) != NULL) {
        strncpy(settings->audio_output_tags[2], value, sizeof(settings->audio_output_tags[2]) - 1);
        settings->audio_output_tags[2][sizeof(settings->audio_output_tags[2]) - 1] = '\0';
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
    if ((value = find_value(content, "stop_on_dropout")) != NULL) {
        settings->stop_on_dropout = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "level_autostop_enabled")) != NULL) {
        settings->level_autostop_enabled = (strcmp(value, "true") == 0);
    }
    if ((value = find_value(content, "level_autostop_level_str")) != NULL) {
        strncpy(settings->level_autostop_level_str, value, sizeof(settings->level_autostop_level_str) - 1);
        settings->level_autostop_level_str[sizeof(settings->level_autostop_level_str) - 1] = '\0';
    }
    if ((value = find_value(content, "level_autostop_duration_str")) != NULL) {
        strncpy(settings->level_autostop_duration_str, value, sizeof(settings->level_autostop_duration_str) - 1);
        settings->level_autostop_duration_str[sizeof(settings->level_autostop_duration_str) - 1] = '\0';
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
        strncpy(settings->playback_file_a, value, MAX_FILENAME_LEN - 1);
        settings->playback_file_a[MAX_FILENAME_LEN - 1] = '\0';
    }
    if ((value = find_value(content, "playback_file_b")) != NULL) {
        strncpy(settings->playback_file_b, value, MAX_FILENAME_LEN - 1);
        settings->playback_file_b[MAX_FILENAME_LEN - 1] = '\0';
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
