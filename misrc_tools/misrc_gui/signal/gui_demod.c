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
#define DEMOD_DEEMPH_TAU_S    75e-6f   // WFM 75us de-emphasis time constant

// Frequency text field mirrors settings.rtlsdr_freq_hz (the active SDR's center freq).
// Synced in render: format Hz->str when not editing, parse str->Hz + live-retune
// via gui_app_set_sdr_frequency() when editing.
static char s_demod_freq_str[32] = {0};

// Bandwidth presets offered by the overlay dropdown (demod_bandwidth_hz).
// 0 = Auto (pick per mode default). The rest are manual overrides in Hz.
#define DEMOD_BW_OPTION_COUNT 7
static const int  DEMOD_BW_VALUES[DEMOD_BW_OPTION_COUNT] =
    { 0, 200000, 180000, 25000, 12500, 9000, 3000 };
static const char *DEMOD_BW_LABELS[DEMOD_BW_OPTION_COUNT] =
    { "Auto", "200k", "180k", "25k", "12.5k", "9k", "3k" };

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
// Demod chains. Input: normalized I,Q in [-1,1]. Output: mono float in [-1,1].
//-----------------------------------------------------------------------------

static float demod_discriminator(demod_state_t *s, float i, float q) {
    // atan2 of the phase difference via dot/cross (avoids wrap unwrap):
    //   dphi = atan2(I_prev*Q - Q_prev*I, I_prev*I + Q_prev*Q)
    float cross = s->prev_i * q - s->prev_q * i;
    float dot   = s->prev_i * i + s->prev_q * q;
    s->prev_i = i;
    s->prev_q = q;
    float dphi = atan2f(cross, dot);          // radians per sample
    // Scale to a reasonable audio amplitude: max FM deviation ~ +/-pi/sample
    // maps to full scale. Multiply by a fixed gain so broadcast FM sits near full scale.
    return dphi * 16000.0f;
}

static float demod_deemphasis(demod_state_t *s, float x, uint32_t fs) {
    // One-pole lowpass: y[n] = x[n] + a*y[n-1], a = exp(-1/(fs*tau))
    float a = expf(-1.0f / ((float)fs * DEMOD_DEEMPH_TAU_S));
    s->deemph_state = x + a * s->deemph_state;
    return s->deemph_state;
}

static float demod_am(demod_state_t *s, float i, float q) {
    float mag = sqrtf(i * i + q * q);
    // DC blocker: one-pole highpass, fc ~ 100 Hz
    float a = expf(-2.0f * (float)M_PI * 100.0f / (float)DEMOD_AUDIO_RATE_HZ);
    s->dc_state = (1.0f - a) * mag + a * s->dc_state;
    float afc = mag - s->dc_state;
    // AGC: track peak with fast attack / slow release
    if (afc < 0.0f) afc = -afc;
    if (afc > s->agc_peak) s->agc_peak = afc;
    else s->agc_peak *= 0.999f;
    if (s->agc_peak < 1e-6f) s->agc_peak = 1e-6f;
    return (afc / s->agc_peak) * 0.9f;
}

static float demod_sideband(demod_state_t *s, float i, float q, int upper) {
    // fs/4 method: shift the complex sample by +/- fs/4 so the chosen sideband
    // folds to baseband, then lowpass to audio bandwidth.
    // Oscillator at fs/4 cycles every 4 samples: phase index 0..3.
    // cos/sin table for +fs/4: [1,0,-1,0] / [0,1,0,-1]; -fs/4 inverts sin.
    static const float ctab[4] = { 1.0f, 0.0f, -1.0f, 0.0f };
    static const float stab[4] = { 0.0f, 1.0f, 0.0f, -1.0f };
    int p = s->sb_phase & 3;
    float c = ctab[p];
    float sn = upper ? stab[p] : -stab[p];
    s->sb_phase++;
    // Complex multiply (i+ jq) * (c + j sn) => real = i*c - q*sn
    float shifted_i = i * c - q * sn;
    (void)shifted_i;  // we keep the imaginary part as the audio (lowpassed real)
    // Actually take the real part after lowpass; use shifted_i lowpassed.
    // Simple one-pole lowpass at ~3 kHz audio cutoff relative to fs.
    // (Stateless here for brevity; use a per-instance LPF below.)
    return shifted_i;
}

// Per-instance lowpass for sideband audio (kept in the demod state via reuse of
// deemph_state field when in SSB mode to avoid growing the struct).
static float demod_sb_lpf(demod_state_t *s, float x, uint32_t fs) {
    float a = expf(-2.0f * (float)M_PI * 3000.0f / (float)fs);
    s->deemph_state = (1.0f - a) * x + a * s->deemph_state;
    return s->deemph_state;
}

//-----------------------------------------------------------------------------
// Process one display frame of I/Q -> mono audio at capture rate
//-----------------------------------------------------------------------------

static void demod_process_frame(demod_state_t *s, gui_app_t *app,
                                 const int16_t *iq_a, const int16_t *iq_b,
                                 size_t count) {
    uint32_t fs = atomic_load(&app->sample_rate);
    if (fs == 0) fs = 2400000U;
    const float scale = 1.0f / 2048.0f;   // 12-bit range -> [-1,1]

#if LIBSOXR_ENABLED
    if (!demod_soxr_ensure(s, fs)) return;
#endif
    // Cap to the input scratch capacity (allocated in create() / grown by
    // demod_soxr_ensure). Applies to both the soxr and no-soxr pass-through paths.
    if (count > s->soxr_in_cap) count = s->soxr_in_cap;

    int16_t *scratch_in = s->soxr_in_buf;
    size_t n = 0;
    for (size_t k = 0; k < count; k++) {
        float i = (float)iq_a[k] * scale;
        float q = (float)iq_b[k] * scale;
        float m = 0.0f;
        switch (s->mode) {
            case DEMOD_MODE_WFM:
                m = demod_discriminator(s, i, q);
                m = demod_deemphasis(s, m, fs);
                break;
            case DEMOD_MODE_NFM:
                m = demod_discriminator(s, i, q);
                break;
            case DEMOD_MODE_AM:
                m = demod_am(s, i, q);
                break;
            case DEMOD_MODE_USB:
            case DEMOD_MODE_LSB: {
                float sb = demod_sideband(s, i, q, s->mode == DEMOD_MODE_USB);
                m = demod_sb_lpf(s, sb, fs);
                break;
            }
            default:
                m = 0.0f;
        }
        // Clamp + convert to int16 (capture-rate mono, pre-resample)
        if (m > 1.0f) m = 1.0f;
        if (m < -1.0f) m = -1.0f;
        if (scratch_in) scratch_in[n] = (int16_t)(m * 32767.0f);
        n++;
    }
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

    // Squelch + volume
    float vol = app->settings.demod_volume;
    float sql = app->settings.demod_squelch;
    // Ensure audio_out buffer is large enough for stereo frames
    if (s->audio_out_cap < frames * 2) {
        int16_t *nb = (int16_t *)realloc(s->audio_out, frames * 2 * sizeof(int16_t));
        if (!nb) { s->audio_out_cap = 0; return; }
        s->audio_out = nb; s->audio_out_cap = frames * 2;
    }

    for (size_t k = 0; k < frames; k++) {
        int16_t a = audio[k];
        // Squelch: gate based on input level (use the pre-resample scratch level)
        if (sql > 0.0f && k < n) {
            int16_t lvl = scratch_in[k] < 0 ? (int16_t)(-scratch_in[k]) : scratch_in[k];
            if (lvl < (int16_t)(sql * 32767.0f)) a = 0;
        }
        int32_t v = (int32_t)(a * vol);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s->audio_out[k * 2 + 0] = (int16_t)v;
        s->audio_out[k * 2 + 1] = (int16_t)v;
        // Waveform history (mono)
        s->wave[s->wave_pos % DEMOD_WAVE_LEN] = (int16_t)v;
        s->wave_pos++;
    }

    // Push to audio monitor (stereo int16 @ 48k)
    gui_audio_push_demod(s->audio_out, frames);
}

//-----------------------------------------------------------------------------
// Frequency field commit + keystroke handling (SDR live-tune)
//-----------------------------------------------------------------------------


// Commit the typed frequency string to the active SDR (live-retune) and stop editing.
static void demod_commit_freq(demod_state_t *s, gui_app_t *app) {
    if (!s || !app) return;
    s->overlay.freq_editing = false;
    unsigned long long hz = strtoull(s_demod_freq_str, NULL, 10);
    if (hz > 0) {
        // gui_app_set_sdr_frequency persists + live-retunes the open SDR.
        if (gui_app_set_sdr_frequency(app, (uint64_t)hz) == 0) {
            // Keep the string in sync with the committed value.
            snprintf(s_demod_freq_str, sizeof(s_demod_freq_str), "%llu",
                     (unsigned long long)app->settings.rtlsdr_freq_hz);
        }
    } else if (app->settings.rtlsdr_freq_hz > 0) {
        // Empty/invalid edit: restore the current value.
        snprintf(s_demod_freq_str, sizeof(s_demod_freq_str), "%llu",
                 (unsigned long long)app->settings.rtlsdr_freq_hz);
    }
}

// Handle keystrokes while the frequency field is being edited (called from render).
// Digits append at the cursor; Backspace deletes left of the cursor.
// Enter/Esc are handled by the caller (commit + live-retune).
static void demod_handle_freq_edit(demod_state_t *s) {
    if (!s || !s->overlay.freq_editing) return;
    int ch = GetCharPressed();
    while (ch > 0) {
        if (ch >= '0' && ch <= '9') {
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
    (void)channel;
    if (!state_ptr || !app) return;
    demod_state_t *s = (demod_state_t *)state_ptr;

    // Mode comes from settings (so the dropdown persists); keep state in sync.
    demod_mode_t want = (demod_mode_t)app->settings.demod_mode;
    if (want < 0 || want >= DEMOD_MODE_COUNT) want = DEMOD_MODE_WFM;
    if (s->mode != want) {
        s->mode = want;
        s->prev_i = s->prev_q = 0.0f;
        s->deemph_state = s->dc_state = s->agc_peak = 0.0f;
        s->sb_phase = 0;
    }

    // Cache whether the active capture device is an SDR (so the overlay shows
    // the frequency tune field only for SDR sources) and keep the freq text
    // field mirrored to settings.rtlsdr_freq_hz when not actively editing it.
    s->overlay.is_sdr = demod_active_device_is_sdr(app);
    if (s->overlay.is_sdr && !s->overlay.freq_editing) {
        snprintf(s_demod_freq_str, sizeof(s_demod_freq_str), "%llu",
                 (unsigned long long)app->settings.rtlsdr_freq_hz);
    }
    // While the frequency field is being edited, handle its keystrokes here
    // (render runs every frame; handle_click only runs on click). Enter/Esc
    // commits the typed value and live-retunes the SDR.
    if (s->overlay.is_sdr && s->overlay.freq_editing) {
        demod_handle_freq_edit(s);
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
            demod_commit_freq(s, app);
        }
    }

    // Pull a new display frame of I/Q when available.
    if (app->display_thread) {
        uint64_t fnow = atomic_load(&app->display_thread->samples.frame);
        if (fnow != s->last_consumed_frame) {
            const int16_t *sa = NULL, *sb = NULL;
            size_t sc = 0;
            if (gui_display_thread_acquire_samples(app->display_thread, &sa, &sb, &sc) && sc > 0) {
                demod_process_frame(s, app, sa, sb, sc);
                s->last_consumed_frame = fnow;
            }
        }
    }

    // Draw the demod audio waveform.
    demod_render_waveform(s, bounds, channel_color);
    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);

    // Panel label (top-left).
    gui_text_draw("Radio", bounds.x + 6, bounds.y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);

    // Overlay controls (right-aligned vertical stack, CVBS-style).
    // Mode (WFM/NFM/AM/USB/LSB), Bandwidth, Squelch, Volume, Output pair.
    demod_render_overlay(s, app, bounds);
}

//-----------------------------------------------------------------------------
// Overlay rendering (right-aligned vertical button stack)
//-----------------------------------------------------------------------------

static void demod_render_overlay(demod_state_t *s, gui_app_t *app, Rectangle bounds) {
    // Double-click on the panel (demod_vtable_handle_click) toggles ALL the
    // overlay chrome — freq field, mode/bw/out dropdowns, SQL/VOL steppers
    // — for a clean demod waveform view. is_visible mirrors the flag so
    // clicks only reach controls that are actually drawn (while hidden the
    // control rectangles are stale and ignored).
    s->overlay.is_visible = !gui_chrome_hide_hidden(&s->overlay.chrome_hide);
    if (!s->overlay.is_visible) return;

    const float btn_h = 18.0f;
    const float gap = 4.0f;
    const float row_step = btn_h + gap;

    // Resolve current settings -> display values
    int bw_cur = 0;  // index into DEMOD_BW_VALUES; default Auto (0) if no match
    for (int i = 0; i < DEMOD_BW_OPTION_COUNT; i++) {
        if (app->settings.demod_bandwidth_hz == DEMOD_BW_VALUES[i]) { bw_cur = i; break; }
    }
    float sql_val = app->settings.demod_squelch;
    float vol_val = app->settings.demod_volume;
    int out_val = app->settings.demod_output_pair;

    char bw_buf[24];
    char sql_buf[20];
    char vol_buf[20];
    const char *out_label = (out_val == 1) ? "CH3/4" : "CH1/2";

    // Measure labels so every button in a row is the same width.
    float bw_text_w  = (float)gui_text_measure("BW: Auto", FONT_SIZE_DROPDOWN_OPT);
    float sql_text_w = (float)gui_text_measure("SQL: Off", FONT_SIZE_DROPDOWN_OPT);
    float vol_text_w = (float)gui_text_measure("VOL: 1.0", FONT_SIZE_DROPDOWN_OPT);
    float out_text_w = (float)gui_text_measure("Out: CH1/2", FONT_SIZE_DROPDOWN_OPT);

    float bw_btn_w  = bw_text_w  + 18.0f; if (bw_btn_w  < 100.0f) bw_btn_w  = 100.0f;
    float sql_btn_w = sql_text_w + 18.0f; if (sql_btn_w < 110.0f) sql_btn_w = 110.0f;
    float vol_btn_w = vol_text_w + 18.0f; if (vol_btn_w < 110.0f) vol_btn_w = 110.0f;
    float out_btn_w = out_text_w + 18.0f; if (out_btn_w < 116.0f) out_btn_w = 116.0f;

    float col_btn_w = bw_btn_w;
    if (sql_btn_w > col_btn_w) col_btn_w = sql_btn_w;
    if (vol_btn_w > col_btn_w) col_btn_w = vol_btn_w;
    if (out_btn_w > col_btn_w) col_btn_w = out_btn_w;

    float x_right = bounds.x + bounds.width - 8.0f;
    float col_x = x_right - col_btn_w;
    float btn_y0 = bounds.y + 8.0f;
    int row = 0;

    // Row 0 (SDR only): Frequency (Hz) live-tune field.
    // Shown only when the active capture device is an SDR, so the demod
    // panel can tune it live (dial in a station). Editing the text commits
    // via gui_app_set_sdr_frequency() which live-retunes the open SDR.
    if (s->overlay.is_sdr) {
        const char *freq_lbl = "Freq (Hz):";
        int freq_lbl_w = gui_text_measure(freq_lbl, FONT_SIZE_DROPDOWN_OPT);
        float freq_field_w = 150.0f;
        float freq_row_w = (float)freq_lbl_w + freq_field_w + 8.0f;
        if (freq_row_w > col_btn_w) col_btn_w = freq_row_w;
        col_x = x_right - col_btn_w;
        float freq_field_x = col_x + (col_btn_w - freq_field_w);
        float freq_y = btn_y0 + row_step * row++;
        s->overlay.freq_btn_rect = (Rectangle){freq_field_x, freq_y, freq_field_w, btn_h};
        // Field background (highlighted when editing)
        Color freq_bg = s->overlay.freq_editing ? COLOR_BUTTON_HOVER : (Color){25,25,30,255};
        DrawRectangleRounded(s->overlay.freq_btn_rect, 0.15f, 4, freq_bg);
        // Label to the left of the field
        gui_text_draw(freq_lbl, col_x + 2.0f,
                   freq_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                   FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);
        // Editable text: show a blinking caret while editing, else static text.
        const char *fstr = s_demod_freq_str[0] ? s_demod_freq_str : "0";
        if (s->overlay.freq_editing) {
            bool caret_on = ((int)(GetTime() * 1.8) % 2) == 0;
            gui_text_draw(fstr, freq_field_x + 6.0f, freq_y + 2.0f, FONT_SIZE_STATS, COLOR_TEXT);
            if (caret_on) {
                // Caret at the end of the text (simple; no per-glyph cursor mapping).
                int tw = gui_text_measure_mono(fstr, FONT_SIZE_STATS);
                DrawLineEx((Vector2){freq_field_x + 6.0f + (float)tw, freq_y + 2.0f},
                              (Vector2){freq_field_x + 6.0f + (float)tw, freq_y + btn_h - 2.0f},
                              1.0f, COLOR_TEXT);
            }
        } else {
            gui_text_draw(fstr, freq_field_x + 6.0f, freq_y + 2.0f, FONT_SIZE_STATS, COLOR_TEXT);
        }
        // Hint to the right of the field
        const char *freq_hint = "(click to tune)";
        gui_text_draw(freq_hint, freq_field_x + freq_field_w + 6.0f,
                   freq_y + (btn_h - FONT_SIZE_STATS) / 2.0f, FONT_SIZE_STATS, COLOR_TEXT_DIM);
    }

    // Row 1: Mode dropdown (WFM/NFM/AM/USB/LSB)
    static const char *mode_labels[DEMOD_MODE_COUNT] = { "WFM", "NFM", "AM", "USB", "LSB" };
    const char *mode_label = (s->mode >= 0 && s->mode < DEMOD_MODE_COUNT) ? mode_labels[s->mode] : "?";
    float mode_text_w = (float)gui_text_measure(mode_label, FONT_SIZE_DROPDOWN_OPT);
    float mode_btn_w = mode_text_w + 18.0f; if (mode_btn_w < 96.0f) mode_btn_w = 96.0f;
    if (mode_btn_w > col_btn_w) col_btn_w = mode_btn_w;
    // Recompute col_x/col_btn_w now that mode column width is known.
    col_x = x_right - col_btn_w;
    float mode_btn_x = col_x + (col_btn_w - mode_btn_w);
    float mode_btn_y = btn_y0 + row_step * row++;
    s->overlay.mode_btn_rect = (Rectangle){mode_btn_x, mode_btn_y, mode_btn_w, btn_h};
    bool mode_open = s->overlay.mode_dropdown_open;
    Color mode_bg = mode_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    DrawRectangleRounded(s->overlay.mode_btn_rect, 0.15f, 4, mode_bg);
    int mode_lbl_w = gui_text_measure(mode_label, FONT_SIZE_DROPDOWN_OPT);
    float mode_arrow_w = 8.0f;
    float mode_total_w = (float)mode_lbl_w + mode_arrow_w + 4.0f;
    gui_text_draw(mode_label, mode_btn_x + (mode_btn_w - mode_total_w) / 2.0f,
               mode_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
               FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    float arrow_size = 5.0f;
    float mode_arrow_x = mode_btn_x + (mode_btn_w - mode_lbl_w) / 2.0f + mode_lbl_w + 6.0f;
    float mode_arrow_cy = mode_btn_y + btn_h / 2.0f;
    if (mode_open) {
        Vector2 top = { mode_arrow_x + arrow_size/2, mode_arrow_cy - arrow_size/2 };
        Vector2 left = { mode_arrow_x, mode_arrow_cy + arrow_size/2 };
        Vector2 right = { mode_arrow_x + arrow_size, mode_arrow_cy + arrow_size/2 };
        DrawTriangle(top, left, right, COLOR_TEXT);
    } else {
        Vector2 bottom = { mode_arrow_x + arrow_size/2, mode_arrow_cy + arrow_size/2 };
        Vector2 left = { mode_arrow_x, mode_arrow_cy - arrow_size/2 };
        Vector2 right = { mode_arrow_x + arrow_size, mode_arrow_cy + arrow_size/2 };
        DrawTriangle(bottom, right, left, COLOR_TEXT);
    }

    // Row 2: Bandwidth dropdown
    float bw_btn_x = col_x + (col_btn_w - bw_btn_w);
    float bw_btn_y = btn_y0 + row_step * row++;
    s->overlay.bw_btn_rect = (Rectangle){bw_btn_x, bw_btn_y, bw_btn_w, btn_h};
    bool bw_open = s->overlay.bw_dropdown_open;
    Color bw_bg = bw_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    DrawRectangleRounded(s->overlay.bw_btn_rect, 0.15f, 4, bw_bg);
    snprintf(bw_buf, sizeof(bw_buf), "BW: %s", DEMOD_BW_LABELS[bw_cur]);
    int bw_lbl_w = gui_text_measure(bw_buf, FONT_SIZE_DROPDOWN_OPT);
    float bw_arrow_w = 8.0f;
    float bw_total_w = (float)bw_lbl_w + bw_arrow_w + 4.0f;
    gui_text_draw(bw_buf, bw_btn_x + (bw_btn_w - bw_total_w) / 2.0f,
               bw_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
               FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    // Dropdown arrow
    float arrow_x = bw_btn_x + (bw_btn_w - bw_lbl_w) / 2.0f + bw_lbl_w + 6.0f;
    float arrow_cy = bw_btn_y + btn_h / 2.0f;
    if (bw_open) {
        Vector2 top = { arrow_x + arrow_size/2, arrow_cy - arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy + arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy + arrow_size/2 };
        DrawTriangle(top, left, right, COLOR_TEXT);
    } else {
        Vector2 bottom = { arrow_x + arrow_size/2, arrow_cy + arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy - arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy - arrow_size/2 };
        DrawTriangle(bottom, right, left, COLOR_TEXT);
    }

    // Row 3: Squelch stepper  [ -  SQL: x.x  + ]
    float sql_btn_y = btn_y0 + row_step * row++;
    float sql_minus_x = col_x + (col_btn_w - sql_btn_w);
    s->overlay.sql_minus_rect = (Rectangle){sql_minus_x, sql_btn_y, 28.0f, btn_h};
    s->overlay.sql_plus_rect  = (Rectangle){sql_minus_x + sql_btn_w - 28.0f, sql_btn_y, 28.0f, btn_h};
    DrawRectangleRounded(s->overlay.sql_minus_rect, 0.15f, 4, COLOR_BUTTON);
    DrawRectangleRounded(s->overlay.sql_plus_rect,  0.15f, 4, COLOR_BUTTON);
    gui_text_draw("-", sql_minus_x + 14.0f, sql_btn_y + 1.0f, FONT_SIZE_NORMAL, COLOR_TEXT);
    gui_text_draw("+", sql_minus_x + sql_btn_w - 14.0f, sql_btn_y + 1.0f, FONT_SIZE_NORMAL, COLOR_TEXT);
    snprintf(sql_buf, sizeof(sql_buf), "SQL: %s", (sql_val <= 0.001f) ? "Off" : "");
    if (sql_val > 0.001f) snprintf(sql_buf + strlen(sql_buf), sizeof(sql_buf) - strlen(sql_buf), "%.1f", sql_val);
    int sql_lbl_w = gui_text_measure(sql_buf, FONT_SIZE_DROPDOWN_OPT);
    gui_text_draw(sql_buf, sql_minus_x + 28.0f + (sql_btn_w - 56.0f - sql_lbl_w) / 2.0f,
              sql_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Row 4: Volume stepper  [ -  VOL: x.x  + ]
    float vol_btn_y = btn_y0 + row_step * row++;
    float vol_minus_x = col_x + (col_btn_w - vol_btn_w);
    s->overlay.vol_minus_rect = (Rectangle){vol_minus_x, vol_btn_y, 28.0f, btn_h};
    s->overlay.vol_plus_rect  = (Rectangle){vol_minus_x + vol_btn_w - 28.0f, vol_btn_y, 28.0f, btn_h};
    DrawRectangleRounded(s->overlay.vol_minus_rect, 0.15f, 4, COLOR_BUTTON);
    DrawRectangleRounded(s->overlay.vol_plus_rect,  0.15f, 4, COLOR_BUTTON);
    gui_text_draw("-", vol_minus_x + 14.0f, vol_btn_y + 1.0f, FONT_SIZE_NORMAL, COLOR_TEXT);
    gui_text_draw("+", vol_minus_x + vol_btn_w - 14.0f, vol_btn_y + 1.0f, FONT_SIZE_NORMAL, COLOR_TEXT);
    snprintf(vol_buf, sizeof(vol_buf), "VOL: %.1f", vol_val);
    int vol_lbl_w = gui_text_measure(vol_buf, FONT_SIZE_DROPDOWN_OPT);
    gui_text_draw(vol_buf, vol_minus_x + 28.0f + (vol_btn_w - 56.0f - vol_lbl_w) / 2.0f,
              vol_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Row 5: Output pair dropdown
    float out_btn_y = btn_y0 + row_step * row++;
    float out_btn_x = col_x + (col_btn_w - out_btn_w);
    s->overlay.out_btn_rect = (Rectangle){out_btn_x, out_btn_y, out_btn_w, btn_h};
    bool out_open = s->overlay.out_dropdown_open;
    Color out_bg = out_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    DrawRectangleRounded(s->overlay.out_btn_rect, 0.15f, 4, out_bg);
    int out_lbl_w = gui_text_measure(out_label, FONT_SIZE_DROPDOWN_OPT);
    float out_total_w = (float)out_lbl_w + bw_arrow_w + 4.0f;
    gui_text_draw(out_label, out_btn_x + (out_btn_w - out_total_w) / 2.0f,
              out_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    float out_arrow_x = out_btn_x + (out_btn_w - out_lbl_w) / 2.0f + out_lbl_w + 6.0f;
    float out_arrow_cy = out_btn_y + btn_h / 2.0f;
    if (out_open) {
        Vector2 top = { out_arrow_x + arrow_size/2, out_arrow_cy - arrow_size/2 };
        Vector2 left = { out_arrow_x, out_arrow_cy + arrow_size/2 };
        Vector2 right = { out_arrow_x + arrow_size, out_arrow_cy + arrow_size/2 };
        DrawTriangle(top, left, right, COLOR_TEXT);
    } else {
        Vector2 bottom = { out_arrow_x + arrow_size/2, out_arrow_cy + arrow_size/2 };
        Vector2 left = { out_arrow_x, out_arrow_cy - arrow_size/2 };
        Vector2 right = { out_arrow_x + arrow_size, out_arrow_cy - arrow_size/2 };
        DrawTriangle(bottom, right, left, COLOR_TEXT);
    }

    // Dropdown option lists (drawn below their button when open)
    if (mode_open) {
        float opt_y = mode_btn_y + btn_h;
        float opt_h = 20.0f;
        DrawRectangleRounded((Rectangle){mode_btn_x, opt_y, mode_btn_w, opt_h * DEMOD_MODE_COUNT},
                             0.1f, 4, COLOR_PANEL_BG);
        for (int i = 0; i < DEMOD_MODE_COUNT; i++) {
            Rectangle opt_rect = {mode_btn_x, opt_y + i * opt_h, mode_btn_w, opt_h};
            s->overlay.mode_options_rect[i] = opt_rect;
            bool is_sel = (i == (int)s->mode);
            Vector2 mouse = gui_ui_get_mouse_position();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);
            Color opt_bg = gui_dropdown_option_color(is_sel, hover);
            DrawRectangleRec(opt_rect, opt_bg);
            int ow = gui_text_measure(mode_labels[i], FONT_SIZE_DROPDOWN_OPT);
            gui_text_draw(mode_labels[i], mode_btn_x + mode_btn_w/2.0f - ow/2.0f,
                      opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
    if (bw_open) {
        float opt_y = bw_btn_y + btn_h;
        float opt_h = 20.0f;
        DrawRectangleRounded((Rectangle){bw_btn_x, opt_y, bw_btn_w, opt_h * DEMOD_BW_OPTION_COUNT},
                             0.1f, 4, COLOR_PANEL_BG);
        for (int i = 0; i < DEMOD_BW_OPTION_COUNT; i++) {
            Rectangle opt_rect = {bw_btn_x, opt_y + i * opt_h, bw_btn_w, opt_h};
            s->overlay.bw_options_rect[i] = opt_rect;
            bool is_sel = (i == bw_cur);
            Vector2 mouse = gui_ui_get_mouse_position();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);
            Color opt_bg = gui_dropdown_option_color(is_sel, hover);
            DrawRectangleRec(opt_rect, opt_bg);
            int ow = gui_text_measure(DEMOD_BW_LABELS[i], FONT_SIZE_DROPDOWN_OPT);
            gui_text_draw(DEMOD_BW_LABELS[i], bw_btn_x + bw_btn_w/2.0f - ow/2.0f,
                      opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
    if (out_open) {
        float opt_y = out_btn_y + btn_h;
        float opt_h = 20.0f;
        const char *opts[] = {"CH1/2", "CH3/4"};
        DrawRectangleRounded((Rectangle){out_btn_x, opt_y, out_btn_w, opt_h * 2.0f},
                             0.1f, 4, COLOR_PANEL_BG);
        for (int i = 0; i < 2; i++) {
            Rectangle opt_rect = {out_btn_x, opt_y + i * opt_h, out_btn_w, opt_h};
            s->overlay.out_options_rect[i] = opt_rect;
            bool is_sel = (i == out_val);
            Vector2 mouse = gui_ui_get_mouse_position();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);
            Color opt_bg = gui_dropdown_option_color(is_sel, hover);
            DrawRectangleRec(opt_rect, opt_bg);
            int ow = gui_text_measure(opts[i], FONT_SIZE_DROPDOWN_OPT);
            gui_text_draw(opts[i], out_btn_x + out_btn_w/2.0f - ow/2.0f,
                      opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
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

    // Frequency field (SDR only): click starts editing; Enter/Esc commits.
    if (s->overlay.is_sdr && CheckCollisionPointRec(click, s->overlay.freq_btn_rect)) {
        if (!s->overlay.freq_editing) {
            s->overlay.freq_editing = true;
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
                app->settings.demod_bandwidth_hz = DEMOD_BW_VALUES[i];
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

    // Double-click anywhere else on the panel toggles ALL overlay chrome
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
            snprintf(s_demod_freq_str, sizeof(s_demod_freq_str), "%llu",
                     (unsigned long long)app->settings.rtlsdr_freq_hz);
        }
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------
static void *demod_vtable_create(void) {
    demod_state_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->mode = DEMOD_MODE_WFM;
    s->last_consumed_frame = 0;
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
    return s;
}

static void demod_vtable_destroy(void *state_ptr) {
    if (!state_ptr) return;
    demod_state_t *s = (demod_state_t *)state_ptr;
    demod_soxr_destroy(s);
    free(s->soxr_in_buf);
    free(s->soxr_out_buf);
    free(s->audio_out);
    free(s);
}

static void demod_vtable_clear(void *state_ptr) {
    if (!state_ptr) return;
    demod_state_t *s = (demod_state_t *)state_ptr;
    s->prev_i = s->prev_q = 0.0f;
    s->deemph_state = s->dc_state = s->agc_peak = 0.0f;
    s->sb_phase = 0;
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
    .handle_scroll = NULL,
    .get_menu_count = NULL,       // Mode dropdown rendered in overlay (UI never wires the panel-menu path)
    .get_menu = NULL,
};

void gui_demod_panel_register(void) {
    panel_register(PANEL_VIEW_DEMOD, &s_demod_vtable);
}
