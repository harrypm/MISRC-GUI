#ifndef GUI_APP_H
#define GUI_APP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stddef.h>
#include <time.h>
#include "raylib.h"
#include "../../common/buffer_manager.h"

// Forward declarations
typedef struct hsdaoh_dev hsdaoh_dev_t;
typedef struct sc_handle sc_handle_t;
typedef struct phosphor_rt phosphor_rt_t;
typedef struct display_thread display_thread_t;

//-----------------------------------------------------------------------------
// Panel View Types (defined here to avoid circular includes)
//-----------------------------------------------------------------------------

typedef enum {
    PANEL_VIEW_WAVEFORM,           // Waveform oscilloscope (line or phosphor mode)
    PANEL_VIEW_FFT,                // FFT spectrum analysis
    PANEL_VIEW_CVBS,               // CVBS luma decoder view
    PANEL_VIEW_HISTOGRAM,          // Amplitude histogram
    PANEL_VIEW_COUNT
    // Future: PANEL_VIEW_XY, PANEL_VIEW_SPECTROGRAM
} panel_view_type_t;

// Waveform render modes (selected via dropdown on panel)
typedef enum {
    WAVEFORM_MODE_LINE,            // Simple line waveform (fast)
    WAVEFORM_MODE_PHOSPHOR,        // Digital phosphor with persistence
    WAVEFORM_MODE_COUNT
} waveform_render_mode_t;

// Per-Channel Panel Configuration
typedef struct channel_panel_config {
    bool split;                    // false = single panel, true = split view
    panel_view_type_t left_view;   // View for left panel (or only panel if not split)
    panel_view_type_t right_view;  // View for right panel (only used if split)
    void *left_state;              // View-specific state (created via panel_create_view_state)
    void *right_state;             // View-specific state for right panel
    Rectangle left_bounds;         // Cached bounds from last render (for click handling)
    Rectangle right_bounds;        // Cached bounds for right panel
} channel_panel_config_t;

// Display buffer size (samples per channel for oscilloscope)
#define DISPLAY_BUFFER_SIZE 4096
#define MAX_DEVICES 16
#define MAX_FILENAME_LEN 256

// Waveform display sample - resampled via libsoxr with anti-aliasing
// Values are normalized floats in range -1.0 to 1.0
typedef struct {
    float value;              // Resampled waveform value (via libsoxr)
} waveform_sample_t;

// VU meter state - tracks positive and negative separately for AC signals
typedef struct vu_meter_state {
    float level_pos;          // Current smoothed positive level (0-1)
    float level_neg;          // Current smoothed negative level (0-1)
    float peak_pos;           // Peak hold positive (0-1)
    float peak_neg;           // Peak hold negative (0-1)
    float peak_hold_time_pos; // Time since positive peak was captured
    float peak_hold_time_neg; // Time since negative peak was captured
} vu_meter_state_t;

// Oscilloscope display modes (per-channel)
typedef enum {
    SCOPE_MODE_LINE,      // Basic line waveform (fast, simple)
    SCOPE_MODE_PHOSPHOR,  // Digital phosphor with heatmap persistence
    SCOPE_MODE_SPLIT,     // Split view: waveform left, FFT waterfall right
    SCOPE_MODE_CVBS,      // CVBS luma decoder view
    SCOPE_MODE_COUNT      // Number of modes (for cycling)
} scope_display_mode_t;

// Phosphor color modes
typedef enum {
    PHOSPHOR_COLOR_HEATMAP,   // Blue-green-yellow-red heatmap based on intensity
    PHOSPHOR_COLOR_OPACITY,   // Channel color with intensity as opacity
    PHOSPHOR_COLOR_COUNT
} phosphor_color_mode_t;

// Trigger modes (per-channel)
typedef enum {
    TRIGGER_MODE_RISING,      // Rising edge crossing level
    TRIGGER_MODE_FALLING,     // Falling edge crossing level
    TRIGGER_MODE_SYNC,        // Sync-locked trigger (CH3 headswitch phase lock)
    TRIGGER_MODE_CVBS_HSYNC,  // CVBS horizontal sync (auto PAL/NTSC)
    TRIGGER_MODE_COUNT
} trigger_mode_t;
// Trigger source selection (absolute channels)
typedef enum {
    TRIGGER_SOURCE_CH1,       // RF channel 1 / Channel A
    TRIGGER_SOURCE_CH2,       // RF channel 2 / Channel B
    TRIGGER_SOURCE_CH3,       // Clockgen headswitch channel
    TRIGGER_SOURCE_COUNT
} trigger_source_t;

// Per-channel trigger configuration and state
typedef struct {
    bool enabled;              // Trigger enabled for this channel
    int16_t level;             // Trigger level (-2048 to +2047, 12-bit range)
    float zoom_scale;          // Samples per pixel (1.0 = max zoom, higher = more zoomed out)
    int trigger_display_pos;   // Where trigger appears in display buffer (-1 if not triggered)
    atomic_int display_width;  // Actual pixel width of oscilloscope display (updated by renderer, read by extraction thread)
    scope_display_mode_t scope_mode;       // Display mode for this channel (line or phosphor)
    trigger_mode_t trigger_mode;           // Trigger mode (rising edge, falling edge, CVBS)
    trigger_source_t trigger_source;       // Trigger source channel (CH1/CH2/CH3)
    phosphor_color_mode_t phosphor_color;  // Phosphor color mode (heatmap or opacity)

    // Resampler state (managed by gui_oscilloscope.c)
    void *resampler;           // soxr_t handle (NULL if not initialized)
    float resampler_ratio;     // Current decimation ratio the resampler is configured for
} channel_trigger_t;

// Zoom limits
#define ZOOM_SCALE_MIN 1.0f    // 1 sample per pixel (max zoom in)
#define ZOOM_SCALE_MAX 128.0f  // 128 samples per pixel (max zoom out)
#define ZOOM_SCALE_DEFAULT 32.0f

// Default sample rate (40 MSPS per channel)
#define DEFAULT_SAMPLE_RATE 40000000

// Digital phosphor display settings
#define PHOSPHOR_MAX_WIDTH 4096   // Maximum phosphor buffer width (pixels)
#define PHOSPHOR_MAX_HEIGHT 512   // Maximum phosphor buffer height (pixels)
#define PHOSPHOR_DECAY_RATE 0.80f // Base decay multiplier per frame (0-1, higher = slower fade)
#define PHOSPHOR_HIT_INCREMENT 0.5f // Intensity added per waveform hit (0-1)

// Device info for enumeration
// Device type enumeration
typedef enum {
    DEVICE_TYPE_HSDAOH,         // Hardware device via hsdaoh
    DEVICE_TYPE_SIMPLE_CAPTURE, // OS video capture
    DEVICE_TYPE_CXADC,          // CXADC RF capture card(s)
    DEVICE_TYPE_SIMULATED,      // Simulated device for testing
    DEVICE_TYPE_PLAYBACK,       // Playback from recorded FLAC files
#ifdef ENABLE_FX3
    DEVICE_TYPE_FX3,            // Cypress FX3 USB device
#endif
#ifdef ENABLE_DDD
    DEVICE_TYPE_DDD,            // DomesdayDuplicator USB device
#endif
} device_type_t;

typedef struct {
    char name[64];
    char serial[64];
    device_type_t type;
    int index;
} device_info_t;

// GUI settings (bound to UI controls) - mirrors all CLI options
typedef struct {
    // Basic settings
    int device_index;
    char output_filename_a[MAX_FILENAME_LEN];
    char output_filename_b[MAX_FILENAME_LEN];
    char output_path[MAX_FILENAME_LEN];        // Default save path (Desktop)
    bool capture_a;
    bool capture_b;

    // Auto naming
    bool auto_names_enabled;                   // If true, derive filenames from output_base_name + parameters
    char output_base_name[MAX_FILENAME_LEN];   // Base name for outputs (no extension)

    // Timestamp behavior
    // If true, append a system-time timestamp captured at *record start* to the base name when generating output filenames.
    // This does not mutate output_base_name; it only affects derived filenames.
    bool append_timestamp_on_capture_start;

    // Capture duration limit (0 = no limit)
    uint32_t capture_limit_seconds;

    // Recording duration limit (0 = no limit)
    uint32_t record_limit_seconds;

    // RF bit depth selection (per-channel)
    // FLAC supports 8/12/16; RAW supports 8/16 (12 is disabled in RAW UI).
    uint8_t rf_bits_a;
    uint8_t rf_bits_b;
    // Optional per-channel RF tags used in auto naming (e.g. "luma", "chroma")
    char rf_channel_tags[2][32];

    // Capture control
    uint64_t sample_count;                     // Number of samples (0 = infinite)
    char capture_time[32];                     // Time string (e.g., "5:30" or "1:30:00")
    bool overwrite_files;                      // Overwrite without asking

    // Output files (used when auto_names_enabled=false, or as fallbacks)
    char aux_filename[MAX_FILENAME_LEN];
    char raw_filename[MAX_FILENAME_LEN];
    char audio_4ch_filename[MAX_FILENAME_LEN];
    char audio_2ch_12_filename[MAX_FILENAME_LEN];
    char audio_2ch_34_filename[MAX_FILENAME_LEN];
    char audio_1ch_filenames[4][MAX_FILENAME_LEN]; // Individual channel files

    // Processing options
    bool pad_lower_bits;                       // Pad lower 4 bits instead of upper
    bool show_peak_levels;                     // Display peak levels
    bool suppress_clip_a;                      // Suppress clipping messages A
    bool suppress_clip_b;                      // Suppress clipping messages B

    // Legacy flags (kept for backward-compatible settings load)
    bool reduce_8bit_a;                        // Reduce A to 8-bit
    bool reduce_8bit_b;                        // Reduce B to 8-bit

    // Resampling (if SOXR enabled)
    bool enable_resample_a;
    bool enable_resample_b;
    float resample_rate_a;                     // kHz
    float resample_rate_b;                     // kHz
    int resample_quality_a;                    // 0-4
    int resample_quality_b;                    // 0-4
    float resample_gain_a;                     // dB
    float resample_gain_b;                     // dB

    // FLAC compression
    bool use_flac;
    bool flac_12bit;                           // Legacy (kept for backward-compatible load)
    int flac_level;                            // 0-8
    bool flac_verification;                    // Verify encoder output
    int flac_threads;                          // Number of threads
    bool flac_affinity_enabled;                // Linux-only: pin FLAC work to selected CPU cores
    char flac_affinity_cpu_list[64];           // Linux-only CPU list/range string, e.g. "10-17,20"

    // Audio output options
    bool enable_audio_4ch;
    bool enable_audio_2ch_12;
    bool enable_audio_2ch_34;
    bool enable_audio_1ch[4];                  // Individual channel enables

    // Audio monitoring
    bool audio_monitor_playback;               // If true, play monitored audio to system output
    bool audio_monitor_ch34;                   // If true, monitor CH3/4; if false, monitor CH1/2
    bool misrc_mode;                           // If true, MISRC mode (default) with A/B channel swap
    bool stop_on_dropout;                      // If true, automatically stop capture when stream dropout is detected

    // Level autostop: stop capture/recording when signal level stays below a
    // configurable percentage for a configurable duration (tape-end detection).
    // Independent from the digital dropout (frame error/missed frame) logic above.
    bool level_autostop_enabled;               // Enable/disable the level-based autostop
    char level_autostop_level_str[16];         // Signal level threshold as a percent string (e.g. "33")
    char level_autostop_duration_str[16];      // Sustain duration as a seconds string (e.g. "5.0")

    // Per-channel audio labels (for auto naming, e.g. "linear", "baseband")
    char audio_1ch_labels[4][32];
    // Optional tags for non-mono audio outputs: [0]=4ch, [1]=stereo ch1/2, [2]=stereo ch3/4
    char audio_output_tags[3][32];
    // Ingest metadata (saved to settings and written to capture log at record start)
    char ingest_project[128];
    char ingest_tape_id[128];
    char ingest_tape_format[128];
    char ingest_tape_size[128];
    char ingest_tape_speed[128];
    char ingest_tape_condition[128];
    char ingest_operator[128];
    char ingest_location[128];
    char ingest_notes[256];

    // Display settings (existing)
    bool show_grid;
    float time_scale;         // Time per division (ms)
    float amplitude_scale;    // Amplitude scale factor

    // Device discovery: V4L2/simple_capture device enumeration is opt-in.
    // Disabled by default since most users use hsdaoh/CXADC/DdD/FX3 backends;
    // enabling it lists OS video capture devices (e.g. MS2130 HDMI capture)
    // in the device dropdown.
    bool discover_simple_capture;
    // Advanced settings visibility: show/hide core-pinning controls in Settings.
    bool show_core_pinning_in_settings;

    // Playback settings
    char playback_file_a[MAX_FILENAME_LEN];   // FLAC file for channel A playback
    char playback_file_b[MAX_FILENAME_LEN];   // FLAC file for channel B playback
} gui_settings_t;

typedef enum {
    GUI_DROPOUT_NONE = 0,
    GUI_DROPOUT_MISSED_FRAME = 1,
    GUI_DROPOUT_FRAME_ERROR = 2,
    GUI_DROPOUT_ERROR_BURST = 3,
    GUI_DROPOUT_CALLBACK_GAP = 4,
    GUI_DROPOUT_DEVICE_ERROR = 5,
    GUI_DROPOUT_BACKPRESSURE = 6,
    GUI_DROPOUT_DISK_SPACE = 7,
    GUI_DROPOUT_LOW_SIGNAL = 8,   // Level autostop: sustained low/no signal (tape end)
} gui_dropout_reason_t;

// Main application state
typedef struct gui_app {
    // Device handles
    hsdaoh_dev_t *hs_dev;
    sc_handle_t *sc_dev;

    // Simulated device state
    void *sim_thread;          // Simulated capture thread handle
    atomic_bool sim_running;   // Flag to stop simulated capture

    // Playback device state
    atomic_bool playback_running;  // Flag for playback mode

#ifdef ENABLE_FX3
    // FX3 device state
    void *fx3_dev;                 // FX3 device handle (cyusb_handle *)
    void *fx3_thread;              // FX3 capture thread handle
    atomic_bool fx3_running;       // Flag for FX3 capture mode
#endif

#ifdef ENABLE_DDD
    // DdD device state
    void *ddd_dev;                 // DdD device handle (libusb_device_handle *)
    void *ddd_thread;              // DdD capture thread handle
    atomic_bool ddd_running;       // Flag for DdD capture mode
#endif

    // Capture state
    bool is_capturing;
    bool is_recording;
    bool user_capture_mode_misrc;      // Authoritative user-selected mode (changes only via mode toggle)
    bool capture_mode_runtime_misrc;   // Mode latched at recording start (stable for recording session)

    // Device enumeration
    device_info_t devices[MAX_DEVICES];
    int device_count;
    int selected_device;

    // Per-channel display buffers for waveform
    waveform_sample_t display_samples_a[DISPLAY_BUFFER_SIZE];
    waveform_sample_t display_samples_b[DISPLAY_BUFFER_SIZE];
    size_t display_samples_available_a;
    size_t display_samples_available_b;

    // VU meter state (updated on main thread from atomic values)
    vu_meter_state_t vu_a;
    vu_meter_state_t vu_b;

    // Atomic values from capture thread (separate pos/neg for AC signals)
    atomic_uint_fast16_t peak_a_pos;  // Maximum positive sample (0-2047)
    atomic_uint_fast16_t peak_a_neg;  // Maximum negative sample (0-2048, stored as positive)
    atomic_uint_fast16_t peak_b_pos;
    atomic_uint_fast16_t peak_b_neg;

    // Statistics (atomic, updated by capture thread)
    atomic_uint_fast64_t total_samples;
    atomic_uint_fast64_t samples_a;         // Per-channel sample count
    atomic_uint_fast64_t samples_b;
    atomic_uint_fast32_t frame_count;
    atomic_uint_fast32_t missed_frame_count;  // Missed frames from sync events
    atomic_uint_fast32_t error_count;       // Combined total (parser + system events)
    atomic_uint_fast32_t parser_error_count; // Frame/parser error totals
    atomic_uint_fast32_t system_error_count; // FLAC/device/sync/system event totals
    atomic_uint_fast32_t error_count_a;     // Per-channel error counts
    atomic_uint_fast32_t error_count_b;
    atomic_uint_fast32_t clip_count_a_pos;  // Positive clipping (sample >= 2047)
    atomic_uint_fast32_t clip_count_a_neg;  // Negative clipping (sample <= -2048)
    atomic_uint_fast32_t clip_count_b_pos;
    atomic_uint_fast32_t clip_count_b_neg;
    atomic_bool stream_synced;
    atomic_uint_fast32_t sample_rate;
    atomic_uint_fast32_t audio_sample_rate;

    // Audio monitoring peaks (24-bit audio magnitude, per channel 1..4)
    atomic_uint_fast32_t audio_peak[4];

    // Buffer manager (centralized ringbuffer management)
    buffer_manager_t buffers;

    // Display thread (decoupled from recording path)
    display_thread_t *display_thread;

    // Backpressure statistics (for debugging buffer contention)
    atomic_uint_fast32_t rb_wait_count;     // Times callback had to wait for buffer space
    atomic_uint_fast32_t rb_drop_count;     // Frames dropped due to full buffer (after timeout)

    // Recording state
    double recording_start_time;
    double last_recording_duration_s;

    // Capture session timing
    double capture_start_time;
    char capture_timestamp[32];                  // yyyy.mm.dd_hh.mm.ss at capture start (empty if not set)
    atomic_uint_fast64_t recording_bytes;        // Total raw bytes recorded
    atomic_uint_fast64_t recording_raw_a;        // Raw input bytes channel A
    atomic_uint_fast64_t recording_raw_b;        // Raw input bytes channel B
    atomic_uint_fast64_t recording_compressed_a; // Compressed output bytes channel A
    atomic_uint_fast64_t recording_compressed_b; // Compressed output bytes channel B

    // GUI settings
    gui_settings_t settings;

    // UI state
    bool settings_panel_open;
    char status_message[256];
    double status_message_time;

    // hsdaoh status message cache (written from hsdaoh thread, applied by UI thread)
    // Support hsdaoh-rp2350 error handling & stats
    atomic_bool hs_msg_pending;
    atomic_int hs_msg_level;
    atomic_flag hs_msg_lock;
    char hs_msg_buf[512];

    // UI-side poll timing (UI thread only)
    uint64_t hs_ui_last_poll_ms;

    // Auto-reconnect state
    bool auto_reconnect_enabled;
    bool reconnect_pending;
    double reconnect_attempt_time;
    int reconnect_attempts;

    // Stop-on-dropout request path (set from capture thread, consumed on main thread)
    atomic_bool dropout_stop_requested;
    atomic_uint_fast32_t dropout_stop_reason;

    // Level autostop state (main thread only): tracks sustained low signal
    float low_signal_time;     // Seconds signal has stayed below the level threshold
    bool low_signal_armed;     // True once a real signal level has been seen above the threshold

    // Device disconnect detection (timestamp of last successful callback)
    atomic_uint_fast64_t last_callback_time_ms;

    // Fonts
    Font *fonts;

    // Per-channel trigger configuration (includes zoom level per channel)
    channel_trigger_t trigger_a;
    channel_trigger_t trigger_b;

    // Digital phosphor - uses shared phosphor_rt module
    phosphor_rt_t *phosphor_a;         // Phosphor render texture for channel A
    phosphor_rt_t *phosphor_b;         // Phosphor render texture for channel B

    // Panel configuration (per-channel, each panel owns its state)
    // FFT state is now owned by panel_config_*.left_state or right_state
    // CVBS decoder state is also owned by panel's left_state or right_state
    // NOTE: Accessed from both UI thread and display thread; protect with panel_config_lock.
    channel_panel_config_t panel_config_a, panel_config_b;

    // Protects panel_config_{a,b} and their *state pointers against concurrent access
    // between UI interactions and the display thread.
    atomic_flag panel_config_lock;

} gui_app_t;

static inline void gui_app_count_parser_errors(gui_app_t *app, uint32_t count) {
    if (!app || count == 0) return;
    atomic_fetch_add(&app->parser_error_count, count);
    atomic_fetch_add(&app->error_count, count);
}

static inline void gui_app_count_system_errors(gui_app_t *app, uint32_t count) {
    if (!app || count == 0) return;
    atomic_fetch_add(&app->system_error_count, count);
    atomic_fetch_add(&app->error_count, count);
}

// Application lifecycle
void gui_app_init(gui_app_t *app);
void gui_app_cleanup(gui_app_t *app);

// Device management
void gui_app_enumerate_devices(gui_app_t *app);
int gui_app_start_capture(gui_app_t *app);
void gui_app_stop_capture(gui_app_t *app);
int gui_app_start_recording(gui_app_t *app);
void gui_app_stop_recording(gui_app_t *app);

// Update functions (called each frame)
void gui_app_update_vu_meters(gui_app_t *app, float dt);
void gui_app_update_display_buffer(gui_app_t *app);

// Clear display (called when device disconnects)
void gui_app_clear_display(gui_app_t *app);

// Status messages
void gui_app_set_status(gui_app_t *app, const char *message);

// Settings persistence
void gui_settings_load(gui_settings_t *settings);
void gui_settings_save(const gui_settings_t *settings);
void gui_settings_init_defaults(gui_settings_t *settings);
const char* gui_settings_get_desktop_path(void);

// Best-effort folder picker for output_path. Returns true if changed.
bool gui_settings_choose_output_folder(gui_settings_t *settings);

// Best-effort file picker for playback_file_{a,b}. channel: 0=A, 1=B.
// Returns true if changed.
bool gui_settings_choose_playback_file(gui_settings_t *settings, int channel);

// Constants for VU meter
#define VU_ATTACK_TIME 0.01f      // 10ms attack
#define VU_RELEASE_TIME 0.3f      // 300ms release
#define PEAK_HOLD_DURATION 2.0f   // 2 second peak hold
#define PEAK_DECAY_RATE 0.5f      // Decay rate after hold

// Note: Color definitions are in gui_ui.h

#endif // GUI_APP_H
