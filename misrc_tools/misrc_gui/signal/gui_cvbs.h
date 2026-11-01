/*
 * MISRC GUI - CVBS Video Decoder Module
 *
 * Decodes composite video (PAL/NTSC) from raw ADC samples.
 * Uses software PLL for H-sync tracking and provides frame buffer display.
 */

#ifndef GUI_CVBS_H
#define GUI_CVBS_H

#include "../core/gui_app.h"
#include "gui_trigger.h"
#include "../visualization/panel_interface.h"
#include "raylib.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

//-----------------------------------------------------------------------------
// Panel Interface Registration
//-----------------------------------------------------------------------------

// Register the CVBS panel vtable with the panel registry.
// Call this once at startup.
void gui_cvbs_panel_register(void);

//-----------------------------------------------------------------------------
// Video Format Constants
//-----------------------------------------------------------------------------

// Video format detection
typedef enum {
    CVBS_FORMAT_UNKNOWN,
    CVBS_FORMAT_PAL,
    CVBS_FORMAT_NTSC,
    CVBS_FORMAT_SECAM
} cvbs_format_t;

// Frame dimensions
#define CVBS_FRAME_WIDTH      720   // Active-area horizontal resolution
#define CVBS_PAL_HEIGHT       576   // 625-line PAL/SECAM active lines
#define CVBS_NTSC_HEIGHT      486   // 525-line NTSC active lines
#define CVBS_PAL_FULL_WIDTH_4FSC   1135  // Full PAL line width at 4fsc
#define CVBS_NTSC_FULL_WIDTH_4FSC   910  // Full NTSC line width at 4fsc
#define CVBS_PAL_FULL_HEIGHT        625  // Full PAL frame lines
#define CVBS_NTSC_FULL_HEIGHT       525  // Full NTSC frame lines
#define CVBS_MAX_WIDTH         CVBS_PAL_FULL_WIDTH_4FSC
#define CVBS_MAX_HEIGHT        CVBS_PAL_FULL_HEIGHT

// Line counts (used for 625-line / 525-line mode naming)
#define CVBS_PAL_TOTAL_LINES  625   // 625-line PAL/SECAM
#define CVBS_NTSC_TOTAL_LINES 525   // 525-line NTSC
#define CVBS_PAL_ACTIVE_LINES 576
#define CVBS_NTSC_ACTIVE_LINES 486

// Timing at 40 MSPS (from gui_trigger.h constants)
#define CVBS_SAMPLES_PER_LINE_PAL   CVBS_PAL_LINE_SAMPLES    // 2560
#define CVBS_SAMPLES_PER_LINE_NTSC  CVBS_NTSC_LINE_SAMPLES   // 2540

//-----------------------------------------------------------------------------
// Decoder State Structures
//-----------------------------------------------------------------------------

// Frame synchronization state
typedef struct {
    cvbs_format_t format;          // Detected video format
    int total_lines;               // Total lines per frame (525/625)
    int active_lines;              // Active video lines (486/576)
    int current_line;              // Current line being decoded (0-based)
    int current_field;             // Current field (0=odd/first, 1=even/second)
    bool in_vsync;                 // Currently in vertical sync region
    bool frame_complete;           // A complete frame is ready for display
    int frames_decoded;            // Total frames decoded
} cvbs_frame_state_t;

// OSD mode controls how much status text is overlaid on the video.
typedef enum {
    CVBS_OSD_OFF     = 0,  // no overlay
    CVBS_OSD_MINIMAL = 1,  // deinterlacer + timing
    CVBS_OSD_STATS   = 2,  // detailed statistics
} cvbs_osd_mode_t;

// Frame view mode controls active-area vs full-frame geometry.
typedef enum {
    CVBS_FRAME_VIEW_ACTIVE = 0,  // Active video only (720x576 / 720x486)
    CVBS_FRAME_VIEW_FULL   = 1,  // Full line/frame geometry (4fsc width, 625/525 lines)
} cvbs_frame_view_t;

// Video level mode controls whether black/white/sync levels keep adapting
// each field (AUTO) or are held fixed after lock (FIXED).
typedef enum {
    CVBS_LEVEL_MODE_AUTO  = 0,
    CVBS_LEVEL_MODE_FIXED = 1,
} cvbs_level_mode_t;
// Sync mode controls whether H/V lock timing keeps adapting (AUTO)
// or uses a fixed lock profile once stable (FIXED).
typedef enum {
    CVBS_SYNC_MODE_AUTO  = 0,
    CVBS_SYNC_MODE_FIXED = 1,
} cvbs_sync_mode_t;

// Decoder family used by preview path.
// CVBS preserves existing behavior; TAPE enables tape-oriented preview tuning.
typedef enum {
    CVBS_DECODER_MODE_CVBS = 0,
    CVBS_DECODER_MODE_TAPE = 1,
} cvbs_decoder_mode_t;

// Tape family selector (aligned with tape-decode-rs family naming where practical).
typedef enum {
    CVBS_TAPE_FORMAT_VHS = 0,
    CVBS_TAPE_FORMAT_SVHS,
    CVBS_TAPE_FORMAT_BETAMAX,
    CVBS_TAPE_FORMAT_VIDEO8,
    CVBS_TAPE_FORMAT_HI8,
    CVBS_TAPE_FORMAT_UMATIC,
} cvbs_tape_format_t;

// Tape speed mode selector.
typedef enum {
    CVBS_TAPE_MODE_SP = 0,
    CVBS_TAPE_MODE_LP,
    CVBS_TAPE_MODE_ELP,
} cvbs_tape_mode_t;

// Software PLL state for H-sync tracking
typedef struct {
    // Core PLL state
    double phase;                  // Current phase within line (0 to line_period)
    double line_period;            // Nominal line period in samples (2560 PAL, 2540 NTSC)
    double freq_adjust;            // Fine frequency adjustment (±small value)

    // Phase correction
    double phase_error;            // Last phase error (for derivative term)
    double phase_integral;         // Integrated phase error (for I term)

    // Lock detection
    int good_sync_count;           // Consecutive syncs within tolerance
    int bad_sync_count;            // Consecutive syncs out of tolerance
    bool locked;                   // True if PLL is locked
    int lines_since_lock;          // Good syncs since the last lock (rate
                                   // refinement settle gate)

    // Line tracking
    int current_line;              // Current line number in field (0-312 for PAL)
    int samples_in_line;           // Samples processed in current line
    size_t total_samples;          // Total samples processed (for debug)
} cvbs_pll_state_t;

// Colour decode mode: Auto decodes burst-gated baseband chroma; Off keeps
// the legacy grayscale output exactly.
typedef enum {
    CVBS_COLOUR_OFF = 0,
    CVBS_COLOUR_AUTO = 1,
} cvbs_colour_mode_t;

// SECAM FM-chroma decimated-line capacity: a full 4fsc line (1135) decimated
// by 4 plus the FIR settle margin.
#define CVBS_SECAM_MAX_DEC 304

// SECAM FM-chroma state (the tbc-tools secamdecoder pipeline ported to the
// streaming real-time decode; see the SECAM section in gui_cvbs.c and the
// reference tbc-tools/src/tbc-chroma-decoder/secamdecoder.cpp). Field-level
// calibration locks ONE FIELD BEHIND (the reference's last-good-reuse
// semantics adapted to a live stream): the previous field's porch
// measurements fix the carrier pair / block offset / line identification
// that demodulates the current field, while each line's own porch median
// still tracks wow/flutter per line (the reference's smoothed rowZero).
typedef struct {
    // Per-field accumulators (reset at each field boundary)
    double porch_freq[320];      // Median discriminated porch Hz per field line
    double porch_env[320];       // Mean analytic envelope per field line
    bool   row_valid[320];       // Porch envelope over the validity gate
    int    rows;                 // Field lines recorded
    double centre_env_median;    // Median active-envelope (killer reference)
    double centre_freq[320];     // Mean discriminated centre frequency (ident)
    double centre_env[320];      // Mean centre envelope (ident gates)

    // Locked for the current field (committed at field start)
    double fob, for_;            // Locked (or nominal) rest carriers
    double carrier_offset;       // Hz from the nominal block centre
    bool   calibrated;           // The locked pair is a measurement
    bool   dr_on_even;           // D'R on even field lines (identification)
    double ident_confidence;     // The winning vote's majority share
    int    ident_source;         // 0 porch, 1 content median, 2 bottles
    double noise_mad_hz;         // Porch MAD (concealment overshoot term)

    // Last-good reuse (a field whose pair fails the gates keeps the last)
    bool   have_last_good;
    double last_good_fob, last_good_for;

    // Per-line streaming rings: the CONCEALED DEMOD of the previous
    // same-component line (deviation units - the click-concealment row-2
    // borrow domain; the one-line-hold finals live file-scope in the .c).
    float  prev_demod[2][CVBS_SECAM_MAX_DEC]; // [0] last D'B, [1] last D'R
    int    prev_dec_count[2];    // Decimated samples held per ring row

    // Filter cache: rebuilt when the locked carrier offset moves
    bool   filters_ready;
    double filter_offset;        // The offset the coefficients were built for

    // Diagnostics (SECAMDIAG-equivalent, surfaced via get_secam_diag)
    int    fields_calibrated;
    int    fields_reused;
    int    fields_nominal;
} cvbs_secam_state_t;

// VBI / field sync state (persistent across buffers). Ported from the
// verified VHS FM decoder model (gui_rf_preview.c): pulses are classified
// by WIDTH (EQ 2.35us / H 4.7us / broad ~27.3us), field parity comes from the
// first-EQ-to-last-H phase (half-line vs integer), the field boundary is
// placed exactly (first_eq - boundary*line), and the next train's cadence
// is validated against 312.5/262.5 lines per field.
typedef struct {
    int last_kind;                 // 0 none, 1 EQ, 2 H, 3 broad
    size_t last_pulse_pos;         // Start position of the last classified pulse
    // Pulse classification state (per low pulse, driven on its rising edge)
    bool in_pulse;                 // Currently inside a low pulse
    size_t pulse_start;            // Sample position where the pulse began
    double pulse_start_sub;        // Sub-sample pulse-start position (linear
                                   // threshold-crossing interpolation): the
                                   // integer crossing quantization is
                                   // +-0.5 samples per pulse and every sync
                                   // edge feeds the PLL snap, so without it
                                   // the converged line period carries the
                                   // quantization bias
    size_t last_h_pos;             // Start of the last classified H pulse
    size_t last_classified_pos;    // Start of the last EQ-or-H classified pulse
                                   // (rate self-calibration spacing source; any
                                   // classified pulse counts because a wrong
                                   // declared rate reclassifies H as EQ)
    // Pre-EQ train tracking
    int eq_run;                     // Consecutive EQ pulses in the current train
    int good_h;                     // H pulses seen since the last EQ/broad pulse
    int v_run;                      // Consecutive broad pulses in the confirmed VBI train
    size_t first_eq_pos;            // Start of the first EQ in the current candidate train
    // Candidate / committed field geometry (global sample positions)
    bool have_field;                // A field start has been committed
    size_t field_start;             // Committed field start position
    int field_parity;               // 0 = first/odd field, 1 = second/even field
    // Diagnostics (surfaced in the OSD stats)
    int vsync_candidates;           // EQ trains observed
    int fields_accepted;           // Field boundaries committed
    int fields_rejected;            // Rejected boundary candidates
    int reject_cadence;             // Rejected for cadence outside profile
    int reject_parity;              // Rejected for parity phase/cadence errors
    char last_reject_reason[80];    // Last rejection reason
} cvbs_vsync_state_t;

// Histogram-based level detection buffer size
#define CVBS_LEVEL_SAMPLE_BUFFER_SIZE  16384  // ~0.4ms of samples at 40 MSPS

// Adaptive threshold state (histogram-based)
typedef struct {
    int16_t sync_tip;              // Estimated sync tip level (lowest ~5%)
    int16_t blanking;              // Estimated blanking level (~30%)
    int16_t black;                 // Estimated black level
    int16_t white;                 // Estimated white level (highest ~95%)
    int16_t threshold;             // Current sync threshold
    // Per-field porch (back porch, true 0 IRE blanking) accumulation: the
    // porch is flat, chroma-free and ramp-free, so its mean is the most
    // robust blanking estimate on colour signals (the histogram's
    // second-lowest peak runs below true blank on saturated chroma).
    // Gated on PLL lock so misaligned acquisition lines never pollute it.
    int64_t porch_sum;             // Per-field porch sample sum
    int porch_count;               // Per-field porch sample count
    int64_t porch_sum_clean;       // Same, lines within ~4 IRE of the committed
    int porch_count_clean;         // blanking (excludes a post-VBI baseline sag)
    int16_t porch_level;           // Committed porch estimate (EMA-smoothed)
    bool porch_valid;              // At least one field committed porch data
    // Per-field FILTERED (luma-domain) maximum: the sync lowpass removes the
    // chroma, so this tracks the true luma peak (the white-bar plateau),
    // immune to the raw max's chroma/noise overshoot.
    int16_t luma_max;              // Per-field filtered-domain maximum
    bool luma_max_valid;           // Filtered max seen this field
    // Per-field sample buffer for histogram analysis
    int16_t *level_sample_buf;     // Buffer for histogram samples (allocated)
    size_t level_sample_count;     // Current count of samples in buffer
    int subsample_counter;         // Counter for subsampling
} cvbs_adaptive_levels_t;

// Line buffer size - stores one complete line of samples for decoding
// We need this because H-sync edges may not align with buffer boundaries.
// Sized for a full PAL line at the highest supported rate (200 MSPS = 12800
// samples) so rate awareness never truncates the line.
#define CVBS_LINE_BUFFER_SIZE   13100

// Main decoder structure
typedef struct cvbs_decoder {
    // Track whether we've received each field for the current frame
    bool field_ready[2];
    int field_line_count[2];         // Valid decoded row count in each field buffer
    bool last_weave;                 // Last deinterlace pass wove both fields (OSD)
    uint64_t field_sequence;          // Monotonic sequence number for completed fields
    uint64_t field_last_update_seq[2];// Last sequence value written to each field buffer
    // Frame state
    cvbs_frame_state_t state;

    // Decoder configuration
    cvbs_decoder_mode_t decoder_mode;   // CVBS vs Tape decoder path
    cvbs_tape_format_t tape_format;     // Tape family preset
    cvbs_tape_mode_t tape_mode;         // SP/LP/ELP
    cvbs_osd_mode_t osd_mode;           // Off / Minimal / Stats
    cvbs_frame_view_t frame_view_mode;  // Active-only vs full-frame view
    cvbs_level_mode_t level_mode;       // Auto-adaptive vs fixed video levels
    cvbs_sync_mode_t sync_mode;         // Auto-adaptive vs fixed H/V sync behavior
    bool fixed_levels_valid;            // Fixed mode has latched levels from signal
    int16_t fixed_black_level;          // Latched black level used in fixed mode
    int16_t fixed_white_level;          // Latched white level used in fixed mode
    bool fixed_sync_locked;             // FIXED mode has acquired lock and frozen line period
    double fixed_line_period;           // Frozen line period used while fixed sync lock is active
    double fixed_period_ema;            // Slow EMA of good-sync phase error = the frozen
                                       // period's sub-sample residual (rate refinement)
    int fixed_bad_syncs;                // Consecutive bad H-syncs while fixed lock is active
    int16_t fixed_sync_threshold;      // Latched sync threshold used while fixed lock is active

    // Field buffers (grayscale, 720 x field_height each)
    // Each field buffer stores half the vertical resolution
    uint8_t *field_buffer[2];      // [0]=even/first field, [1]=odd/second field
    int field_height;              // Height of each field (288 PAL, 243 NTSC)

    // Deinterlaced frame buffer (720 x full_height)
    uint8_t *frame_buffer;         // Deinterlaced output frame
    int frame_width;               // Active: 720, Full: 910/1135 (NTSC/PAL 4fsc)
    int frame_height;              // Active: 486/576, Full: 525/625

    // Double buffering for thread-safe display
    // Display thread writes to back buffer, render thread reads from front
    uint8_t *display_front;        // Front buffer - read by render thread
    uint8_t *display_back;         // Back buffer - written by display thread
    atomic_int display_ready;      // 1 when back buffer has new frame to swap

    /* Optional borrowed RGB24 frame callback; set only while decode is stopped. */
    void (*monitor_frame_sink)(void *context, const uint8_t *rgb,
                               unsigned width, unsigned height, uint64_t timestamp_us);
    void *monitor_frame_sink_context;

    // GPU resources for video display
    Image frame_image;
    Texture2D frame_texture;
    bool texture_valid;

    // Line buffer for cross-boundary line assembly
    int16_t *line_buffer;          // Buffer for current line samples
    int line_buffer_count;         // Samples currently in line buffer

    // Live capture rate (Hz). 0 = not yet known: all timing falls back to the
    // 40 MSPS legacy default until the display thread reports a real rate.
    uint32_t sample_rate_hz;

    // Self-calibrated capture rate (Hz), measured from the H-to-H sync
    // spacing (median-filtered) and refined by the locked PLL's converged
    // line period (AUTO mode: the converged line_period+freq_adjust; FIXED
    // lock: the frozen period plus the good-sync phase-error EMA). The
    // median alone is edge-quantized (real-capture jitter biased it 39.98
    // MHz against a true 40.000 - enough to rotate late-line hues ~10 deg),
    // so the refinement is what makes the hue decode exact. The declared
    // rate above is only a bootstrap: input paths
    // have been measured feeding a 20 MSPS stream while the app-level rate
    // stays at the legacy 40 MSPS default (live capture AND playback) - a
    // wrong declared rate leaves the geometry scale-invariant (every
    // window derives through the same rate) so the picture still looks
    // structurally perfect, but the subcarrier demod's carrier table runs
    // at the wrong frequency and every hue decodes wrong. The measured rate
    // overrides the declared value for every window, the PLL nominal and
    // the demod carrier as soon as it is established.
    uint32_t measured_rate_hz;
    bool measured_rate_valid;
    bool rate_trusted;            // The DECLARED rate is exact by construction
                                   // (the panel plugin's fixed-rate soxr
                                   // resampler output): the rate self-
                                   // calibration (median + refinement) is
                                   // disabled - its own error would override
                                   // an exact rate
    double hh_ring[64];            // Recent classified-pulse spacings (samples)
    int hh_ring_count;
    double hh_period_samples;      // Median spacing = the line period estimate
    int hh_reject_streak;          // Consecutive band rejections (rate-change escape)

    // Colour decode control + per-line chroma reference state. The burst
    // vector is measured per decoded line in the line-local carrier frame;
    // prev_burst_phase carries the PAL V-switch alternation between lines.
    cvbs_colour_mode_t colour_mode; // Auto: burst-gated baseband chroma
    double prev_burst_phase;         // Previous line's burst phase (PAL V-switch)
    int prev_burst_valid;           // Previous line had a usable burst

    // SECAM FM-chroma state (active when the format is CVBS_FORMAT_SECAM and
    // colour_mode is AUTO; see cvbs_secam_state_t above)
    cvbs_secam_state_t secam;

    // Software PLL and sync detection (persistent across buffers)
    cvbs_adaptive_levels_t adaptive;   // Adaptive threshold tracking
    cvbs_pll_state_t pll;              // Software PLL for H-sync
    cvbs_vsync_state_t vsync;          // V-sync detection state

    // Lowpass filter state for sync detection (fixed-point Q16)
    int32_t lpf_state;             // IIR lowpass filter accumulator (Q16 fixed-point)

    // Edge detection state (persistent across buffers)
    bool last_filtered_above;      // Was last filtered sample above threshold?
    size_t global_sample_pos;      // Global sample position counter

    // H-sync pulse tracking
    bool in_hsync_pulse;           // Currently inside a potential H-sync pulse
    size_t hsync_pulse_start;      // Sample position where H-sync pulse started

    // V-sync detector state (separate from H-sync)
    size_t vsync_last_edge_pos;    // Position of last falling edge for V-sync

    // Sync tracking state
    cvbs_levels_t levels;          // Current signal levels
    int lines_since_vsync;         // Lines counted since last V-sync detection

    // Per-line DC restoration (the back-porch clamp a hardware sync
    // separator performs). dc_offset = this line's measured back-porch
    // level minus the field-committed blanking (adaptive.porch_level), in
    // raw counts; it shifts the sync slicing threshold for the following
    // edges and the line's own luma black. Measured on the EBU HackDAC
    // capture: the signal baseline sags >20 IRE right after the vertical
    // broad pulses (AC coupling) and recovers over ~30 lines - with a fixed
    // threshold the H syncs of field lines 8-31 were never seen (blanking
    // below the threshold) and lines 32-55 sliced 11..1 samples late, each
    // decoded line's origin following that error (the top-of-picture
    // horizontal shift decaying over ~20 rows).
    double dc_offset;
    bool dc_offset_valid;

    // Statistics
    int sync_errors;               // Count of sync detection failures
    int format_changes;            // Count of format auto-detections
    int stream_jumps;              // Locked H syncs > 0.3 line off prediction =
                                   // sample-stream discontinuities (feed drops)

    // Debug statistics
    struct {
        int fields_decoded;            // Total fields successfully decoded
        int vsync_found;               // V-sync detection count
        int hsyncs_last_field;         // H-syncs found in last field
        int last_half_line_count;      // Half-line count from last V-sync
        int log_counter;               // Per-instance debug log counter
        bool mode_trace_initialized;   // CVBS mode transition trace initialized
        int last_decoder_mode;         // Last observed cvbs_decoder_mode_t value
        int last_tape_format;          // Last observed cvbs_tape_format_t value
        int last_tape_mode;            // Last observed cvbs_tape_mode_t value
        int last_level_mode;           // Last observed cvbs_level_mode_t value
        int last_sync_mode;            // Last observed cvbs_sync_mode_t value
        bool last_fixed_sync_locked;   // Last observed fixed-sync lock state
        bool last_fixed_levels_valid;  // Last observed fixed-level latch state
    } debug;

    // UI overlay state (for panel-top controls)
    struct {
        // Buttons (right-aligned vertical): [Decoder], [OSD], [Frame], [Levels], [Sync], [System]
        Rectangle decoder_btn_rect;    // Hit box for decoder mode button
        Rectangle osd_btn_rect;        // Hit box for OSD mode button
        Rectangle frame_btn_rect;      // Hit box for frame mode button
        Rectangle level_btn_rect;      // Hit box for levels mode button
        Rectangle sync_btn_rect;       // Hit box for sync mode button
        Rectangle colour_btn_rect;    // Hit box for colour mode button
        Rectangle system_btn_rect;     // Hit box for system selector button
        Rectangle tape_btn_rect;       // Hit box for tape family selector button
        Rectangle tape_format_btn_rect;// Hit box for tape format selector button
        Rectangle tape_mode_btn_rect;  // Hit box for tape mode cycle button

        // Dropdown options for system selector (0=PAL, 1=NTSC, 2=SECAM)
        Rectangle system_options_rect[3];
        // Dropdown options for tape family selector.
        Rectangle tape_options_rect[6];
        // Dropdown options for tape format selector (PAL/NTSC/SECAM).
        Rectangle tape_format_options_rect[3];

        bool is_visible;               // Whether overlay was rendered (for click detection)
        bool system_dropdown_open;     // Whether system dropdown is currently open
        bool tape_dropdown_open;       // Whether tape family dropdown is currently open
        bool tape_format_dropdown_open;// Whether tape format dropdown is currently open
        int selected_system;           // 0=PAL, 1=NTSC, 2=SECAM (the CVBS-mode
                                       // System selector's pick; the button
                                       // label itself tracks the decoder's
                                       // ACTUAL format)
        int selected_tape_format;      // 0=PAL, 1=NTSC, 2=SECAM
    } overlay;
} cvbs_decoder_t;

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

// Initialize decoder with maximum dimensions
// Returns true on success, false on allocation failure
bool gui_cvbs_init(cvbs_decoder_t *decoder);

/* Callback executes synchronously on decoder worker; must not block.
 * Set/unset only before/after decoding, never concurrently with processing.
 * Pixel pointer is borrowed for the duration of the callback only. */
void gui_cvbs_set_monitor_frame_sink(cvbs_decoder_t *decoder,
    void (*callback)(void *, const uint8_t *, unsigned, unsigned, uint64_t),
    void *context);

// Cleanup decoder resources
void gui_cvbs_cleanup(cvbs_decoder_t *decoder);

// Reset decoder state (clear frame, reset sync)
void gui_cvbs_reset(cvbs_decoder_t *decoder);

//-----------------------------------------------------------------------------
// Decoding
//-----------------------------------------------------------------------------

// Process a buffer of raw ADC samples at the LIVE capture rate
// (0/invalid falls back to the last known / 40 MSPS default). Every
// profile timing window derives from the rate.
void gui_cvbs_process_buffer(cvbs_decoder_t *decoder,
                              const int16_t *buf, size_t count,
                              uint32_t sample_rate);

// Cumulative throughput profile (microseconds since process start): the
// panel resampler, the per-sample process_buffer pass, the per-line pixel
// decode inside it, and the number of decoded lines. Headless harness
// readout for the real-time budget split; any pointer may be NULL.
void gui_cvbs_profile_get(uint64_t *resample_us, uint64_t *process_us,
                          uint64_t *lines_us, uint64_t *lines);

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

// Swap buffers if new frame available (called from render thread before rendering)
// Copies back buffer to front buffer if display_ready flag is set.
void gui_cvbs_swap_buffers(cvbs_decoder_t *decoder);

// Render the decoded video frame
// Scales to fit within the given rectangle while maintaining aspect ratio
// Call gui_cvbs_swap_buffers() before this to get latest frame.
void gui_cvbs_render_frame(cvbs_decoder_t *decoder,
                            float x, float y, float width, float height);

//-----------------------------------------------------------------------------
// Configuration
//-----------------------------------------------------------------------------

// Set video format manually (PAL/NTSC)
// Uses cvbs_format_select_t from gui_app.h (CVBS_SELECT_PAL/CVBS_SELECT_NTSC)
void gui_cvbs_set_format(cvbs_decoder_t *decoder, int format_select);

// Startup default for the System selector (0=PAL, 1=NTSC, 2=SECAM), pushed
// once from the persisted app settings after gui_settings_load; every
// CVBS panel decoder is created in this system. The System dropdown's
// click handler persists the user's pick back into the settings.
void gui_cvbs_set_default_system(int system_select);

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

// Get detected format
cvbs_format_t gui_cvbs_get_format(cvbs_decoder_t *decoder);

// Get format name string
const char *gui_cvbs_get_format_name(cvbs_decoder_t *decoder);

// SECAM diagnostics for the OSD/harness: the locked carrier pair, the block
// offset, the identification (D'R-on-even, confidence, source 0/1/2), the
// porch noise MAD, and the cal/reuse/nominal field counters. All zeros
// until the first field commits.
void gui_cvbs_get_secam_diag(cvbs_decoder_t *decoder,
                             double *fob_hz, double *for_hz,
                             double *carrier_offset_hz, int *dr_on_even,
                             double *ident_confidence, int *ident_source,
                             double *noise_mad_hz,
                             int *fields_calibrated, int *fields_reused,
                             int *fields_nominal);

// UI publishes active routes once per frame. Extraction does not use the display DSP lock.
void gui_video_update_rf_routes(gui_app_t *app);
// Continuous RF input from extraction; samples are copied into bounded workers.
#include "../processing/gui_rf_preview_worker.h"
void gui_video_submit_rf(gui_app_t *app, const int16_t *samples_a, const int16_t *samples_b,
                          size_t count_a, size_t count_b, const gui_rf_block_info_t info[2],
                          bool input_supported);
#endif // GUI_CVBS_H
