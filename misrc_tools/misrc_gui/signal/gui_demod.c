/*
 * MISRC GUI - Demodulator signal module + Demod panel implementation
 *
 * Device-agnostic. The panel pulls complex I/Q (channel A = I, channel B = Q) from
 * the display thread's latest frame inside render(), runs the selected demod
 * chain, resamples to a fixed 48 k via libsoxr, applies squelch + volume, and
 * pushes stereo int16 audio to the monitor path (gui_audio_push_demod) so the
 * "Audio Mon" button plays it. A scrolling waveform of the demod audio is
 * drawn, plus a mode dropdown (WFM/NFM/AM/USB/LSB) via the panel menu system.
 *
 * Demod runs in render() (not process()) because the panel vtable process() only
 * receives one channel, while FM/SSB need both I and Q. render() has the app
 * pointer and can read both channels from app->display_thread->samples. Demod
 * only processes a frame when the display thread has produced a new one (tracked
 * via the frame counter), so it neither reprocesses nor starves.
 */

#include "gui_demod.h"
#include "../core/gui_app.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include "../visualization/gui_text.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_dropdown.h"  // gui_dropdown_option_color()
#include "../ui/gui_chrome_hide.h"  // double-click overlay toggle (shared helper)
#include "../input/gui_capture.h"  // gui_app_set_sdr_frequency() (generic SDR retune)
#include "../input/gui_playback.h"  // file sample rate (player mode)
#include "../visualization/gui_fft.h"   // complex FFT (I/Q spectrum)
#include "../visualization/gui_spectrogram.h"   // shared waterfall palette
#include "gui_cvbs.h"   // gui_video_set_radio_tap (continuous capture feed)
#include "../../common/threading.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdatomic.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEMOD_AUDIO_RATE_HZ   48000
#define DEMOD_WAVE_LEN        1024   // displayed waveform history (audio samples)
// WFM de-emphasis: 50 / 75 us by Region (demod_deemph_tau_s).

// SDR-style tuning view (harrypm 2026-11-27: "frequency target mode line ...
// sort of like a real SDR i.e. SDR++"): I/Q spectrum + waterfall (the
// Spectro palette), a tuning line with its channel bandwidth; click / drag /
// wheel tunes. The demod mixes the tuned channel to DC, filters it to the
// bandwidth and decimates before demodulating.
#define DEMOD_FFT_N          8192    // I/Q spectrum points (two-sided; fine enough to zoom)
#define DEMOD_ZOOM_MAX       32      // spectrum zoom 1x .. 32x
#define DEMOD_SWEEP_N        4096    // sweep spectrum points across the swept range
#define DEMOD_WF_W           1024    // waterfall texture columns
#define DEMOD_WF_H           256     // waterfall history rows
#define DEMOD_FIR_TAPS       31      // channel filter (Hamming windowed sinc)
#define DEMOD_SPEC_PERIOD_S  0.033   // spectrum / waterfall update period
#define DEMOD_SPEC_SPAN_DB   80.0f   // displayed dynamic range
#define DEMOD_FFTW_ESTIMATE  (1U << 6)   // FFTW_ESTIMATE

// Frequency text field mirrors settings.rtlsdr_freq_hz (the active SDR's center freq).
// Synced in render: format Hz->str when not editing, parse str->Hz + live-retune
// via gui_app_set_sdr_frequency() when editing.
static char s_demod_freq_str[32] = {0};

// SDR tuning limits (Hz): RTL-SDR Blog V4 HF upconverter low end .. R820T /
// R828D top. A typed value outside is refused with a status message.
#define DEMOD_SDR_MIN_HZ 500000ULL
#define DEMOD_SDR_MAX_HZ 1766000000ULL

// Band presets for the SDR frequency box (harrypm 2026-11-27: "fix the
// frequency box to have ranges"): the SDR centre goes 300 kHz below the
// band's centre and the tuning line onto it (clear of the RTL DC spike).
// Each band is its range (harrypm 2026-11-27: "FIRST BOX is Band and its a
// range limited" / "show the whole band at once"): picking one wider than
// the SDR's live view shows the whole lo..hi by hopping (the sweep engine);
// a click on a station tunes it and plays. Narrow bands just tune.
typedef struct { const char *label; double centre_hz; int mode; double lo, hi; } demod_band_t;
static const demod_band_t s_demod_bands[] = {
    { "FM 87.5-108M",     98.0e6,  0,  87.5e6, 108.0e6 },   // WFM
    { "Airband 118-137M", 125.0e6, 2, 118.0e6, 137.0e6 },   // AM
    { "2m 144-146M",      145.0e6, 1, 144.0e6, 146.0e6 },   // NFM
    { "Marine 156-162M",  156.8e6, 1, 156.0e6, 162.0e6 },
    { "70cm 430-440M",    433.5e6, 1, 430.0e6, 440.0e6 },
    { "PMR446 446.0-446.2M", 446.1e6, 1, 446.0e6, 446.2e6 },
};
#define DEMOD_BAND_COUNT ((int)(sizeof(s_demod_bands) / sizeof(s_demod_bands[0])))

// SDR sample rates (the Settings SDR presets): the spectrum span.
// 2.56 / 2.88 / 3.2 MSPS (harrypm 2026-11-27: "bandwidth should be much
// higher"): the RTL2832U ceiling; above 2.56 many dongles / USB hosts drop
// samples (status warning; watch Missed).
static const uint32_t s_demod_rates[] = { 250000, 1024000, 1200000, 1536000, 2000000, 2048000, 2400000,
                                          2560000, 2880000, 3200000 };
static const char *const s_demod_rate_labels[] = { "0.25M", "1.024M", "1.2M", "1.536M", "2.0M", "2.048M", "2.4M",
                                                   "2.56M", "2.88M !", "3.2M !" };
#define DEMOD_RATE_COUNT ((int)(sizeof(s_demod_rates) / sizeof(s_demod_rates[0])))

typedef struct { const char *label; double lo, hi; } demod_sweep_t;
static const demod_sweep_t s_demod_sweeps[] = {
    { "Off", 0.0, 0.0 },
    { "80-100M", 80.0e6, 100.0e6 },
    { "88-108M", 87.5e6, 108.0e6 },
    { "Air 118-137M", 118.0e6, 137.0e6 },
};
#define DEMOD_SWEEP_COUNT ((int)(sizeof(s_demod_sweeps) / sizeof(s_demod_sweeps[0])))

static void demod_freq_text(char *buf, size_t size, uint64_t hz) {
    snprintf(buf, size, "%.3f", (double)hz / 1e6);   // MHz
}

// Bandwidth presets offered by the overlay dropdown (demod_bandwidth_hz).
// 0 = Auto (pick per mode default). The rest are manual overrides in Hz.
// Per mode (harrypm 2026-11-27: "fix the bandwidth mode"): the old single
// list offered 200k to SSB / AM and a manual pick stuck across mode changes;
// now each mode lists its own widths and a mode change resets BW to Auto.
// Wider choices (harrypm 2026-11-27: "bandwidth should be much higher"): a
// stereo + RDS FM station is ~256-270 kHz wide (Carson: 2 x (75 + 53..60) kHz).
#define DEMOD_BW_OPTION_COUNT 7
static const int s_bw_values[5][DEMOD_BW_OPTION_COUNT] = {
    { 0, 500000, 400000, 300000, 250000, 200000, 150000 },   // WFM
    { 0, 50000, 25000, 12500, 10000, 8330, 6250 },           // NFM
    { 0, 20000, 15000, 10000, 9000, 6000, 3000 },            // AM
    { 0, 4000, 3600, 3000, 2700, 2400, 1800 },               // USB
    { 0, 4000, 3600, 3000, 2700, 2400, 1800 },               // LSB
};

// Squelch / volume step
#define DEMOD_SQL_STEP  0.1f
#define DEMOD_VOL_STEP  0.1f
#define DEMOD_SQL_MAX  1.0f
#define DEMOD_VOL_MAX  2.0f

#if LIBSOXR_ENABLED
#include <soxr.h>
#endif

//-----------------------------------------------------------------------------
// Demod panel state
//-----------------------------------------------------------------------------

typedef struct {
    demod_mode_t mode;                 // current demod mode

    // DSP state (continuous across frames)
    float prev_i;                      // previous I (for discriminator)
    float prev_q;                      // previous Q
    float deemph_state;                 // 75us de-emphasis one-pole state (WFM)
    float dc_state;                     // DC blocker state (AM)
    float agc_peak;                     // AM AGC running peak
    int   sb_phase;                     // sideband fs/4 oscillator phase index (USB/LSB)

    // soxr: capture rate -> 48k, int16 mono -> int16 mono.
    // The scratch buffers are always present so the Demod panel builds and runs
    // even when libsoxr is unavailable (no-resample pass-through at capture rate).
    void  *soxr;                      // soxr_t (only used when LIBSOXR_ENABLED)
    uint32_t soxr_in_rate;             // configured input rate (0 = not created)
    int16_t *soxr_in_buf;             // input scratch (capture-rate mono samples)
    size_t  soxr_in_cap;
    int16_t *soxr_out_buf;            // output scratch (48k mono samples)
    size_t  soxr_out_cap;

    int16_t *audio_out;               // 48k stereo buffer pushed to monitor
    size_t  audio_out_cap;

    int16_t  wave[DEMOD_WAVE_LEN];   // scrolling audio waveform history
    size_t  wave_pos;                 // ring write position

    uint64_t last_consumed_frame;     // last display frame counter we demod'd

    // Tuning + channel chain (input rate -> NCO -> integrate & dump by
    // dec_D -> FIR (bandwidth) by fir_dec -> demod at fs_out).
    double vfo_hz;                    // tuning line: offset from the I/Q centre (Hz)
    double chan_fs, chan_mix;         // configured input rate / mix frequency
    int chan_bw, chan_mode, chan_region;
    bool chan_mix_valid;
    float nco_c, nco_s, nco_dc, nco_ds;
    int nco_renorm;
    int dec_D, dec_n;
    float acc_i, acc_q;
    float fir_taps[DEMOD_FIR_TAPS];
    float fir_hi[DEMOD_FIR_TAPS], fir_hq[DEMOD_FIR_TAPS];
    int fir_pos, fir_dec, fir_phase;
    double fs_out;
    float fm_gain, deemph_a, dc_a, agc_release;
    float post_c, post_s, post_dc, post_ds;   // SSB: shift the sideband back to audio
    // Squelch on the channel's power (harrypm 2026-11-27: the old per-sample
    // audio-level gate chopped every waveform near zero = heavy distortion):
    // ~20 ms power average, 3 dB hysteresis, 5 ms fade.
    float sq_pow, sq_a, sq_gain, sq_ramp;
    bool sq_open;
    atomic_int sq_db10;               // channel power for the SQL readout (0.1 dBFS)

    // Spectrum + waterfall (render thread)
    void *fft_plan;
    float *fft_in, *fft_out;          // interleaved complex, DEMOD_FFT_N points
    float fft_win[DEMOD_FFT_N];
    float spec_db[DEMOD_FFT_N];       // smoothed, fftshifted (0 = -fs/2)
    bool spec_valid;
    double spec_time;
    float floor_db;
    unsigned char lut[256][4];
    unsigned char *wf_pixels;         // DEMOD_WF_W x DEMOD_WF_H RGBA ring
    int wf_head;                      // newest row
    bool wf_row_dirty, wf_full_upload;
    Texture2D wf_tex;
    bool wf_tex_valid;
    // View mapping cached by render for clicks / wheel
    Rectangle tune_rect;
    double view_fs, view_center;
    int view_mode, view_region;
    bool tune_drag;
    double view_lo, view_hi;          // shown range: I/Q offsets +-fs/2; real: RF of the zone
    // Real input, undersampling (harrypm 2026-11-27: the MISRC filter is wide
    // open, FM should be a software-only mode): RF zone z covers
    // z*fs/2 .. (z+1)*fs/2 and folds onto 0..fs/2 (odd zones mirrored).
    bool view_real;
    // Zoom (harrypm 2026-11-27: "range selection is not fine enough"):
    // the shown span is the full band / zoom, kept around the tuning line.
    double full_lo, full_hi;          // the whole receivable band (I/Q or zone)
    int zoom;                         // 1, 2, 4 .. DEMOD_ZOOM_MAX
    double view_mid;
    bool view_mid_valid;
    int zone;                         // 0 = direct (0..fs/2)
    double tune_bb;                   // tuning line folded to the sampled band (Hz)
    bool tune_inv;                    // mirrored zone: sidebands swap

    // Input (harrypm 2026-11-27: "it should work in both modes"): 0 Auto
    // (SDR -> I/Q, else this channel real), 1 I/Q (A = I, B = Q), 2 Real.
    int input_sel;
    bool zone_launch_applied;
    atomic_int feed_input;            // resolved: 0 I/Q, 1 real CH A, 2 real CH B
    atomic_uint_fast64_t feed_blocks; // chunks the feed worker demodulated
    uint64_t seen_feed_blocks;
    double last_render;               // feed / audio ownership follows the drawn panel
    // Sweep (harrypm 2026-11-27: "wider view to see 80-100 MHz"): retune the
    // SDR across lo..hi in steps of 80 % of the rate (no save), average a few
    // FFTs per step, stitch the clean middle of each into one spectrum; one
    // waterfall row per sweep. Audio is muted while sweeping.
    bool sweep_on;
    double sweep_lo, sweep_hi, sweep_centre;
    int sweep_step, sweep_steps, sweep_wait, sweep_avg_n;
    float *sweep_db;                  // DEMOD_SWEEP_N, dBFS (NAN = not swept yet)
    float *sweep_acc;                 // DEMOD_FFT_N linear power of this step
    bool sweep_wf_clear, sweep_launch_done;
    // Band listening (harrypm 2026-11-27: "clean linear left/right scrubbing
    // across the band range"): the whole band stays on screen, the tuning
    // line follows the mouse linearly across it with audio, the SDR retunes
    // only when the line leaves its live slice; that slice updates live.
    bool sweep_listen;
    double sweep_line_hz, scrub_centre, scrub_retune_t, sweep_row_t;
    // Pan (harrypm 2026-11-27: "a scrollable range like SDR++"): drag the band
    // sideways; the SDR retunes live (unsaved, <= ~30 / s), saved on release;
    // the tuning line keeps its absolute frequency; the waterfall shifts with it.
    bool pan_drag;
    int pan_button;                   // MOUSE_BUTTON_LEFT (scale strip) / RIGHT
    float pan_last_x;
    double pan_centre, pan_retune_t, pan_wf_accum;
    Rectangle scale_rect;             // frequency scale strip (bottom of the spectrum)
    gui_app_t *app_ref;               // for the wheel handler (no app argument)
    double feed_seen_time;
    atomic_flag chain_lock;           // the chain runs on the worker OR the render path
    // Spectrum snapshot from the feed (worker writes, render FFTs)
    atomic_flag snap_lock;
    int16_t snap_a[DEMOD_FFT_N], snap_b[DEMOD_FFT_N];
    size_t snap_n;
    bool snap_iq;
    atomic_uint_fast64_t snap_seq;
    uint64_t snap_seen;

    // Overlay controls (drawn on the panel, CVBS-style right-aligned stack).
    // The mode dropdown is rendered here too (the panel-menu get_menu path is
    // never wired into the UI, so we render Mode ourselves like BW/Out).
    // Frequency is shown only when the active capture device is an SDR (so the
    // demod panel can tune it live via gui_app_set_sdr_frequency).
    struct {
        Rectangle freq_btn_rect;       // Frequency (Hz) edit field hit box (SDR only)
        Rectangle mode_btn_rect;       // Mode dropdown button
        Rectangle bw_btn_rect;        // Bandwidth dropdown button
        Rectangle sql_minus_rect;     // Squelch -
        Rectangle sql_plus_rect;       // Squelch +
        Rectangle vol_minus_rect;      // Volume -
        Rectangle vol_plus_rect;       // Volume +
        Rectangle out_btn_rect;        // Output pair dropdown button
        Rectangle in_btn_rect;         // Input: Auto / I/Q / Real
        Rectangle region_btn_rect;     // Region: Auto / EU/UK / Americas / Japan
        Rectangle zone_btn_rect;       // Range: RF zone (real input)
        Rectangle zoom_btn_rect;       // Zoom: 1x .. 32x
        Rectangle band_btn_rect;       // Band presets (SDR)
        Rectangle gain_minus_rect, gain_plus_rect;   // SDR tuner gain stepper
        Rectangle agc_btn_rect;        // RTL AGC toggle
        Rectangle rate_btn_rect;       // SDR sample rate (span)
        Rectangle sweep_btn_rect;      // Sweep presets
        Rectangle sweep_options_rect[6];
        bool sweep_dropdown_open;
        Rectangle rate_options_rect[10];
        bool rate_dropdown_open;
        Rectangle band_options_rect[8];
        bool band_dropdown_open;

        Rectangle mode_options_rect[DEMOD_MODE_COUNT];   // Mode dropdown options
        Rectangle bw_options_rect[DEMOD_BW_OPTION_COUNT];   // BW dropdown options
        Rectangle out_options_rect[2];                       // Out dropdown options (CH1/2, CH3/4)

        bool mode_dropdown_open;
        bool bw_dropdown_open;
        bool out_dropdown_open;
        bool is_visible;             // overlay rendered this frame (for click detection)
        bool is_sdr;                // active capture device is an SDR (cached per render)
        // Frequency field local edit state (SDR live-tune field)
        bool freq_editing;
        int  freq_cursor;           // char index into s_demod_freq_str

        // Double-click chrome-hide tracker (see ui/gui_chrome_hide.h):
        // toggles ALL overlay controls (freq field, mode/bw/out dropdowns,
        // SQL/VOL steppers); the demod waveform keeps rendering.
        gui_chrome_hide_t chrome_hide;
    } overlay;
} demod_state_t;

//-----------------------------------------------------------------------------

bool gui_demod_available(void) { return true; }

// True iff the active capture device is an SDR backend (today only RTL-SDR).
// Device-agnostic: add future SDR backends here so the demod frequency control
// shows for them too.
static bool demod_active_device_is_sdr(const gui_app_t *app) {
    if (!app || app->selected_device < 0 || app->selected_device >= app->device_count) return false;
#ifdef ENABLE_RTLSDR
    return app->devices[app->selected_device].type == DEVICE_TYPE_RTLSDR;
#else
    return false;
#endif
}

// Launch state (--radio).
#define DEMOD_ZONES 6                  // 0..120 MHz at 40 MSPS (covers FM broadcast)
static struct { double tune_hz; int input; int region; int zone; int zoom; double sweep_lo, sweep_hi; bool from_flag; }
    s_radio_launch = { 0.0, 0, -1, -1, 1, 0.0, 0.0, false };
static bool s_radio_region_applied;
// Settings keys of --radio (-1 / NAN = not given).
static struct {
    int mode, bw, out, agc, gain_auto, band;
    float sql_db, vol, gain_db;
    double freq_hz;
    uint32_t rate;
    bool tune_given;
} s_radio_set = { -1, -1, -1, -1, -1, -1, NAN, NAN, NAN, 0.0, 0, false };

// <number>[k|M] -> value (Hz). False on junk.
static bool radio_parse_hz(const char *v, double *out) {
    char *end = NULL;
    double x = strtod(v, &end);
    if (!end || end == v) return false;
    if (*end == 'k' || *end == 'K') { x *= 1e3; end++; }
    else if (*end == 'M' || *end == 'm') { x *= 1e6; end++; }
    if (*end) return false;
    *out = x;
    return true;
}

// Region (harrypm 2026-11-27): broadcast conventions per area.
//   Europe/UK, Japan: WFM 50 us de-emphasis, AM (MW / LW) 9 kHz raster.
//   Americas (+ South Korea): WFM 75 us, AM 10 kHz raster.
// Auto follows the TV system setting: NTSC -> Americas, else Europe/UK.
enum { DEMOD_REGION_AUTO = 0, DEMOD_REGION_EU, DEMOD_REGION_AMERICAS, DEMOD_REGION_JAPAN, DEMOD_REGION_COUNT };
static const char *const s_region_names[DEMOD_REGION_COUNT] = { "Auto", "EU/UK", "Americas", "Japan" };

static int demod_region(const gui_app_t *app) {
    const int r = app->settings.demod_region;
    if (r > DEMOD_REGION_AUTO && r < DEMOD_REGION_COUNT) return r;
    return app->settings.video_system == 1 ? DEMOD_REGION_AMERICAS : DEMOD_REGION_EU;
}

static float demod_deemph_tau_s(int region) {
    return region == DEMOD_REGION_AMERICAS ? 75e-6f : 50e-6f;
}

static int demod_am_raster_hz(int region) {
    return region == DEMOD_REGION_AMERICAS ? 10000 : 9000;
}

static const char *const s_radio_input_names[3] = { "Auto", "I/Q", "Real" };

bool gui_demod_apply_launch_arg(const char *arg) {
    static const char *usage =
        "--radio: expected comma-separated key=value pairs: tune=<Hz> (signed offset "
        "from the I/Q centre / frequency on a real input; k / M suffixes), input=auto|iq|real, "
        "region=auto|eu|us|jp, range=1..6 (real input: RF zone n covers (n-1)*fs/2..n*fs/2), "
        "zoom=1|2|4|8|16|32, mode=wfm|nfm|am|usb|lsb, bw=auto|<Hz>, sql=off|<dBFS -100..-1>, "
        "vol=0..2, out=12|34, freq=<tuned Hz / MHz> (SDR), band=fm|air|2m|marine|70cm|pmr, "
        "gain=auto|<dB 0..60>, agc=on|off, rate=0.25M|1.024M|1.2M|1.536M|2.0M|2.048M|2.4M|2.56M|2.88M|3.2M, "
        "sweep=off|<lo>-<hi> (e.g. 80M-100M; SDR, 1-200 MHz wide)";
    const char *p = arg;
    if (!p || !*p) goto bad;
    while (*p) {
        const char *comma = strchr(p, ',');
        const size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char kv[48];
        if (len == 0 || len >= sizeof(kv)) goto bad;
        memcpy(kv, p, len);
        kv[len] = 0;
        if (strncmp(kv, "tune=", 5) == 0 && kv[5]) {
            char *end = NULL;
            double v = strtod(kv + 5, &end);
            if (end && (*end == 'k' || *end == 'K')) { v *= 1e3; end++; }
            else if (end && (*end == 'M' || *end == 'm')) { v *= 1e6; end++; }
            if (!end || *end) goto bad;
            s_radio_launch.tune_hz = v;
            s_radio_set.tune_given = true;
        } else if (strcmp(kv, "input=auto") == 0) s_radio_launch.input = 0;
        else if (strcmp(kv, "input=iq") == 0) s_radio_launch.input = 1;
        else if (strcmp(kv, "input=real") == 0) s_radio_launch.input = 2;
        else if (strncmp(kv, "range=", 6) == 0 && kv[6] >= '1' && kv[6] <= '0' + DEMOD_ZONES && !kv[7])
            s_radio_launch.zone = kv[6] - '1';
        else if (strncmp(kv, "zoom=", 5) == 0) {
            const int z = atoi(kv + 5);
            if (z != 1 && z != 2 && z != 4 && z != 8 && z != 16 && z != 32) goto bad;
            s_radio_launch.zoom = z;
        }
        else if (strncmp(kv, "mode=", 5) == 0) {
            static const char *const m[DEMOD_MODE_COUNT] = { "wfm", "nfm", "am", "usb", "lsb" };
            int f = -1;
            for (int k = 0; k < DEMOD_MODE_COUNT; k++) if (strcmp(kv + 5, m[k]) == 0) f = k;
            if (f < 0) goto bad;
            s_radio_set.mode = f;
        } else if (strcmp(kv, "bw=auto") == 0) s_radio_set.bw = 0;
        else if (strncmp(kv, "bw=", 3) == 0) {
            double v;
            if (!radio_parse_hz(kv + 3, &v) || v < 100.0 || v > 2.4e6) goto bad;
            s_radio_set.bw = (int)(v + 0.5);
        } else if (strcmp(kv, "sql=off") == 0) s_radio_set.sql_db = 0.0f;
        else if (strncmp(kv, "sql=", 4) == 0) {
            char *e = NULL;
            const double db = strtod(kv + 4, &e);
            if (!e || *e || db < -100.0 || db >= 0.0) goto bad;
            s_radio_set.sql_db = (float)db;
        } else if (strncmp(kv, "vol=", 4) == 0) {
            char *e = NULL;
            const double v = strtod(kv + 4, &e);
            if (!e || *e || v < 0.0 || v > 2.0) goto bad;
            s_radio_set.vol = (float)v;
        } else if (strcmp(kv, "out=12") == 0) s_radio_set.out = 0;
        else if (strcmp(kv, "out=34") == 0) s_radio_set.out = 1;
        else if (strncmp(kv, "freq=", 5) == 0) {
            double v;
            if (!radio_parse_hz(kv + 5, &v)) goto bad;
            if (v < 10000.0) v *= 1e6;   // plain MHz ("97.1")
            if (v < (double)DEMOD_SDR_MIN_HZ || v > (double)DEMOD_SDR_MAX_HZ) goto bad;
            s_radio_set.freq_hz = v;
        } else if (strncmp(kv, "band=", 5) == 0) {
            static const char *const b[] = { "fm", "air", "2m", "marine", "70cm", "pmr" };
            int f = -1;
            for (int k = 0; k < DEMOD_BAND_COUNT && k < 6; k++) if (strcmp(kv + 5, b[k]) == 0) f = k;
            if (f < 0) goto bad;
            s_radio_set.band = f;
        } else if (strcmp(kv, "gain=auto") == 0) s_radio_set.gain_auto = 1;
        else if (strncmp(kv, "gain=", 5) == 0) {
            char *e = NULL;
            const double db = strtod(kv + 5, &e);
            if (!e || *e || db < 0.0 || db > 60.0) goto bad;
            s_radio_set.gain_auto = 0;
            s_radio_set.gain_db = (float)db;
        }
        else if (strcmp(kv, "sweep=off") == 0) { s_radio_launch.sweep_lo = s_radio_launch.sweep_hi = 0.0; }
        else if (strncmp(kv, "sweep=", 6) == 0) {   // sweep=<lo>-<hi> (Hz, k / M)
            char tmp[48];
            snprintf(tmp, sizeof(tmp), "%s", kv + 6);
            char *dash = strchr(tmp, '-');
            double lo, hi;
            if (!dash) goto bad;
            *dash = 0;
            if (!radio_parse_hz(tmp, &lo) || !radio_parse_hz(dash + 1, &hi)) goto bad;
            if (lo < 10000.0) lo *= 1e6;
            if (hi < 10000.0) hi *= 1e6;
            if (lo < (double)DEMOD_SDR_MIN_HZ || hi > (double)DEMOD_SDR_MAX_HZ || hi - lo < 1e6 || hi - lo > 200e6) goto bad;
            s_radio_launch.sweep_lo = lo;
            s_radio_launch.sweep_hi = hi;
        }
        else if (strcmp(kv, "agc=on") == 0) s_radio_set.agc = 1;
        else if (strcmp(kv, "agc=off") == 0) s_radio_set.agc = 0;
        else if (strncmp(kv, "rate=", 5) == 0) {
            double v;
            if (!radio_parse_hz(kv + 5, &v)) goto bad;
            if (v < 10000.0) v *= 1e6;   // plain MSPS ("2.4")
            int f = -1;
            for (int k = 0; k < DEMOD_RATE_COUNT; k++) if (fabs((double)s_demod_rates[k] - v) < 1.0) f = k;
            if (f < 0) goto bad;
            s_radio_set.rate = s_demod_rates[f];
        }
        else if (strcmp(kv, "region=auto") == 0) s_radio_launch.region = DEMOD_REGION_AUTO;
        else if (strcmp(kv, "region=eu") == 0) s_radio_launch.region = DEMOD_REGION_EU;
        else if (strcmp(kv, "region=us") == 0) s_radio_launch.region = DEMOD_REGION_AMERICAS;
        else if (strcmp(kv, "region=jp") == 0) s_radio_launch.region = DEMOD_REGION_JAPAN;
        else goto bad;
        p += len + (comma ? 1 : 0);
    }
    s_radio_launch.from_flag = true;
    fprintf(stderr, "[AUTO] --radio: tune=%+.0f Hz input=%s region=%s\n", s_radio_launch.tune_hz,
            s_radio_input_names[s_radio_launch.input],
            s_radio_launch.region >= 0 ? s_region_names[s_radio_launch.region] : "(saved)");
    return true;
bad:
    fprintf(stderr, "%s (got '%s')\n", usage, arg ? arg : "");
    return false;
}

void gui_demod_apply_launch_settings(gui_app_t *app) {
    if (!app || !s_radio_launch.from_flag) return;
    gui_settings_t *st = &app->settings;
    if (s_radio_set.band >= 0) {   // centre 300 kHz below the band centre, line on it
        const demod_band_t *b = &s_demod_bands[s_radio_set.band];
        st->rtlsdr_freq_hz = (uint64_t)(b->centre_hz - 300000.0);
        st->demod_mode = b->mode;
        if (!s_radio_set.tune_given) s_radio_launch.tune_hz = 300000.0;
        // Like the Band box: a band wider than the live view opens whole.
        if (s_radio_launch.sweep_hi <= 0.0 && b->hi - b->lo > 0.8 * (double)st->rtlsdr_sample_rate_hz) {
            s_radio_launch.sweep_lo = b->lo;
            s_radio_launch.sweep_hi = b->hi;
        }
    }
    if (s_radio_set.freq_hz > 0.0) {   // tuned frequency: line at +tune (default +300 kHz)
        if (!s_radio_set.tune_given) s_radio_launch.tune_hz = 300000.0;
        st->rtlsdr_freq_hz = (uint64_t)(s_radio_set.freq_hz - s_radio_launch.tune_hz + 0.5);
    }
    if (s_radio_set.mode >= 0) st->demod_mode = s_radio_set.mode;
    if (s_radio_set.bw >= 0) st->demod_bandwidth_hz = s_radio_set.bw;
    else if (s_radio_set.mode >= 0 || s_radio_set.band >= 0) st->demod_bandwidth_hz = 0;   // Auto
    if (!isnan(s_radio_set.sql_db)) st->demod_squelch = s_radio_set.sql_db == 0.0f ? 0.0f : (s_radio_set.sql_db + 100.0f) / 100.0f;
    if (!isnan(s_radio_set.vol)) st->demod_volume = s_radio_set.vol;
    if (s_radio_set.out >= 0) st->demod_output_pair = s_radio_set.out;
    if (s_radio_launch.region >= 0) { st->demod_region = s_radio_launch.region; s_radio_region_applied = true; }
    if (s_radio_set.gain_auto == 1) st->rtlsdr_gain_mode = 0;
    if (s_radio_set.gain_auto == 0) { st->rtlsdr_gain_mode = 1; st->rtlsdr_gain_tenths_db = (int)(s_radio_set.gain_db * 10.0f + 0.5f); }
    if (s_radio_set.agc >= 0) st->rtlsdr_agc = s_radio_set.agc == 1;
    if (s_radio_set.rate) st->rtlsdr_sample_rate_hz = s_radio_set.rate;
    fprintf(stderr, "[AUTO] --radio settings: mode=%d bw=%d sql=%.2f vol=%.1f out=%d region=%d "
            "sdr=%.6f MHz gain=%s%.1f agc=%d rate=%u tune=%+.0f\n",
            st->demod_mode, st->demod_bandwidth_hz, st->demod_squelch, st->demod_volume,
            st->demod_output_pair, st->demod_region, st->rtlsdr_freq_hz / 1e6,
            st->rtlsdr_gain_mode == 1 ? "" : "auto/", st->rtlsdr_gain_tenths_db / 10.0,
            st->rtlsdr_agc ? 1 : 0, st->rtlsdr_sample_rate_hz, s_radio_launch.tune_hz);
}

// Channel bandwidth per mode when BW is Auto (SDR++-like defaults).
static int demod_auto_bw(int mode, int region) {
    switch (mode) {
        case DEMOD_MODE_WFM: return 250000;   // full stereo + RDS channel
        case DEMOD_MODE_NFM: return 12500;
        case DEMOD_MODE_AM:  return demod_am_raster_hz(region);
        default:             return 3000;   // USB / LSB
    }
}

static int demod_effective_bw(const gui_app_t *app, int mode, double fs) {
    int bw = app->settings.demod_bandwidth_hz > 0 ? app->settings.demod_bandwidth_hz
                                                  : demod_auto_bw(mode, demod_region(app));
    if (fs > 0.0 && bw > fs * 0.9) bw = (int)(fs * 0.9);
    return bw < 100 ? 100 : bw;
}

// Click / wheel snap of the tuned frequency (SDR++ defaults per mode).
static double demod_snap_hz(int mode, int region) {
    switch (mode) {
        case DEMOD_MODE_WFM: return 100000.0;
        case DEMOD_MODE_NFM: return 2500.0;
        case DEMOD_MODE_AM:  return (double)demod_am_raster_hz(region);   // MW / LW channel raster
        default:             return 100.0;
    }
}

// True input rate (Hz): the playback file's resolved rate in player mode,
// else the live rate (kHz-domain values are scaled up).
static double demod_input_rate(gui_app_t *app) {
    if (app->selected_device >= 0 && app->selected_device < app->device_count &&
        app->devices[app->selected_device].type == DEVICE_TYPE_PLAYBACK) {
        playback_file_info_t info = {0};
        if (gui_playback_get_selected_file_info(app, 0, &info) && info.real_sample_rate_hz > 0.0)
            return info.real_sample_rate_hz;
    }
    double fs = (double)atomic_load(&app->sample_rate);
    if (fs <= 0.0) return 2400000.0;
    return fs < 100000.0 ? fs * 1000.0 : fs;
}

// Forward declarations (overlay render is defined after the vtable render that calls it).
static void demod_render_overlay(demod_state_t *s, gui_app_t *app, Rectangle bounds);
static void demod_commit_freq(demod_state_t *s, gui_app_t *app);

//-----------------------------------------------------------------------------
// soxr setup / teardown
//-----------------------------------------------------------------------------

#if LIBSOXR_ENABLED
static void demod_soxr_destroy(demod_state_t *s) {
    if (s->soxr) { soxr_delete((soxr_t)s->soxr); s->soxr = NULL; }
    s->soxr_in_rate = 0;
}

static bool demod_soxr_ensure(demod_state_t *s, uint32_t in_rate) {
    if (s->soxr && s->soxr_in_rate == in_rate) return true;
    demod_soxr_destroy(s);
    if (in_rate == 0) return false;

    soxr_io_spec_t io = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
    soxr_quality_spec_t q = soxr_quality_spec(SOXR_MQ, 0);
    soxr_error_t err = NULL;
    soxr_t st = soxr_create((double)in_rate, (double)DEMOD_AUDIO_RATE_HZ, 1, &err, &io, &q, NULL);
    if (!st || err) {
        fprintf(stderr, "[DEMOD] soxr_create(%u->%u) failed: %s\n",
                in_rate, DEMOD_AUDIO_RATE_HZ, err ? soxr_strerror(err) : "?");
        if (st) soxr_delete(st);
        return false;
    }
    s->soxr = (void *)st;
    s->soxr_in_rate = in_rate;

    // Size scratch buffers for the largest expected block (one display frame).
    size_t cap = 65536;
    if (s->soxr_in_cap < cap) {
        int16_t *nb = (int16_t *)realloc(s->soxr_in_buf, cap * sizeof(int16_t));
        if (!nb) { demod_soxr_destroy(s); return false; }
        s->soxr_in_buf = nb; s->soxr_in_cap = cap;
    }
    size_t out_cap = (size_t)((double)cap * DEMOD_AUDIO_RATE_HZ / (double)in_rate) + 64;
    if (s->soxr_out_cap < out_cap) {
        int16_t *ob = (int16_t *)realloc(s->soxr_out_buf, out_cap * sizeof(int16_t));
        if (!ob) { demod_soxr_destroy(s); return false; }
        s->soxr_out_buf = ob; s->soxr_out_cap = out_cap;
    }
    return true;
}
#else
static void demod_soxr_destroy(demod_state_t *s) { (void)s; }
static bool demod_soxr_ensure(demod_state_t *s, uint32_t in_rate) { (void)s; (void)in_rate; return false; }
#endif

//-----------------------------------------------------------------------------
// Channel chain: NCO mix (tuning line -> DC), integrate & dump, FIR to the
// channel bandwidth, then the demod at fs_out (~2x bandwidth).
//-----------------------------------------------------------------------------

static void demod_reset_demod_state(demod_state_t *s) {
    s->prev_i = s->prev_q = 0.0f;
    s->deemph_state = s->dc_state = 0.0f;
    s->agc_peak = 0.0f;
    s->post_c = 1.0f;
    s->post_s = 0.0f;
}

static void demod_channel_setup(demod_state_t *s, double fs, int bw, int mode, int region) {
    if (fs != s->chan_fs || bw != s->chan_bw || mode != s->chan_mode || region != s->chan_region) {
        int D = (int)(fs / (4.0 * (double)bw));
        if (D < 1) D = 1;
        const double mid = fs / (double)D;
        const int fd = mid >= 3.0 * (double)bw ? 2 : 1;
        double fc = 0.5 * (double)bw / mid;   // cycles / sample at the mid rate
        if (fc > 0.45) fc = 0.45;
        double sum = 0.0;
        for (int k = 0; k < DEMOD_FIR_TAPS; k++) {
            const double n = (double)k - (DEMOD_FIR_TAPS - 1) * 0.5;
            const double h = n == 0.0 ? 2.0 * fc : sin(2.0 * M_PI * fc * n) / (M_PI * n);
            const double w = 0.54 - 0.46 * cos(2.0 * M_PI * (double)k / (DEMOD_FIR_TAPS - 1));
            s->fir_taps[k] = (float)(h * w);
            sum += h * w;
        }
        for (int k = 0; k < DEMOD_FIR_TAPS; k++) s->fir_taps[k] = (float)(s->fir_taps[k] / sum);
        memset(s->fir_hi, 0, sizeof(s->fir_hi));
        memset(s->fir_hq, 0, sizeof(s->fir_hq));
        s->fir_pos = s->fir_phase = 0;
        s->dec_D = D;
        s->dec_n = 0;
        s->acc_i = s->acc_q = 0.0f;
        s->fir_dec = fd;
        s->fs_out = mid / (double)fd;
        // FM: the channel's peak deviation maps to 0.8 of full scale.
        const double dev = mode == DEMOD_MODE_WFM ? 75000.0 : 0.4 * (double)bw;
        s->fm_gain = (float)(0.8 * s->fs_out / (2.0 * M_PI * dev));
        s->deemph_a = expf(-1.0f / ((float)s->fs_out * demod_deemph_tau_s(region)));
        s->dc_a = expf(-2.0f * (float)M_PI * 30.0f / (float)s->fs_out);
        s->agc_release = expf(-1.0f / (0.2f * (float)s->fs_out));   // 200 ms release
        s->sq_a = expf(-1.0f / (0.02f * (float)s->fs_out));
        s->sq_ramp = 1.0f / (0.005f * (float)s->fs_out);
        const double w2 = (mode == DEMOD_MODE_USB ? 1.0 : -1.0) * 2.0 * M_PI * 0.5 * (double)bw / s->fs_out;
        s->post_dc = (float)cos(w2);
        s->post_ds = (float)sin(w2);
        demod_reset_demod_state(s);
        s->chan_fs = fs;
        s->chan_bw = bw;
        s->chan_mode = mode;
        s->chan_region = region;
        s->chan_mix_valid = false;
    }
    // SSB: the sideband's centre goes to DC (USB above, LSB below the line).
    const double mix = s->tune_bb + (mode == DEMOD_MODE_USB ? 0.5 * bw : mode == DEMOD_MODE_LSB ? -0.5 * bw : 0.0);
    if (!s->chan_mix_valid || mix != s->chan_mix) {
        const double w = -2.0 * M_PI * mix / fs;
        s->nco_dc = (float)cos(w);
        s->nco_ds = (float)sin(w);
        if (!s->chan_mix_valid) { s->nco_c = 1.0f; s->nco_s = 0.0f; }
        s->chan_mix = mix;
        s->chan_mix_valid = true;
    }
}

static float demod_agc(demod_state_t *s, float x) {
    const float a = fabsf(x);
    if (a > s->agc_peak) s->agc_peak = a;
    else s->agc_peak *= s->agc_release;
    if (s->agc_peak < 1e-5f) s->agc_peak = 1e-5f;
    return x / s->agc_peak * 0.7f;
}

// One channel sample (fs_out) -> mono audio in [-1, 1].
static float demod_channel_sample(demod_state_t *s, float i, float q) {
    switch (s->mode) {
        case DEMOD_MODE_WFM:
        case DEMOD_MODE_NFM: {
            const float cross = s->prev_i * q - s->prev_q * i;
            const float dot = s->prev_i * i + s->prev_q * q;
            s->prev_i = i;
            s->prev_q = q;
            float m = atan2f(cross, dot) * s->fm_gain;
            if (s->mode == DEMOD_MODE_WFM) {
                s->deemph_state = (1.0f - s->deemph_a) * m + s->deemph_a * s->deemph_state;
                m = s->deemph_state;
            }
            return m;
        }
        case DEMOD_MODE_AM: {
            const float mag = sqrtf(i * i + q * q);
            if (s->agc_peak == 0.0f) s->dc_state = mag;   // seed: no start-up step into the AGC
            s->dc_state = (1.0f - s->dc_a) * mag + s->dc_a * s->dc_state;
            return demod_agc(s, mag - s->dc_state);
        }
        case DEMOD_MODE_USB:
        case DEMOD_MODE_LSB: {
            const float re = i * s->post_c - q * s->post_s;
            const float nc = s->post_c * s->post_dc - s->post_s * s->post_ds;
            s->post_s = s->post_c * s->post_ds + s->post_s * s->post_dc;
            s->post_c = nc;
            return demod_agc(s, 2.0f * re);
        }
        default:
            return 0.0f;
    }
}

//-----------------------------------------------------------------------------
// Process one display frame of I/Q -> mono audio -> 48 k monitor
//-----------------------------------------------------------------------------

// iq_b NULL = real input (Q = 0): the NCO + channel filter keep the
// positive-frequency copy, so tuning works the same on a real channel.
static bool radio_feed_owns(const demod_state_t *s);   // the panel that owns the audio
static void demod_process_frame(demod_state_t *s, gui_app_t *app,
                                 const int16_t *iq_a, const int16_t *iq_b,
                                 size_t count, double fs) {
    const int bw = demod_effective_bw(app, s->mode, fs);
    // A mirrored zone swaps the sidebands (FM / AM only flip polarity).
    int emode = s->mode;
    if (s->tune_inv && emode == DEMOD_MODE_USB) emode = DEMOD_MODE_LSB;
    else if (s->tune_inv && emode == DEMOD_MODE_LSB) emode = DEMOD_MODE_USB;
    demod_channel_setup(s, fs, bw, emode, demod_region(app));
    const float scale = 1.0f / 2048.0f;   // 12-bit range -> [-1,1]

#if LIBSOXR_ENABLED
    if (!demod_soxr_ensure(s, (uint32_t)(s->fs_out + 0.5))) return;
#endif
    int16_t *scratch_in = s->soxr_in_buf;
    if (!scratch_in) return;
    // SQL 0..1 -> threshold -100..0 dBFS of channel power (0 = off).
    const float sql = app->settings.demod_squelch;
    const float sq_thr = sql > 0.001f ? powf(10.0f, (-100.0f + 100.0f * sql) / 10.0f) : 0.0f;
    const size_t cap = s->soxr_in_cap;
    size_t n = 0;
    float c = s->nco_c, sn = s->nco_s;
    const float dc = s->nco_dc, ds = s->nco_ds;
    const int D = s->dec_D;
    const float inv_d = 1.0f / (float)D;
    for (size_t k = 0; k < count && n < cap; k++) {
        const float i = (float)iq_a[k] * scale;
        const float q = iq_b ? (float)iq_b[k] * scale : 0.0f;
        s->acc_i += i * c - q * sn;
        s->acc_q += i * sn + q * c;
        const float nc = c * dc - sn * ds;
        sn = c * ds + sn * dc;
        c = nc;
        if (++s->nco_renorm >= 1024) {
            const float g = 1.0f / sqrtf(c * c + sn * sn);
            c *= g;
            sn *= g;
            s->nco_renorm = 0;
        }
        if (++s->dec_n < D) continue;
        s->dec_n = 0;
        s->fir_hi[s->fir_pos] = s->acc_i * inv_d;
        s->fir_hq[s->fir_pos] = s->acc_q * inv_d;
        s->acc_i = s->acc_q = 0.0f;
        if (++s->fir_pos == DEMOD_FIR_TAPS) s->fir_pos = 0;
        if (++s->fir_phase < s->fir_dec) continue;
        s->fir_phase = 0;
        float fi = 0.0f, fq = 0.0f;
        // Ring oldest-first (fir_pos = next write = oldest): the taps must
        // stay aligned to sample age, or the filter turns time-varying.
        for (int t = 0, idx = s->fir_pos; t < DEMOD_FIR_TAPS; t++) {
            fi += s->fir_taps[t] * s->fir_hi[idx];
            fq += s->fir_taps[t] * s->fir_hq[idx];
            if (++idx == DEMOD_FIR_TAPS) idx = 0;
        }
        float m = demod_channel_sample(s, fi, fq);
        s->sq_pow = s->sq_a * s->sq_pow + (1.0f - s->sq_a) * (fi * fi + fq * fq);
        if (sq_thr > 0.0f) {
            if (s->sq_open) { if (s->sq_pow < sq_thr * 0.5f) s->sq_open = false; }   // 3 dB hysteresis
            else if (s->sq_pow > sq_thr) s->sq_open = true;
        } else {
            s->sq_open = true;
        }
        const float target = s->sq_open ? 1.0f : 0.0f;
        if (s->sq_gain < target) { s->sq_gain += s->sq_ramp; if (s->sq_gain > target) s->sq_gain = target; }
        else if (s->sq_gain > target) { s->sq_gain -= s->sq_ramp; if (s->sq_gain < target) s->sq_gain = target; }
        m *= s->sq_gain;
        if (m > 1.0f) m = 1.0f;
        if (m < -1.0f) m = -1.0f;
        scratch_in[n++] = (int16_t)(m * 32767.0f);
    }
    s->nco_c = c;
    s->nco_s = sn;
    atomic_store(&s->sq_db10, (int)(100.0f * log10f(s->sq_pow + 1e-12f)));
    if (n == 0) return;

#if LIBSOXR_ENABLED
    size_t out_cap = s->soxr_out_cap;
    size_t idone = 0, odone = 0;
    soxr_error_t err = soxr_process((soxr_t)s->soxr,
                                    scratch_in, n, &idone,
                                    s->soxr_out_buf, out_cap, &odone);
    if (err || odone == 0) return;
    int16_t *audio = s->soxr_out_buf;
    size_t frames = odone;
#else
    int16_t *audio = scratch_in;
    size_t frames = n;   // no resampling available; pass through (wrong rate)
#endif

    // Volume (squelch is applied on the channel power above)
    float vol = app->settings.demod_volume;
    if (s->audio_out_cap < frames * 2) {
        int16_t *nb = (int16_t *)realloc(s->audio_out, frames * 2 * sizeof(int16_t));
        if (!nb) { s->audio_out_cap = 0; return; }
        s->audio_out = nb; s->audio_out_cap = frames * 2;
    }
    for (size_t k = 0; k < frames; k++) {
        int16_t a = audio[k];
        int32_t v = (int32_t)(a * vol);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s->audio_out[k * 2 + 0] = (int16_t)v;
        s->audio_out[k * 2 + 1] = (int16_t)v;
        s->wave[s->wave_pos % DEMOD_WAVE_LEN] = (int16_t)v;
        s->wave_pos++;
    }
    if (radio_feed_owns(s) && (!s->sweep_on || s->sweep_listen)) {   // one voice; muted while hopping
        gui_audio_push_demod(s->audio_out, frames);
        int32_t pk = 0;   // CH3/4 meters (mono: both)
        for (size_t k = 0; k < frames; k++) {
            const int32_t v = s->audio_out[k * 2];
            const int32_t a = v < 0 ? -v : v;
            if (a > pk) pk = a;
        }
        atomic_store(&app->radio_peak, (uint32_t)pk << 8);   // 16 -> 24-bit scale
        atomic_store(&app->radio_peak_ms, (uint64_t)(GetTime() * 1000.0));
    }
}

//-----------------------------------------------------------------------------
// Continuous capture feed -> worker thread (the display frames are only the
// latest 64 k samples per drawn frame: ~10 % of a 40 MSPS stream, so the
// audio needs the capture blocks themselves).
//-----------------------------------------------------------------------------

#define RADIO_RING (1u << 22)          // samples per channel ring (~100 ms at 40 MSPS)
#define RADIO_CHUNK 65536              // worker block (== soxr input scratch)

static struct {
    int16_t *ring[2];
    atomic_uint_fast64_t w[2];         // samples written per channel (monotonic)
    atomic_uint_fast32_t rate_hz[2];
    atomic_uint_fast32_t gen;          // discontinuity: the worker resyncs
    atomic_int need;                   // bit0 CH A, bit1 CH B (0 = feed off)
    atomic_int panels;
    _Atomic(demod_state_t *) active;   // the panel that owns the audio
    _Atomic(demod_state_t *) busy;     // the panel the worker is inside
    _Atomic(gui_app_t *) app;
    bool started;
    thrd_t thread;
} s_feed;

static bool radio_feed_owns(const demod_state_t *s) {
    return atomic_load(&s_feed.active) == s;
}

static void radio_feed_write(int ch, const int16_t *src, size_t n) {
    int16_t *ring = s_feed.ring[ch];
    uint64_t w = atomic_load_explicit(&s_feed.w[ch], memory_order_relaxed);
    while (n) {
        const size_t at = (size_t)(w & (RADIO_RING - 1));
        size_t m = RADIO_RING - at;
        if (m > n) m = n;
        memcpy(ring + at, src, m * sizeof(int16_t));
        src += m;
        n -= m;
        w += m;
    }
    atomic_store_explicit(&s_feed.w[ch], w, memory_order_release);
}

// Extraction / RTL-SDR thread: copy only, never wait.
static void radio_feed_tap(gui_app_t *app, const int16_t *a, const int16_t *b,
                           size_t count_a, size_t count_b,
                           const uint8_t *iq_u8, size_t iq_pairs, const gui_rf_block_info_t info[2]) {
    const int need = atomic_load_explicit(&s_feed.need, memory_order_relaxed);
    if (!need || !s_feed.ring[0] || !info) return;
    atomic_store_explicit(&s_feed.app, app, memory_order_relaxed);
    if (iq_u8 && iq_pairs) {
        if (info[0].preview_only) return;
        if (info[0].discontinuity) atomic_fetch_add(&s_feed.gen, 1);
        atomic_store(&s_feed.rate_hz[0], info[0].sample_rate_hz);
        atomic_store(&s_feed.rate_hz[1], info[0].sample_rate_hz);
        int16_t ti[1024], tq[1024];
        for (size_t k = 0; k < iq_pairs;) {
            size_t m = iq_pairs - k < 1024 ? iq_pairs - k : 1024;
            for (size_t j = 0; j < m; j++) {   // 8-bit -> the 12-bit scale
                ti[j] = (int16_t)(((int)iq_u8[2 * (k + j)] - 128) * 16);
                tq[j] = (int16_t)(((int)iq_u8[2 * (k + j) + 1] - 128) * 16);
            }
            radio_feed_write(0, ti, m);
            radio_feed_write(1, tq, m);
            k += m;
        }
        return;
    }
    if (a && count_a && (need & 1) && !info[0].preview_only) {
        if (info[0].discontinuity) atomic_fetch_add(&s_feed.gen, 1);
        if (info[0].sample_rate_hz) atomic_store(&s_feed.rate_hz[0], info[0].sample_rate_hz);
        radio_feed_write(0, a, count_a);
    }
    if (b && count_b && (need & 2) && !info[1].preview_only) {
        if (info[1].discontinuity) atomic_fetch_add(&s_feed.gen, 1);
        if (info[1].sample_rate_hz) atomic_store(&s_feed.rate_hz[1], info[1].sample_rate_hz);
        radio_feed_write(1, b, count_b);
    }
}

static void radio_ring_copy(int ch, uint64_t from, int16_t *dst, size_t n) {
    const int16_t *ring = s_feed.ring[ch];
    while (n) {
        const size_t at = (size_t)(from & (RADIO_RING - 1));
        size_t m = RADIO_RING - at;
        if (m > n) m = n;
        memcpy(dst, ring + at, m * sizeof(int16_t));
        dst += m;
        n -= m;
        from += m;
    }
}

static void demod_sweep_stop(demod_state_t *s, gui_app_t *app);   /* defined below */

static int radio_feed_worker(void *arg) {
    (void)arg;
    static int16_t buf_a[RADIO_CHUNK], buf_b[RADIO_CHUNK];
    uint64_t r = 0;
    uint32_t gen = 0;
    int last_input = -1;
    bool synced = false;
    for (;;) {
        demod_state_t *s = atomic_load(&s_feed.active);
        gui_app_t *app = atomic_load(&s_feed.app);
        if (!s || !app) { synced = false; thrd_sleep_ms(5); continue; }
        atomic_store(&s_feed.busy, s);
        if (atomic_load(&s_feed.active) != s) { atomic_store(&s_feed.busy, NULL); continue; }
        // Not drawn any more (harrypm 2026-11-27: the radio kept running after
        // the panel was switched to another decoder; a switched-away panel is
        // parked, not destroyed): let go of the feed, the sample copying and
        // the audio; a whole-band view puts the SDR back on its saved centre.
        // The panel takes the feed again on its next drawn frame.
        if (s->last_render > 0.0 && GetTime() - s->last_render > 0.3) {
            demod_state_t *expect = s;
            if (atomic_compare_exchange_strong(&s_feed.active, &expect, NULL)) {
                atomic_store(&s_feed.need, 0);
                if (s->sweep_on) demod_sweep_stop(s, app);
                fprintf(stderr, "[RADIO] panel not shown: feed, audio and band view released\n");
            }
            atomic_store(&s_feed.busy, NULL);
            synced = false;
            continue;
        }
        const int input = atomic_load(&s->feed_input);
        const uint64_t w0 = atomic_load_explicit(&s_feed.w[0], memory_order_acquire);
        const uint64_t w1 = atomic_load_explicit(&s_feed.w[1], memory_order_acquire);
        const uint64_t w = input == 0 ? (w0 < w1 ? w0 : w1) : input == 1 ? w0 : w1;
        const uint32_t g = atomic_load(&s_feed.gen);
        if (!synced || g != gen || input != last_input || w < r) {
            r = w;   // start from now (both I/Q channels share the sample index)
            gen = g;
            last_input = input;
            synced = true;
        }
        uint64_t avail = w - r;
        if (avail > RADIO_RING - RADIO_CHUNK) {   // fell behind: drop, keep the newest
            r = w - RADIO_RING / 4;
            avail = w - r;
        }
        if (avail < 2048) { atomic_store(&s_feed.busy, NULL); thrd_sleep_ms(2); continue; }
        const size_t n = avail > RADIO_CHUNK ? RADIO_CHUNK : (size_t)avail;
        const int ch_i = input == 2 ? 1 : 0;
        radio_ring_copy(ch_i, r, buf_a, n);
        if (input == 0) radio_ring_copy(1, r, buf_b, n);
        r += n;
        double fs = (double)atomic_load(&s_feed.rate_hz[ch_i]);
        if (fs <= 0.0) fs = demod_input_rate(app);
        if (!atomic_flag_test_and_set(&s->chain_lock)) {
            demod_process_frame(s, app, buf_a, input == 0 ? buf_b : NULL, n, fs);
            atomic_flag_clear(&s->chain_lock);
        }
        // Spectrum snapshot: the newest DEMOD_FFT_N samples of this block.
        if (!atomic_flag_test_and_set(&s->snap_lock)) {
            const size_t m = n < DEMOD_FFT_N ? n : DEMOD_FFT_N;
            memcpy(s->snap_a, buf_a + (n - m), m * sizeof(int16_t));
            if (input == 0) memcpy(s->snap_b, buf_b + (n - m), m * sizeof(int16_t));
            s->snap_n = m;
            s->snap_iq = input == 0;
            atomic_fetch_add(&s->snap_seq, 1);
            atomic_flag_clear(&s->snap_lock);
        }
        atomic_fetch_add(&s->feed_blocks, 1);
        atomic_store(&s_feed.busy, NULL);
    }
    return 0;
}

static void radio_feed_attach(demod_state_t *s) {
    if (!s_feed.ring[0]) {
        s_feed.ring[0] = calloc(RADIO_RING, sizeof(int16_t));
        s_feed.ring[1] = calloc(RADIO_RING, sizeof(int16_t));
        if (!s_feed.ring[0] || !s_feed.ring[1]) {
            free(s_feed.ring[0]); free(s_feed.ring[1]);
            s_feed.ring[0] = s_feed.ring[1] = NULL;
            return;
        }
    }
    if (!s_feed.started) {
        if (thrd_create(&s_feed.thread, radio_feed_worker, NULL) != 0) return;
        s_feed.started = true;   // lives for the process (idle without a panel)
        gui_video_set_radio_tap(radio_feed_tap);
    }
    atomic_fetch_add(&s_feed.panels, 1);
    (void)s;
}

static void radio_feed_detach(demod_state_t *s) {
    demod_state_t *expect = s;
    if (atomic_compare_exchange_strong(&s_feed.active, &expect, NULL))
        atomic_store(&s_feed.need, 0);
    while (atomic_load(&s_feed.busy) == s) thrd_sleep_ms(1);
    atomic_fetch_sub(&s_feed.panels, 1);
}

//-----------------------------------------------------------------------------
// Spectrum + waterfall (render thread, at most every DEMOD_SPEC_PERIOD_S)
//-----------------------------------------------------------------------------

static int demod_cmp_float(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static bool demod_spectrum_alloc(demod_state_t *s) {
    if (s->fft_plan) return true;
    if (!s->fft_in) s->fft_in = gui_fft_alloc(sizeof(float) * 2 * DEMOD_FFT_N);
    if (!s->fft_out) s->fft_out = gui_fft_alloc(sizeof(float) * 2 * DEMOD_FFT_N);
    if (!s->wf_pixels) {
        s->wf_pixels = calloc((size_t)DEMOD_WF_W * DEMOD_WF_H, 4);
        if (s->wf_pixels)
            for (size_t k = 0; k < (size_t)DEMOD_WF_W * DEMOD_WF_H; k++) s->wf_pixels[k * 4 + 3] = 255;
        s->wf_full_upload = true;
    }
    if (!s->fft_in || !s->fft_out || !s->wf_pixels) return false;
    for (int k = 0; k < DEMOD_FFT_N; k++)
        s->fft_win[k] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)k / (float)(DEMOD_FFT_N - 1));
    s->fft_plan = gui_fft_plan_create_c2c(DEMOD_FFT_N, s->fft_in, s->fft_out, DEMOD_FFTW_ESTIMATE);
    return s->fft_plan != NULL;
}

// Offset (Hz) -> fftshifted bin.
static inline int demod_bin_of(double f, double fs) {
    int k = (int)floor(f / fs * DEMOD_FFT_N + DEMOD_FFT_N / 2);
    return k < 0 ? 0 : k >= DEMOD_FFT_N ? DEMOD_FFT_N - 1 : k;
}

// Shown frequency -> sampled-band offset (real input: fold the RF zone).
static inline double demod_view_to_bb(const demod_state_t *s, double f) {
    if (!s->view_real) return f;
    const double half = 0.5 * s->view_fs;
    const double d = f - (double)s->zone * half;
    return (s->zone & 1) ? half - d : d;
}

// Bins under the shown range f0..f1, ordered (a mirrored zone runs backwards).
static inline void demod_view_bins(const demod_state_t *s, double f0, double f1, int *b0, int *b1) {
    int a = demod_bin_of(demod_view_to_bb(s, f0), s->view_fs);
    int b = demod_bin_of(demod_view_to_bb(s, f1), s->view_fs);
    if (b < a) { const int t = a; a = b; b = t; }
    if (b <= a) b = a + 1;
    *b0 = a;
    *b1 = b > DEMOD_FFT_N ? DEMOD_FFT_N : b;
}

// b NULL = real input (Q = 0; the shown range is 0..fs/2).
static void demod_spectrum_update(demod_state_t *s, const int16_t *a, const int16_t *b, size_t count) {
    const double now = GetTime();
    if (now - s->spec_time < DEMOD_SPEC_PERIOD_S || count == 0 || s->view_fs <= 0.0) return;
    if (!demod_spectrum_alloc(s)) return;
    s->spec_time = now;
    const float scale = 1.0f / 2048.0f;
    const size_t n = count < DEMOD_FFT_N ? count : DEMOD_FFT_N;
    const size_t start = count - n;
    for (size_t k = 0; k < DEMOD_FFT_N; k++) {
        if (k < n) {
            s->fft_in[2 * k] = (float)a[start + k] * scale * s->fft_win[k];
            s->fft_in[2 * k + 1] = b ? (float)b[start + k] * scale * s->fft_win[k] : 0.0f;
        } else {
            s->fft_in[2 * k] = s->fft_in[2 * k + 1] = 0.0f;
        }
    }
    gui_fft_plan_execute(s->fft_plan);
    // dBFS: a full-scale complex tone reads 0 dB (Hann coherent gain 0.5).
    // A real full-scale tone splits into two half-amplitude lines: +6 dB keeps
    // it at 0 dBFS too.
    const float norm = 1.0f / ((float)DEMOD_FFT_N * 0.5f * (float)DEMOD_FFT_N * 0.5f) * (b ? 1.0f : 4.0f);
    for (int k = 0; k < DEMOD_FFT_N; k++) {
        const int src = (k + DEMOD_FFT_N / 2) % DEMOD_FFT_N;   // fftshift: 0 = -fs/2
        const float re = s->fft_out[2 * src], im = s->fft_out[2 * src + 1];
        const float db = 10.0f * log10f((re * re + im * im) * norm + 1e-20f);
        s->spec_db[k] = s->spec_valid ? 0.5f * s->spec_db[k] + 0.5f * db : db;
    }
    // Noise floor: median of the shown range.
    int k0, k1;
    demod_view_bins(s, s->view_lo, s->view_hi, &k0, &k1);
    if (k1 >= DEMOD_FFT_N) k1 = DEMOD_FFT_N - 1;
    float sample[DEMOD_FFT_N / 4];
    int ns = 0;
    for (int k = k0; k <= k1 && ns < DEMOD_FFT_N / 4; k += 4) sample[ns++] = s->spec_db[k];
    if (ns > 0) {
        qsort(sample, (size_t)ns, sizeof(float), demod_cmp_float);
        const float med = sample[ns / 2];
        s->floor_db = s->spec_valid ? 0.9f * s->floor_db + 0.1f * med : med;
    }
    s->spec_valid = true;

    // Waterfall row (newest at wf_head): max-hold of the shown bins per column.
    gui_spectrogram_palette_lut(s->lut);
    s->wf_head = (s->wf_head + DEMOD_WF_H - 1) % DEMOD_WF_H;
    unsigned char *row = s->wf_pixels + (size_t)s->wf_head * DEMOD_WF_W * 4;
    const float lo = s->floor_db - 10.0f;
    const double span = s->view_hi - s->view_lo;
    for (int x = 0; x < DEMOD_WF_W; x++) {
        int b0, b1;
        demod_view_bins(s, s->view_lo + span * x / DEMOD_WF_W, s->view_lo + span * (x + 1) / DEMOD_WF_W, &b0, &b1);
        float m = s->spec_db[b0];
        for (int k = b0 + 1; k < b1 && k < DEMOD_FFT_N; k++) if (s->spec_db[k] > m) m = s->spec_db[k];
        float v = (m - lo) / DEMOD_SPEC_SPAN_DB;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        memcpy(row + (size_t)x * 4, s->lut[(int)(v * 255.0f + 0.5f)], 4);
    }
    s->wf_row_dirty = true;
}

// Shift the waterfall history by cols (positive = content moves right).
static void demod_wf_shift(demod_state_t *s, int cols) {
    if (!s->wf_pixels || cols == 0) return;
    if (cols >= DEMOD_WF_W || cols <= -DEMOD_WF_W) cols = cols > 0 ? DEMOD_WF_W : -DEMOD_WF_W;
    const size_t row_b = (size_t)DEMOD_WF_W * 4;
    const size_t mv = (size_t)(DEMOD_WF_W - (cols > 0 ? cols : -cols)) * 4;
    for (int r = 0; r < DEMOD_WF_H; r++) {
        unsigned char *row = s->wf_pixels + (size_t)r * row_b;
        if (cols > 0) {
            memmove(row + (size_t)cols * 4, row, mv);
            for (int x = 0; x < cols; x++) { row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0; row[x * 4 + 3] = 255; }
        } else {
            memmove(row, row + (size_t)(-cols) * 4, mv);
            for (int x = DEMOD_WF_W + cols; x < DEMOD_WF_W; x++) { row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0; row[x * 4 + 3] = 255; }
        }
    }
    s->wf_full_upload = true;
}

// Move the SDR centre by df (Hz). The tuning line keeps its absolute frequency
// (offset -= df), dragged along only when it would leave the band. live = the
// unsaved retune used during a drag (rate-limited); else saved.
static void demod_pan_by(demod_state_t *s, gui_app_t *app, double df, bool live) {
    if (!s->overlay.is_sdr || s->view_fs <= 0.0 || df == 0.0) return;
    double c = s->pan_drag ? s->pan_centre : (double)app->settings.rtlsdr_freq_hz;
    c += df;
    if (c < (double)DEMOD_SDR_MIN_HZ) { df -= c - (double)DEMOD_SDR_MIN_HZ; c = (double)DEMOD_SDR_MIN_HZ; }
    if (c > (double)DEMOD_SDR_MAX_HZ) { df -= c - (double)DEMOD_SDR_MAX_HZ; c = (double)DEMOD_SDR_MAX_HZ; }
    s->pan_centre = c;
    double v = s->vfo_hz - df;
    const double half = 0.5 * s->view_fs;
    if (v > half) v = half;
    if (v < -half) v = -half;
    s->vfo_hz = v;
    // History follows the band.
    s->pan_wf_accum += -df / (s->view_hi - s->view_lo) * DEMOD_WF_W;
    const int cols = (int)s->pan_wf_accum;
    if (cols != 0) { demod_wf_shift(s, cols); s->pan_wf_accum -= cols; }
    app->settings.rtlsdr_freq_hz = (uint64_t)(c + 0.5);   // labels follow at once
    if (live) {
        const double now = GetTime();
        if (now - s->pan_retune_t >= 0.033) {
            s->pan_retune_t = now;
            (void)gui_app_sdr_retune_live(app, (uint64_t)(c + 0.5));
        }
    } else {
        (void)gui_app_set_sdr_frequency(app, (uint64_t)(c + 0.5));   // saved
    }
}

// ---- Sweep engine (render thread) ----
static void demod_wf_clear(demod_state_t *s) {
    if (!s->wf_pixels) return;
    for (size_t k = 0; k < (size_t)DEMOD_WF_W * DEMOD_WF_H; k++) {
        s->wf_pixels[k * 4] = s->wf_pixels[k * 4 + 1] = s->wf_pixels[k * 4 + 2] = 0;
        s->wf_pixels[k * 4 + 3] = 255;
    }
    s->wf_full_upload = true;
}

static void demod_sweep_retune(demod_state_t *s, gui_app_t *app) {
    const double use = 0.8 * s->view_fs;
    s->sweep_centre = s->sweep_lo + use * ((double)s->sweep_step + 0.5);
    (void)gui_app_sdr_retune_live(app, (uint64_t)(s->sweep_centre + 0.5));
    s->sweep_wait = 2;            // discard snapshots that may predate the retune
    s->sweep_avg_n = 0;
    if (s->sweep_acc) memset(s->sweep_acc, 0, sizeof(float) * DEMOD_FFT_N);
}

static bool demod_sweep_start(demod_state_t *s, gui_app_t *app, double lo, double hi) {
    if (!s->overlay.is_sdr || s->view_fs <= 0.0 || hi <= lo) return false;
    if (!s->sweep_db) s->sweep_db = malloc(sizeof(float) * DEMOD_SWEEP_N);
    if (!s->sweep_acc) s->sweep_acc = calloc(DEMOD_FFT_N, sizeof(float));
    if (!s->sweep_db || !s->sweep_acc || !demod_spectrum_alloc(s)) return false;
    for (int i = 0; i < DEMOD_SWEEP_N; i++) s->sweep_db[i] = NAN;
    s->sweep_lo = lo;
    s->sweep_hi = hi;
    s->sweep_steps = (int)ceil((hi - lo) / (0.8 * s->view_fs));
    if (s->sweep_steps < 1) s->sweep_steps = 1;
    s->sweep_step = 0;
    s->sweep_on = true;
    s->sweep_listen = false;
    demod_wf_clear(s);
    demod_sweep_retune(s, app);
    char msg[80];
    snprintf(msg, sizeof(msg), "Radio sweep %.1f-%.1f MHz (%d steps)", lo / 1e6, hi / 1e6, s->sweep_steps);
    gui_app_set_status(app, msg);
    return true;
}

static void demod_sweep_stop(demod_state_t *s, gui_app_t *app) {
    if (!s->sweep_on) return;
    s->sweep_on = false;
    s->sweep_listen = false;
    demod_wf_clear(s);
    s->spec_valid = false;
    (void)gui_app_sdr_retune_live(app, app->settings.rtlsdr_freq_hz);   // back to the saved centre
}

// One snapshot while sweeping: average 3 FFTs per step, stitch the middle 80 %.
static void demod_sweep_capture(demod_state_t *s, gui_app_t *app, const int16_t *a, const int16_t *b, size_t count) {
    if (!b || count < DEMOD_FFT_N / 2 || !s->sweep_acc) return;
    if (s->sweep_wait > 0) { s->sweep_wait--; return; }
    const float scale = 1.0f / 2048.0f;
    const size_t n = count < DEMOD_FFT_N ? count : DEMOD_FFT_N;
    const size_t start = count - n;
    for (size_t k = 0; k < DEMOD_FFT_N; k++) {
        const bool in = k < n;
        s->fft_in[2 * k] = in ? (float)a[start + k] * scale * s->fft_win[k] : 0.0f;
        s->fft_in[2 * k + 1] = in ? (float)b[start + k] * scale * s->fft_win[k] : 0.0f;
    }
    gui_fft_plan_execute(s->fft_plan);
    for (int k = 0; k < DEMOD_FFT_N; k++) {
        const int src = (k + DEMOD_FFT_N / 2) % DEMOD_FFT_N;
        const float re = s->fft_out[2 * src], im = s->fft_out[2 * src + 1];
        s->sweep_acc[k] += re * re + im * im;
    }
    if (++s->sweep_avg_n < 3) return;
    const float norm = 1.0f / ((float)DEMOD_FFT_N * 0.5f * (float)DEMOD_FFT_N * 0.5f * (float)s->sweep_avg_n);
    // DC spike of the RTL at the step centre: bridge +-3 bins.
    const int c = DEMOD_FFT_N / 2;
    for (int k = c - 3; k <= c + 3; k++) s->sweep_acc[k] = 0.5f * (s->sweep_acc[c - 4] + s->sweep_acc[c + 4]);
    const double fs = s->view_fs, span = s->sweep_hi - s->sweep_lo;
    // This step's points: cleared, then the strongest bin under each.
    {
        int ia = (int)((s->sweep_centre - 0.4 * fs - s->sweep_lo) / span * DEMOD_SWEEP_N);
        int ib = (int)((s->sweep_centre + 0.4 * fs - s->sweep_lo) / span * DEMOD_SWEEP_N);
        if (ia < 0) ia = 0;
        if (ib >= DEMOD_SWEEP_N) ib = DEMOD_SWEEP_N - 1;
        for (int i = ia; i <= ib; i++) s->sweep_db[i] = -200.0f;
    }
    for (int k = 0; k < DEMOD_FFT_N; k++) {
        const double f = ((double)k - DEMOD_FFT_N / 2) * fs / DEMOD_FFT_N;
        if (fabs(f) > 0.4 * fs) continue;
        const int idx = (int)((s->sweep_centre + f - s->sweep_lo) / span * DEMOD_SWEEP_N);
        if (idx < 0 || idx >= DEMOD_SWEEP_N) continue;
        const float db = 10.0f * log10f(s->sweep_acc[k] * norm + 1e-20f);
        if (isnan(s->sweep_db[idx]) || db > s->sweep_db[idx]) s->sweep_db[idx] = db;
    }
    if (++s->sweep_step >= s->sweep_steps) {   // a full sweep: floor + one waterfall row
        s->sweep_step = 0;
        float sample[DEMOD_SWEEP_N / 8];
        int ns = 0;
        for (int i = 0; i < DEMOD_SWEEP_N; i += 8) if (!isnan(s->sweep_db[i])) sample[ns++] = s->sweep_db[i];
        if (ns > 0) {
            qsort(sample, (size_t)ns, sizeof(float), demod_cmp_float);
            s->floor_db = s->spec_valid ? 0.8f * s->floor_db + 0.2f * sample[ns / 2] : sample[ns / 2];
            s->spec_valid = true;
        }
        gui_spectrogram_palette_lut(s->lut);
        s->wf_head = (s->wf_head + DEMOD_WF_H - 1) % DEMOD_WF_H;
        unsigned char *row = s->wf_pixels + (size_t)s->wf_head * DEMOD_WF_W * 4;
        const float lo = s->floor_db - 10.0f;
        for (int x = 0; x < DEMOD_WF_W; x++) {
            const int i0 = x * DEMOD_SWEEP_N / DEMOD_WF_W, i1 = (x + 1) * DEMOD_SWEEP_N / DEMOD_WF_W;
            float m = -200.0f;
            for (int i = i0; i < i1; i++) if (!isnan(s->sweep_db[i]) && s->sweep_db[i] > m) m = s->sweep_db[i];
            float v = (m - lo) / DEMOD_SPEC_SPAN_DB;
            v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            memcpy(row + (size_t)x * 4, s->lut[(int)(v * 255.0f + 0.5f)], 4);
        }
        s->wf_row_dirty = true;
    }
    demod_sweep_retune(s, app);
}

static double demod_tune_step_hz(const demod_state_t *s);   // defined below

// Band listening: one FFT of the live slice into the whole-band picture
// (smoothed), and a waterfall row ~10 times a second.
static void demod_sweep_live(demod_state_t *s, const int16_t *a, const int16_t *b, size_t count, double centre) {
    if (!b || !s->sweep_db || count < DEMOD_FFT_N / 2 || !demod_spectrum_alloc(s)) return;
    const float scale = 1.0f / 2048.0f;
    const size_t n = count < DEMOD_FFT_N ? count : DEMOD_FFT_N;
    const size_t start = count - n;
    for (size_t k = 0; k < DEMOD_FFT_N; k++) {
        const bool in = k < n;
        s->fft_in[2 * k] = in ? (float)a[start + k] * scale * s->fft_win[k] : 0.0f;
        s->fft_in[2 * k + 1] = in ? (float)b[start + k] * scale * s->fft_win[k] : 0.0f;
    }
    gui_fft_plan_execute(s->fft_plan);
    const float norm = 1.0f / ((float)DEMOD_FFT_N * 0.5f * (float)DEMOD_FFT_N * 0.5f);
    const double fs = s->view_fs, span = s->sweep_hi - s->sweep_lo;
    static float slice[DEMOD_SWEEP_N];
    int ia = (int)((centre - 0.4 * fs - s->sweep_lo) / span * DEMOD_SWEEP_N);
    int ib = (int)((centre + 0.4 * fs - s->sweep_lo) / span * DEMOD_SWEEP_N);
    if (ia < 0) ia = 0;
    if (ib >= DEMOD_SWEEP_N) ib = DEMOD_SWEEP_N - 1;
    for (int i = ia; i <= ib; i++) slice[i] = -200.0f;
    const int c = DEMOD_FFT_N / 2;
    for (int k = 0; k < DEMOD_FFT_N; k++) {
        const double f = ((double)k - c) * fs / DEMOD_FFT_N;
        if (fabs(f) > 0.4 * fs) continue;
        const int kk = (k >= c - 3 && k <= c + 3) ? (k < c ? c - 4 : c + 4) : k;   // bridge the DC spike
        const int src = (kk + DEMOD_FFT_N / 2) % DEMOD_FFT_N;
        const float re = s->fft_out[2 * src], im = s->fft_out[2 * src + 1];
        const float db = 10.0f * log10f((re * re + im * im) * norm + 1e-20f);
        const int idx = (int)((centre + f - s->sweep_lo) / span * DEMOD_SWEEP_N);
        if (idx >= ia && idx <= ib && db > slice[idx]) slice[idx] = db;
    }
    for (int i = ia; i <= ib; i++) {
        if (slice[i] <= -199.0f) continue;
        s->sweep_db[i] = isnan(s->sweep_db[i]) ? slice[i] : 0.5f * s->sweep_db[i] + 0.5f * slice[i];
    }
    const double now = GetTime();
    if (now - s->sweep_row_t < 0.1 || !s->wf_pixels) return;
    s->sweep_row_t = now;
    gui_spectrogram_palette_lut(s->lut);
    s->wf_head = (s->wf_head + DEMOD_WF_H - 1) % DEMOD_WF_H;
    unsigned char *row = s->wf_pixels + (size_t)s->wf_head * DEMOD_WF_W * 4;
    const float lo = s->floor_db - 10.0f;
    for (int x = 0; x < DEMOD_WF_W; x++) {
        const int i0 = x * DEMOD_SWEEP_N / DEMOD_WF_W, i1 = (x + 1) * DEMOD_SWEEP_N / DEMOD_WF_W;
        float m = -200.0f;
        for (int i = i0; i < i1; i++) if (!isnan(s->sweep_db[i]) && s->sweep_db[i] > m) m = s->sweep_db[i];
        float v = (m - lo) / DEMOD_SPEC_SPAN_DB;
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        memcpy(row + (size_t)x * 4, s->lut[(int)(v * 255.0f + 0.5f)], 4);
    }
    s->wf_row_dirty = true;
}

// Put the tuning line on f (absolute Hz, inside the band). The SDR retunes
// (line a quarter of the rate above its centre, clear of the DC spike) only
// when f leaves the clean middle of the live slice; rate-limited while
// dragging, saved when final.
static void demod_band_scrub_to(demod_state_t *s, gui_app_t *app, double f, bool final) {
    if (!s->sweep_on || !app || s->view_fs <= 0.0) return;
    const double snap = demod_tune_step_hz(s);
    f = round(f / snap) * snap;
    if (f < s->sweep_lo) f = s->sweep_lo;
    if (f > s->sweep_hi) f = s->sweep_hi;
    const double fs = s->view_fs;
    double c = s->scrub_centre;
    const double now = GetTime();
    const bool outside = c <= 0.0 || f < c - 0.4 * fs || f > c + 0.4 * fs || fabs(f - c) < 50000.0;
    if (outside && (final || c <= 0.0 || now - s->scrub_retune_t >= 0.033)) {
        c = f - 0.25 * fs;
        if (gui_app_sdr_retune_live(app, (uint64_t)(c + 0.5)) == 0) {
            s->scrub_centre = c;
            s->scrub_retune_t = now;
            app->settings.rtlsdr_freq_hz = (uint64_t)(c + 0.5);
        }
    }
    s->sweep_line_hz = f;
    double v = f - (double)app->settings.rtlsdr_freq_hz;
    if (v > 0.5 * fs) v = 0.5 * fs;
    if (v < -0.5 * fs) v = -0.5 * fs;
    s->vfo_hz = v;
    if (final) (void)gui_app_set_sdr_frequency(app, app->settings.rtlsdr_freq_hz);   // save
}

static void demod_band_scrub_x(demod_state_t *s, gui_app_t *app, float x, bool final) {
    const Rectangle r = s->tune_rect;
    if (r.width <= 0.0f) return;
    float t = (x - r.x) / r.width;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    demod_band_scrub_to(s, app, s->sweep_lo + (double)t * (s->sweep_hi - s->sweep_lo), final);
}

static void demod_format_hz(char *buf, size_t size, double hz, bool fine) {
    const double a = fabs(hz);
    if (a >= 1e6) snprintf(buf, size, fine ? "%.6f MHz" : "%.4gM", hz / 1e6);
    else if (a >= 1e3) snprintf(buf, size, fine ? "%.3f kHz" : "%.4gk", hz / 1e3);
    else snprintf(buf, size, fine ? "%.0f Hz" : "%.0f", hz);
}

// Snap step: the mode's (SDR++ defaults), or 100 Hz while Shift is held.
static double demod_tune_step_hz(const demod_state_t *s) {
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) return 100.0;
    return demod_snap_hz(s->view_mode, s->view_region);
}

static void demod_set_vfo(demod_state_t *s, double offset_hz) {
    if (s->full_hi > s->full_lo) {
        if (offset_hz > s->full_hi) offset_hz = s->full_hi;
        if (offset_hz < s->full_lo) offset_hz = s->full_lo;
    }
    s->vfo_hz = offset_hz;
}

static inline float demod_x_of(const demod_state_t *s, Rectangle r, double off) {
    return r.x + (float)((off - s->view_lo) / (s->view_hi - s->view_lo)) * r.width;
}

// Tune to a spot (x in the spectrum / waterfall), snapped like SDR++ (on the
// absolute frequency when an SDR gives one).
static void demod_tune_to_x(demod_state_t *s, float x) {
    const Rectangle r = s->tune_rect;
    if (r.width <= 0.0f || s->view_hi <= s->view_lo) return;
    const double off = s->view_lo + (double)(x - r.x) / (double)r.width * (s->view_hi - s->view_lo);
    const double snap = demod_tune_step_hz(s);
    const double abs_hz = round((s->view_center + off) / snap) * snap;
    demod_set_vfo(s, abs_hz - s->view_center);
}

static void demod_render_spectrum(demod_state_t *s, Rectangle spec, Rectangle wf, Color color, int bw) {
    const double center = s->view_center;
    const double lo_f = s->view_lo, hi_f = s->view_hi, span = hi_f - lo_f;
    const float lo = s->floor_db - 10.0f;
    DrawRectangleRec(spec, COLOR_METER_BG);
    DrawRectangleRec(wf, BLACK);
    // Waterfall (ring: newest row wf_head at the top).
    if (s->wf_pixels) {
        if (!s->wf_tex_valid) {
            Image img = { .data = s->wf_pixels, .width = DEMOD_WF_W, .height = DEMOD_WF_H,
                          .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
            s->wf_tex = LoadTextureFromImage(img);
            SetTextureFilter(s->wf_tex, TEXTURE_FILTER_BILINEAR);
            s->wf_tex_valid = s->wf_tex.id != 0;
            s->wf_full_upload = false;
            s->wf_row_dirty = false;
        } else if (s->wf_full_upload) {
            UpdateTexture(s->wf_tex, s->wf_pixels);
            s->wf_full_upload = s->wf_row_dirty = false;
        } else if (s->wf_row_dirty) {
            UpdateTextureRec(s->wf_tex, (Rectangle){ 0, (float)s->wf_head, DEMOD_WF_W, 1 },
                             s->wf_pixels + (size_t)s->wf_head * DEMOD_WF_W * 4);
            s->wf_row_dirty = false;
        }
        if (s->wf_tex_valid) {
            const int n1 = DEMOD_WF_H - s->wf_head;
            const float h1 = wf.height * (float)n1 / (float)DEMOD_WF_H;
            DrawTexturePro(s->wf_tex, (Rectangle){ 0, (float)s->wf_head, DEMOD_WF_W, (float)n1 },
                           (Rectangle){ wf.x, wf.y, wf.width, h1 }, (Vector2){ 0, 0 }, 0.0f, WHITE);
            if (s->wf_head > 0)
                DrawTexturePro(s->wf_tex, (Rectangle){ 0, 0, DEMOD_WF_W, (float)s->wf_head },
                               (Rectangle){ wf.x, wf.y + h1, wf.width, wf.height - h1 },
                               (Vector2){ 0, 0 }, 0.0f, WHITE);
        }
    }
    // dB grid (10 dB, coarser when the spectrum is short) with labels on the left.
    int db_step = 10;
    while ((float)db_step / DEMOD_SPEC_SPAN_DB * spec.height < 16.0f && db_step < 80) db_step *= 2;
    for (int db = (int)ceilf(lo / (float)db_step) * db_step; (float)db <= lo + DEMOD_SPEC_SPAN_DB; db += db_step) {
        const float y = spec.y + spec.height - ((float)db - lo) / DEMOD_SPEC_SPAN_DB * spec.height;
        DrawLineV((Vector2){ spec.x, y }, (Vector2){ spec.x + spec.width, y }, COLOR_GRID);
        if (y > spec.y + 22.0f && y < spec.y + spec.height - 14.0f) {
            char lbl[12];
            snprintf(lbl, sizeof(lbl), "%d", db);
            gui_text_draw_mono(lbl, spec.x + 4.0f, y - FONT_SIZE_OSC_SCALE / 2.0f, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
        }
    }
    // Frequency grid: a 1-2-5 step near span/8 (absolute when an SDR gives a centre).
    if (span > 0.0) {
        const double raw = span / 8.0;
        const double mag = pow(10.0, floor(log10(raw)));
        const double r = raw / mag;
        const double step = (r < 1.5 ? 1.0 : r < 3.5 ? 2.0 : r < 7.5 ? 5.0 : 10.0) * mag;
        for (double f = ceil((center + lo_f) / step) * step; f <= center + hi_f; f += step) {
            const float x = demod_x_of(s, spec, f - center);
            DrawLineV((Vector2){ x, spec.y }, (Vector2){ x, spec.y + spec.height }, COLOR_GRID);
            char lbl[24];
            demod_format_hz(lbl, sizeof(lbl), f, false);
            const int lw = gui_text_measure_mono(lbl, FONT_SIZE_OSC_SCALE);
            if (x - lw / 2 > spec.x + 30.0f && x + lw / 2 < spec.x + spec.width - 2.0f)
                gui_text_draw_mono(lbl, x - lw / 2, spec.y + spec.height - FONT_SIZE_OSC_SCALE - 2.0f,
                                   FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
        }
    }
    if (s->sweep_on && s->sweep_db && span > 0.0) {   // swept spectrum
        const int w = (int)spec.width;
        Vector2 prev = { 0 };
        bool have_prev = false;
        for (int px = 0; px < w; px++) {
            const int i0 = px * DEMOD_SWEEP_N / w;
            int i1 = (px + 1) * DEMOD_SWEEP_N / w;
            if (i1 <= i0) i1 = i0 + 1;
            float m = NAN;
            for (int i = i0; i < i1 && i < DEMOD_SWEEP_N; i++) if (!isnan(s->sweep_db[i]) && (isnan(m) || s->sweep_db[i] > m)) m = s->sweep_db[i];
            if (isnan(m)) { have_prev = false; continue; }
            float v = (m - lo) / DEMOD_SPEC_SPAN_DB;
            v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            const Vector2 cur = { spec.x + (float)px, spec.y + spec.height - v * spec.height };
            if (have_prev) DrawLineV(prev, cur, color);
            prev = cur;
            have_prev = true;
        }
        if (!s->sweep_listen) {   // where the hop is now
            const float xs = demod_x_of(s, spec, s->sweep_centre);
            DrawLineV((Vector2){ xs, spec.y }, (Vector2){ xs, spec.y + spec.height }, (Color){ 255, 255, 255, 60 });
        } else {   // the live slice
            const double c = s->scrub_centre;
            const float xa = demod_x_of(s, spec, c - 0.4 * s->view_fs), xb = demod_x_of(s, spec, c + 0.4 * s->view_fs);
            DrawRectangleRec((Rectangle){ xa, spec.y + spec.height - 3.0f, xb - xa, 3.0f }, (Color){ 120, 200, 255, 160 });
        }
    }
    // Spectrum trace (max of the bins under each pixel column).
    if (!s->sweep_on && s->spec_valid && span > 0.0) {
        const int w = (int)spec.width;
        Vector2 prev = { 0 };
        for (int px = 0; px < w; px++) {
            int b0, b1;
            demod_view_bins(s, lo_f + span * px / w, lo_f + span * (px + 1) / w, &b0, &b1);
            float m = s->spec_db[b0];
            for (int k = b0 + 1; k < b1 && k < DEMOD_FFT_N; k++) if (s->spec_db[k] > m) m = s->spec_db[k];
            float v = (m - lo) / DEMOD_SPEC_SPAN_DB;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            const Vector2 cur = { spec.x + (float)px, spec.y + spec.height - v * spec.height };
            if (px > 0) DrawLineV(prev, cur, color);
            prev = cur;
        }
    }
    // Tuning line + its channel (SSB: one side of the line); in the band view
    // only while listening, at its absolute frequency.
    if (span > 0.0 && (!s->sweep_on || s->sweep_listen)) {
        const int mode = s->view_mode;
        const double lf = s->sweep_on ? s->sweep_line_hz : s->vfo_hz;
        double f_lo = lf - 0.5 * bw, f_hi = lf + 0.5 * bw;
        if (mode == DEMOD_MODE_USB) { f_lo = lf; f_hi = lf + bw; }
        if (mode == DEMOD_MODE_LSB) { f_lo = lf - bw; f_hi = lf; }
        const float x0 = demod_x_of(s, spec, f_lo), x1 = demod_x_of(s, spec, f_hi);
        const float xv = demod_x_of(s, spec, lf);
        const float bw_w = x1 - x0 < 2.0f ? 2.0f : x1 - x0;
        DrawRectangleRec((Rectangle){ x0, spec.y, bw_w, spec.height + wf.height }, (Color){ 255, 255, 255, 38 });
        DrawLineEx((Vector2){ xv, spec.y }, (Vector2){ xv, wf.y + wf.height }, 1.5f, (Color){ 235, 60, 60, 255 });
        char lbl[32];
        if (s->sweep_on) demod_format_hz(lbl, sizeof(lbl), lf, true);
        else if (center != 0.0 || s->view_lo >= 0.0) demod_format_hz(lbl, sizeof(lbl), center + s->vfo_hz, true);
        else { char t[28]; demod_format_hz(t, sizeof(t), s->vfo_hz, true); snprintf(lbl, sizeof(lbl), "%s%s", s->vfo_hz > 0 ? "+" : "", t); }
        const int lw = gui_text_measure(lbl, FONT_SIZE_DROPDOWN_OPT);
        float lx = xv + 6.0f;
        if (lx + lw + 6.0f > spec.x + spec.width) lx = xv - 6.0f - (float)lw;
        const float ly = spec.y + 24.0f;
        DrawRectangleRec((Rectangle){ lx - 3.0f, ly - 1.0f, (float)lw + 6.0f, FONT_SIZE_DROPDOWN_OPT + 2.0f }, (Color){ 0, 0, 0, 150 });
        gui_text_draw(lbl, lx, ly, FONT_SIZE_DROPDOWN_OPT, (Color){ 255, 120, 120, 255 });
        char pw[24];
        snprintf(pw, sizeof(pw), "%.1f dBFS%s", (float)atomic_load(&s->sq_db10) / 10.0f, s->sq_open ? "" : " (SQL)");
        gui_text_draw_mono(pw, lx, ly + FONT_SIZE_DROPDOWN_OPT + 3.0f, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
    }
    // Hover readout (where a click would tune).
    const Vector2 mouse = gui_ui_get_mouse_position();
    if (!s->tune_drag && span > 0.0 && CheckCollisionPointRec(mouse, s->tune_rect)) {
        DrawLineV((Vector2){ mouse.x, spec.y }, (Vector2){ mouse.x, wf.y + wf.height }, (Color){ 255, 255, 255, 70 });
        const double off = lo_f + (double)(mouse.x - spec.x) / (double)spec.width * span;
        char lbl[32];
        demod_format_hz(lbl, sizeof(lbl), center + off, true);
        gui_text_draw_mono(lbl, mouse.x + 6.0f, wf.y + 4.0f, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
    }
}

//-----------------------------------------------------------------------------
// Frequency field commit + keystroke handling (SDR live-tune)
//-----------------------------------------------------------------------------


// Commit the typed frequency string to the active SDR (live-retune) and stop editing.
static void demod_commit_freq(demod_state_t *s, gui_app_t *app) {
    if (!s || !app) return;
    s->overlay.freq_editing = false;
    // MHz ("99.1"); a plain integer above 10000 is taken as Hz.
    const double v = strtod(s_demod_freq_str, NULL);
    unsigned long long hz = (strchr(s_demod_freq_str, '.') || v < 10000.0)
        ? (unsigned long long)(v * 1e6 + 0.5) : (unsigned long long)(v + 0.5);
    if (hz > 0 && (hz < DEMOD_SDR_MIN_HZ || hz > DEMOD_SDR_MAX_HZ)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "SDR frequency out of range (%.1f-%.0f MHz)",
                 DEMOD_SDR_MIN_HZ / 1e6, DEMOD_SDR_MAX_HZ / 1e6);
        gui_app_set_status(app, msg);
        hz = 0;
    }
    if (hz > 0) {
        // The box is the TUNED frequency (harrypm 2026-11-27: typing 97.1 left
        // the line at its old offset). Inside the received band only the line
        // moves; outside it the SDR retunes with the line 300 kHz above its
        // centre (clear of the RTL DC spike).
        const double centre = (double)app->settings.rtlsdr_freq_hz;
        const double fs = s->view_fs > 0.0 ? s->view_fs : 2.4e6;
        const double off = (double)hz - centre;
        if (centre > 0.0 && fabs(off) > 50000.0 && fabs(off) < 0.45 * fs) {
            s->vfo_hz = off;
            s->view_mid_valid = false;
        } else if (gui_app_set_sdr_frequency(app, (uint64_t)hz - 300000ULL) == 0) {
            s->vfo_hz = 300000.0;
            s->view_mid_valid = false;
        }
    } else if (app->settings.rtlsdr_freq_hz > 0) {
        // Empty/invalid edit: restore the current value.
        demod_freq_text(s_demod_freq_str, sizeof(s_demod_freq_str), app->settings.rtlsdr_freq_hz);
    }
}

// Handle keystrokes while the frequency field is being edited (called from render).
// Digits append at the cursor; Backspace deletes left of the cursor.
// Enter/Esc are handled by the caller (commit + live-retune).
static void demod_handle_freq_edit(demod_state_t *s) {
    if (!s || !s->overlay.freq_editing) return;
    int ch = GetCharPressed();
    while (ch > 0) {
        if ((ch >= '0' && ch <= '9') || (ch == '.' && !strchr(s_demod_freq_str, '.'))) {
            size_t len = strlen(s_demod_freq_str);
            if (len + 1 < sizeof(s_demod_freq_str)) {
                if (s->overlay.freq_cursor < 0) s->overlay.freq_cursor = 0;
                if ((size_t)s->overlay.freq_cursor > len) s->overlay.freq_cursor = (int)len;
                memmove(s_demod_freq_str + s->overlay.freq_cursor + 1,
                        s_demod_freq_str + s->overlay.freq_cursor,
                        len - (size_t)s->overlay.freq_cursor + 1);
                s_demod_freq_str[s->overlay.freq_cursor] = (char)ch;
                s->overlay.freq_cursor++;
            }
        }
        ch = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE)) {
        int cur = s->overlay.freq_cursor;
        size_t len = strlen(s_demod_freq_str);
        if (cur > 0 && (size_t)cur <= len) {
            memmove(s_demod_freq_str + cur - 1,
                    s_demod_freq_str + cur,
                    len - (size_t)cur + 1);
            s->overlay.freq_cursor = cur - 1;
        }
    }
}

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

static void demod_render_waveform(demod_state_t *s, Rectangle b, Color color) {
    DrawRectangleRec(b, COLOR_METER_BG);
    if (s->wave_pos < 2) return;
    float mid = b.y + b.height / 2.0f;
    float xstep = b.width / (float)DEMOD_WAVE_LEN;
    Vector2 prev = { b.x, mid };
    size_t start = (s->wave_pos >= DEMOD_WAVE_LEN) ? (s->wave_pos % DEMOD_WAVE_LEN) : 0;
    size_t avail = (s->wave_pos >= DEMOD_WAVE_LEN) ? DEMOD_WAVE_LEN : s->wave_pos;
    for (size_t k = 0; k < avail; k++) {
        size_t idx = (start + k) % DEMOD_WAVE_LEN;
        int16_t v = s->wave[idx];
        float y = mid - ((float)v / 32768.0f) * (b.height / 2.0f);
        Vector2 cur = { b.x + (float)k * xstep, y };
        if (k > 0) DrawLineEx(prev, cur, 1.5f, color);
        prev = cur;
    }
}

static void demod_vtable_render(void *state_ptr, gui_app_t *app, int channel,
                                Rectangle bounds, Color channel_color) {
    if (!state_ptr || !app) return;
    demod_state_t *s = (demod_state_t *)state_ptr;

    // Mode comes from settings (so the dropdown persists); keep state in sync.
    demod_mode_t want = (demod_mode_t)app->settings.demod_mode;
    if (want < 0 || want >= DEMOD_MODE_COUNT) want = DEMOD_MODE_WFM;
    if (s->mode != want) {
        s->mode = want;
        demod_reset_demod_state(s);
    }

    // SDR frequency field mirror + its keystrokes (Enter / Esc commit).
    s->overlay.is_sdr = demod_active_device_is_sdr(app);
    if (s->overlay.is_sdr && !s->overlay.freq_editing) {
        demod_freq_text(s_demod_freq_str, sizeof(s_demod_freq_str), app->settings.rtlsdr_freq_hz);
    }
    if (s->overlay.is_sdr && s->overlay.freq_editing) {
        demod_handle_freq_edit(s);
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
            demod_commit_freq(s, app);
        }
    }

    // Input: Auto = I/Q on an SDR, else this panel's channel as a real signal.
    const int ch = channel ? 1 : 0;
    const int input = s->input_sel == 1 ? 0 : s->input_sel == 2 ? 1 + ch : (s->overlay.is_sdr ? 0 : 1 + ch);
    atomic_store(&s->feed_input, input);
    // Feed + audio owner: the drawn Radio panel. A panel parked by a layout
    // change (still alive, not drawn) kept the feed and its audio while the
    // visible one ran the display-frame fallback = two audio streams, ~97k
    // frames/s into a 48k monitor (harrypm 2026-11-27: distortion on 99.1).
    s->last_render = GetTime();
    {
        demod_state_t *owner = atomic_load(&s_feed.active);
        if (owner != s && (owner == NULL || s->last_render - owner->last_render > 0.25))
            atomic_store(&s_feed.active, s);
    }
    if (atomic_load(&s_feed.active) == s) {
        atomic_store(&s_feed.app, app);
        atomic_store(&s_feed.need, input == 0 ? 3 : input == 1 ? 1 : 2);
    }

    // Is the continuous feed running this panel? (else: display frames)
    const double now = GetTime();
    const uint64_t blocks = atomic_load(&s->feed_blocks);
    if (blocks != s->seen_feed_blocks) { s->seen_feed_blocks = blocks; s->feed_seen_time = now; }
    const bool fed = now - s->feed_seen_time < 0.3;

    // View: I/Q shows -fs/2..fs/2 around the SDR centre, a real channel 0..fs/2.
    double fs = 0.0;
    if (fed) fs = (double)atomic_load(&s_feed.rate_hz[input == 2 ? 1 : 0]);
    if (fs <= 0.0) fs = demod_input_rate(app);
    s->view_fs = fs;
    s->view_center = (input == 0 && s->overlay.is_sdr) ? (double)app->settings.rtlsdr_freq_hz : 0.0;
    s->view_real = input != 0;
    if (s_radio_launch.zone >= 0 && !s->zone_launch_applied) {   // --radio range=
        s->zone = s_radio_launch.zone;
        s->zone_launch_applied = true;
    }
    // A tuning line beyond the shown zone (e.g. --radio tune=95.2M) picks its zone.
    if (s->view_real && fs > 0.0 && s->vfo_hz >= 0.0) {
        const int z = (int)(s->vfo_hz / (0.5 * fs));
        if (z != s->zone && (s->vfo_hz < s->zone * 0.5 * fs || s->vfo_hz > (s->zone + 1) * 0.5 * fs))
            s->zone = z < 0 ? 0 : z >= DEMOD_ZONES ? DEMOD_ZONES - 1 : z;
    }
    if (!s->view_real) s->zone = 0;
    s->full_lo = input == 0 ? -0.5 * fs : (double)s->zone * 0.5 * fs;
    s->full_hi = input == 0 ? 0.5 * fs : s->full_lo + 0.5 * fs;
    if (s->vfo_hz < s->full_lo || s->vfo_hz > s->full_hi) demod_set_vfo(s, s->vfo_hz);
    {   // Zoomed view around the tuning line; it recentres only when the line leaves it.
        const double span = (s->full_hi - s->full_lo) / (double)(s->zoom < 1 ? 1 : s->zoom);
        if (s->zoom <= 1) {
            s->view_mid = 0.5 * (s->full_lo + s->full_hi);
        } else {
            if (!s->view_mid_valid || s->vfo_hz < s->view_mid - 0.5 * span || s->vfo_hz > s->view_mid + 0.5 * span)
                s->view_mid = s->vfo_hz;
            if (s->view_mid < s->full_lo + 0.5 * span) s->view_mid = s->full_lo + 0.5 * span;
            if (s->view_mid > s->full_hi - 0.5 * span) s->view_mid = s->full_hi - 0.5 * span;
        }
        s->view_mid_valid = true;
        s->view_lo = s->view_mid - 0.5 * span;
        s->view_hi = s->view_mid + 0.5 * span;
    }
    s->view_mode = s->mode;
    if (s_radio_launch.region >= 0 && !s_radio_region_applied) {   // --radio region=
        app->settings.demod_region = s_radio_launch.region;
        s_radio_region_applied = true;
    }
    s->view_region = demod_region(app);

    if (fed) {
        if (atomic_load(&s->snap_seq) != s->snap_seen && !atomic_flag_test_and_set(&s->snap_lock)) {
            int16_t la[DEMOD_FFT_N], lb[DEMOD_FFT_N];
            const size_t n = s->snap_n;
            const bool iq = s->snap_iq;
            memcpy(la, s->snap_a, n * sizeof(int16_t));
            if (iq) memcpy(lb, s->snap_b, n * sizeof(int16_t));
            s->snap_seen = atomic_load(&s->snap_seq);
            atomic_flag_clear(&s->snap_lock);
            if (s->sweep_on && !s->sweep_listen) demod_sweep_capture(s, app, la, iq ? lb : NULL, n);
            else if (s->sweep_on) demod_sweep_live(s, la, iq ? lb : NULL, n, (double)app->settings.rtlsdr_freq_hz);
            else demod_spectrum_update(s, la, iq ? lb : NULL, n);
        }
    } else if (app->display_thread) {
        // No continuous feed (e.g. a remote source): the latest display frame.
        uint64_t fnow = atomic_load(&app->display_thread->samples.frame);
        if (fnow != s->last_consumed_frame) {
            const int16_t *sa = NULL, *sb = NULL;
            size_t sc = 0;
            if (gui_display_thread_acquire_samples(app->display_thread, &sa, &sb, &sc) && sc > 0) {
                const int16_t *ia = input == 2 ? sb : sa;
                const int16_t *qb = input == 0 ? sb : NULL;
                if (ia && !atomic_flag_test_and_set(&s->chain_lock)) {
                    demod_process_frame(s, app, ia, qb, sc, fs);
                    atomic_flag_clear(&s->chain_lock);
                }
                if (ia) demod_spectrum_update(s, ia, qb, sc);
                s->last_consumed_frame = fnow;
            }
        }
    }

    // Layout: spectrum (top), waterfall, demod audio strip (bottom).
    const float wave_h = fmaxf(36.0f, bounds.height * 0.18f);
    const float spec_h = (bounds.height - wave_h) * 0.45f;
    const Rectangle spec = { bounds.x, bounds.y, bounds.width, spec_h };
    const Rectangle wf = { bounds.x, bounds.y + spec_h, bounds.width, bounds.height - wave_h - spec_h };
    const Rectangle wave = { bounds.x, bounds.y + bounds.height - wave_h, bounds.width, wave_h };
    s->tune_rect = (Rectangle){ spec.x, spec.y, spec.width, spec.height + wf.height };
    s->tune_bb = demod_view_to_bb(s, s->vfo_hz);
    // --radio sweep=: start once the SDR stream runs (the feed gives its rate).
    if (s_radio_launch.sweep_hi > 0.0 && !s->sweep_launch_done && fed && s->overlay.is_sdr) {
        s->sweep_launch_done = demod_sweep_start(s, app, s_radio_launch.sweep_lo, s_radio_launch.sweep_hi);
    }
    if (s->sweep_on && (!s->overlay.is_sdr || !fed)) demod_sweep_stop(s, app);
    if (s->sweep_on) {   // the shown range is the swept one (absolute Hz)
        s->view_center = 0.0;
        s->view_lo = s->sweep_lo;
        s->view_hi = s->sweep_hi;
    }
    s->tune_inv = s->view_real && (s->zone & 1);
    s->scale_rect = (Rectangle){ spec.x, spec.y + spec.height - 18.0f, spec.width, 18.0f };
    s->app_ref = app;
    if (s->tune_drag) {
        const bool down = gui_ui_mouse_button_down(MOUSE_BUTTON_LEFT);
        if (s->sweep_on) demod_band_scrub_x(s, app, gui_ui_get_mouse_position().x, !down);
        else if (down) demod_tune_to_x(s, gui_ui_get_mouse_position().x);
        if (!down) s->tune_drag = false;
    }
    // Pan: right-drag anywhere in the spectrum / waterfall (left-drag on the
    // scale strip starts in the click handler).
    if (!s->pan_drag && !s->sweep_on && s->overlay.is_sdr && gui_ui_mouse_button_pressed(MOUSE_BUTTON_RIGHT) &&
        CheckCollisionPointRec(gui_ui_get_mouse_position(), s->tune_rect)) {
        s->pan_drag = true;
        s->pan_button = MOUSE_BUTTON_RIGHT;
        s->pan_last_x = gui_ui_get_mouse_position().x;
        s->pan_centre = (double)app->settings.rtlsdr_freq_hz;
    }
    if (s->pan_drag) {
        const float mx = gui_ui_get_mouse_position().x;
        const double df = -(double)(mx - s->pan_last_x) / (double)spec.width * (s->view_hi - s->view_lo);
        s->pan_last_x = mx;
        if (df != 0.0) demod_pan_by(s, app, df, true);
        if (!gui_ui_mouse_button_down(s->pan_button)) {   // release: save the final centre
            s->pan_drag = false;
            (void)gui_app_set_sdr_frequency(app, app->settings.rtlsdr_freq_hz);
        }
    }
    demod_render_spectrum(s, spec, wf, channel_color, demod_effective_bw(app, s->mode, fs));
    demod_render_waveform(s, wave, channel_color);
    DrawLineV((Vector2){ wave.x, wave.y }, (Vector2){ wave.x + wave.width, wave.y }, COLOR_GRID_MAJOR);
    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);

    gui_text_draw("Radio", bounds.x + 6, bounds.y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);
    demod_render_overlay(s, app, bounds);
}

//-----------------------------------------------------------------------------
// Overlay rendering (right-aligned vertical button stack)
//-----------------------------------------------------------------------------

// Compact control column (harrypm 2026-11-27: "make the buttons smaller"):
// 15 px text, 19 px rows, one width for every row (the freq row may be wider;
// harrypm 2026-11-27: 13 / 16 was "a bit too small").
#define RUI_FONT 15
#define RUI_H 19.0f
#define RUI_GAP 3.0f

static void rui_arrow(float x, float cy, bool up) {
    const float a = 4.0f;
    if (up) DrawTriangle((Vector2){ x + a / 2, cy - a / 2 }, (Vector2){ x, cy + a / 2 }, (Vector2){ x + a, cy + a / 2 }, COLOR_TEXT);
    else DrawTriangle((Vector2){ x + a / 2, cy + a / 2 }, (Vector2){ x + a, cy - a / 2 }, (Vector2){ x, cy - a / 2 }, COLOR_TEXT);
}

static void rui_button(Rectangle r, const char *label, bool dropdown, bool open) {
    const bool hov = CheckCollisionPointRec(gui_ui_get_mouse_position(), r);
    DrawRectangleRounded(r, 0.2f, 4, (open || hov) ? COLOR_BUTTON_HOVER : COLOR_BUTTON);
    const int lw = gui_text_measure(label, RUI_FONT);
    const float aw = dropdown ? 8.0f : 0.0f;
    const float x = r.x + (r.width - (float)lw - aw) / 2.0f;
    gui_text_draw(label, x, r.y + (r.height - RUI_FONT) / 2.0f, RUI_FONT, COLOR_TEXT);
    if (dropdown) rui_arrow(x + (float)lw + 4.0f, r.y + r.height / 2.0f, open);
}

static void rui_options(Rectangle anchor, const char *const *labels, int n, int sel, Rectangle *rects) {
    const float oh = RUI_H + 2.0f;
    DrawRectangleRec((Rectangle){ anchor.x, anchor.y + anchor.height, anchor.width, oh * n }, COLOR_PANEL_BG);
    const Vector2 m = gui_ui_get_mouse_position();
    for (int i = 0; i < n; i++) {
        const Rectangle r = { anchor.x, anchor.y + anchor.height + oh * i, anchor.width, oh };
        rects[i] = r;
        DrawRectangleRec(r, gui_dropdown_option_color(i == sel, CheckCollisionPointRec(m, r)));
        const int lw = gui_text_measure(labels[i], RUI_FONT);
        gui_text_draw(labels[i], r.x + (r.width - (float)lw) / 2.0f, r.y + (oh - RUI_FONT) / 2.0f, RUI_FONT, COLOR_TEXT);
    }
}

// [ - | label | + ] stepper (ends highlight on hover).
static void rui_stepper(Rectangle row, const char *label, Rectangle *minus, Rectangle *plus) {
    const float end_w = 18.0f;
    *minus = (Rectangle){ row.x, row.y, end_w, row.height };
    *plus = (Rectangle){ row.x + row.width - end_w, row.y, end_w, row.height };
    const Vector2 m = gui_ui_get_mouse_position();
    DrawRectangleRounded(row, 0.2f, 4, COLOR_BUTTON);
    if (CheckCollisionPointRec(m, *minus)) DrawRectangleRounded(*minus, 0.2f, 4, COLOR_BUTTON_HOVER);
    if (CheckCollisionPointRec(m, *plus)) DrawRectangleRounded(*plus, 0.2f, 4, COLOR_BUTTON_HOVER);
    DrawLineV((Vector2){ minus->x + end_w, row.y + 3.0f }, (Vector2){ minus->x + end_w, row.y + row.height - 3.0f }, COLOR_GRID_MAJOR);
    DrawLineV((Vector2){ plus->x, row.y + 3.0f }, (Vector2){ plus->x, row.y + row.height - 3.0f }, COLOR_GRID_MAJOR);
    const float ty = row.y + (row.height - RUI_FONT) / 2.0f;
    gui_text_draw("-", minus->x + (end_w - (float)gui_text_measure("-", RUI_FONT)) / 2.0f, ty, RUI_FONT, COLOR_TEXT);
    gui_text_draw("+", plus->x + (end_w - (float)gui_text_measure("+", RUI_FONT)) / 2.0f, ty, RUI_FONT, COLOR_TEXT);
    const int lw = gui_text_measure(label, RUI_FONT);
    gui_text_draw(label, row.x + (row.width - (float)lw) / 2.0f, ty, RUI_FONT, COLOR_TEXT);
}

static void demod_render_overlay(demod_state_t *s, gui_app_t *app, Rectangle bounds) {
    // Double-click (on the audio strip) toggles ALL the overlay chrome; while
    // hidden the control rectangles are stale and ignored (is_visible).
    s->overlay.is_visible = !gui_chrome_hide_hidden(&s->overlay.chrome_hide);
    if (!s->overlay.is_visible) return;

    static const char *const mode_labels[DEMOD_MODE_COUNT] = { "WFM", "NFM", "AM", "USB", "LSB" };
    static const char *const out_labels[2] = { "CH1/2", "CH3/4" };
    int bw_cur = 0;
    const int bw_mode = (s->mode >= 0 && s->mode < DEMOD_MODE_COUNT) ? (int)s->mode : 0;
    const int *bw_vals = s_bw_values[bw_mode];
    bw_cur = -1;
    for (int i = 0; i < DEMOD_BW_OPTION_COUNT; i++)
        if (app->settings.demod_bandwidth_hz == bw_vals[i]) { bw_cur = i; break; }
    static char bw_opt_buf[DEMOD_BW_OPTION_COUNT][20];
    const char *bw_opts[DEMOD_BW_OPTION_COUNT];
    for (int i = 0; i < DEMOD_BW_OPTION_COUNT; i++) {
        char v[12];
        demod_format_hz(v, sizeof(v), i ? bw_vals[i] : demod_auto_bw(bw_mode, demod_region(app)), false);
        snprintf(bw_opt_buf[i], sizeof(bw_opt_buf[i]), i ? "%s" : "Auto %s", v);
        bw_opts[i] = bw_opt_buf[i];
    }
    const float sql_val = app->settings.demod_squelch;
    const int out_val = app->settings.demod_output_pair ? 1 : 0;
    const int mode = (s->mode >= 0 && s->mode < DEMOD_MODE_COUNT) ? (int)s->mode : 0;

    // Labels (short).
    char in_lbl[16], zone_lbl[32], zoom_lbl[24], rgn_lbl[24], bw_lbl[16], sql_lbl[16], vol_lbl[16];
    static const char *const in_short[3] = { "Auto", "I/Q", "Real" };
    if (s->input_sel == 0) snprintf(in_lbl, sizeof(in_lbl), "In %s", s->view_real ? "Real" : "I/Q");
    else snprintf(in_lbl, sizeof(in_lbl), "In %s", in_short[s->input_sel]);
    zone_lbl[0] = 0;
    if (s->view_real && s->view_fs > 0.0) {
        char a[16], b[16];
        demod_format_hz(a, sizeof(a), s->full_lo, false);
        demod_format_hz(b, sizeof(b), s->full_hi, false);
        snprintf(zone_lbl, sizeof(zone_lbl), "%s-%s%s", a, b, (s->zone & 1) ? " inv" : "");
    }
    {
        char sp[16];
        demod_format_hz(sp, sizeof(sp), s->view_hi - s->view_lo, false);
        snprintf(zoom_lbl, sizeof(zoom_lbl), "%dx %s", s->zoom, sp);
    }
    const int rsel = app->settings.demod_region;
    if (rsel > DEMOD_REGION_AUTO && rsel < DEMOD_REGION_COUNT) snprintf(rgn_lbl, sizeof(rgn_lbl), "%s", s_region_names[rsel]);
    else snprintf(rgn_lbl, sizeof(rgn_lbl), "Auto %s", s_region_names[demod_region(app)]);
    if (bw_cur >= 0) snprintf(bw_lbl, sizeof(bw_lbl), "BW %s", bw_opts[bw_cur]);
    else {   // a value from a flag / older setting that the mode does not list
        char v[12];
        demod_format_hz(v, sizeof(v), app->settings.demod_bandwidth_hz, false);
        snprintf(bw_lbl, sizeof(bw_lbl), "BW %s", v);
    }
    if (sql_val <= 0.001f) snprintf(sql_lbl, sizeof(sql_lbl), "SQL off");
    else snprintf(sql_lbl, sizeof(sql_lbl), "SQL %.0f", -100.0f + 100.0f * sql_val);
    snprintf(vol_lbl, sizeof(vol_lbl), "VOL %.1f", app->settings.demod_volume);

    // One column width: the widest label (+ arrow / stepper ends).
    float w = 0.0f;
    const char *const plain[] = { in_lbl, zone_lbl, zoom_lbl, rgn_lbl };
    for (int i = 0; i < 4; i++) w = fmaxf(w, (float)gui_text_measure(plain[i], RUI_FONT) + 12.0f);
    w = fmaxf(w, (float)gui_text_measure("WFM", RUI_FONT) + 22.0f);
    w = fmaxf(w, (float)gui_text_measure(bw_lbl, RUI_FONT) + 22.0f);
    w = fmaxf(w, (float)gui_text_measure("BW Auto 250k", RUI_FONT) + 22.0f);
    w = fmaxf(w, (float)gui_text_measure("CH1/2", RUI_FONT) + 22.0f);
    w = fmaxf(w, (float)gui_text_measure("SQL -100", RUI_FONT) + 40.0f);
    w = fmaxf(w, (float)gui_text_measure(vol_lbl, RUI_FONT) + 40.0f);
    if (s->overlay.is_sdr) {
        w = fmaxf(w, (float)gui_text_measure("Gain 49.6", RUI_FONT) + 40.0f);
        w = fmaxf(w, (float)gui_text_measure("RTL AGC off", RUI_FONT) + 12.0f);
        w = fmaxf(w, (float)gui_text_measure("Rate 1.536M", RUI_FONT) + 22.0f);
    }
    const float x_right = bounds.x + bounds.width - 6.0f;
    const float x = x_right - w;
    float y = bounds.y + 6.0f;
#define RUI_ROW() (Rectangle){ x, (y += RUI_H + RUI_GAP) - (RUI_H + RUI_GAP), w, RUI_H }

    // SDR: [Band v][ 99.100 MHz ] (click the box to type MHz; Enter / Esc commits).
    s->overlay.band_btn_rect = (Rectangle){ 0 };
    if (s->overlay.is_sdr) {
        char fbuf[40];
        if (s->overlay.freq_editing) snprintf(fbuf, sizeof(fbuf), "%s", s_demod_freq_str);
        else snprintf(fbuf, sizeof(fbuf), "%.3f MHz", ((double)app->settings.rtlsdr_freq_hz + s->vfo_hz) / 1e6);
        const float band_w = (float)gui_text_measure("Band", RUI_FONT) + 20.0f;
        const float field_w = fmaxf(w, (float)gui_text_measure("1766.000 MHz", RUI_FONT) + 12.0f);
        const float fx = x_right - field_w;
        s->overlay.band_btn_rect = (Rectangle){ fx - band_w - RUI_GAP, y, band_w, RUI_H };
        s->overlay.freq_btn_rect = (Rectangle){ fx, y, field_w, RUI_H };
        rui_button(s->overlay.band_btn_rect, "Band", true, s->overlay.band_dropdown_open);
        DrawRectangleRounded(s->overlay.freq_btn_rect, 0.2f, 4,
                             s->overlay.freq_editing ? COLOR_BUTTON_HOVER : (Color){ 25, 25, 30, 255 });
        const float ty = y + (RUI_H - RUI_FONT) / 2.0f;
        gui_text_draw(fbuf, fx + 6.0f, ty, RUI_FONT, COLOR_TEXT);
        if (s->overlay.freq_editing && ((int)(GetTime() * 1.8) % 2) == 0) {
            const float cx = fx + 6.0f + (float)gui_text_measure(fbuf, RUI_FONT) + 1.0f;
            DrawLineV((Vector2){ cx, y + 2.0f }, (Vector2){ cx, y + RUI_H - 2.0f }, COLOR_TEXT);
            gui_text_draw("MHz", cx + 4.0f, ty, RUI_FONT, COLOR_TEXT_DIM);
        }
        y += RUI_H + RUI_GAP;
        char gl[24];
        if (app->settings.rtlsdr_gain_mode == 1) snprintf(gl, sizeof(gl), "Gain %.1f", app->settings.rtlsdr_gain_tenths_db / 10.0);
        else snprintf(gl, sizeof(gl), "Gain auto");
        rui_stepper(RUI_ROW(), gl, &s->overlay.gain_minus_rect, &s->overlay.gain_plus_rect);
        s->overlay.agc_btn_rect = RUI_ROW();
        rui_button(s->overlay.agc_btn_rect, app->settings.rtlsdr_agc ? "RTL AGC on" : "RTL AGC off", false, app->settings.rtlsdr_agc);
        char rl[24];
        snprintf(rl, sizeof(rl), "Rate %.4gM", app->settings.rtlsdr_sample_rate_hz / 1e6);
        s->overlay.rate_btn_rect = RUI_ROW();
        rui_button(s->overlay.rate_btn_rect, rl, true, s->overlay.rate_dropdown_open);
        s->overlay.sweep_btn_rect = (Rectangle){ 0 };   // the Band box picks the range
    } else {
        s->overlay.gain_minus_rect = s->overlay.gain_plus_rect = s->overlay.agc_btn_rect = (Rectangle){ 0 };
        s->overlay.rate_btn_rect = (Rectangle){ 0 };
        s->overlay.sweep_btn_rect = (Rectangle){ 0 };
    }
    s->overlay.in_btn_rect = RUI_ROW();
    rui_button(s->overlay.in_btn_rect, in_lbl, false, false);
    s->overlay.zone_btn_rect = (Rectangle){ 0 };
    if (zone_lbl[0]) {
        s->overlay.zone_btn_rect = RUI_ROW();
        rui_button(s->overlay.zone_btn_rect, zone_lbl, false, false);
    }
    s->overlay.zoom_btn_rect = RUI_ROW();
    rui_button(s->overlay.zoom_btn_rect, zoom_lbl, false, false);
    s->overlay.region_btn_rect = RUI_ROW();
    rui_button(s->overlay.region_btn_rect, rgn_lbl, false, false);
    s->overlay.mode_btn_rect = RUI_ROW();
    rui_button(s->overlay.mode_btn_rect, mode_labels[mode], true, s->overlay.mode_dropdown_open);
    s->overlay.bw_btn_rect = RUI_ROW();
    rui_button(s->overlay.bw_btn_rect, bw_lbl, true, s->overlay.bw_dropdown_open);
    rui_stepper(RUI_ROW(), sql_lbl, &s->overlay.sql_minus_rect, &s->overlay.sql_plus_rect);
    rui_stepper(RUI_ROW(), vol_lbl, &s->overlay.vol_minus_rect, &s->overlay.vol_plus_rect);
    s->overlay.out_btn_rect = RUI_ROW();
    rui_button(s->overlay.out_btn_rect, out_labels[out_val], true, s->overlay.out_dropdown_open);
#undef RUI_ROW

    // Open option lists, on top.
    if (s->overlay.mode_dropdown_open)
        rui_options(s->overlay.mode_btn_rect, mode_labels, DEMOD_MODE_COUNT, mode, s->overlay.mode_options_rect);
    if (s->overlay.bw_dropdown_open)
        rui_options(s->overlay.bw_btn_rect, bw_opts, DEMOD_BW_OPTION_COUNT, bw_cur, s->overlay.bw_options_rect);
    if (s->overlay.out_dropdown_open)
        rui_options(s->overlay.out_btn_rect, out_labels, 2, out_val, s->overlay.out_options_rect);
    if (s->overlay.sweep_dropdown_open && s->overlay.is_sdr) {
        const char *labels[DEMOD_SWEEP_COUNT];
        int sel = 0;
        for (int i = 0; i < DEMOD_SWEEP_COUNT; i++) {
            labels[i] = s_demod_sweeps[i].label;
            if (s->sweep_on && fabs(s_demod_sweeps[i].lo - s->sweep_lo) < 1.0 && fabs(s_demod_sweeps[i].hi - s->sweep_hi) < 1.0) sel = i;
        }
        if (s->sweep_on && sel == 0) sel = -1;   // a custom (flag) range
        rui_options(s->overlay.sweep_btn_rect, labels, DEMOD_SWEEP_COUNT, sel, s->overlay.sweep_options_rect);
    }
    if (s->overlay.rate_dropdown_open && s->overlay.is_sdr) {
        int sel = -1;
        for (int i = 0; i < DEMOD_RATE_COUNT; i++)
            if (s_demod_rates[i] == app->settings.rtlsdr_sample_rate_hz) sel = i;
        rui_options(s->overlay.rate_btn_rect, s_demod_rate_labels, DEMOD_RATE_COUNT, sel, s->overlay.rate_options_rect);
    }
    if (s->overlay.band_dropdown_open && s->overlay.is_sdr) {
        const char *labels[DEMOD_BAND_COUNT];
        int sel = -1;
        for (int i = 0; i < DEMOD_BAND_COUNT; i++) {
            labels[i] = s_demod_bands[i].label;
            const double c = s_demod_bands[i].centre_hz;
            if (fabs((double)app->settings.rtlsdr_freq_hz + s->vfo_hz - c) < 1.0e6) sel = i;
        }
        float bw_max = 0.0f;
        for (int i = 0; i < DEMOD_BAND_COUNT; i++) bw_max = fmaxf(bw_max, (float)gui_text_measure(labels[i], RUI_FONT) + 14.0f);
        Rectangle anchor = s->overlay.band_btn_rect;
        anchor.x = anchor.x + anchor.width - bw_max;
        anchor.width = bw_max;
        rui_options(anchor, labels, DEMOD_BAND_COUNT, sel, s->overlay.band_options_rect);
    }
}

//-----------------------------------------------------------------------------
// Overlay click handling
//-----------------------------------------------------------------------------

// Overlay control clicks (freq field, Mode/BW/Out dropdowns, SQL/VOL
// steppers). Returns true when a control consumed the click; false when no
// control was hit, so the caller can offer the click to the double-click
// chrome-hide tracker. Assumes the overlay is visible (is_visible).
static bool demod_handle_control_click(demod_state_t *s, gui_app_t *app,
                                       Vector2 click) {

    // Sweep presets.
    if (s->overlay.sweep_dropdown_open) {
        for (int i = 0; i < DEMOD_SWEEP_COUNT; i++) {
            if (!CheckCollisionPointRec(click, s->overlay.sweep_options_rect[i])) continue;
            s->overlay.sweep_dropdown_open = false;
            if (i == 0) demod_sweep_stop(s, app);
            else demod_sweep_start(s, app, s_demod_sweeps[i].lo, s_demod_sweeps[i].hi);
            return true;
        }
        s->overlay.sweep_dropdown_open = false;
        if (CheckCollisionPointRec(click, s->overlay.sweep_btn_rect)) return true;
    }
    if (s->overlay.sweep_btn_rect.width > 0.0f && CheckCollisionPointRec(click, s->overlay.sweep_btn_rect)) {
        s->overlay.sweep_dropdown_open = true;
        s->overlay.rate_dropdown_open = s->overlay.band_dropdown_open = false;
        s->overlay.mode_dropdown_open = s->overlay.bw_dropdown_open = s->overlay.out_dropdown_open = false;
        return true;
    }
    // SDR rate (span): restarts a running capture with the new rate.
    if (s->overlay.rate_dropdown_open) {
        for (int i = 0; i < DEMOD_RATE_COUNT; i++) {
            if (!CheckCollisionPointRec(click, s->overlay.rate_options_rect[i])) continue;
            s->overlay.rate_dropdown_open = false;
            if (gui_app_set_sdr_rate(app, s_demod_rates[i]) == 0) s->view_mid_valid = false;
            return true;
        }
        s->overlay.rate_dropdown_open = false;
        if (CheckCollisionPointRec(click, s->overlay.rate_btn_rect)) return true;
    }
    if (s->overlay.rate_btn_rect.width > 0.0f && CheckCollisionPointRec(click, s->overlay.rate_btn_rect)) {
        s->overlay.rate_dropdown_open = true;
        s->overlay.band_dropdown_open = s->overlay.mode_dropdown_open = false;
        s->overlay.bw_dropdown_open = s->overlay.out_dropdown_open = false;
        return true;
    }
    // Band presets: SDR centre 300 kHz below the band centre, line on it, mode set.
    if (s->overlay.band_dropdown_open) {
        for (int i = 0; i < DEMOD_BAND_COUNT; i++) {
            if (!CheckCollisionPointRec(click, s->overlay.band_options_rect[i])) continue;
            const demod_band_t *b = &s_demod_bands[i];
            if (s->view_fs > 0.0 && b->hi - b->lo > 0.8 * s->view_fs) {
                app->settings.demod_mode = b->mode;
                app->settings.demod_bandwidth_hz = 0;
                app->settings.rtlsdr_freq_hz = (uint64_t)(b->centre_hz - 300000.0);   // where it returns to
                gui_settings_save(&app->settings);
                s->vfo_hz = 300000.0;
                s->view_mid_valid = false;
                if (demod_sweep_start(s, app, b->lo, b->hi)) {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "Radio: %s - whole band; click a station to listen", b->label);
                    gui_app_set_status(app, msg);
                }
                s->overlay.band_dropdown_open = false;
                return true;
            }
            demod_sweep_stop(s, app);
            if (gui_app_set_sdr_frequency(app, (uint64_t)(b->centre_hz - 300000.0)) == 0) {
                s->vfo_hz = 300000.0;
                s->view_mid_valid = false;
                app->settings.demod_mode = b->mode;
                app->settings.demod_bandwidth_hz = 0;   // Auto for the band's mode
                gui_settings_save(&app->settings);
                char msg[64];
                snprintf(msg, sizeof(msg), "Radio: %s", b->label);
                gui_app_set_status(app, msg);
            }
            s->overlay.band_dropdown_open = false;
            return true;
        }
        s->overlay.band_dropdown_open = false;
        if (CheckCollisionPointRec(click, s->overlay.band_btn_rect)) return true;
    }
    if (s->overlay.band_btn_rect.width > 0.0f && CheckCollisionPointRec(click, s->overlay.band_btn_rect)) {
        s->overlay.band_dropdown_open = true;
        s->overlay.mode_dropdown_open = s->overlay.bw_dropdown_open = s->overlay.out_dropdown_open = false;
        return true;
    }
    // SDR gain stepper / RTL AGC (live).
    if (s->overlay.gain_minus_rect.width > 0.0f && CheckCollisionPointRec(click, s->overlay.gain_minus_rect)) {
        gui_app_sdr_gain_step(app, -1);
        return true;
    }
    if (s->overlay.gain_plus_rect.width > 0.0f && CheckCollisionPointRec(click, s->overlay.gain_plus_rect)) {
        gui_app_sdr_gain_step(app, +1);
        return true;
    }
    if (s->overlay.agc_btn_rect.width > 0.0f && CheckCollisionPointRec(click, s->overlay.agc_btn_rect)) {
        gui_app_sdr_set_agc(app, !app->settings.rtlsdr_agc);
        return true;
    }
    // Zoom cycle: 1x -> 2x -> .. -> 32x -> 1x, centred on the tuning line.
    if (CheckCollisionPointRec(click, s->overlay.zoom_btn_rect)) {
        s->zoom = s->zoom >= DEMOD_ZOOM_MAX ? 1 : s->zoom * 2;
        s->view_mid_valid = false;
        return true;
    }
    // Range cycle: the next RF zone, keeping the line's place in it.
    if (s->overlay.zone_btn_rect.width > 0.0f && CheckCollisionPointRec(click, s->overlay.zone_btn_rect)) {
        const double half = 0.5 * s->view_fs;
        const double inside = s->vfo_hz - (double)s->zone * half;
        s->zone = (s->zone + 1) % DEMOD_ZONES;
        s->vfo_hz = (double)s->zone * half + inside;
        return true;
    }
    // Region cycle: Auto -> EU/UK -> Americas -> Japan (saved).
    if (CheckCollisionPointRec(click, s->overlay.region_btn_rect)) {
        app->settings.demod_region = (app->settings.demod_region + 1) % DEMOD_REGION_COUNT;
        gui_settings_save(&app->settings);
        s->overlay.mode_dropdown_open = false;
        s->overlay.bw_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        return true;
    }
    // Input cycle: Auto -> I/Q -> Real.
    if (CheckCollisionPointRec(click, s->overlay.in_btn_rect)) {
        s->input_sel = (s->input_sel + 1) % 3;
        s->overlay.mode_dropdown_open = false;
        s->overlay.bw_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        return true;
    }

    // Frequency field (SDR only): click starts editing; Enter/Esc commits.
    if (s->overlay.is_sdr && CheckCollisionPointRec(click, s->overlay.freq_btn_rect)) {
        if (!s->overlay.freq_editing) {
            s->overlay.freq_editing = true;
            demod_freq_text(s_demod_freq_str, sizeof(s_demod_freq_str),
                            (uint64_t)((double)app->settings.rtlsdr_freq_hz + s->vfo_hz + 0.5));
            s->overlay.freq_cursor = (int)strlen(s_demod_freq_str);
        }
        s->overlay.mode_dropdown_open = false;
        s->overlay.bw_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        return true;
    }
    // Click outside the freq field while editing commits the typed value.
    if (s->overlay.freq_editing &&
        !CheckCollisionPointRec(click, s->overlay.freq_btn_rect)) {
        demod_commit_freq(s, app);
        // fall through so other controls can handle this click too
    }

    // Mode button toggles its dropdown (closes BW + Out dropdowns)
    if (CheckCollisionPointRec(click, s->overlay.mode_btn_rect)) {
        s->overlay.mode_dropdown_open = !s->overlay.mode_dropdown_open;
        s->overlay.bw_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        s->overlay.freq_editing = false;  // stop freq edit on mode click
        if (s->overlay.freq_editing) demod_commit_freq(s, app);
        return true;
    }
    // Mode dropdown options
    if (s->overlay.mode_dropdown_open) {
        for (int i = 0; i < DEMOD_MODE_COUNT; i++) {
            if (CheckCollisionPointRec(click, s->overlay.mode_options_rect[i])) {
                if (app->settings.demod_mode != i) app->settings.demod_bandwidth_hz = 0;   // BW back to Auto
                app->settings.demod_mode = i;
                gui_settings_save(&app->settings);
                s->mode = (demod_mode_t)i;  // apply immediately
                s->overlay.mode_dropdown_open = false;
                return true;
            }
        }
        s->overlay.mode_dropdown_open = false;  // click outside closes
    }
    // Bandwidth button toggles its dropdown (closes Mode + Out dropdowns)
    if (CheckCollisionPointRec(click, s->overlay.bw_btn_rect)) {
        if (s->overlay.freq_editing) demod_commit_freq(s, app);
        s->overlay.bw_dropdown_open = !s->overlay.bw_dropdown_open;
        s->overlay.mode_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        return true;
    }
    // Bandwidth dropdown options
    if (s->overlay.bw_dropdown_open) {
        for (int i = 0; i < DEMOD_BW_OPTION_COUNT; i++) {
            if (CheckCollisionPointRec(click, s->overlay.bw_options_rect[i])) {
                app->settings.demod_bandwidth_hz = s_bw_values[(s->mode >= 0 && s->mode < DEMOD_MODE_COUNT) ? (int)s->mode : 0][i];
                gui_settings_save(&app->settings);
                s->overlay.bw_dropdown_open = false;
                return true;
            }
        }
        s->overlay.bw_dropdown_open = false;  // click outside closes
        s->overlay.mode_dropdown_open = false;
    }

    // Squelch - / +
    if (CheckCollisionPointRec(click, s->overlay.sql_minus_rect)) {
        if (s->overlay.freq_editing) demod_commit_freq(s, app);
        float v = app->settings.demod_squelch - DEMOD_SQL_STEP;
        if (v < 0.0f) v = 0.0f;
        app->settings.demod_squelch = v;
        gui_settings_save(&app->settings);
        s->overlay.bw_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        s->overlay.mode_dropdown_open = false;
        return true;
    }
    if (CheckCollisionPointRec(click, s->overlay.sql_plus_rect)) {
        if (s->overlay.freq_editing) demod_commit_freq(s, app);
        float v = app->settings.demod_squelch + DEMOD_SQL_STEP;
        if (v > DEMOD_SQL_MAX) v = DEMOD_SQL_MAX;
        app->settings.demod_squelch = v;
        gui_settings_save(&app->settings);
        s->overlay.bw_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        s->overlay.mode_dropdown_open = false;
        return true;
    }
    // Volume - / +
    if (CheckCollisionPointRec(click, s->overlay.vol_minus_rect)) {
        if (s->overlay.freq_editing) demod_commit_freq(s, app);
        float v = app->settings.demod_volume - DEMOD_VOL_STEP;
        if (v < 0.0f) v = 0.0f;
        app->settings.demod_volume = v;
        gui_settings_save(&app->settings);
        s->overlay.bw_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        s->overlay.mode_dropdown_open = false;
        return true;
    }
    if (CheckCollisionPointRec(click, s->overlay.vol_plus_rect)) {
        if (s->overlay.freq_editing) demod_commit_freq(s, app);
        float v = app->settings.demod_volume + DEMOD_VOL_STEP;
        if (v > DEMOD_VOL_MAX) v = DEMOD_VOL_MAX;
        app->settings.demod_volume = v;
        gui_settings_save(&app->settings);
        s->overlay.bw_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        s->overlay.mode_dropdown_open = false;
        return true;
    }
    // Output pair button toggles its dropdown (closes Mode + BW dropdowns)
    if (CheckCollisionPointRec(click, s->overlay.out_btn_rect)) {
        if (s->overlay.freq_editing) demod_commit_freq(s, app);
        s->overlay.out_dropdown_open = !s->overlay.out_dropdown_open;
        s->overlay.bw_dropdown_open = false;
        s->overlay.mode_dropdown_open = false;
        return true;
    }
    if (s->overlay.out_dropdown_open) {
        for (int i = 0; i < 2; i++) {
            if (CheckCollisionPointRec(click, s->overlay.out_options_rect[i])) {
                app->settings.demod_output_pair = i;
                gui_settings_save(&app->settings);
                s->overlay.out_dropdown_open = false;
                return true;
            }
        }
        s->overlay.out_dropdown_open = false;
    }
    return false;
}

static bool demod_vtable_handle_click(void *state_ptr, gui_app_t *app, int channel,
                                      Vector2 click, Rectangle bounds) {
    (void)channel;
    if (!state_ptr || !app) return false;
    demod_state_t *s = (demod_state_t *)state_ptr;

    // Click outside the panel resets double-click tracking.
    if (!CheckCollisionPointRec(click, bounds)) {
        gui_chrome_hide_reset(&s->overlay.chrome_hide);
        return false;
    }

    // Controls only while the overlay is shown: while hidden the control
    // rectangles are stale (the overlay is not drawn), so ignore them
    // entirely; only the double-click restores the overlay. A consumed
    // control click also resets the tracker so a double-click ON a control
    // never toggles the chrome.
    if (s->overlay.is_visible &&
        demod_handle_control_click(s, app, click)) {
        gui_chrome_hide_reset(&s->overlay.chrome_hide);
        return true;
    }

    // Frequency scale strip: left-drag pans the band (SDR++ style).
    if (s->overlay.is_sdr && !s->sweep_on && CheckCollisionPointRec(click, s->scale_rect)) {
        s->pan_drag = true;
        s->pan_button = MOUSE_BUTTON_LEFT;
        s->pan_last_x = click.x;
        s->pan_centre = (double)app->settings.rtlsdr_freq_hz;
        gui_chrome_hide_reset(&s->overlay.chrome_hide);
        return true;
    }
    // Swept view: a click tunes the SDR there (line 300 kHz above its centre)
    // and stops the sweep.
    if (s->sweep_on && CheckCollisionPointRec(click, s->tune_rect)) {
        if (!s->sweep_listen) {   // stop hopping; the whole band stays on screen
            s->sweep_listen = true;
            s->scrub_centre = 0.0;   // the SDR sits on the last hop: retune now
        }
        demod_band_scrub_x(s, app, click.x, false);
        s->tune_drag = true;   // drag = linear scrub (render)
        gui_chrome_hide_reset(&s->overlay.chrome_hide);
        return true;
    }
    // Spectrum / waterfall: click (and drag, tracked in render) tunes.
    if (CheckCollisionPointRec(click, s->tune_rect)) {
        demod_tune_to_x(s, click.x);
        s->tune_drag = true;
        gui_chrome_hide_reset(&s->overlay.chrome_hide);
        return true;
    }

    // Double-click on the audio strip toggles ALL overlay chrome
    // (freq field, mode/bw/out dropdowns, SQL/VOL steppers) for a clean
    // demod waveform view.
    if (gui_chrome_hide_feed_click(&s->overlay.chrome_hide, click)) {
        s->overlay.mode_dropdown_open = false;
        s->overlay.bw_dropdown_open = false;
        s->overlay.out_dropdown_open = false;
        // Cancel a pending frequency edit without retuning: restore the
        // field text from the persisted setting.
        if (s->overlay.freq_editing) {
            s->overlay.freq_editing = false;
            demod_freq_text(s_demod_freq_str, sizeof(s_demod_freq_str), app->settings.rtlsdr_freq_hz);
        }
        return true;
    }
    return false;
}

static bool demod_vtable_handle_scroll(void *state_ptr, float delta, Rectangle bounds) {
    (void)bounds;
    if (!state_ptr || delta == 0.0f) return false;
    demod_state_t *s = (demod_state_t *)state_ptr;
    if (!CheckCollisionPointRec(gui_ui_get_mouse_position(), s->tune_rect)) return false;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {   // zoom in / out
        const int z = delta > 0.0f ? s->zoom * 2 : s->zoom / 2;
        s->zoom = z < 1 ? 1 : z > DEMOD_ZOOM_MAX ? DEMOD_ZOOM_MAX : z;
        s->view_mid_valid = false;
        return true;
    }
    // Wheel = scroll the band (harrypm 2026-11-27: right-drag panning felt
    // unintuitive; the wheel should scroll the limited view): a tenth of the
    // view per notch, up = higher. Shift + wheel steps the tuning line.
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!shift && s->app_ref && s->overlay.is_sdr && !s->sweep_on) {
        demod_pan_by(s, s->app_ref, (delta > 0.0f ? 0.1 : -0.1) * (s->view_hi - s->view_lo), false);
        return true;
    }
    if (s->sweep_on && s->sweep_listen && s->app_ref) {
        const double st = demod_tune_step_hz(s);
        demod_band_scrub_to(s, s->app_ref, s->sweep_line_hz + (delta > 0.0f ? st : -st), true);
        return true;
    }
    const double snap = demod_snap_hz(s->view_mode, s->view_region);   // Shift + wheel: the mode's step
    const double abs_hz = round((s->view_center + s->vfo_hz) / snap) * snap + (delta > 0.0f ? snap : -snap);
    demod_set_vfo(s, abs_hz - s->view_center);
    return true;
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------
static void *demod_vtable_create(void) {
    demod_state_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->mode = DEMOD_MODE_WFM;
    s->last_consumed_frame = 0;
    s->vfo_hz = s_radio_launch.tune_hz;
    s->input_sel = s_radio_launch.input;
    s->sq_gain = 1.0f;
    s->sq_open = true;
    atomic_init(&s->sq_db10, -1200);
    s->zoom = s_radio_launch.zoom;
    atomic_init(&s->feed_input, 0);
    atomic_init(&s->feed_blocks, 0);
    atomic_init(&s->snap_seq, 0);
    atomic_flag_clear(&s->chain_lock);
    atomic_flag_clear(&s->snap_lock);
    s->chan_mode = -1;
    s->post_c = s->nco_c = 1.0f;
    if (s_radio_launch.from_flag)
        fprintf(stderr, "[AUTO] Radio panel: tune=%+.0f Hz input=%s\n", s->vfo_hz, s_radio_input_names[s->input_sel]);
    s->wave_pos = 0;
    // Scratch buffers are always allocated so demod_process_frame can fill the
    // pre-resample mono buffer on the no-soxr pass-through path too.
    s->soxr_in_cap = 65536;
    s->soxr_in_buf = (int16_t *)malloc(s->soxr_in_cap * sizeof(int16_t));
    s->soxr_out_cap = 4096;
    s->soxr_out_buf = (int16_t *)malloc(s->soxr_out_cap * sizeof(int16_t));
    if (!s->soxr_in_buf || !s->soxr_out_buf) {
        free(s->soxr_in_buf); free(s->soxr_out_buf); free(s);
        return NULL;
    }
    s->audio_out_cap = 4096 * 2;
    s->audio_out = (int16_t *)malloc(s->audio_out_cap * sizeof(int16_t));
    if (!s->audio_out) {
        demod_soxr_destroy(s);
        free(s);
        return NULL;
    }
    radio_feed_attach(s);
    return s;
}

static void demod_vtable_destroy(void *state_ptr) {
    if (!state_ptr) return;
    demod_state_t *s = (demod_state_t *)state_ptr;
    radio_feed_detach(s);
    demod_soxr_destroy(s);
    free(s->soxr_in_buf);
    free(s->soxr_out_buf);
    free(s->audio_out);
    gui_fft_plan_destroy(s->fft_plan);
    gui_fft_free(s->fft_in);
    gui_fft_free(s->fft_out);
    free(s->wf_pixels);
    free(s->sweep_db);
    free(s->sweep_acc);
    if (s->wf_tex_valid) UnloadTexture(s->wf_tex);
    free(s);
}

static void demod_vtable_clear(void *state_ptr) {
    if (!state_ptr) return;
    demod_state_t *s = (demod_state_t *)state_ptr;
    demod_reset_demod_state(s);
    s->chan_mode = -1;   // re-derive the channel chain
    s->wave_pos = 0;
    s->last_consumed_frame = 0;
    memset(s->wave, 0, sizeof(s->wave));
    s->overlay.bw_dropdown_open = false;
    s->overlay.out_dropdown_open = false;
    s->overlay.mode_dropdown_open = false;
    s->overlay.freq_editing = false;
    s->overlay.is_visible = false;
}

//-----------------------------------------------------------------------------
// Vtable + Registration
//-----------------------------------------------------------------------------

// Double-click chrome-hide state, for the grid's pop-out button.
static bool demod_vtable_chrome_hidden(void *state) {
    const demod_state_t *p = (const demod_state_t *)state;
    return p && gui_chrome_hide_hidden(&p->overlay.chrome_hide);
}

static const panel_vtable_t s_demod_vtable = {
    .chrome_hidden = demod_vtable_chrome_hidden,
    .name = "Radio",
    .create = demod_vtable_create,
    .destroy = demod_vtable_destroy,
    .clear = demod_vtable_clear,
    .process = NULL,                 // DSP done in render() (needs both I+Q)
    .render = demod_vtable_render,
    .render_overlay = NULL,
    .handle_click = demod_vtable_handle_click,
    .handle_scroll = demod_vtable_handle_scroll,
    .get_menu_count = NULL,       // Mode dropdown rendered in overlay (UI never wires the panel-menu path)
    .get_menu = NULL,
};

void gui_demod_panel_register(void) {
    panel_register(PANEL_VIEW_DEMOD, &s_demod_vtable);
}
