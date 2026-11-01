/*
 * MISRC GUI - CVBS Video Decoder Module
 *
 * Decodes composite video (PAL/NTSC) from raw ADC samples.
 * Uses software PLL for H-sync tracking and provides frame buffer display.
 */

#include "gui_cvbs.h"
#include "../output/gui_audio.h"
#include "gui_trigger.h"
#include "gui_vhs_fm.h"
#include "../processing/gui_rf_preview_pressure.h"
#include "../visualization/gui_text.h"
#include "../ui/gui_ui.h"
#include "../../common/threading.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef LIBSOXR_ENABLED
#define LIBSOXR_ENABLED 0
#endif
#if LIBSOXR_ENABLED
#include <soxr.h>
#endif

// Live diagnostics: a 2048-entry H-sync ring plus a periodic full state +
// woven-frame (P6) dump to the CWD every CVBS_LIVE_DIAG_EVERY fields. OFF in
// the GUI build (the periodic 2 MB file writes ran on the display thread,
// a self-inflicted stall every ~2.5 s); the headless harness
// (test/gui_cvbs_test.c) defines it to 1 before including this file, so
// every headless decode of a capture still leaves the per-interval dumps
// for inspection.
#ifndef CVBS_LIVE_DIAG
#define CVBS_LIVE_DIAG 0
#endif

// Debug timing for luma filter performance (per-field accumulation)
static uint64_t g_field_decode_us = 0;      // Accumulated decode time for current field
static double g_field_decode_avg_ms = 0.0;  // Last completed field's decode time (for OSD)

// Cumulative throughput profile (process-lifetime, microseconds): the panel
// resampler, the whole per-sample process_buffer pass, and the per-line
// pixel decode inside it. Read through gui_cvbs_profile_get() by the
// headless harness to split the real-time budget (a PAL field is 20 ms).
static uint64_t g_prof_resample_us = 0;
static uint64_t g_prof_process_us = 0;
static uint64_t g_prof_lines_us = 0;
static uint64_t g_prof_lines = 0;

void gui_cvbs_profile_get(uint64_t *resample_us, uint64_t *process_us,
                          uint64_t *lines_us, uint64_t *lines) {
    if (resample_us) *resample_us = g_prof_resample_us;
    if (process_us) *process_us = g_prof_process_us;
    if (lines_us) *lines_us = g_prof_lines_us;
    if (lines) *lines = g_prof_lines;
}

#if CVBS_LIVE_DIAG
// Ring of the last 2048 H-syncs: the reconstructed phase at the pulse start
// (the origin offset) + the wrapped phase error (the period error), with the
// line/field identity - the horizontal-shift fingerprint per line.
typedef struct { int line, field; double phase_at_sync, phase_error; } cvbs_diag_sync_t;
static cvbs_diag_sync_t g_diag_ring[2048];
static unsigned g_diag_ring_head;
static int g_diag_dumps;
// Per-line DC clamp trace for the first 64 lines of the most recent field.
typedef struct { int line; double porch, tip, swing, offset; int accepted; int thr; } cvbs_diag_dc_t;
static cvbs_diag_dc_t g_diag_dc[64];
static int g_diag_dc_count;
static void cvbs_diag_record(int line, int field, double at_sync, double perr) {
    cvbs_diag_sync_t *e = &g_diag_ring[g_diag_ring_head & 2047u];
    e->line = line; e->field = field;
    e->phase_at_sync = at_sync; e->phase_error = perr;
    g_diag_ring_head++;
}
// Dump every CVBS_LIVE_DIAG_EVERY frames (~5 s at PAL 25 fps) to the CWD:
// the full decoder state + the ring + the woven frame as P6 PPM.
#define CVBS_LIVE_DIAG_EVERY 125
static void cvbs_diag_dump(cvbs_decoder_t *decoder) {
    char txt_path[64], ppm_path[64];
    snprintf(txt_path, sizeof(txt_path), "cvbs_live_diag_%d.txt", g_diag_dumps);
    snprintf(ppm_path, sizeof(ppm_path), "cvbs_live_diag_%d.ppm", g_diag_dumps);
    FILE *f = fopen(txt_path, "w");
    if (f) {
        fprintf(f, "dump %d frames=%d field=%d line=%d\n", g_diag_dumps,
                decoder->state.frames_decoded, decoder->state.current_field,
                decoder->pll.current_line);
        fprintf(f, "pll: phase=%.3f period=%.4f freq_adj=%.4f "
                   "locked=%d good=%d bad=%d gsl=%d perr=%.3f\n",
                decoder->pll.phase, decoder->pll.line_period,
                decoder->pll.freq_adjust, decoder->pll.locked ? 1 : 0,
                decoder->pll.good_sync_count, decoder->pll.bad_sync_count,
                decoder->pll.lines_since_lock, decoder->pll.phase_error);
        fprintf(f, "fixed: locked=%d period=%.4f ema=%+.5f bad=%d "
                   "thresh=%d sync_mode=%d\n",
                decoder->fixed_sync_locked ? 1 : 0, decoder->fixed_line_period,
                decoder->fixed_period_ema, decoder->fixed_bad_syncs,
                (int)decoder->fixed_sync_threshold, (int)decoder->sync_mode);
        fprintf(f, "rate: declared=%u measured=%u valid=%d trusted=%d\n",
                decoder->sample_rate_hz, decoder->measured_rate_hz,
                decoder->measured_rate_valid ? 1 : 0,
                decoder->rate_trusted ? 1 : 0);
        fprintf(f, "levels: black=%d white=%d tip=%d thresh=%d "
                   "porch=%d/%d dc=%+.1f/%d lvl_mode=%d\n",
                (int)decoder->levels.black_level,
                (int)decoder->levels.white_level,
                (int)decoder->levels.sig_min,
                (int)decoder->levels.sync_threshold,
                (int)decoder->adaptive.porch_level,
                decoder->adaptive.porch_valid,
                decoder->dc_offset, decoder->dc_offset_valid ? 1 : 0,
                (int)decoder->level_mode);
        fprintf(f, "vbi: acc=%d rej=%d (cad %d par %d) cands=%d "
                   "have=%d parity=%d start=%llu reason='%s'\n",
                decoder->vsync.fields_accepted, decoder->vsync.fields_rejected,
                decoder->vsync.reject_cadence, decoder->vsync.reject_parity,
                decoder->vsync.vsync_candidates, decoder->vsync.have_field,
                decoder->vsync.field_parity,
                (unsigned long long)decoder->vsync.field_start,
                decoder->vsync.last_reject_reason);
        fprintf(f, "dc clamp trace (last field, first %d lines): line porch tip swing offset acc thr\n",
                g_diag_dc_count);
        for (int i = 0; i < g_diag_dc_count; i++) {
            fprintf(f, "dc %d %.0f %.0f %.0f %+.0f %d %d\n", g_diag_dc[i].line,
                    g_diag_dc[i].porch, g_diag_dc[i].tip, g_diag_dc[i].swing,
                    g_diag_dc[i].offset, g_diag_dc[i].accepted, g_diag_dc[i].thr);
        }
        unsigned n = g_diag_ring_head < 2048u ? g_diag_ring_head : 2048u;
        unsigned base = g_diag_ring_head - n;
        fprintf(f, "syncs %u (oldest first): line field at_sync perr\n", n);
        for (unsigned i = 0; i < n; i++) {
            cvbs_diag_sync_t *e = &g_diag_ring[(base + i) & 2047u];
            fprintf(f, "%d %d %.3f %.3f\n",
                    e->line, e->field, e->phase_at_sync, e->phase_error);
        }
        fclose(f);
    }
    int fw = decoder->frame_width, fh = decoder->frame_height;
    if (fw > CVBS_MAX_WIDTH) fw = CVBS_MAX_WIDTH;
    if (fh > CVBS_MAX_HEIGHT) fh = CVBS_MAX_HEIGHT;
    if (fw > 0 && decoder->frame_buffer) {
        FILE *p = fopen(ppm_path, "wb");
        if (p) {
            fprintf(p, "P6\n%d %d\n255\n", fw, fh);
            fwrite(decoder->frame_buffer, 1u,
                   (size_t)fw * 3u * (size_t)fh, p);
            fclose(p);
        }
    }
    g_diag_dumps++;
}
#else
#define cvbs_diag_record(...) ((void)0)
#endif

static inline double cvbs_nominal_line_period(const cvbs_decoder_t *decoder);
static inline double cvbs_effective_line_period(const cvbs_decoder_t *decoder);
static inline void cvbs_reset_fixed_sync_lock(cvbs_decoder_t *decoder);
static const char *cvbs_decoder_mode_name(cvbs_decoder_mode_t mode);
static const char *cvbs_tape_format_name(cvbs_tape_format_t format);
static const char *cvbs_tape_mode_name(cvbs_tape_mode_t mode);
static const char *cvbs_level_mode_name(cvbs_level_mode_t mode);
static const char *cvbs_sync_mode_name(cvbs_sync_mode_t mode);
static void cvbs_trace_mode_watch(cvbs_decoder_t *decoder, const char *source);
static void cvbs_decoder_set_level_mode(cvbs_decoder_t *decoder, cvbs_level_mode_t mode, const char *source);
static void cvbs_decoder_cycle_level_mode(cvbs_decoder_t *decoder, const char *source);
static void cvbs_decoder_set_sync_mode(cvbs_decoder_t *decoder, cvbs_sync_mode_t mode, const char *source);
static void cvbs_decoder_cycle_sync_mode(cvbs_decoder_t *decoder, const char *source);
static void cvbs_build_level_label(const cvbs_decoder_t *decoder, char *dst, size_t dst_len);
static void cvbs_build_sync_label(const cvbs_decoder_t *decoder, char *dst, size_t dst_len);

typedef struct {
    const int16_t *coeffs;
    int taps;
    int shift;
} cvbs_luma_kernel_t;

typedef struct {
    double phase_gain;
    double integral_gain;
    double fixed_phase_gain;
    double lock_threshold;
} cvbs_pll_tuning_t;

static cvbs_luma_kernel_t cvbs_select_luma_kernel(const cvbs_decoder_t *decoder);
static cvbs_pll_tuning_t cvbs_select_pll_tuning(const cvbs_decoder_t *decoder);

//-----------------------------------------------------------------------------
// Baseband Profile (ported from the verified VHS FM decoder, gui_rf_preview.c)
//-----------------------------------------------------------------------------

// Per-format composite geometry. Values are format constants in microseconds
// / lines / Hz; every sample count derives from the LIVE capture rate
// (cvbs_rate()) so any rate decodes correctly - the legacy implementation
// hardcoded 40 MSPS everywhere (and the display-thread sample_rate was even
// discarded on the way in). Sources: the RF decoder's verified rf_profiles
// plus the ITU baseband subcarrier frequencies - the RF decoder's ~629 kHz
// carrier is VHS color-under and does NOT apply to baseband composite.
typedef struct {
    double line_us;             // 64.0 PAL / 63.5556 NTSC
    double eq_pulse_us;         // 2.35 / 2.3 (equalizing pulse width)
    double h_pulse_us;          // 4.7 / 4.7 (H-sync tip width)
    double broad_pulse_us;      // 27.3 / 27.1 (vertical broad pulse width)
    int eq_count;               // 5 / 6 (pulses per EQ train)
    double burst_start_us;      // 5.6 / 5.3 (colour burst window, from H-sync start)
    double burst_end_us;        // 7.85 / 7.8
    double active_start_us;     // 10.5 / 9.45 (active picture window, from H-sync start)
    double active_end_us;       // 62.5 / 62.5556
    int field_lines[2];         // Lines per field by parity: PAL {312,313}, NTSC {263,262}
    double field_cadence_lines; // 312.5 / 262.5 (expected lines between field starts)
    double boundary_lines[2];  // Field start before the first EQ pulse by parity:
                               // PAL {0.5,1.0}, NTSC {1.0,0.5}
    double fsc_hz;              // BASEBAND subcarrier: 4.43361875 MHz PAL / 3.579545 MHz NTSC
    int picture_start_line;     // First active PICTURE line of a field (anchored
                                // numbering): PAL 22 / NTSC 20 (frame line 23/21)
    int picture_lines;          // Active picture lines per field: PAL 288 / NTSC 242.
                                // Field lines outside [start, start+lines) are the
                                // VBI, rendered with the full-frame hybrid treatment
                                // (see decode_line_to_pixels).
} cvbs_baseband_profile_t;

static const cvbs_baseband_profile_t cvbs_baseband_profiles[2] = {
    // PAL / SECAM 625-line (SECAM shares the timing; its FM chroma is not
    // decoded - colour is suppressed for SECAM).
    { 64.0, 2.35, 4.7, 27.3, 5, 5.6, 7.85, 10.5, 62.5,
      {312, 313}, 312.5, {0.5, 1.0}, 4433618.75, 22, 288 },
    // NTSC 525-line
    { 63.55555555555556, 2.3, 4.7, 27.1, 6, 5.3, 7.8, 9.45, 62.55555555555556,
      {263, 262}, 262.5, {1.0, 0.5}, 3579545.0, 20, 242 },
};

static inline const cvbs_baseband_profile_t *cvbs_profile(const cvbs_decoder_t *decoder) {
    // Index by 625-line vs 525-line; SECAM uses the PAL (625) geometry.
    return (decoder && decoder->state.format == CVBS_FORMAT_NTSC)
           ? &cvbs_baseband_profiles[1] : &cvbs_baseband_profiles[0];
}

static inline int cvbs_us_to_samples(const cvbs_decoder_t *decoder, double us);

// Live capture rate in Hz. The SELF-CALIBRATED rate (measured from the
// signal's own H-to-H sync spacing) wins whenever it is established - the
// declared rate is only a bootstrap and has been measured wrong on live
// input paths (the app-level rate staying at the 40 MSPS default while the
// stream is 20 MSPS). Falls back to the declared rate, then the 40 MSPS
// legacy default.
static inline double cvbs_rate(const cvbs_decoder_t *decoder) {
    if (decoder && decoder->measured_rate_valid &&
        decoder->measured_rate_hz > 1000000u) {
        return (double)decoder->measured_rate_hz;
    }
    double rate = decoder ? (double)decoder->sample_rate_hz : 0.0;
    return rate > 1000000.0 ? rate : 40000000.0;
}

// Re-derive every sample-domain tracker after the effective (live or
// measured) rate changed: the PLL period is sized at the rate before any
// data arrives, and the period integral is capped at +-50 samples, so a
// wrong scale can never track to the truth. Acquisition re-locks within a
// few lines; a stable stream only fires this once per rate change.
static void cvbs_rederive_sample_domain(cvbs_decoder_t *decoder) {
    decoder->pll.line_period = cvbs_nominal_line_period(decoder);
    decoder->pll.freq_adjust = 0.0;
    decoder->pll.phase_integral = 0.0;
    decoder->pll.good_sync_count = 0;
    decoder->pll.bad_sync_count = 0;
    decoder->pll.locked = false;
    decoder->dc_offset = 0.0;
    decoder->dc_offset_valid = false;
    cvbs_reset_fixed_sync_lock(decoder);
}

// Feed one H-to-H sync spacing (samples) into the self-calibration: a
// median over the recent spacings (robust to the ~one multi-line gap per
// VBI) becomes the line period, and the implied rate (line_us / period)
// is committed as the measured rate. A fresh commit re-derives the
// sample-domain state exactly like a declared-rate change.
static void cvbs_measure_hh_spacing(cvbs_decoder_t *decoder, double spacing) {
    // A TRUSTED declared rate (the panel plugin's fixed-rate resampler
    // output, exact by construction) disables the self-calibration: the
    // median's edge quantization (+-0.5 samples/pulse) and the refinement's
    // own residual error are both far worse than exact, and at the panel's
    // 4x-fsc decode rate even 0.01% of rate error slips the absolute-frame
    // carrier table tens of degrees per line (measured on the resampled
    // EBU capture: the refinement committed ~0.01-0.06% off, the per-line
    // burst-phase steps lost the +-90 PAL alternation, the V-switch flipped
    // on every other line and half the frame's hues scrambled - while the
    // same decoder at the exact resampler rate decodes every bar within
    // +-0.2 deg of the EBU vectors).
    if (decoder->rate_trusted) return;
    const cvbs_baseband_profile_t *profile = cvbs_profile(decoder);
    // Acceptance band. Pre-commit it is anchored to the DECLARED nominal
    // (a CONSTANT): anchoring to the running median made the FIRST accepted
    // spacing define the band, and on a stream that starts inside a VBI the
    // first accepted spacing is the pre-EQ-to-post-EQ gap across the
    // broad-pulse region (~3 lines), which then rejected every true line
    // spacing forever (measured: a 3840-sample gap anchoring a band whose
    // floor was 1344 while the true line spacing was 1280). The declared
    // anchor trades that lock-in for early pollution from train half-line
    // spacings, which the 64-entry median survives (the active-area line
    // spacings outnumber them ~10:1). Post-commit the band tightens around
    // the MEASURED nominal (half-lines and VBI gaps rejected).
    double nominal = (double)cvbs_us_to_samples(decoder, profile->line_us);
    double lo = decoder->measured_rate_valid ? 0.65 * nominal : 0.35 * nominal;
    double hi = decoder->measured_rate_valid ? 1.60 * nominal : 2.60 * nominal;
    if (spacing < lo || spacing > hi) {
        // Consecutive-rejection escape: a real rate change (stream switch)
        // rejects every spacing against the old nominal forever - after a
        // long streak, drop the measurement and re-bootstrap. A clean
        // stream's streaks stay short (the ~10-12 train half-lines per VBI).
        if (++decoder->hh_reject_streak > 48) {
            decoder->hh_ring_count = 0;
            decoder->hh_period_samples = 0.0;
            decoder->measured_rate_hz = 0;
            decoder->measured_rate_valid = false;
            decoder->hh_reject_streak = 0;
        }
        return;  // VBI gap / outlier
    }
    decoder->hh_reject_streak = 0;

    // Bounded recent-spacings window. 64 entries: the active-area line
    // spacings outnumber the train half-line spacings roughly 10:1, so the
    // median is immune to a train's burst of half-line entries even
    // mid-train (a 24-entry window was measured flipping its median during
    // a train and committing a bogus half-rate on a correctly-declared
    // stream). The median is computed over a sorted copy (insertion sort is
    // trivial at this size).
    if (decoder->hh_ring_count < 64) {
        decoder->hh_ring[decoder->hh_ring_count++] = spacing;
    } else {
        memmove(&decoder->hh_ring[0], &decoder->hh_ring[1],
                sizeof(double) * 63);
        decoder->hh_ring[63] = spacing;
    }
    double sorted[64];
    memcpy(sorted, decoder->hh_ring, sizeof(double) * (size_t)decoder->hh_ring_count);
    for (int i = 1; i < decoder->hh_ring_count; i++) {
        double v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = v;
    }
    int n = decoder->hh_ring_count;
    double median = (n & 1)
        ? sorted[n / 2]
        : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
    // The MEDIAN is the line-period estimate in every phase: robust to the
    // minority junk the bands admit (train half-lines pre-commit, the few
    // 1.5-2.5-line boundary gaps that fit inside the tight band - a MEAN
    // estimator was measured biased +0.05% by those gaps, rotating the
    // late-active hues by ~45 deg). It lands on the modal integer spacing,
    // so the rate error stays within one edge-quantization sample (~0.04%
    // worst case, far below the ~0.5% the hue decode tolerates).
    decoder->hh_period_samples = median;

    if (n < 32) return;
    double rate_est = median * 1000000.0 / profile->line_us;
    if (rate_est < 1000000.0 || rate_est > 250000000.0) return;
    if (!decoder->measured_rate_valid) {
        // First commit: if the declared rate was already right, latch the
        // measurement WITHOUT re-deriving anything (no mid-stream glitch).
        decoder->measured_rate_hz = (uint32_t)(rate_est + 0.5);
        decoder->measured_rate_valid = true;
        double declared = decoder->sample_rate_hz > 1000000u
            ? (double)decoder->sample_rate_hz : 40000000.0;
        if (fabs(rate_est - declared) > 0.02 * rate_est) {
            cvbs_rederive_sample_domain(decoder);
        }
    } else if (fabs(rate_est - (double)decoder->measured_rate_hz) > 0.02 * rate_est) {
        decoder->measured_rate_hz = (uint32_t)(rate_est + 0.5);
        cvbs_rederive_sample_domain(decoder);
    }
}

// Sample count for a microsecond window at the live rate.
static inline int cvbs_us_to_samples(const cvbs_decoder_t *decoder, double us) {
    return (int)(us * cvbs_rate(decoder) / 1000000.0 + 0.5);
}

//-----------------------------------------------------------------------------
// Internal Constants
//-----------------------------------------------------------------------------
// (All format geometry lives in cvbs_baseband_profiles above. The only
// remaining legacy constants are rate-independent line counts used by the
// deinterlacer comments; field line counts come from the profile by parity.)

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------
static const char *cvbs_decoder_mode_name(cvbs_decoder_mode_t mode) {
    return (mode == CVBS_DECODER_MODE_TAPE) ? "Tape Decode" : "CVBS";
}

static const char *cvbs_tape_format_name(cvbs_tape_format_t format) {
    switch (format) {
        case CVBS_TAPE_FORMAT_SVHS:    return "S-VHS";
        case CVBS_TAPE_FORMAT_BETAMAX: return "Betamax";
        case CVBS_TAPE_FORMAT_VIDEO8:  return "Video8";
        case CVBS_TAPE_FORMAT_HI8:     return "Hi8";
        case CVBS_TAPE_FORMAT_UMATIC:  return "U-matic";
        case CVBS_TAPE_FORMAT_VHS:
        default:                       return "VHS";
    }
}

static const char *cvbs_tape_mode_name(cvbs_tape_mode_t mode) {
    switch (mode) {
        case CVBS_TAPE_MODE_LP:  return "LP";
        case CVBS_TAPE_MODE_ELP: return "ELP";
        case CVBS_TAPE_MODE_SP:
        default:                 return "SP";
    }
}
static const char *cvbs_level_mode_name(cvbs_level_mode_t mode) {
    return (mode == CVBS_LEVEL_MODE_FIXED) ? "Fixed" : "Auto";
}

static const char *cvbs_sync_mode_name(cvbs_sync_mode_t mode) {
    return (mode == CVBS_SYNC_MODE_FIXED) ? "Fixed" : "Auto";
}

// Select luma smoothing preset.
// CVBS mode keeps original behavior; Tape mode applies tape-family/speed bias.
static cvbs_luma_kernel_t cvbs_select_luma_kernel(const cvbs_decoder_t *decoder) {
    // Original balanced kernel (legacy default).
    static const int16_t kernel_balanced[17] = {
        1, 2, 5, 9, 14, 21, 28, 33, 35, 33, 28, 21, 14, 9, 5, 2, 1
    };
    // Slightly sharper kernel.
    static const int16_t kernel_sharp[11] = {
        2, 6, 14, 25, 35, 40, 35, 25, 14, 6, 2
    };
    // Slightly softer kernel.
    static const int16_t kernel_soft[23] = {
        1, 1, 2, 3, 5, 8, 12, 16, 20, 24, 26, 27,
        26, 24, 20, 16, 12, 8, 5, 3, 2, 1, 1
    };

    int preset = 1;  // balanced
    if (decoder && decoder->decoder_mode == CVBS_DECODER_MODE_TAPE) {
        preset = (decoder->tape_mode == CVBS_TAPE_MODE_SP) ? 0
               : (decoder->tape_mode == CVBS_TAPE_MODE_ELP) ? 2
               : 1;

        // Tape-family bias.
        if (decoder->tape_format == CVBS_TAPE_FORMAT_SVHS ||
            decoder->tape_format == CVBS_TAPE_FORMAT_HI8) {
            preset -= 1;  // sharper
        } else if (decoder->tape_format == CVBS_TAPE_FORMAT_UMATIC) {
            preset += 1;  // slightly softer/noise-tolerant
        }
        if (preset < 0) preset = 0;
        if (preset > 2) preset = 2;
    }

    switch (preset) {
        case 0:  return (cvbs_luma_kernel_t){ kernel_sharp, 11, 8 };
        case 2:  return (cvbs_luma_kernel_t){ kernel_soft, 23, 8 };
        case 1:
        default: return (cvbs_luma_kernel_t){ kernel_balanced, 17, 8 };
    }
}

// Select PLL tuning profile.
// CVBS mode stays at original constants.
static cvbs_pll_tuning_t cvbs_select_pll_tuning(const cvbs_decoder_t *decoder) {
    cvbs_pll_tuning_t t = {
        .phase_gain = 0.15,
        .integral_gain = 0.005,
        .fixed_phase_gain = 0.03,
        .lock_threshold = 100.0,
    };

    if (!decoder || decoder->decoder_mode != CVBS_DECODER_MODE_TAPE) {
        return t;
    }

    switch (decoder->tape_mode) {
        case CVBS_TAPE_MODE_SP:
            t.phase_gain = 0.17;
            t.integral_gain = 0.006;
            t.fixed_phase_gain = 0.035;
            t.lock_threshold = 90.0;
            break;
        case CVBS_TAPE_MODE_LP:
            t.phase_gain = 0.14;
            t.integral_gain = 0.0045;
            t.fixed_phase_gain = 0.028;
            t.lock_threshold = 110.0;
            break;
        case CVBS_TAPE_MODE_ELP:
            t.phase_gain = 0.11;
            t.integral_gain = 0.003;
            t.fixed_phase_gain = 0.020;
            t.lock_threshold = 140.0;
            break;
        default:
            break;
    }

    if (decoder->tape_format == CVBS_TAPE_FORMAT_SVHS ||
        decoder->tape_format == CVBS_TAPE_FORMAT_HI8) {
        t.lock_threshold -= 10.0;
    } else if (decoder->tape_format == CVBS_TAPE_FORMAT_UMATIC) {
        t.lock_threshold += 10.0;
    }

    if (t.lock_threshold < 70.0) t.lock_threshold = 70.0;
    if (t.lock_threshold > 180.0) t.lock_threshold = 180.0;
    return t;
}

static void cvbs_trace_mode_watch(cvbs_decoder_t *decoder, const char *source) {
    if (!decoder) return;
    if (!decoder->debug.mode_trace_initialized) {
        decoder->debug.mode_trace_initialized = true;
        decoder->debug.last_decoder_mode = (int)decoder->decoder_mode;
        decoder->debug.last_tape_format = (int)decoder->tape_format;
        decoder->debug.last_tape_mode = (int)decoder->tape_mode;
        decoder->debug.last_level_mode = (int)decoder->level_mode;
        decoder->debug.last_sync_mode = (int)decoder->sync_mode;
        decoder->debug.last_fixed_sync_locked = decoder->fixed_sync_locked;
        decoder->debug.last_fixed_levels_valid = decoder->fixed_levels_valid;
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s init decoder=%s tape=%s mode=%s level=%s sync=%s fixed_sync_locked=%d fixed_levels_valid=%d",
                 (source && source[0]) ? source : "unknown",
                 cvbs_decoder_mode_name(decoder->decoder_mode),
                 cvbs_tape_format_name(decoder->tape_format),
                 cvbs_tape_mode_name(decoder->tape_mode),
                 cvbs_level_mode_name(decoder->level_mode),
                 cvbs_sync_mode_name(decoder->sync_mode),
                 decoder->fixed_sync_locked ? 1 : 0,
                 decoder->fixed_levels_valid ? 1 : 0);
        return;
    }
    if (decoder->debug.last_decoder_mode != (int)decoder->decoder_mode) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=decoder_mode old=%s new=%s",
                 (source && source[0]) ? source : "unknown",
                 cvbs_decoder_mode_name((cvbs_decoder_mode_t)decoder->debug.last_decoder_mode),
                 cvbs_decoder_mode_name(decoder->decoder_mode));
    }
    if (decoder->debug.last_tape_format != (int)decoder->tape_format) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=tape_format old=%s new=%s",
                 (source && source[0]) ? source : "unknown",
                 cvbs_tape_format_name((cvbs_tape_format_t)decoder->debug.last_tape_format),
                 cvbs_tape_format_name(decoder->tape_format));
    }
    if (decoder->debug.last_tape_mode != (int)decoder->tape_mode) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=tape_mode old=%s new=%s",
                 (source && source[0]) ? source : "unknown",
                 cvbs_tape_mode_name((cvbs_tape_mode_t)decoder->debug.last_tape_mode),
                 cvbs_tape_mode_name(decoder->tape_mode));
    }

    if (decoder->debug.last_level_mode != (int)decoder->level_mode) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=level_mode old=%s new=%s",
                 (source && source[0]) ? source : "unknown",
                 cvbs_level_mode_name((cvbs_level_mode_t)decoder->debug.last_level_mode),
                 cvbs_level_mode_name(decoder->level_mode));
    }
    if (decoder->debug.last_sync_mode != (int)decoder->sync_mode) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=sync_mode old=%s new=%s",
                 (source && source[0]) ? source : "unknown",
                 cvbs_sync_mode_name((cvbs_sync_mode_t)decoder->debug.last_sync_mode),
                 cvbs_sync_mode_name(decoder->sync_mode));
    }
    if (decoder->debug.last_fixed_sync_locked != decoder->fixed_sync_locked) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=fixed_sync_locked old=%d new=%d sync_mode=%s",
                 (source && source[0]) ? source : "unknown",
                 decoder->debug.last_fixed_sync_locked ? 1 : 0,
                 decoder->fixed_sync_locked ? 1 : 0,
                 cvbs_sync_mode_name(decoder->sync_mode));
    }
    if (decoder->debug.last_fixed_levels_valid != decoder->fixed_levels_valid) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=fixed_levels_valid old=%d new=%d level_mode=%s",
                 (source && source[0]) ? source : "unknown",
                 decoder->debug.last_fixed_levels_valid ? 1 : 0,
                 decoder->fixed_levels_valid ? 1 : 0,
                 cvbs_level_mode_name(decoder->level_mode));
    }
    decoder->debug.last_decoder_mode = (int)decoder->decoder_mode;
    decoder->debug.last_tape_format = (int)decoder->tape_format;
    decoder->debug.last_tape_mode = (int)decoder->tape_mode;

    decoder->debug.last_level_mode = (int)decoder->level_mode;
    decoder->debug.last_sync_mode = (int)decoder->sync_mode;
    decoder->debug.last_fixed_sync_locked = decoder->fixed_sync_locked;
    decoder->debug.last_fixed_levels_valid = decoder->fixed_levels_valid;
}

static void cvbs_build_level_label(const cvbs_decoder_t *decoder, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    if (decoder && decoder->decoder_mode == CVBS_DECODER_MODE_TAPE) {
        bool ire0_adjust = (decoder->level_mode == CVBS_LEVEL_MODE_FIXED);
        snprintf(dst, dst_len, "Level: --ire0-adjust %s", ire0_adjust ? "ON" : "OFF");
        return;
    }
    cvbs_level_mode_t mode = decoder ? decoder->level_mode : CVBS_LEVEL_MODE_AUTO;
    snprintf(dst, dst_len, "Levels: %s", cvbs_level_mode_name(mode));
}

static void cvbs_build_colour_label(const cvbs_decoder_t *decoder, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    snprintf(dst, dst_len, "Colour: %s",
             (decoder && decoder->colour_mode == CVBS_COLOUR_AUTO) ? "Auto" : "Off");
}

static void cvbs_build_sync_label(const cvbs_decoder_t *decoder, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    const char *prefix = (decoder && decoder->decoder_mode == CVBS_DECODER_MODE_TAPE)
                         ? "Tape Sync"
                         : "Sync";
    cvbs_sync_mode_t mode = decoder ? decoder->sync_mode : CVBS_SYNC_MODE_AUTO;
    snprintf(dst, dst_len, "%s: %s", prefix, cvbs_sync_mode_name(mode));
}

static void cvbs_decoder_set_level_mode(cvbs_decoder_t *decoder, cvbs_level_mode_t mode, const char *source) {
    if (!decoder) return;
    if (mode != CVBS_LEVEL_MODE_AUTO && mode != CVBS_LEVEL_MODE_FIXED) {
        mode = CVBS_LEVEL_MODE_AUTO;
    }

    cvbs_level_mode_t old_level_mode = decoder->level_mode;
    if (old_level_mode == mode) {
        return;
    }

    if (mode == CVBS_LEVEL_MODE_AUTO) {
        decoder->level_mode = CVBS_LEVEL_MODE_AUTO;
        decoder->fixed_levels_valid = false;
    } else {
        decoder->level_mode = CVBS_LEVEL_MODE_FIXED;
        bool have_current_levels =
            (decoder->levels.range >= 100) ||
            (decoder->adaptive.threshold != 0 &&
             decoder->adaptive.white > decoder->adaptive.sync_tip);
        if (have_current_levels) {
            int16_t black = decoder->levels.black_level;
            int16_t white = decoder->levels.white_level;
            if (white <= black) {
                black = decoder->adaptive.black;
                white = decoder->adaptive.white;
            }
            if (white <= black) {
                white = (int16_t)(black + 100);
            }
            decoder->fixed_black_level = black;
            decoder->fixed_white_level = white;
            decoder->fixed_levels_valid = true;
        } else {
            decoder->fixed_levels_valid = false;
        }
    }

    if (old_level_mode != decoder->level_mode) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=level_mode old=%s new=%s",
                 (source && source[0]) ? source : "set_level_mode",
                 cvbs_level_mode_name(old_level_mode),
                 cvbs_level_mode_name(decoder->level_mode));
    }
    cvbs_trace_mode_watch(decoder, source ? source : "set_level_mode");
}

static void cvbs_decoder_cycle_level_mode(cvbs_decoder_t *decoder, const char *source) {
    if (!decoder) return;
    cvbs_level_mode_t next_mode = (decoder->level_mode == CVBS_LEVEL_MODE_FIXED)
                                  ? CVBS_LEVEL_MODE_AUTO
                                  : CVBS_LEVEL_MODE_FIXED;
    cvbs_decoder_set_level_mode(decoder, next_mode, source);
}

static void cvbs_decoder_set_sync_mode(cvbs_decoder_t *decoder, cvbs_sync_mode_t mode, const char *source) {
    if (!decoder) return;
    if (mode != CVBS_SYNC_MODE_AUTO && mode != CVBS_SYNC_MODE_FIXED) {
        mode = CVBS_SYNC_MODE_AUTO;
    }

    cvbs_sync_mode_t old_sync_mode = decoder->sync_mode;
    bool old_fixed_lock = decoder->fixed_sync_locked;
    if (old_sync_mode == mode) {
        return;
    }

    decoder->sync_mode = mode;
    cvbs_reset_fixed_sync_lock(decoder);
    decoder->pll.locked = false;
    decoder->pll.good_sync_count = 0;
    decoder->pll.bad_sync_count = 0;
    decoder->pll.line_period = cvbs_nominal_line_period(decoder);
    decoder->pll.phase_integral = 0.0;
    decoder->pll.freq_adjust = 0.0;

    if (old_sync_mode != decoder->sync_mode) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=sync_mode old=%s new=%s",
                 (source && source[0]) ? source : "set_sync_mode",
                 cvbs_sync_mode_name(old_sync_mode),
                 cvbs_sync_mode_name(decoder->sync_mode));
    }
    if (old_fixed_lock != decoder->fixed_sync_locked) {
        TraceLog(LOG_INFO,
                 "CVBS MODE TRACE: source=%s field=fixed_sync_locked old=%d new=%d",
                 (source && source[0]) ? source : "set_sync_mode",
                 old_fixed_lock ? 1 : 0,
                 decoder->fixed_sync_locked ? 1 : 0);
    }
    cvbs_trace_mode_watch(decoder, source ? source : "set_sync_mode");
}

static void cvbs_decoder_cycle_sync_mode(cvbs_decoder_t *decoder, const char *source) {
    if (!decoder) return;
    cvbs_sync_mode_t next_mode = (decoder->sync_mode == CVBS_SYNC_MODE_FIXED)
                                 ? CVBS_SYNC_MODE_AUTO
                                 : CVBS_SYNC_MODE_FIXED;
    cvbs_decoder_set_sync_mode(decoder, next_mode, source);
}


// Pre-filtered line buffer (avoids per-pixel convolution)
// Must cover a full line at the highest supported rate (200 MSPS PAL line
// = 12800 samples) so rate awareness never truncates the decode window.
#define FILTERED_LINE_MAX 13120
static int16_t g_filtered_line[FILTERED_LINE_MAX];

// Vectorisable FIR accumulator for the luma kernel (outer loop over taps,
// inner over samples - the form GCC's vectoriser handles at -O3).
static int32_t g_fir_acc[FILTERED_LINE_MAX];

// Per-line PREFIX SUMS over the line-local carrier frame (index k = sample
// index within the line, entry k = the sum over samples [0, k)):
//   g_pf_s  = sum src              g_pf_c = sum cos        g_pf_n = sum sin
//   g_pf_i  = sum src*cos          g_pf_q = sum src*sin
// Every window measurement (the burst, each pixel's mean-subtracted I/Q
// correlation and its box-mean luma) is then two loads and a subtraction
// per quantity instead of a loop over the window - the per-pixel window
// loops plus their divisions measured 56 us per PAL line at the panel's
// 4x-fsc rate (88% of the real-time budget on their own: the display
// thread fell behind, the feed buffer overran after ~8-10 s and every
// drop kicked the sync chain). Doubles: the running sums reach ~4e7 and
// window differences must keep full sample precision.
static double g_pf_s[FILTERED_LINE_MAX + 1];
static double g_pf_c[FILTERED_LINE_MAX + 1];
static double g_pf_n[FILTERED_LINE_MAX + 1];
static double g_pf_i[FILTERED_LINE_MAX + 1];
static double g_pf_q[FILTERED_LINE_MAX + 1];

// Sync-edge delay of the pulse classifier, in SAMPLES: the PLL wrap (and so
// the line buffer's k = 0) lands at the sync-pulse START CROSSING of the
// ~500kHz filtered signal, which lags the true (raw) sync edge by the
// single-pole IIR's step delay tau*ln(2) with tau = (1-alpha)/alpha = 11.8
// samples = ~8.2 samples at the mid-pulse (-50%) threshold the porch-based
// level commit guarantees. All profile windows (burst / porch / active)
// are measured from the TRUE sync start, so every window position shifts
// by this constant (rate-independent: the filter runs per sample). Without
// the compensation the burst window starts ~8 samples into the burst
// (measured amp 586/563 vs the true 640) and the porch window's upper edge
// overlaps the active area (measured: the 82 IRE marker bar polluting every
// line's porch mean by ~+200 counts). The per-window +/-1/2 trims absorb
// the residual sub-sample quantization.
#define CVBS_SYNC_EDGE_DELAY_SAMPLES 8

//============================================================================
// SECAM FM chroma (backlog item 4)
//============================================================================
// The tbc-tools secamdecoder pipeline (libchromadec v0.1.0 parameters - the
// vapoursynth-analog SECAM decoder) ported to the GUI's streaming real-time
// per-line decode. Reference: tbc-tools/src/tbc-chroma-decoder/
// secamdecoder.cpp + the 2026-10-15/18 port notes there. Everything the
// reference does per FIELD in the block-FFT domain runs here per LINE in the
// time domain inside the real-time budget contract:
//  - The chroma band x bell (cloche) mask becomes a real 41-tap windowed-sinc
//    low-pass on the mixed-down signal (the pre-port tbc-tools decoder's own
//    band choice, which the user ran for years) followed by the linearised
//    cloche: F = f/c - c/f = 2*fb/c + O(x^2), |x| <= 0.14 over the block, so
//    the bell's first-order networks are exact two-tap complex derivative
//    sections (< 0.5% error across the +-156 kHz carrier pair, < 3% at the
//    +-580 kHz skirt; the numeric match is a harness contract).
//  - The bell-free porch path is the SAME low-pass output taken before the
//    cloche (the reference's separate band-only mask, for free).
//  - The discriminator is the reference's 9-tap maximally-flat central
//    difference on the mixed-down analytic signal: the exact phase
//    derivative Im(conj(a)*a')/|a|^2.
//  - Field calibration (cluster medians + the EXACT 10*fH pair-split lock +
//    the separation/sanity gates + last-good reuse + recentre) commits at
//    each field boundary from the PREVIOUS field's porch ring - ONE FIELD
//    BEHIND, the reference's own last-good semantics in a live stream.
//  - Identification: the porch parity vote with the reference's ladder
//    (bottles cross-check on the early VBI lines, content-median fallback,
//    >= 0.75 / >= 4-row gates).
//  - Per-line rest: THIS line's own porch median (the reference's rowZero,
//    causal - future lines do not exist in a live stream).
//  - Click concealment ("SECAM fire"): the reference's envelope-dip +
//    noise-adaptive deviation-overshoot detection; short spans interpolate,
//    wider spans borrow row-2 (the previous same-component line, in the
//    ring). Deviation rails run BEFORE de-emphasis like the reference.
//  - De-emphasis: the reference's COMPLEX inverseLf mask - the exact causal
//    lead-lag (1 + j f/(3 f1))/(1 + j f/f1) WITH ITS PHASE, applied per
//    field with overlap-save so the state runs continuously down the field.
//    The streaming port is the same network as a CAUSAL first-order
//    bilinear IIR at the decimated rate with its state carried across
//    lines. (The first port realized it as a zero-phase symmetric FIR
//    reset per line - wrong on both counts: magnitude-only, and no
//    continuity. Measured on the bars fixture: the symmetrised kernel put
//    half its energy into the FUTURE, bleeding each bar into its neighbours
//    by up to 26% at the bar centres, and the phase could never cancel the
//    encoder pre-correction's.) The reference's 1.6/1.9 MHz colour-diff
//    LPF is subsumed by the decimation's +-1.107 MHz Nyquist (documented
//    streaming adaptation).
//  - The one-line hold is causal (row-2, a real SECAM delay line) instead
//    of the offline bidirectional fill.
// U/V handoff: the pixel loop's normalized pair maps straight from the
// demodulated deviations: u01 = D'B/(1.505*2.032062),
// v01 = -D'R/(1.902*1.139883) - the reference's verified matrix constants.

// Parameters VERBATIM from the reference (the guard pins them):
#define CVBS_SECAM_FOB_HZ        4250000.0  // D'b rest carrier (272 fH)
#define CVBS_SECAM_FOR_HZ        4406250.0  // D'r rest carrier (282 fH)
#define CVBS_SECAM_DEV_DB_HZ     230000.0   // nominal deviation for D'B = 1
#define CVBS_SECAM_DEV_DR_HZ     280000.0   // nominal deviation for D'R = 1
#define CVBS_SECAM_DB_SCALE      1.505      // D'B = +1.505 (E'B - E'Y)
#define CVBS_SECAM_DR_SCALE      (-1.902)   // D'R = -1.902 (E'R - E'Y)
#define CVBS_SECAM_BELL_CENTRE_HZ 4286000.0 // HF pre-correction f0
#define CVBS_SECAM_LF_BREAK_HZ   85000.0    // LF pre-correction f1
#define CVBS_SECAM_BAND_FLAT_HZ  450000.0   // combined band: flat off centre
#define CVBS_SECAM_BAND_STOP_HZ  580000.0   // combined band: stop
#define CVBS_SECAM_CAL_MIN_ROWS      8
#define CVBS_SECAM_CAL_MIN_SEP_HZ    90000.0
#define CVBS_SECAM_CAL_MAX_SEP_HZ    230000.0
#define CVBS_SECAM_CAL_MAX_OFFSET_HZ 350000.0
#define CVBS_SECAM_RECENTRE_HZ       10000.0
#define CVBS_SECAM_PAIR_SPLIT_HZ     78125.0  // EXACT: the pair is 10*fH apart
#define CVBS_SECAM_KILLER_RATIO      0.25
#define CVBS_SECAM_DEV_DB_LO   (-350000.0 / 230000.0)
#define CVBS_SECAM_DEV_DB_HI   ( 506000.0 / 230000.0)
#define CVBS_SECAM_DEV_DR_LO   (-506000.0 / 280000.0)
#define CVBS_SECAM_DEV_DR_HI   ( 350000.0 / 280000.0)
#define CVBS_SECAM_LP_PASS_HZ  1600000.0   // reference colour-diff LPF pass
#define CVBS_SECAM_LP_STOP_HZ  1950000.0   // reference colour-diff LPF stop
// Streaming adaptations (documented above):
// The decimation is RATE-RELATIVE: decim = round(rate / 2214771) (the
// 4fsc/8 target), so the decimated rate is always ~2.21 MHz and the
// downstream constants (discriminator exactness, the cloche, the
// de-emphasis FIR - all designed at fs_dec) stay valid at ANY input rate
// (the panel's 4fsc path, the direct 40 MSPS path, self-calibrated rates).
#define CVBS_SECAM_DECIM_TARGET_HZ 2214771.0 // 4x-fsc PAL / 8
#define CVBS_SECAM_DECIM_MIN   4
#define CVBS_SECAM_FIR_TAPS_REF 41          // band taps at the 4fsc reference
#define CVBS_SECAM_RATE_REF_HZ 17734475.0   // 4x-fsc PAL (the design point)
#define CVBS_SECAM_FIR_TAPS_MAX 127        // band tap ceiling (array size)
// De-emphasis is a causal first-order bilinear IIR at the decimated rate
// (no design constants: the coefficients derive directly from
// CVBS_SECAM_LF_BREAK_HZ in cvbs_secam_build_filters).
#define CVBS_SECAM_U_SCALE (1.0 / (1.505 * 2.032062))  // D'B -> u01
#define CVBS_SECAM_V_SCALE (-1.0 / (1.902 * 1.139883)) // D'R -> v01 (sign folded)
#define CVBS_SECAM_CLICK_LEVEL  1.0         // concealment level (reference default)
#define CVBS_SECAM_PORCH_TAIL_HZ_GATE 78125.0 // porch median plausibility vs pair

// Discriminator taps: maximally-flat central difference (K=4), the
// reference's exact coefficients.
static const double g_secam_diff_taps[9] = {
    -1.0/280.0, 4.0/105.0, -1.0/5.0, 4.0/5.0, 0.0,
    -4.0/5.0, 1.0/5.0, -4.0/105.0, 1.0/280.0
};

// Per-line scratch (display-thread-owned, like g_filtered_line):
static double g_secam_mix_re[FILTERED_LINE_MAX];     // mixed-down full rate
static double g_secam_mix_im[FILTERED_LINE_MAX];
static double g_secam_lpre[CVBS_SECAM_MAX_DEC];      // band-only analytic (porch path)
static double g_secam_lpim[CVBS_SECAM_MAX_DEC];
static double g_secam_ire[CVBS_SECAM_MAX_DEC];       // band x bell analytic
static double g_secam_iim[CVBS_SECAM_MAX_DEC];
static double g_secam_f[CVBS_SECAM_MAX_DEC];         // discriminated Hz (bell path)
static double g_secam_fref[CVBS_SECAM_MAX_DEC];      // discriminated Hz (bell-free: porch/ident)
static double g_secam_demod[CVBS_SECAM_MAX_DEC];     // deviation in D'x units
static double g_secam_final[CVBS_SECAM_MAX_DEC];     // post de-emphasis D'x (IRE fractions)
static float  g_secam_u[CVBS_SECAM_MAX_DEC];         // pixel-loop u01 per dec sample
static float  g_secam_v[CVBS_SECAM_MAX_DEC];
static double g_secam_porch_f[40];                   // porch window fInst (median)

// Rate-relative decimation state (set by cvbs_secam_build_filters; the
// pixel loop and the porch windows read these):
static int g_secam_decim = 8;        // decimation factor for the current rate
static int g_secam_lp_taps = 41;     // band FIR tap count for the current rate
static int g_secam_dec_count = 0;    // the last line's decimated sample count

// One-line-hold rings: the previous same-component line's FINAL D'x (IRE
// fractions, post de-emphasis) per component. File-scope like the other
// per-line scratch (single decoder instance; cleared when the filters
// rebuild).
static float  g_secam_hold_final[2][CVBS_SECAM_MAX_DEC];
static int    g_secam_hold_count[2];

static void cvbs_secam_bell_apply(int dec_count);

// Filter cache: the coefficients depend on (rate, block centre); rebuilt
// lazily when either moves past the recentre threshold.
static double g_secam_coeffs_valid = false;
static double g_secam_coeffs_rate = 0.0;
static double g_secam_coeffs_centre = 0.0;
static double g_secam_lp[CVBS_SECAM_FIR_TAPS_MAX];    // band low-pass (real, symmetric)
// De-emphasis causal lead-lag IIR: coefficients (built at the decimated
// rate) + the CARRIED STATE - continuous across lines and fields like the
// reference's flat per-field overlap-save buffer; reset only on a filter
// rebuild (rate/centre move), where the state re-enters from the
// porch-zero rest and settles within the sync/porch region.
static double g_secam_deemph_b0, g_secam_deemph_b1, g_secam_deemph_a1;
static double g_secam_deemph_x1, g_secam_deemph_y1;
// Cloche (see cvbs_secam_bell_apply): the reference bell factored about
// the mix-down (block) centre into C * (1 + j n~ fb)/(1 + j d~ fb) and
// bilinear-mapped to a complex one-pole/one-zero section (12 real mults
// per decimated sample). All complex coefficients stored re/im, pre-divided
// by the denominator's a0.
static double  g_secam_bell_b0_re, g_secam_bell_b0_im;  // numerator z^0 coeff
static double  g_secam_bell_b1_re, g_secam_bell_b1_im;  // numerator z^-1 coeff
static double  g_secam_bell_a1_re, g_secam_bell_a1_im;  // denominator z^-1 coeff
static double  g_secam_notch_b0, g_secam_notch_b1, g_secam_notch_b2; // luma notch
static double  g_secam_notch_a1, g_secam_notch_a2;

// Windowed-sinc symmetric low-pass: pass flat to pass_hz, -6 dB at cut_hz,
// Hamming window (the pre-port tbc-tools decoder's own band model).
static void cvbs_secam_design_lowpass(double *taps, int ntaps,
                                      double cut_hz, double rate) {
    const int M = ntaps - 1;
    const double fc = cut_hz / rate;   // normalized (cycles/sample)
    for (int n = 0; n < ntaps; n++) {
        double m = (double)(n - M / 2);
        double sinc = (m == 0.0) ? 2.0 * fc
                                 : sin(2.0 * M_PI * fc * m) / (M_PI * m);
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * (double)n / (double)M);
        taps[n] = sinc * w;
    }
}

// (Re)build the SECAM filter set for the current rate + locked block
// centre. Called lazily from the line decode when (rate, centre) moved.
static void cvbs_secam_build_filters(double rate, double block_centre_hz) {
    // Rate-relative decimation + band tap scaling: the decimated rate is
    // pinned at ~2.21 MHz (the 4fsc/8 design point) so the downstream
    // constants hold at any input rate, and the band FIR's ABSOLUTE
    // transition scales its tap count with the rate (41 taps at the 4fsc
    // reference, 93 at 40 MSPS) - a fixed 41 taps at 40 MSPS left skirts
    // 2.3x too wide and the picture demod railed on leaked luma.
    g_secam_decim = (int)(rate / CVBS_SECAM_DECIM_TARGET_HZ + 0.5);
    if (g_secam_decim < CVBS_SECAM_DECIM_MIN) g_secam_decim = CVBS_SECAM_DECIM_MIN;
    g_secam_lp_taps = (int)(CVBS_SECAM_FIR_TAPS_REF * rate
                             / CVBS_SECAM_RATE_REF_HZ + 0.5);
    if (g_secam_lp_taps < CVBS_SECAM_FIR_TAPS_REF)
        g_secam_lp_taps = CVBS_SECAM_FIR_TAPS_REF;
    if (g_secam_lp_taps > CVBS_SECAM_FIR_TAPS_MAX)
        g_secam_lp_taps = CVBS_SECAM_FIR_TAPS_MAX;
    if (g_secam_lp_taps % 2 == 0) g_secam_lp_taps++;   // odd (symmetric)

    // Band low-pass on the mixed-down FULL-RATE signal: flat over the
    // +-450 kHz excursion band, -6 dB at ~500 kHz, Hamming skirts (the
    // pre-port tbc-tools band model: soft skirts, proven on real tape).
    cvbs_secam_design_lowpass(g_secam_lp, g_secam_lp_taps,
                              0.5 * (CVBS_SECAM_BAND_FLAT_HZ
                                     + CVBS_SECAM_BAND_STOP_HZ), rate);

    // Clear the one-line-hold rings on any rebuild (rate/centre change:
    // the hold would bridge across a discontinuity).
    g_secam_hold_count[0] = 0;
    g_secam_hold_count[1] = 0;

    // Cloche factors: the reference bell A(f) = (1 + j1.26 F)/(1 + j16 F),
    // F = f/c_bell - c_bell/f, at the TRUE frequency f. After mix-down by
    // the BLOCK centre f0 (which differs from the bell centre c_bell by
    // ~42 kHz), fb maps to f = f0 + fb and
    //   F = F0 + slope*fb + O(fb^2),
    //   F0 = f0/c - c/f0   (NONZERO - the naive F ~ 2*fb/f0 misses it),
    //   slope = 1/c + c/f0^2.
    // Both first-order networks factor exactly:
    //   A(fb) = C * (1 + j n~ fb) / (1 + j d~ fb),
    //   C = (1+j kn)/(1+j kd) (the bell at the block centre),
    //   n~ = n1/(1+j kn), d~ = d1/(1+j kd)  (both complex).
    // Bilinear map of the whole thing (s = 2 fs (1-z^-1)/(1+z^-1), fb =
    // s/(j 2 pi)): the (1+z^-1) factors cancel and
    //   A(z) = C * [(1+gn) + (1-gn) z^-1] / [(1+gd) + (1-gd) z^-1],
    //   gn = n~/(pi fs_dec), gd = d~/(pi fs_dec).
    // A complex one-pole/one-zero section (12 real mults per decimated
    // sample after folding 1/a0 in). The bilinear's digital response is
    // the exact analog factorisation at the pre-warped frequency
    // (2 tan(w/2) instead of w) - the remaining error vs the reference is
    // the F-linearisation (quadratic in fb) plus the warp, BOTH small at
    // the carriers and growing toward the skirt. MEASURED envelope
    // (Python replication of the exact constants): carriers (|fb| <=
    // 100 kHz) ~0.7%, deviation (<=350 kHz) ~5%, skirt (<=580 kHz) ~17%.
    {
        double fs_dec = rate / (double)g_secam_decim;
        double c_bell = CVBS_SECAM_BELL_CENTRE_HZ;
        double f0 = block_centre_hz;
        double F0 = f0 / c_bell - c_bell / f0;
        double slope = 1.0 / c_bell + c_bell / (f0 * f0);
        double kn = 1.26 * F0;
        double kd = 16.0 * F0;
        double n1 = 1.26 * slope;
        double d1 = 16.0 * slope;
        // C = (1 + j kn)/(1 + j kd)
        double dd = 1.0 + kd * kd;
        double c_re = (1.0 + kn * kd) / dd;
        double c_im = (kn - kd) / dd;
        // n~ = n1(1 - j kn)/(1 + kn^2)
        double nn = 1.0 + kn * kn;
        double tn_re = n1 / nn;
        double tn_im = -n1 * kn / nn;
        // d~ = d1(1 - j kd)/(1 + kd^2)
        double td_re = d1 / dd;
        double td_im = -d1 * kd / dd;
        // gn = n~ * fs / pi, gd = d~ * fs / pi  (DIMENSIONLESS: n~ has
        // seconds, from the fb = s/(j 2 pi) substitution - the first-order
        // section is (1 + (n~/2pi) s) and s = 2 fs (1-z^-1)/(1+z^-1), so
        // multiplying through by (1+z^-1) gives the coefficient n~ fs/pi.
        // An inverted 1/(pi fs) here collapsed the bell to the flat
        // constant and failed the numeric match at 37%/178%/303%.)
        double gsc = fs_dec / M_PI;
        double gn_re = tn_re * gsc, gn_im = tn_im * gsc;
        double gd_re = td_re * gsc, gd_im = td_im * gsc;
        // Numerator: C * [(1+gn) + (1-gn) z^-1]; denominator a0 = 1+gd,
        // a1 = 1-gd. Fold 1/a0 into everything.
        double an0_re = 1.0 + gn_re, an0_im = gn_im;
        double an1_re = 1.0 - gn_re, an1_im = -gn_im;
        double ad0_re = 1.0 + gd_re, ad0_im = gd_im;
        double ad1_re = 1.0 - gd_re, ad1_im = -gd_im;
        // 1/(1+gd)
        double admag2 = ad0_re * ad0_re + ad0_im * ad0_im;
        double ia_re = ad0_re / admag2;
        double ia_im = -ad0_im / admag2;
        // b0 = C*(1+gn)/(1+gd), b1 = C*(1-gn)/(1+gd), a1 = (1-gd)/(1+gd)
        // (complex multiply + divide; all stored pre-folded)
        double m0_re = c_re * an0_re - c_im * an0_im;
        double m0_im = c_im * an0_re + c_re * an0_im;
        double m1_re = c_re * an1_re - c_im * an1_im;
        double m1_im = c_im * an1_re + c_re * an1_im;
        g_secam_bell_b0_re = m0_re * ia_re - m0_im * ia_im;
        g_secam_bell_b0_im = m0_im * ia_re + m0_re * ia_im;
        g_secam_bell_b1_re = m1_re * ia_re - m1_im * ia_im;
        g_secam_bell_b1_im = m1_im * ia_re + m1_re * ia_im;
        g_secam_bell_a1_re = ad1_re * ia_re - ad1_im * ia_im;
        g_secam_bell_a1_im = ad1_im * ia_re + ad1_re * ia_im;
    }

    // De-emphasis IIR at the decimated rate: the reference's COMPLEX
    // inverseLf mask - the causal lead-lag H(s) = (1 + s/wz)/(1 + s/wp),
    // wp = 2pi f1 (85 kHz pole), wz = 2pi 3f1 (255 kHz zero) - bilinear-
    //mapped, s = 2 fs (1-z^-1)/(1+z^-1):
    //   H(z) = [(1+kz) + (1-kz) z^-1] / [(1+kp) + (1-kp) z^-1],
    //   kz = 2 fs/(wz warp), kp = 2 fs/(wp warp).
    // The COMMON pre-warp factor is what makes this realizable: a plain
    // bilinear measured 2.4% worst across the deviation band (the warp
    // peaks MID-SHELF, not at the corners), pre-warping the corners
    // independently lands them exact but stretches the mid-shelf to 3.7%
    // and breaks the shelf ratio, and the geometric-centre match
    // (sqrt(f1*3f1) = 147 kHz) still left 2.0% at the 350 kHz band edge.
    // Matched at 220 kHz - the measured error curve's balance point - the
    // deviation band holds ~1.5% worst. The common factor keeps DC gain
    // (1) and the HF shelf ratio (wz/wp = 1/3) EXACT by construction (it
    // cancels in both). The causal state resets inline here, joining the
    // hold-ring clear above: a rebuild re-enters from the porch-zero rest.
    {
        double fs_dec = rate / (double)g_secam_decim;
        double wp = 2.0 * M_PI * CVBS_SECAM_LF_BREAK_HZ;
        double wz = 2.0 * M_PI * 3.0 * CVBS_SECAM_LF_BREAK_HZ;
        double f_match = 220000.0;
        double w_match = M_PI * f_match / fs_dec;
        double warp = (w_match > 1e-12 && w_match < M_PI / 2.0)
            ? tan(w_match) / w_match : 1.0;
        double kz = 2.0 * fs_dec / (wz * warp);
        double kp = 2.0 * fs_dec / (wp * warp);
        g_secam_deemph_b0 = (1.0 + kz) / (1.0 + kp);
        g_secam_deemph_b1 = (1.0 - kz) / (1.0 + kp);
        g_secam_deemph_a1 = (1.0 - kp) / (1.0 + kp);
        g_secam_deemph_x1 = 0.0;
        g_secam_deemph_y1 = 0.0;
    }

    // Luma notch (2nd-order, broad) at the block centre: removes the steady
    // subcarrier dot pattern from the SECAM luma path (the reference's
    // combined-input block-notch, streaming-cheap form).
    {
        double w0 = 2.0 * M_PI * block_centre_hz / rate;
        double r = 0.92;   // pole radius: notch width ~ +-300 kHz at 4fsc
        g_secam_notch_b0 = 1.0;
        g_secam_notch_b1 = -2.0 * cos(w0);
        g_secam_notch_b2 = 1.0;
        g_secam_notch_a1 = g_secam_notch_b1 * r;
        g_secam_notch_a2 = r * r;
        // Unity DC gain.
        // Normalized by the FULL DC gain H(1) = (b0+b1+b2)/(1+a1+a2):
        // normalizing only the numerator's b-sum left H(1) = 2.41 and
        // railed every line's luma white (the first bars run failed
        // all-white including the black bar).
        double num_dc = g_secam_notch_b0 + g_secam_notch_b1
                        + g_secam_notch_b2;
        double den_dc = 1.0 + g_secam_notch_a1 + g_secam_notch_a2;
        if (fabs(num_dc) > 1e-9 && fabs(den_dc) > 1e-9) {
            double scale = den_dc / num_dc;   // H(1) = 1 exactly
            g_secam_notch_b0 *= scale;
            g_secam_notch_b1 *= scale;
            g_secam_notch_b2 *= scale;
        }
    }

    g_secam_coeffs_rate = rate;
    g_secam_coeffs_centre = block_centre_hz;
    g_secam_coeffs_valid = true;
}

// In-place SECAM luma notch on the filtered line.
static void cvbs_secam_notch_line(int16_t *line, int n) {
    double x1 = line[0], x2 = line[0];
    double y1 = line[0], y2 = line[0];
    for (int i = 0; i < n; i++) {
        double x0 = (double)line[i];
        double y0 = g_secam_notch_b0 * x0 + g_secam_notch_b1 * x1
                    + g_secam_notch_b2 * x2
                    - g_secam_notch_a1 * y1 - g_secam_notch_a2 * y2;
        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;
        line[i] = (int16_t)lrint(y0);
    }
}

// Median of a small double array (insertion sort; porch windows are ~10).
static double cvbs_secam_median(double *v, int count) {
    if (count <= 0) return 0.0;
    for (int i = 1; i < count; i++) {
        double x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    return v[count / 2];
}

// Field-boundary commit: vote the identification + lock the calibration
// from the PREVIOUS field's accumulators, then reset them for the new
// field. Called wherever the VBI machinery commits a new field.
static void cvbs_secam_field_commit(cvbs_decoder_t *decoder) {
    cvbs_secam_state_t *s = &decoder->secam;
    const double nominal_centre =
        0.5 * (CVBS_SECAM_FOB_HZ + CVBS_SECAM_FOR_HZ);

    // Median active-envelope over the recorded lines: the colour-killer and
    // ident envelope-gate reference for THIS field (locked at commit).
    {
        double envs[320];
        int ne = 0;
        for (int r = 0; r < s->rows && r < 320; r++) {
            if (s->centre_env[r] > 0.0) envs[ne++] = s->centre_env[r];
        }
        s->centre_env_median = cvbs_secam_median(envs, ne);
    }

    // Collect the valid porch frequencies (envelope over the killer gate
    // against the field's median active envelope).
    double valid_freqs[320];
    int nvalid = 0;
    for (int r = 0; r < s->rows && r < 320; r++) {
        if (s->row_valid[r]) valid_freqs[nvalid++] = s->porch_freq[r];
    }

    // Carrier calibration: split the porch medians into the two clusters,
    // take per-cluster medians, apply the separation + sanity gates, LOCK
    // the 10*fH split around the measured centre (BT.470: the pair sits
    // exactly 156.25 kHz apart in the sample clock; the medians locate the
    // centre but their split is skewed by porch ringing - the reference
    // measured ~178 kHz on combined input and it zeroed every neutral).
    bool calibrated = false;
    double fob = s->have_last_good ? s->last_good_fob : CVBS_SECAM_FOB_HZ;
    double for_ = s->have_last_good ? s->last_good_for : CVBS_SECAM_FOR_HZ;
    if (nvalid >= CVBS_SECAM_CAL_MIN_ROWS) {
        // valid_freqs is not sorted; sort a copy for the split-point scan.
        double sorted[320];
        memcpy(sorted, valid_freqs, (size_t)nvalid * sizeof(double));
        for (int i = 1; i < nvalid; i++) {
            double x = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > x) { sorted[j + 1] = sorted[j]; j--; }
            sorted[j + 1] = x;
        }
        const int half = nvalid / 2;
        const double low_median = sorted[half / 2];
        const double high_median =
            sorted[half + (nvalid - half) / 2];
        const double separation = high_median - low_median;
        if (separation > CVBS_SECAM_CAL_MIN_SEP_HZ
            && separation < CVBS_SECAM_CAL_MAX_SEP_HZ
            && fabs(low_median - CVBS_SECAM_FOB_HZ) <= CVBS_SECAM_CAL_MAX_OFFSET_HZ
            && fabs(high_median - CVBS_SECAM_FOR_HZ) <= CVBS_SECAM_CAL_MAX_OFFSET_HZ) {
            const double centre = 0.5 * (low_median + high_median);
            fob = centre - CVBS_SECAM_PAIR_SPLIT_HZ;
            for_ = centre + CVBS_SECAM_PAIR_SPLIT_HZ;
            calibrated = true;
        }
    }
    if (calibrated) {
        s->last_good_fob = fob;
        s->last_good_for = for_;
        s->have_last_good = true;
        s->fields_calibrated++;
    } else if (s->have_last_good) {
        s->fields_reused++;
    } else {
        s->fields_nominal++;
    }

    s->fob = fob;
    s->for_ = for_;
    s->calibrated = calibrated;
    s->carrier_offset = 0.5 * (fob + for_) - nominal_centre;

    // Identification: porch parity vote against the measured pair's split,
    // with the reference's confidence gates + the bottles/content fallbacks.
    const double split = 0.5 * (fob + for_);
    int agree_even_red = 0, picture = 0;
    for (int r = 0; r < s->rows && r < 320; r++) {
        if (!s->row_valid[r]) continue;
        bool measured_red = s->porch_freq[r] > split;
        if (measured_red == ((r % 2) == 0)) agree_even_red++;
        picture++;
    }
    double confidence = (picture > 0)
        ? (double)(agree_even_red > picture - agree_even_red
                   ? agree_even_red : picture - agree_even_red) / (double)picture
        : 0.0;
    int ident_source = 0;
    if (picture < CVBS_SECAM_CAL_MIN_ROWS || confidence < 0.75) {
        // Bottles cross-check (BT.470-6 option B, early VBI lines): the
        // trapezoids deviate the subcarrier to fob-350k / for+350k, so only
        // rows reading an actual bottle plateau vote.
        int bottle_rows = 0, bottle_agree = 0;
        const double bottle_db = fob - 350000.0;
        const double bottle_dr = for_ + 350000.0;
        for (int r = 5; r <= 15 && r < s->rows; r++) {
            if (r >= 320) break;
            double env = s->centre_env[r];
            if (s->centre_env_median <= 0.0 || env < 0.5 * s->centre_env_median)
                continue;
            double f = s->centre_freq[r];
            if (fabs(f - bottle_db) > 200000.0 && fabs(f - bottle_dr) > 200000.0)
                continue;
            bool red = f > split;
            if (red == ((r % 2) == 0)) bottle_agree++;
            bottle_rows++;
        }
        double bottle_conf = (bottle_rows > 0)
            ? (double)(bottle_agree > bottle_rows - bottle_agree
                       ? bottle_agree : bottle_rows - bottle_agree) / (double)bottle_rows
            : 0.0;
        if (bottle_rows >= 4 && bottle_conf >= 0.75) {
            agree_even_red = bottle_agree;
            picture = bottle_rows;
            ident_source = 2;
        } else {
            // Content-median fallback: a line's mean centre frequency sits
            // near its own carrier; a median split recovers the alternation.
            double content_f[320];
            int content_rows[320];
            int ncontent = 0;
            for (int r = 0; r < s->rows && r < 320; r++) {
                if (r >= 320) break;
                if (s->centre_env_median <= 0.0
                    || s->centre_env[r] < 0.3 * s->centre_env_median) continue;
                content_f[ncontent] = s->centre_freq[r];
                content_rows[ncontent] = r;
                ncontent++;
            }
            if (ncontent >= 16) {
                double med[320];
                memcpy(med, content_f, (size_t)ncontent * sizeof(double));
                double median = cvbs_secam_median(med, ncontent);
                agree_even_red = 0;
                picture = 0;
                for (int i = 0; i < ncontent; i++) {
                    bool red = content_f[i] > median;
                    if (red == ((content_rows[i] % 2) == 0)) agree_even_red++;
                    picture++;
                }
                ident_source = 1;
            }
        }
        confidence = (picture > 0)
            ? (double)(agree_even_red > picture - agree_even_red
                       ? agree_even_red : picture - agree_even_red) / (double)picture
            : 0.0;
    }
    s->dr_on_even = (picture > 0) ? (agree_even_red * 2 >= picture) : false;
    s->ident_confidence = confidence;
    s->ident_source = ident_source;

    // The D'R/D'B alternation is continuous across the FRAME (BT.470: the
    // sequence never resets at a field boundary). The vote above is
    // FIELD-LOCAL ("D'R on even lines of the field JUST COMPLETED"); the
    // next field's local parity mapping flips when the completed field had
    // an ODD line count (PAL 312/313 alternation: field starts shift by
    // lines mod 2). Without this flip the NEXT field inherits the PREVIOUS
    // field's mapping and every field following a parity flip decodes with
    // inverted ident (measured: fields 1 and 4 of the studio fixture, porch
    // reading foB on is_red lines) - the reference votes per field and
    // applies it to the SAME field (offline), so it never hits this.
    {
        int completed_parity = decoder->state.current_field;
        int completed_lines =
            (completed_parity >= 0 && completed_parity <= 1)
                ? cvbs_profile(decoder)->field_lines[completed_parity] : 0;
        if (completed_lines & 1) {
            s->dr_on_even = !s->dr_on_even;
        }
    }

    // Porch noise MAD (the concealment overshoot term): median absolute
    // deviation of the valid porch frequencies about their component's
    // carrier. Only meaningful when the porch vote was used.
    if (ident_source == 0) {
        double devs[320];
        int ndev = 0;
        for (int r = 0; r < s->rows && r < 320; r++) {
            if (!s->row_valid[r]) continue;
            double centre = s->porch_freq[r] > split ? for_ : fob;
            devs[ndev++] = fabs(s->porch_freq[r] - centre);
        }
        s->noise_mad_hz = cvbs_secam_median(devs, ndev);
    } else {
        s->noise_mad_hz = 0.0;
    }

    // Reset the per-field ACCUMULATORS for the new field. centre_env_median
    // is NOT an accumulator - it is the committed killer reference for the
    // new field (an earlier `= 0.0` here destroyed it the instant it was
    // computed: every field decoded with the colour killer disabled and
    // the bars came out gray).
    s->rows = 0;
    memset(s->porch_freq, 0, sizeof(s->porch_freq));
    memset(s->porch_env, 0, sizeof(s->porch_env));
    memset(s->row_valid, 0, sizeof(s->row_valid));
    memset(s->centre_freq, 0, sizeof(s->centre_freq));
    memset(s->centre_env, 0, sizeof(s->centre_env));
}

// Decode one SECAM line's FM chroma into g_secam_u/g_secam_v (per decimated
// sample) + update the field accumulators. Runs on the display thread
// inside decode_line_to_pixels, inside the per-line budget.
static void cvbs_secam_decode_line(cvbs_decoder_t *decoder,
                                   const int16_t *src, int n,
                                   const cvbs_baseband_profile_t *profile,
                                   int line_now, bool picture_line) {
    cvbs_secam_state_t *s = &decoder->secam;
    const double rate = cvbs_rate(decoder);
    const double block_centre =
        0.5 * (CVBS_SECAM_FOB_HZ + CVBS_SECAM_FOR_HZ) + s->carrier_offset;

    // Lazy filter build/rebuild on MATERIAL (rate, centre) movement. The
    // rate compare is a THRESHOLD, never an exact float: the self-
    // calibration refines the measured rate by tiny quanta continuously,
    // and an exact compare rebuilt the filters (and WIPED the one-line
    // hold rings) on every tick - zeroing most lines' held component.
    if (!g_secam_coeffs_valid
        || fabs(rate - g_secam_coeffs_rate) > CVBS_SECAM_RECENTRE_HZ
        || fabs(block_centre - g_secam_coeffs_centre) > CVBS_SECAM_RECENTRE_HZ) {
        cvbs_secam_build_filters(rate, block_centre);
    }

    // ---- Mix the block band down to baseband (incremental NCO, resync
    // every 256 samples like the PAL carrier tables). Per-line phase reset
    // is exact for FM: the discriminator measures phase differences within
    // the line only.
    const double dphase = 2.0 * M_PI * block_centre / rate;
    double cd = cos(dphase), sd = sin(dphase);
    double c = 1.0, sn = 0.0;
    for (int k = 0; k < n; k++) {
        if ((k & 255) == 0) {
            double ph = (double)(k & ~255) * dphase;
            c = cos(ph);
            sn = sin(ph);
        }
        double v = (double)src[k];
        g_secam_mix_re[k] = v * c;
        g_secam_mix_im[k] = -v * sn;   // e^{-jw t} * x
        double nc = c * cd - sn * sd;
        sn = sn * cd + c * sd;
        c = nc;
    }

    // ---- Band low-pass + decimate (real symmetric FIR on re/im), keeping
    // BOTH the bell-free (porch/calibration path) and the to-be-belled
    // analytic. Edge-clamped history: the settle lands in the sync region.
    const int decim = g_secam_decim;
    const int half = g_secam_lp_taps / 2;
    int dec_count = 0;
    for (int out = 0; out * decim < n; out++) {
        int centre_idx = out * decim + decim / 2;
        double ar = 0.0, ai = 0.0;
        for (int t = 0; t < g_secam_lp_taps; t++) {
            int idx = centre_idx - half + t;
            if (idx < 0) idx = 0;
            if (idx >= n) idx = n - 1;
            ar += g_secam_lp[t] * g_secam_mix_re[idx];
            ai += g_secam_lp[t] * g_secam_mix_im[idx];
        }
        if (out >= CVBS_SECAM_MAX_DEC) break;
        g_secam_lpre[out] = ar;
        g_secam_lpim[out] = ai;
        dec_count++;
    }

    // ---- Cloche (linearised reference network; see
    // cvbs_secam_bell_apply + the numeric-match harness contract).
    cvbs_secam_bell_apply(dec_count);

    // ---- Discriminate (9-tap maximally-flat central difference on the
    // mixed-down analytic signal; the exact phase derivative). TWO passes:
    // the bell path (the picture demodulation) and the BELL-FREE path (the
    // porch + ident measurements) - the reference's analyticRef/fInstRef.
    // The bell's asymmetric phase response around its centre (42 kHz below
    // the block centre) BIASES the discriminated steady carriers: measured
    // D'R porch medians -29 kHz / D'B +10 kHz, pulling the ident split
    // and every line's zero (the reference's documented reason for the
    // bell-free reference path).
    {
        const double freq_scale = rate / (2.0 * M_PI * (double)decim);
        for (int i = 0; i < dec_count; i++) {
            double ar = g_secam_ire[i], ai = g_secam_iim[i];
            double mag2 = ar * ar + ai * ai;
            if (mag2 < 1e-9) {
                g_secam_f[i] = block_centre;
            } else {
                double dr = 0.0, di = 0.0;
                for (int m = -4; m <= 4; m++) {
                    if (m == 0) continue;
                    int j = i - m;
                    if (j < 0) j = 0;
                    if (j >= dec_count) j = dec_count - 1;
                    double tap = g_secam_diff_taps[m + 4];
                    dr += tap * g_secam_ire[j];
                    di += tap * g_secam_iim[j];
                }
                // Im(conj(a) * a') = ar*di - ai*dr
                g_secam_f[i] = block_centre
                               + freq_scale * (ar * di - ai * dr) / mag2;
            }
            double br = g_secam_lpre[i], bi = g_secam_lpim[i];
            double bmag2 = br * br + bi * bi;
            if (bmag2 < 1e-9) {
                g_secam_fref[i] = block_centre;
            } else {
                double dr = 0.0, di = 0.0;
                for (int m = -4; m <= 4; m++) {
                    if (m == 0) continue;
                    int j = i - m;
                    if (j < 0) j = 0;
                    if (j >= dec_count) j = dec_count - 1;
                    double tap = g_secam_diff_taps[m + 4];
                    dr += tap * g_secam_lpre[j];
                    di += tap * g_secam_lpim[j];
                }
                g_secam_fref[i] = block_centre
                                  + freq_scale * (br * di - bi * dr) / bmag2;
            }
        }
    }

    // ---- Porch measurement (bell-free path): median discriminated Hz +
    // mean envelope over the back-porch carrier window, from the TRUE sync
    // start (the porch region spans the profile's burst + back porch in
    // SECAM - the rest carrier runs from the breezeway to active start).
    int p0 = cvbs_us_to_samples(decoder, profile->burst_start_us) + 2
             - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    int p1 = cvbs_us_to_samples(decoder, profile->active_start_us) - 2
             - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    if (p0 < 1) p0 = 1;
    if (p1 > n) p1 = n;
    double porch_freq = 0.0, porch_env = 0.0;
    bool porch_ok = false;
    if (p1 - p0 >= 8) {
        int nf = 0;
        double env_sum = 0.0;
        int env_n = 0;
        for (int x = p0; x < p1 && nf < 40; x += decim) {
            int di = (x - decim / 2) / decim;
            if (di < 0) di = 0;
            if (di >= dec_count) break;
            g_secam_porch_f[nf++] = g_secam_fref[di];
            env_sum += sqrt(g_secam_lpre[di] * g_secam_lpre[di]
                            + g_secam_lpim[di] * g_secam_lpim[di]);
            env_n++;
        }
        if (nf >= 5) {
            porch_freq = cvbs_secam_median(g_secam_porch_f, nf);
            porch_env = env_n > 0 ? env_sum / (double)env_n : 0.0;
            porch_ok = true;
        }
    }

    // ---- Line identification. PRIMARY: this line's OWN porch - the
    // bell-free discriminator reads the carriers cleanly (tens of Hz on
    // the synthetic fixture); a porch within 40 kHz of ONE carrier is
    // DECISIVE (the pair is 156.25 kHz apart, so it cannot be within 40
    // kHz of both). The gate is measured against the CARRIERS, never
    // against the pair split - the carriers sit exactly 78125 Hz from
    // the split, so a split-relative gate fails AT the carriers by
    // single-digit Hz and falls back to the parity mapping (measured:
    // is_red=1 on a 4250148 Hz porch, 8 Hz past the gate).
    // FALLBACK: the field's tracked parity mapping (the vote + the
    // 312/313 line-count flip at the field commit).
    int field_line = line_now;   // 0-based within the field
    bool near_fob = porch_ok && fabs(porch_freq - s->fob) <= 40000.0;
    bool near_for = porch_ok && fabs(porch_freq - s->for_) <= 40000.0;
    bool is_red = (near_for && !near_fob)
        ? true
        : ((near_fob && !near_for)
               ? false
               : (((field_line % 2) == 0) == s->dr_on_even));

    // ---- This line's rest (the reference's rowZero, causal): its own
    // porch median when plausible, else the field pair.
    double field_zero = is_red ? s->for_ : s->fob;
    double zero = field_zero;
    if (porch_ok && fabs(porch_freq - field_zero) <= CVBS_SECAM_PORCH_TAIL_HZ_GATE) {
        zero = porch_freq;
    }

    // ---- Demodulate the active area: deviation in D'x units, colour
    // killer, rails, click concealment (row-2 borrow), de-emphasis + LPF,
    // scale, and the causal one-line hold for the missing component.
    const double deviation = is_red ? CVBS_SECAM_DEV_DR_HZ : CVBS_SECAM_DEV_DB_HZ;
    const double lo_bound = is_red ? CVBS_SECAM_DEV_DR_LO : CVBS_SECAM_DEV_DB_LO;
    const double hi_bound = is_red ? CVBS_SECAM_DEV_DR_HI : CVBS_SECAM_DEV_DB_HI;
    const double killer_level = CVBS_SECAM_KILLER_RATIO * s->centre_env_median;

    // Active sample window in decimated indices.
    int a0 = (cvbs_us_to_samples(decoder, profile->active_start_us)
              - CVBS_SYNC_EDGE_DELAY_SAMPLES) / decim;
    int a1 = (cvbs_us_to_samples(decoder, profile->active_end_us)
              - CVBS_SYNC_EDGE_DELAY_SAMPLES) / decim;
    if (a0 < 0) a0 = 0;
    if (a1 > dec_count) a1 = dec_count;

    // The picture demod runs on the BELL-FREE discriminator (g_secam_fref):
    // measured on the studio fixture, the bilinear cloche's asymmetric phase
    // response (its centre sits 42 kHz below the block centre) biased the
    // BELLED carrier readings by -19.5 kHz at foR / ~0-10 kHz at foB, and
    // its amplitude tilt dropped the strongly-deviated sidebands' envelope
    // (|A|~0.3 at +-390 kHz) below the concealment's 12 dB dip floor,
    // squashing whole bars to ~+0.25 D'x. The bell-free path reads the
    // carriers within tens of Hz (the porch/ident already use it - the
    // reference's fInstRef). The bell stays built + verified by the
    // numeric-match contract; carrying it into the demod is future work
    // (pre-warp correction or a magnitude-only post-demod shaping).
    for (int i = 0; i < dec_count; i++) {
        double d = 0.0;
        if (picture_line && i >= a0 && i < a1 && killer_level > 0.0) {
            double env = sqrt(g_secam_lpre[i] * g_secam_lpre[i]
                              + g_secam_lpim[i] * g_secam_lpim[i]);
            if (env >= killer_level) {
                d = (g_secam_fref[i] - zero) / deviation;
            }
        }
        if (d < lo_bound) d = lo_bound;
        if (d > hi_bound) d = hi_bound;
        g_secam_demod[i] = d;
    }

    // Click concealment ("SECAM fire"): envelope dip + noise-adaptive
    // deviation overshoot; short spans interpolate, wide spans borrow the
    // previous same-component line (row-2, in the ring).
    {
        const double level = CVBS_SECAM_CLICK_LEVEL;
        const double dip_db = 12.0 - 6.0 * level;
        const double env_ratio = pow(10.0, -dip_db / 20.0);
        const double overshoot = fmax(2.6 - 1.6 * level, 1.15)
                                 + 6.0 * s->noise_mad_hz / 506000.0;
        const double flag_lo = lo_bound * overshoot;
        const double flag_hi = hi_bound * overshoot;
        // Row median envelope over the active region, BELL-FREE (the
        // carrier amplitude is flat across deviations on that path, so the
        // dip floor only trips on genuine carrier dropouts - the belled
        // envelope's amplitude tilt false-flagged every strongly-deviated
        // bar).
        double envs[CVBS_SECAM_MAX_DEC];
        int nenv = 0;
        for (int i = a0; i < a1 && i < dec_count; i++) {
            envs[nenv++] = sqrt(g_secam_lpre[i] * g_secam_lpre[i]
                                + g_secam_lpim[i] * g_secam_lpim[i]);
        }
        double env_ref = cvbs_secam_median(envs, nenv);
        if (env_ref > 0.0) {
            double floor_env = env_ref * env_ratio;
            bool flagged[CVBS_SECAM_MAX_DEC] = {false};
            bool any = false;
            for (int i = a0; i < a1 && i < dec_count; i++) {
                double env = sqrt(g_secam_lpre[i] * g_secam_lpre[i]
                                  + g_secam_lpim[i] * g_secam_lpim[i]);
                if (g_secam_demod[i] < flag_lo || g_secam_demod[i] > flag_hi
                    || env < floor_env) {
                    flagged[i] = true;
                    any = true;
                }
            }
            if (any) {
                int i = a0;
                while (i < a1 && i < dec_count) {
                    if (!flagged[i]) { i++; continue; }
                    int run_end = i;
                    while (run_end < a1 && run_end < dec_count && flagged[run_end])
                        run_end++;
                    int span_start = i - 3;
                    int span_end = run_end + 3;
                    if (span_start < 0) span_start = 0;
                    if (span_end > dec_count) span_end = dec_count;
                    if (span_end - span_start <= 6 && span_start > 0
                        && span_end < dec_count) {
                        // Short span: linearly interpolate across the click.
                        double before = g_secam_demod[span_start - 1];
                        double after = g_secam_demod[span_end];
                        double dn = (double)(span_end - span_start + 1);
                        for (int t = span_start; t < span_end; t++) {
                            g_secam_demod[t] = before
                                + (after - before) * (double)(t - span_start + 1) / dn;
                        }
                    } else {
                    // Wide span: borrow the previous same-component line
                    // (row-2, already concealed - the reference's Phase B
                    // model; the ring holds concealed deviation units).
                    {
                        const float *prev_same = s->prev_demod[is_red ? 1 : 0];
                        int prev_count = s->prev_dec_count[is_red ? 1 : 0];
                        if (prev_count > 0) {
                            for (int t = span_start; t < span_end; t++) {
                                g_secam_demod[t] =
                                    (double)prev_same[t < prev_count ? t : prev_count - 1];
                            }
                        } else {
                            for (int t = span_start; t < span_end; t++) {
                                g_secam_demod[t] = 0.0;
                            }
                        }
                    }
                    }  // wide-span borrow
                    i = span_end;
                }
            }
        }
    }

    // ---- De-emphasis (causal IIR, state carried across lines - the
    // reference's continuous overlap-save field semantics) + scale to IRE
    // fractions of the colour difference. Two handoff rings: the struct's
    // prev_demod keeps the CONCEALED DEMOD (deviation units, the row-2
    // borrow domain, next line's concealment); the file-scope hold ring
    // keeps the FINALS (IRE fractions, the one-line hold domain).
    {
        // The held (missing) component is the OTHER component's previous
        // line: on a D'R line hold D'B (slot 0), on a D'B line hold D'R
        // (slot 1). An earlier [is_red ? 1 : 0] here read the line's OWN
        // component slot - feeding the previous own-component value into
        // the wrong axis and scrambling every bar.
        const float *hold_prev = g_secam_hold_final[is_red ? 0 : 1];
        const int hold_count = g_secam_hold_count[is_red ? 0 : 1];
        for (int i = 0; i < dec_count; i++) {
            double y = g_secam_deemph_b0 * g_secam_demod[i]
                       + g_secam_deemph_b1 * g_secam_deemph_x1
                       - g_secam_deemph_a1 * g_secam_deemph_y1;
            g_secam_deemph_x1 = g_secam_demod[i];
            g_secam_deemph_y1 = y;
            g_secam_final[i] = y;
        }
        // u01/v01 per decimated sample: this line's component from the
        // demod, the missing component from the previous SAME-component
        // line (causal one-line hold; zero until the first line of each
        // component renders).
        bool hold_valid = (hold_count > 0);
        const double own_scale = is_red ? CVBS_SECAM_V_SCALE
                                         : CVBS_SECAM_U_SCALE;
        for (int i = 0; i < dec_count; i++) {
            float own = (float)(g_secam_final[i] * own_scale);
            float held = hold_valid
                ? hold_prev[i < hold_count ? i : hold_count - 1] : 0.0f;
            if (is_red) {
                g_secam_v[i] = own;    // this line's D'R
                g_secam_u[i] = held;   // the previous D'B line, SCALED
            } else {
                g_secam_u[i] = own;    // this line's D'B
                g_secam_v[i] = held;   // the previous D'R line, SCALED
            }
        }
        // Push this line into its rings. The HOLD ring stores the SCALED
        // per-component value (u01/v01 units - the next line's held
        // component reads it directly; storing the raw deviation units
        // here left the held component ~3x oversized and unnormalised,
        // scrambling every bar). The struct ring keeps the CONCEALED DEMOD
        // (deviation units) for the next line's row-2 concealment borrow.
        for (int i = 0; i < dec_count; i++) {
            g_secam_hold_final[is_red ? 1 : 0][i] =
                (float)(g_secam_final[i] * own_scale);
        }
        g_secam_hold_count[is_red ? 1 : 0] = dec_count;
        for (int i = 0; i < dec_count; i++) {
            s->prev_demod[is_red ? 1 : 0][i] = (float)g_secam_demod[i];
        }
        s->prev_dec_count[is_red ? 1 : 0] = dec_count;
    }

    // Publish the decimated geometry for the pixel loop.
    g_secam_dec_count = dec_count;

    // ---- Field accumulators (porch ring + centre stats for the ident
    // fallbacks + the active-envelope median source).
    if (line_now >= 0 && line_now < 320) {
        int r = line_now;
        s->porch_freq[r] = porch_ok ? porch_freq : 0.0;
        s->porch_env[r] = porch_ok ? porch_env : 0.0;
        s->row_valid[r] = porch_ok;
        // Centre stats over the middle of the line (bottles/content idents).
        double fsum = 0.0, esum = 0.0;
        int cn = 0;
        int c0 = a0 + 2, c1 = a1 - 2;
        for (int i = c0; i < c1 && i < dec_count; i++) {
            fsum += g_secam_fref[i];   // bell-free: the ident's fInstRef
            esum += sqrt(g_secam_lpre[i] * g_secam_lpre[i]
                         + g_secam_lpim[i] * g_secam_lpim[i]);
            cn++;
        }
        s->centre_freq[r] = cn > 0 ? fsum / (double)cn : 0.0;
        s->centre_env[r] = cn > 0 ? esum / (double)cn : 0.0;
        if (r + 1 > s->rows) s->rows = r + 1;
    }
}

// Cloche (the receiver's HF "bell" network). The reference applies
// A(f) = (1 + j1.26 F)/(1 + j16 F), F = f/c_bell - c_bell/f, per FFT bin
// at the TRUE frequency. Here: the bilinear-mapped factorisation (see
// cvbs_secam_build_filters) as a complex one-pole/one-zero section,
// y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1] - 12 real multiplies per
// decimated sample, all coefficients pre-folded. Per-line reset
// (stateless like the reference's per-row FFT - the settle lands in the
// sync region, before the porch and active areas). The harness
// numeric-match pins the digital pipeline's envelope against the
// reference closed form.
static void cvbs_secam_bell_apply(int dec_count) {
    const double b0_re = g_secam_bell_b0_re, b0_im = g_secam_bell_b0_im;
    const double b1_re = g_secam_bell_b1_re, b1_im = g_secam_bell_b1_im;
    const double a1_re = g_secam_bell_a1_re, a1_im = g_secam_bell_a1_im;
    double xr_prev = g_secam_lpre[0], xi_prev = g_secam_lpim[0];
    // Filter state, initialised to the DC steady state of the first input
    // (the settle lands in the sync region).
    double yr = xr_prev, yi = xi_prev;
    for (int i = 0; i < dec_count; i++) {
        double xr = g_secam_lpre[i], xi = g_secam_lpim[i];
        // y = b0*x + b1*x_prev - a1*y_prev   (complex)
        double ynr = b0_re * xr - b0_im * xi
                     + b1_re * xr_prev - b1_im * xi_prev
                     - a1_re * yr + a1_im * yi;
        double yni = b0_im * xr + b0_re * xi
                     + b1_im * xr_prev + b1_re * xi_prev
                     - a1_im * yr - a1_re * yi;
        yr = ynr; yi = yni;
        g_secam_ire[i] = yr;
        g_secam_iim[i] = yi;
        xr_prev = xr; xi_prev = xi;
    }
}

// Decode a single video line from samples to RGB24 pixels.
// Luma: pre-filters the entire line, then resamples to the output width.
// Chroma (colour_mode AUTO): per-line burst-gated quadrature demodulation at
// the profile's BASEBAND fsc (4.43/3.58 MHz - NOT VHS color-under). The burst
// is measured in the line-local carrier frame; the same frame demodulates
// the active-area chroma, so rotating by the burst-derived reference cancels
// ALL line-start jitter (burst-locked demod). PAL's V-switch comes from the
// burst phase alternation between lines (+-90 degree steps). Weak or missing
// bursts (or SECAM, whose chroma is FM) suppress colour for that line and
// the pixel falls back to the exact legacy grayscale mapping.
static void decode_line_to_pixels(cvbs_decoder_t *decoder,
                                  const int16_t *samples, size_t sample_count,
                                  const cvbs_levels_t *levels,
                                  uint8_t *pixels, int pixel_width) {
    if (!decoder || !samples || !levels || !pixels || pixel_width <= 0) return;

    // Always decode the full line (sync + blanking + active).
    // Active vs Full is decided at render time by cropping the src rectangle.
    if (sample_count < 16) return;

    const int16_t *src = samples;
    int n = (int)sample_count;

    // ABSOLUTE carrier anchor: the sample-stream position of the line
    // buffer's k=0 (the first sample after the previous wrap). Captured
    // before the trims below so it spans the buffer's real content.
    uint64_t carrier_anchor = decoder->global_sample_pos + 1
                               - (uint64_t)sample_count;

    const cvbs_baseband_profile_t *profile = cvbs_profile(decoder);
    int full_line_samples = cvbs_us_to_samples(decoder, profile->line_us);
    if (n > full_line_samples) n = full_line_samples;
    if (n > FILTERED_LINE_MAX) n = FILTERED_LINE_MAX;
    if (n < 16) return;

    // Pre-calculate level normalization (fixed-point: multiply by 255, divide by range)
    // DC restoration: this line's black is the committed black shifted by
    // the line's own measured back-porch offset (the mid-line clamp in
    // gui_cvbs_process_buffer), so a sagging baseline decodes at the right
    // brightness line by line. The gain (range) is the field's.
    int32_t black = levels->black_level;
    int32_t range = levels->white_level - black;
    if (range < 1) range = 1;
    if (decoder->pll.locked && decoder->dc_offset_valid) {
        black += (int32_t)lrint(decoder->dc_offset);
    }
    cvbs_luma_kernel_t kernel = cvbs_select_luma_kernel(decoder);
    const int half_taps = kernel.taps / 2;

    // =========================================================================
    // Pass 1: Apply kernel filter to entire line (one convolution per sample)
    // This is O(n * taps) but we only do it once, not per output pixel
    // =========================================================================

    // Handle left edge (first half_taps samples) - clamp to edge
    for (int i = 0; i < half_taps && i < n; i++) {
        int32_t sum = 0;
        for (int k = 0; k < kernel.taps; k++) {
            int idx = i - half_taps + k;
            if (idx < 0) idx = 0;
            sum += src[idx] * kernel.coeffs[k];
        }
        g_filtered_line[i] = (int16_t)(sum >> kernel.shift);
    }

    // Main body: outer loop over taps, inner over samples, so each inner
    // loop is a plain widening multiply-accumulate the vectoriser maps to
    // SIMD (the former per-sample inner reduction over a runtime tap count
    // stayed scalar: ~20k scalar MACs per line).
    int end = n - half_taps;
    if (end > half_taps) {
        const int body = end - half_taps;
        int32_t *acc = g_fir_acc;
        {
            const int32_t c0 = kernel.coeffs[0];
            for (int i = 0; i < body; i++) acc[i] = (int32_t)src[i] * c0;
        }
        for (int k = 1; k < kernel.taps; k++) {
            const int32_t ck = kernel.coeffs[k];
            const int16_t *p = src + k;
            for (int i = 0; i < body; i++) acc[i] += (int32_t)p[i] * ck;
        }
        const int shift = kernel.shift;
        for (int i = 0; i < body; i++) {
            g_filtered_line[half_taps + i] = (int16_t)(acc[i] >> shift);
        }
    }

    // Handle right edge (last half_taps samples) - clamp to edge
    for (int i = end; i < n; i++) {
        int32_t sum = 0;
        for (int k = 0; k < kernel.taps; k++) {
            int idx = i - half_taps + k;
            if (idx >= n) idx = n - 1;
            sum += src[idx] * kernel.coeffs[k];
        }
        g_filtered_line[i] = (int16_t)(sum >> kernel.shift);
    }

    // =========================================================================
    // Full-frame hybrid split (tbc-tools analyse HYBRID mode): the active
    // horizontal window of PICTURE lines keeps the leveled studio-swing
    // mapping; everything else - VBI lines plus the blanking/sync/burst
    // regions of picture lines - uses the fixed IRE-domain hybrid
    // background mapping (constants shared with the RF preview, translated
    // from tbc-tools' OutputWriter offsets) so the EQ/broad pulse trains and
    // any VBI data (teletext/VITS) stay readable at CRT/production-panel
    // contrast BELOW active black. The background keeps the decoded chroma
    // wherever a usable burst gates the colour decode on that line (tbc's
    // hybrid chroma mode; burstless lines decode gray either way). The
    // split is POSITIONAL like tbc's activeVideoStart/End check and the RF
    // preview's verified active_x0/x1, derived from the SAME profile active
    // window (sync-edge-delay compensated) the render crop uses.
    // =========================================================================
    bool pal625 = (decoder->state.format != CVBS_FORMAT_NTSC);
    int line_now = decoder->pll.current_line;
    bool picture_line = line_now >= profile->picture_start_line &&
                        line_now < profile->picture_start_line + profile->picture_lines;
    int s0 = cvbs_us_to_samples(decoder, profile->active_start_us)
             - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    int s1 = cvbs_us_to_samples(decoder, profile->active_end_us)
             - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    int ax0 = (int)(((int64_t)s0 * pixel_width + n - 1) / n);
    int ax1 = (int)(((int64_t)s1 * pixel_width) / n);
    if (ax0 < 0) ax0 = 0;
    if (ax1 > pixel_width) ax1 = pixel_width;
    if (ax1 < ax0) ax1 = ax0;
    // Hybrid background mapping in the IRE domain (see gui_rf_preview.h):
    // PAL 0 IRE blanking renders ~57/255 dark grey; PAL's deep (-42.9 IRE)
    // sync tip clamps at the floor - the CRT/production-panel VBI contrast
    // of tbc-tools' hybrid mode (an earlier per-line contrast-stretch stood
    // in for this; the fixed IRE-domain mapping is the real tbc model).
    double hybrid_black_ire = pal625 ? RF_HYBRID_BLACK_IRE_PAL
                                     : RF_HYBRID_BLACK_IRE_NTSC;
    double hybrid_span_ire = pal625 ? RF_HYBRID_SPAN_IRE_PAL
                                    : RF_HYBRID_SPAN_IRE_NTSC;

    // =========================================================================
    // Chroma setup
    // =========================================================================
    // SECAM: the FM-chroma path replaces the burst-gated quadrature entirely
    // (the D'R/D'B block decoded per line; see the SECAM section). PAL/NTSC
    // keep the burst-locked demod below.
    bool secam_active = (decoder->colour_mode == CVBS_COLOUR_AUTO) &&
                        (decoder->state.format == CVBS_FORMAT_SECAM) &&
                        decoder->pll.locked;
    bool pal = pal625;  // PAL/SECAM 625
    bool colour = !secam_active &&
                  (decoder->colour_mode == CVBS_COLOUR_AUTO);
    double burst_phase = 0.0, burst_amp = 0.0;
    int vswitch = 1;
    int secam_dec_count = 0;
    if (secam_active) {
        // The SECAM line decode fills g_secam_u/g_secam_v per decimated
        // sample (the pixel loop below samples them) and updates the field
        // accumulators. The luma takes the block-notch (no chroma dots).
        cvbs_secam_notch_line(g_filtered_line, n);
        cvbs_secam_decode_line(decoder, src, n, profile,
                               decoder->pll.current_line, picture_line);
        secam_dec_count = g_secam_dec_count;
        if (secam_dec_count > CVBS_SECAM_MAX_DEC) {
            secam_dec_count = CVBS_SECAM_MAX_DEC;
        }
    }
    if (colour) {
        double rate = cvbs_rate(decoder);
        double dphase = 2.0 * M_PI * profile->fsc_hz / rate;

        // Carrier table frame, by rate trust:
        // - TRUSTED declared rate (the panel plugin's fixed-rate resampler
        //   output, exact by construction): the ABSOLUTE sample-stream
        //   frame - the phase at buffer index k is (carrier_anchor + k) *
        //   dphase. A per-line k=0 reset carries every quantized wrap/snap
        //   offset of the line origin into the measured burst phase; at the
        //   panel's fixed 4x-fsc decode rate one sample of origin wobble is
        //   ~89 degrees, and the per-line burst-phase steps measured on the
        //   resampled EBU capture landed on a +-90-scattered grid instead
        //   of the true +-90 PAL alternation, flipping the V-switch on every
        //   other line and scrambling half the frame's hues. With the exact
        //   trusted rate the absolute frame is quantization-free: the
        //   sample positions are exact and dphase is exact, so the
        //   per-line steps are the true alternation (measured: every bar
        //   within +-0.2 deg of the EBU vectors, coherence 1.00).
        // - Self-calibrated rate (the direct input paths): the PER-LINE
        //   k=0 reset. The absolute frame demands an exact rate - the
        //   self-calibration's ~0.01% residual error accumulates into tens
        //   of degrees of per-line slip against the real carrier and
        //   scrambles the hues (measured on the direct-rate EBU decode
        //   after the absolute frame landed: FIXED-sync coherence 0.28) -
        //   while the per-line reset absorbs the rate residual per line
        //   (the direct-path decode measured every bar within +-0.7 deg
        //   through the refinement). Incremental rotation with a resync
        //   every 256 samples (drift far below the demod resolution).
        uint64_t table_anchor = decoder->rate_trusted ? carrier_anchor : 0;
        double cd = cos(dphase), sd = sin(dphase);
        double c = cos((double)table_anchor * dphase);
        double s = sin((double)table_anchor * dphase);
        // One pass over the line: the incremental carrier rotation feeds the
        // five prefix sums directly (no per-sample table store; every
        // window below reads prefix differences).
        double acc_s = 0.0, acc_c = 0.0, acc_n = 0.0, acc_i = 0.0, acc_q = 0.0;
        g_pf_s[0] = g_pf_c[0] = g_pf_n[0] = g_pf_i[0] = g_pf_q[0] = 0.0;
        for (int k = 0; k < n; k++) {
            if ((k & 255) == 0) {
                double ph = (double)(table_anchor + (uint64_t)k) * dphase;
                c = cos(ph);
                s = sin(ph);
            }
            double v = (double)src[k];
            acc_s += v;
            acc_c += c;
            acc_n += s;
            acc_i += v * c;
            acc_q += v * s;
            g_pf_s[k + 1] = acc_s;
            g_pf_c[k + 1] = acc_c;
            g_pf_n[k + 1] = acc_n;
            g_pf_i[k + 1] = acc_i;
            g_pf_q[k + 1] = acc_q;
            double nc = c * cd - s * sd;
            s = s * cd + c * sd;
            c = nc;
        }

        // Burst window from the profile (trim one settling sample at each
        // edge, like the RF decoder's burst trim). The window is measured
        // from the H-sync START, which is the line-local phase origin,
        // compensated for the sync-lowpass edge delay (see below).
        int b0 = cvbs_us_to_samples(decoder, profile->burst_start_us) + 1
                 - CVBS_SYNC_EDGE_DELAY_SAMPLES;
        int b1 = cvbs_us_to_samples(decoder, profile->burst_end_us) - 1
                 - CVBS_SYNC_EDGE_DELAY_SAMPLES;
        if (b0 < 1) b0 = 1;
        if (b1 > n) b1 = n;
        // Snap the burst window to WHOLE carrier cycles (shrink-only): a
        // fractional-cycle window leaves a truncation phasor in the burst
        // measurement (the NTSC window is 8.95 cycles at 40 MSPS - measured:
        // a few degrees of burst-phase bias rotating every hue slightly,
        // e.g. the red bar's blue channel +0.15). Whole cycles make both
        // the mean subtraction and the correlation exact, and the measured
        // burst amplitude becomes the TRUE amplitude (the gain exact).
        if (b1 - b0 >= 4) {
            double cyc = rate / profile->fsc_hz;
            int n_cyc = (int)((double)(b1 - b0) / cyc);
            if (n_cyc >= 1) b1 = b0 + (int)(n_cyc * cyc + 0.5);
        }
        if (b1 - b0 >= 4) {
            // Mean-subtracted correlation: the window spans a non-integer
            // number of carrier cycles (88 samples ~ 9.75 PAL cycles), so
            // WITHOUT mean removal the blanking DC (8192 counts) leaks a
            // ~240-count constant phasor into the measurement - measured:
            // amp 687/944 alternating around the true 640 with a 57 deg
            // phase step instead of the true +-90 deg (V-switch) - every
            // downstream hue wrong. Removing the window mean kills the DC
            // term exactly; the carrier term is unaffected.
            // sum (src - mean) * carrier = sum src*carrier - mean * sum carrier,
            // all four sums as prefix differences.
            double wlen = (double)(b1 - b0);
            double wmean = (g_pf_s[b1] - g_pf_s[b0]) / wlen;
            double sum_i = (g_pf_i[b1] - g_pf_i[b0])
                           - wmean * (g_pf_c[b1] - g_pf_c[b0]);
            double sum_q = (g_pf_q[b1] - g_pf_q[b0])
                           - wmean * (g_pf_n[b1] - g_pf_n[b0]);
            double norm = 2.0 / wlen;
            double bi = sum_i * norm, bq = sum_q * norm;
            burst_amp = sqrt(bi * bi + bq * bq);
            burst_phase = atan2(bq, bi);
        }

        // Weak-burst gate: a healthy burst peaks at 20 IRE = 0.2 of the
        // black..white range; below 10% of that the line decodes gray
        // (never amplify noise into colour, mirroring the RF decoder's
        // burst-valid gating).
        if (burst_amp < 0.02 * (double)range) {
            colour = false;
        } else {
            // PAL V-switch from the burst phase alternation between lines
            // (+-90 degree steps). NTSC bursts sit on the -U axis with no
            // alternation (vswitch stays +1).
            if (pal && decoder->prev_burst_valid) {
                double step = burst_phase - decoder->prev_burst_phase;
                while (step > M_PI) step -= 2.0 * M_PI;
                while (step < -M_PI) step += 2.0 * M_PI;
                vswitch = (step >= 0.0) ? 1 : -1;
            }
            decoder->prev_burst_phase = burst_phase;
            decoder->prev_burst_valid = 1;
        }
    }
    if (!colour) {
        decoder->prev_burst_valid = 0;  // a gray line breaks the V-switch chain
    }

    // Back-porch mean (burst end .. active start, trimmed two samples at
    // each edge): ALWAYS true 0 IRE blanking - flat, chroma-free, ramp-free
    // (and blank on VBI lines too: the line is exactly two half-lines in
    // both profiles, so the local porch window never touches an EQ pulse;
    // only the ~2.5 broad-pulse lines contribute tip samples, <1% of the
    // field mean). Accumulated per field and committed as blanking/black/
    // threshold - the histogram's second-lowest peak runs below true blank
    // on saturated chroma (measured: -30.5 IRE on 100% bars). Gated on PLL
    // lock: acquisition-era wraps land mid-line and their "porch" windows
    // sit on arbitrary content.
    if (decoder->pll.locked) {
        int p0 = cvbs_us_to_samples(decoder, profile->burst_end_us) + 2
                 - CVBS_SYNC_EDGE_DELAY_SAMPLES;
        int p1 = cvbs_us_to_samples(decoder, profile->active_start_us) - 2
                 - CVBS_SYNC_EDGE_DELAY_SAMPLES;
        if (p0 < 1) p0 = 1;
        if (p1 > n) p1 = n;
        if (p1 - p0 >= 8) {
            int64_t psum = 0;
            for (int k = p0; k < p1; k++) psum += src[k];
            decoder->adaptive.porch_sum += psum;
            decoder->adaptive.porch_count += (p1 - p0);
            // Clean subset: lines on the committed baseline (the per-line
            // clamp's offset within ~4 IRE). The field commit prefers this
            // subset so the ~30 sagging lines after a vertical interval do
            // not bias the committed blanking (measured: +1.5 IRE), and
            // falls back to all lines when most of the field moved.
            bool on_baseline = true;
            if (decoder->adaptive.porch_valid && decoder->dc_offset_valid) {
                double swing_nom = (double)decoder->adaptive.porch_level
                                   - (double)levels->sig_min;
                if (swing_nom > 0.0 && fabs(decoder->dc_offset) > 0.1 * swing_nom) {
                    on_baseline = false;
                }
            }
            if (on_baseline) {
                decoder->adaptive.porch_sum_clean += psum;
                decoder->adaptive.porch_count_clean += (p1 - p0);
            }
        }
    }

    // Burst-locked rotation. The measured burst angle carries ALL common-mode
    // line-start jitter (the line-local carrier frame is anchored at the PLL
    // wrap point, not the exact sync edge), and so does the measured chroma -
    // rotating by (expected burst angle - measured burst angle) cancels it.
    //
    // The expected angles follow the REAL ITU encoder convention (validated
    // on live captures - a self-invented convention here decoded U correctly
    // but V INVERTED on real signals, mirroring every hue): modulated chroma
    // is C = U sin(wt) + V cos(wt) (red at ~103 deg on the vectorscope), so
    // in this decoder's (cos, sin) correlation frame the measured pair is
    // R(-delta) * (V, U) - the axes are SWAPPED relative to the correlation
    // basis. The NTSC burst sits at 180 deg = -U, which in this frame is the
    // -sin axis = -90 deg; PAL's burst swings +/-45 deg about -U (i.e. -90
    // +/- 45 deg here) with its V component matching the transmitted V
    // inversion, so the vswitch-detected expected angle is -90 + vswitch*45.
    // The recovered (uc, vc) is then (V, U): the U/V assignment below swaps
    // them back, and vswitch unflips PAL's alternating transmitted V.
    double alpha = 0.0, gain = 0.0;
    if (colour) {
        double psi_expected = pal
            ? (-M_PI / 2.0 + (double)vswitch * (M_PI / 4.0))
            : -M_PI / 2.0;
        alpha = psi_expected - burst_phase;
        while (alpha > M_PI) alpha -= 2.0 * M_PI;
        while (alpha < -M_PI) alpha += 2.0 * M_PI;
        // Chroma gain: the burst peak maps to 20 IRE = 0.2 of the Y range,
        // exactly like the RF decoder's burst normalization AGC.
        gain = 0.2 / burst_amp;
    }
    // Carrier period in samples (PAL 9 / NTSC 11 at 40 MSPS): the per-pixel
    // chroma integration window half-width (see the pixel loop below).
    int chroma_half = (int)(cvbs_rate(decoder) / profile->fsc_hz + 0.5);
    if (chroma_half < 2) chroma_half = 2;

    // =========================================================================
    // Pass 2: Resample filtered line to output pixels (simple lookup) + chroma
    // =========================================================================
    //
    // THROUGHPUT CONTRACT. This loop runs for every output pixel of every
    // line (~18M pixels/s at the panel's 4x-fsc PAL rate), so nothing that
    // is constant per line may be computed here: no per-pixel divisions,
    // trig, rate lookups or window loops. The earlier form of this loop
    // (two window loops + ~5 divisions + cos/sin per pixel) measured 56 us
    // per line = 88% of the real-time budget by itself; the display thread
    // then fell behind the feed, the capture buffer overran after ~8-10 s
    // and every dropped chunk was a stream jump into the sync chain (the
    // live "decoder chokes after 8-10 s" + growing row shift). Every window
    // quantity is a prefix-sum difference; every reciprocal is a per-line
    // (or per-window-length) constant.

    // Per-line constants hoisted out of the pixel loop.
    const double ire_per_count = 100.0 / (double)range;
    const double inv_hybrid_span = 1.0 / hybrid_span_ire;
    const double ca = colour ? cos(alpha) : 1.0;
    const double sa = colour ? sin(alpha) : 0.0;
    const double vsw_gain = (double)vswitch * gain;

    // Whole-cycle window snap by window LENGTH (the burst block's rule: the
    // box mean and the I/Q correlation are exact over whole carrier cycles,
    // shrink-only). The nominal window is 2*chroma_half samples; only the
    // line edges clamp it shorter, so a small table indexed by the clamped
    // length replaces the per-pixel division + rounding. Alongside it the
    // per-length reciprocals (mean and 2/len correlation norm).
    enum { CVBS_WIN_TABLE = 256 };
    int snap_len[CVBS_WIN_TABLE];
    double snap_inv[CVBS_WIN_TABLE];
    double snap_norm[CVBS_WIN_TABLE];
    int win_nominal = 2 * chroma_half;
    if (win_nominal > CVBS_WIN_TABLE - 1) win_nominal = CVBS_WIN_TABLE - 1;
    if (colour) {
        double cyc = cvbs_rate(decoder) / profile->fsc_hz;
        for (int len = 0; len <= win_nominal; len++) {
            int n_cyc = (int)((double)len / cyc);
            int snapped = (n_cyc >= 1) ? (int)(n_cyc * cyc + 0.5) : 0;
            if (snapped > len) snapped = len;
            snap_len[len] = snapped;
            snap_inv[len] = snapped > 0 ? 1.0 / (double)snapped : 0.0;
            snap_norm[len] = snapped > 0 ? 2.0 / (double)snapped : 0.0;
        }
    }

    // Exact integer window centre per pixel, x_center(px) =
    // floor(((2px+1) * n) / (2W)), advanced incrementally (Bresenham) -
    // bit-identical to the former per-pixel 64-bit division.
    const int64_t xc_den = 2 * (int64_t)pixel_width;
    const int64_t xc_inc_q = (2 * (int64_t)n) / xc_den;
    const int64_t xc_inc_r = (2 * (int64_t)n) % xc_den;
    int64_t xc = (int64_t)n / xc_den;
    int64_t xc_rem = (int64_t)n % xc_den;

    // Fixed-point position tracking (16.16 format)
    int32_t pos = 0;
    int32_t step = (n << 16) / pixel_width;

    for (int px = 0; px < pixel_width; px++) {
        int idx = pos >> 16;
        if (idx >= n) idx = n - 1;

        // Hybrid split (see the setup above): leveled studio-swing mapping
        // for the active window of picture lines, the fixed IRE-domain
        // hybrid background mapping everywhere else. Luma in the IRE domain
        // (black = 0 IRE blank, white = 100 IRE - the level path's model);
        // the colour path below refines it through the chroma window's box
        // mean where a window is available. Gray lines (no burst / Off /
        // SECAM) write R=G=B through the same split: chroma-free behaviour
        // unchanged, only the level transfer differs from the legacy raw
        // 0..255 mapping.
        bool leveled = picture_line && px >= ax0 && px < ax1;
        double ire = (double)(g_filtered_line[idx] - black) * ire_per_count;

        // Chroma: integrate I/Q over a window of TWO carrier periods
        // centered on the pixel (chroma_half each side), clamped to the
        // LINE, not the active window: the hybrid background keeps the
        // decoded chroma (tbc's hybrid chroma mode) - VBI lines carrying a
        // burst (teletext-style) and the blanking/burst regions of picture
        // lines decode their true chroma. The pixel span alone
        // (n/pixel_width samples) is NARROWER than one carrier period, so
        // integrating only the span aliases the subcarrier into full-
        // saturation alternating pixels (measured: 255,255,0 / 0,255,0 /
        // 0,0,255 patterns). The wider window is the demod lowpass; the
        // line-local prefix sums carry the carrier phase.
        double u01 = 0.0, v01 = 0.0;
        if (secam_active) {
            // SECAM: sample the per-decimated-sample u/v (nearest decimated
            // sample). Chroma on the ACTIVE window of picture lines only:
            // the porch/VBI carriers render neutral (the reference's
            // V-interval neutralization; the horizontal blanking too).
            if (picture_line && px >= ax0 && px < ax1 && secam_dec_count > 0) {
                int dec = idx / g_secam_decim;
                if (dec >= secam_dec_count) dec = secam_dec_count - 1;
                u01 = g_secam_u[dec];
                v01 = g_secam_v[dec];
            }
        } else if (colour) {
            int x_center = (int)xc;
            int c0 = x_center - chroma_half;
            int c1 = x_center + chroma_half;
            if (c0 < 1) c0 = 1;
            if (c1 > n) c1 = n;
            int len = c1 - c0;
            if (len > win_nominal) len = win_nominal;
            int wl = len > 0 ? snap_len[len] : 0;
            if (wl > 0) {
                c1 = c0 + wl;
                // Same mean subtraction as the burst measurement: the local
                // luma DC (up to 100 IRE at the white bar) leaks into a
                // non-integer-cycle window and shifts u/v by up to ~0.2
                // (bar-dependent hue error). Removing the window mean makes
                // the chroma measurement luma-independent. Colour-path
                // luma: the SAME box window's mean (the +-2-carrier-period
                // box rejects the subcarrier to ~2-3%, while the 17-tap
                // luma kernel still passes ~10% of it - measured: every
                // strongly-coloured bar's luma lifted by ~10% of its chroma
                // amplitude). All sums are prefix differences.
                double cmean = (g_pf_s[c1] - g_pf_s[c0]) * snap_inv[len];
                ire = (cmean - (double)black) * ire_per_count;
                double sum_i = (g_pf_i[c1] - g_pf_i[c0])
                               - cmean * (g_pf_c[c1] - g_pf_c[c0]);
                double sum_q = (g_pf_q[c1] - g_pf_q[c0])
                               - cmean * (g_pf_n[c1] - g_pf_n[c0]);
                double norm = snap_norm[len];
                double ic = sum_i * norm, qc = sum_q * norm;
                // Correlation algebra for the REAL encoder convention
                // (C = U sin(wt) + V cos(wt)): the mean-subtracted pair is
                // (ic, qc) = R(-delta) * (V, U); alpha = expected_burst -
                // measured_burst = delta exactly, so R(+delta) recovers
                // (uc, vc) = (V, U) with NO residual rotation. The U/V
                // assignment swaps the axes back (u01 from vc, v01 from
                // uc) and vswitch unflips PAL's alternating V. Validated by
                // the synthetic bar-hue contract (red ~103 deg, burst 180
                // deg vectorscope) and the EBU capture (every bar within
                // +-0.3 deg).
                double uc = ic * ca - qc * sa;
                double vc = ic * sa + qc * ca;
                u01 = vc * gain;
                v01 = uc * vsw_gain;
            }
            // Advance the exact window centre to the next pixel.
            xc += xc_inc_q;
            xc_rem += xc_inc_r;
            if (xc_rem >= xc_den) { xc_rem -= xc_den; xc++; }
        }

        // Display luma through the hybrid split: studio-swing mapping for
        // the leveled active window (shared rf_display_level contract -
        // sync below black clamps at the floor like a CRT), the fixed
        // IRE-domain hybrid background mapping everywhere else (clamped at
        // its own floor, so sync tips stay faintly visible below the
        // blanking band). + the standard YUV->RGB matrix the RF renderer
        // uses (u01/v01 are 0 on gray lines -> R=G=B).
        double dy = leveled
            ? rf_display_level(ire * 0.01)
            : (ire - hybrid_black_ire) * inv_hybrid_span;
        if (dy < 0.0) dy = 0.0;
        if (dy > 1.0) dy = 1.0;
        double r01 = dy + 1.140 * v01;
        double g01 = dy - 0.395 * u01 - 0.581 * v01;
        double b01 = dy + 2.032 * u01;
        if (r01 < 0.0) r01 = 0.0;
        if (r01 > 1.0) r01 = 1.0;
        if (g01 < 0.0) g01 = 0.0;
        if (g01 > 1.0) g01 = 1.0;
        if (b01 < 0.0) b01 = 0.0;
        if (b01 > 1.0) b01 = 1.0;
        pixels[px * 3 + 0] = (uint8_t)(r01 * 255.0 + 0.5);
        pixels[px * 3 + 1] = (uint8_t)(g01 * 255.0 + 0.5);
        pixels[px * 3 + 2] = (uint8_t)(b01 * 255.0 + 0.5);
        pos += step;
    }

}

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

bool gui_cvbs_init(cvbs_decoder_t *decoder) {
    if (!decoder) return false;

    memset(decoder, 0, sizeof(cvbs_decoder_t));

    // Colour decode defaults ON (burst-gated: signals without a usable
    // burst decode the exact legacy grayscale anyway; the Off mode is one
    // button click away).
    decoder->colour_mode = CVBS_COLOUR_AUTO;

    // Allocate field buffers (each field is half the frame height).
    // RGB24 rows (3 bytes per pixel) since the colour decode writes full
    // RGB per pixel; grayscale mode writes R=G=B so one buffer layout serves
    // both. Max field height: PAL 313 lines (taller parity).
    decoder->field_height = cvbs_baseband_profiles[0].field_lines[1];
    size_t field_size = (size_t)CVBS_MAX_WIDTH * 3u *
                        (size_t)((CVBS_MAX_HEIGHT + 1) / 2);

    decoder->field_buffer[0] = (uint8_t *)calloc(field_size, 1);
    if (!decoder->field_buffer[0]) {
        return false;
    }

    decoder->field_buffer[1] = (uint8_t *)calloc(field_size, 1);
    if (!decoder->field_buffer[1]) {
        free(decoder->field_buffer[0]);
        decoder->field_buffer[0] = NULL;
        return false;
    }

    // Allocate deinterlaced frame buffer at full-frame resolution (D1, RGB24)
    decoder->frame_width = CVBS_FRAME_WIDTH;
    decoder->frame_height = CVBS_PAL_HEIGHT;  // Start with PAL full-frame height
    decoder->frame_buffer = (uint8_t *)calloc((size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT * 3u, 1);
    if (!decoder->frame_buffer) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        return false;
    }

    // Allocate double-buffered display buffers for thread safety (RGB24)
    // Display thread writes to back buffer, render thread reads from front
    decoder->display_front = (uint8_t *)calloc((size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT * 3u, 1);
    decoder->display_back = (uint8_t *)calloc((size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT * 3u, 1);
    if (!decoder->display_front || !decoder->display_back) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        free(decoder->display_front);
        free(decoder->display_back);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        decoder->display_front = NULL;
        decoder->display_back = NULL;
        return false;
    }
    atomic_init(&decoder->display_ready, 0);

    // Create raylib Image for video display (RGBA format at full-frame resolution)
    // Allocate our own pixel buffer so we control the memory
    Color *frame_pixels = (Color *)calloc((size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT, sizeof(Color));
    if (!frame_pixels) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        free(decoder->display_front);
        free(decoder->display_back);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        decoder->display_front = NULL;
        decoder->display_back = NULL;
        return false;
    }
    decoder->frame_image.data = frame_pixels;
    decoder->frame_image.width = CVBS_MAX_WIDTH;
    decoder->frame_image.height = CVBS_MAX_HEIGHT;
    decoder->frame_image.mipmaps = 1;
    decoder->frame_image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    decoder->texture_valid = false;

    // Allocate line buffer for cross-boundary line assembly
    decoder->line_buffer = (int16_t *)calloc(CVBS_LINE_BUFFER_SIZE, sizeof(int16_t));
    if (!decoder->line_buffer) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        free(decoder->display_front);
        free(decoder->display_back);
        free(decoder->frame_image.data);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        decoder->display_front = NULL;
        decoder->display_back = NULL;
        decoder->frame_image.data = NULL;
        return false;
    }
    decoder->line_buffer_count = 0;

    // Initialize adaptive levels and allocate histogram sample buffer
    memset(&decoder->adaptive, 0, sizeof(decoder->adaptive));
    decoder->adaptive.level_sample_buf = (int16_t *)calloc(CVBS_LEVEL_SAMPLE_BUFFER_SIZE, sizeof(int16_t));
    if (!decoder->adaptive.level_sample_buf) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        free(decoder->display_front);
        free(decoder->display_back);
        free(decoder->frame_image.data);
        free(decoder->line_buffer);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        decoder->display_front = NULL;
        decoder->display_back = NULL;
        decoder->frame_image.data = NULL;
        decoder->line_buffer = NULL;
        return false;
    }

    // Initialize software PLL with PAL default
    decoder->pll.phase = 0;
    decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_PAL;
    decoder->pll.freq_adjust = 0;
    decoder->pll.phase_error = 0;
    decoder->pll.phase_integral = 0;
    decoder->pll.good_sync_count = 0;
    decoder->pll.bad_sync_count = 0;
    decoder->pll.locked = false;
    decoder->pll.current_line = 0;
    decoder->pll.samples_in_line = 0;
    decoder->pll.total_samples = 0;

    // Initialize lowpass filter state
    decoder->lpf_state = 0;

    // Initialize edge detection state (uses filtered signal)
    decoder->last_filtered_above = true;
    decoder->global_sample_pos = 0;

    // Initialize H-sync pulse tracking
    decoder->in_hsync_pulse = false;
    decoder->hsync_pulse_start = 0;

    // Initialize V-sync detector state
    decoder->vsync_last_edge_pos = 0;

    // Initialize V-sync detection state
    memset(&decoder->vsync, 0, sizeof(decoder->vsync));

    // Initialize legacy state
    decoder->lines_since_vsync = 0;

    // Initialize frame state
    decoder->state.format = CVBS_FORMAT_UNKNOWN;
    decoder->state.total_lines = 0;
    decoder->state.active_lines = 0;
    decoder->state.current_line = 0;
    decoder->state.current_field = 0;
    decoder->state.in_vsync = false;
    decoder->state.frame_complete = false;
    decoder->state.frames_decoded = 0;
    decoder->level_mode = CVBS_LEVEL_MODE_AUTO;
    decoder->sync_mode = CVBS_SYNC_MODE_AUTO;
    decoder->fixed_levels_valid = false;
    cvbs_reset_fixed_sync_lock(decoder);

    // SECAM state: the carrier pair starts at NOMINAL (the whole-struct
    // memset above zeroed it - a zero pair makes the mix block centre 0 Hz
    // on the first field) and the ident defaults to the studio D'R-on-even
    // convention until the first field's vote lands.
    decoder->secam.fob = CVBS_SECAM_FOB_HZ;
    decoder->secam.for_ = CVBS_SECAM_FOR_HZ;
    decoder->secam.dr_on_even = true;

    return true;
}

void gui_cvbs_cleanup(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    if (decoder->texture_valid) {
        UnloadTexture(decoder->frame_texture);
        decoder->texture_valid = false;
    }

    // Free frame image data (we allocated it ourselves)
    if (decoder->frame_image.data) {
        free(decoder->frame_image.data);
        decoder->frame_image.data = NULL;
    }

    free(decoder->field_buffer[0]);
    free(decoder->field_buffer[1]);
    free(decoder->frame_buffer);
    free(decoder->display_front);
    free(decoder->display_back);
    free(decoder->line_buffer);
    free(decoder->adaptive.level_sample_buf);

    decoder->field_buffer[0] = NULL;
    decoder->field_buffer[1] = NULL;
    decoder->frame_buffer = NULL;
    decoder->display_front = NULL;
    decoder->display_back = NULL;
    decoder->line_buffer = NULL;
    decoder->adaptive.level_sample_buf = NULL;
}

void gui_cvbs_reset(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    // Clear field buffers (RGB24)
    size_t field_size = (size_t)CVBS_MAX_WIDTH * 3u *
                        (size_t)((CVBS_MAX_HEIGHT + 1) / 2);
    if (decoder->field_buffer[0]) {
        memset(decoder->field_buffer[0], 0, field_size);
    }
    if (decoder->field_buffer[1]) {
        memset(decoder->field_buffer[1], 0, field_size);
    }

    // Clear frame buffers (RGB24)
    if (decoder->frame_buffer) {
        memset(decoder->frame_buffer, 0, (size_t)CVBS_MAX_WIDTH * 3u * (size_t)CVBS_MAX_HEIGHT);
    }
    // Clear both display buffers
    if (decoder->display_front) {
        memset(decoder->display_front, 0, (size_t)CVBS_MAX_WIDTH * 3u * (size_t)CVBS_MAX_HEIGHT);
    }
    if (decoder->display_back) {
        memset(decoder->display_back, 0, (size_t)CVBS_MAX_WIDTH * 3u * (size_t)CVBS_MAX_HEIGHT);
    }
    atomic_store(&decoder->display_ready, 0);

    // Reset the chroma reference chain (a stale V-switch would poison the
    // first post-reset line) and keep the colour MODE as configured.
    decoder->prev_burst_phase = 0.0;
    decoder->prev_burst_valid = 0;

    // Reset the SECAM FM-chroma state (calibration, ident, rings) and the
    // file-scope filter cache (a format/rate switch rebuilds it lazily).
    // The carrier pair starts at NOMINAL (not zero: a zero pair makes the
    // mix block centre 0 Hz on the first field - garbage until the first
    // commit) and the ident defaults to the studio D'R-on-even convention
    // until the first field's vote lands.
    memset(&decoder->secam, 0, sizeof(decoder->secam));
    decoder->secam.fob = CVBS_SECAM_FOB_HZ;
    decoder->secam.for_ = CVBS_SECAM_FOR_HZ;
    decoder->secam.dr_on_even = true;
    g_secam_coeffs_valid = false;
    g_secam_hold_count[0] = 0;
    g_secam_hold_count[1] = 0;

    // Reset line buffer
    decoder->line_buffer_count = 0;

    // Reset adaptive levels (preserve allocated buffer pointer)
    int16_t *level_buf = decoder->adaptive.level_sample_buf;
    memset(&decoder->adaptive, 0, sizeof(decoder->adaptive));
    decoder->adaptive.level_sample_buf = level_buf;

    // Reset software PLL (keep line period consistent with selected system)
    decoder->pll.phase = 0;
    decoder->pll.line_period = cvbs_nominal_line_period(decoder);
    decoder->pll.freq_adjust = 0;
    decoder->pll.phase_error = 0;
    decoder->pll.phase_integral = 0;
    decoder->pll.good_sync_count = 0;
    decoder->pll.bad_sync_count = 0;
    decoder->pll.locked = false;
    decoder->pll.current_line = 0;
    decoder->pll.samples_in_line = 0;
    // Don't reset total_samples - keep for debug

    // Reset lowpass filter state
    decoder->lpf_state = 0;

    // Reset edge detection state (uses filtered signal)
    decoder->last_filtered_above = true;
    decoder->global_sample_pos = 0;

    // Reset H-sync pulse tracking
    decoder->in_hsync_pulse = false;
    decoder->hsync_pulse_start = 0;

    // Reset V-sync detector state
    decoder->vsync_last_edge_pos = 0;

    // Reset V-sync detection state
    memset(&decoder->vsync, 0, sizeof(decoder->vsync));

    // Reset field ready flags (important when switching modes)
    decoder->field_ready[0] = false;
    decoder->field_ready[1] = false;
    decoder->field_line_count[0] = 0;
    decoder->field_line_count[1] = 0;
    decoder->field_sequence = 0;
    decoder->field_last_update_seq[0] = 0;
    decoder->field_last_update_seq[1] = 0;
    decoder->fixed_levels_valid = false;
    decoder->fixed_black_level = 0;
    decoder->fixed_white_level = 0;
    // Self-calibrated rate state: a reset stream re-measures from scratch.
    decoder->measured_rate_hz = 0;
    decoder->measured_rate_valid = false;
    decoder->hh_ring_count = 0;
    decoder->hh_period_samples = 0.0;
    decoder->hh_reject_streak = 0;
    cvbs_reset_fixed_sync_lock(decoder);

    // Reset legacy state
    decoder->lines_since_vsync = 0;
    decoder->dc_offset = 0.0;
    decoder->dc_offset_valid = false;

    // Reset frame state (preserve decoder->state.format set by gui_cvbs_set_format)
    decoder->state.current_line = 0;
    decoder->state.current_field = 0;
    decoder->state.in_vsync = false;
    decoder->state.frame_complete = false;
    // Note: display_ready already cleared atomically above

    // Reset statistics
    decoder->sync_errors = 0;
    decoder->format_changes = 0;
    decoder->stream_jumps = 0;

    // Reset debug statistics
    memset(&decoder->debug, 0, sizeof(decoder->debug));
}

//-----------------------------------------------------------------------------
// Adaptive Level Estimation (Histogram-Based)
//-----------------------------------------------------------------------------

// Accumulate samples for histogram analysis (called from main processing loop)
// Subsamples to keep buffer size manageable while capturing signal distribution
// Uses per-decoder buffer to support multiple simultaneous decoders
static inline void accumulate_level_sample(cvbs_decoder_t *decoder, int16_t sample) {
    if (!decoder->adaptive.level_sample_buf) return;

    // Subsample: collect every 16th sample to spread across the field
    if (++decoder->adaptive.subsample_counter >= 16) {
        decoder->adaptive.subsample_counter = 0;
        if (decoder->adaptive.level_sample_count < CVBS_LEVEL_SAMPLE_BUFFER_SIZE) {
            decoder->adaptive.level_sample_buf[decoder->adaptive.level_sample_count++] = sample;
        }
    }
}

// Commit levels at end of field using histogram-based detection
// Uses trigger_analyze_cvbs_levels() for robust sync tip and blanking detection
static void commit_adaptive_levels(cvbs_decoder_t *decoder) {
    if (!decoder->adaptive.level_sample_buf) return;

    // Need minimum samples for reliable histogram
    if (decoder->adaptive.level_sample_count < 1000) {
        decoder->adaptive.level_sample_count = 0;
        return;
    }

    // Analyze collected samples using histogram-based detection
    cvbs_levels_t detected;
    trigger_analyze_cvbs_levels(decoder->adaptive.level_sample_buf,
                                decoder->adaptive.level_sample_count, &detected);

    // Reset sample buffer for next field
    decoder->adaptive.level_sample_count = 0;

    // Picker failure fallback: the x16 raw subsampling ALIASES the
    // subcarrier into pseudo-noise that breaks the two-peak picker on some
    // fields (measured: every NTSC field of the synthetic fixture). The
    // porch path below is self-sufficient for every level the commit uses
    // (tip = the raw min, which never fails; blanking/black/threshold = the
    // porch; white = porch + 90% of the filtered luma max), so a picker
    // failure only falls back to "keep previous levels" when there is no
    // porch data yet (before the first porch commit).
    bool picker_ok = (detected.range >= 100);


    // Porch-derived blanking + filtered-domain luma max. The back porch is
    // always true 0 IRE blanking (flat, chroma-free, ramp-free): once a
    // field's per-line porch mean has data it owns blanking/black/threshold
    // (the sync tip still comes from the raw histogram - chroma never
    // reaches the tip), and the white comes from the filtered luma max
    // (blank + 90% of the way to the true luma peak, so a 100 IRE white-bar
    // plateau maps to display white exactly).
    bool have_porch = (decoder->adaptive.porch_count >= 64);
    bool have_luma_max = decoder->adaptive.luma_max_valid;
    if (have_porch) {
        // Prefer the on-baseline subset when it covers at least half of the
        // field's porch samples (see decode_line_to_pixels).
        int64_t psum = decoder->adaptive.porch_sum;
        int pcount = decoder->adaptive.porch_count;
        if (decoder->adaptive.porch_count_clean >= 64 &&
            decoder->adaptive.porch_count_clean * 2 >= pcount) {
            psum = decoder->adaptive.porch_sum_clean;
            pcount = decoder->adaptive.porch_count_clean;
        }
        int16_t porch_now = (int16_t)(psum / (int64_t)pcount);
        if (!decoder->adaptive.porch_valid) {
            decoder->adaptive.porch_level = porch_now;
            decoder->adaptive.porch_valid = true;
        } else {
            decoder->adaptive.porch_level = (int16_t)(0.75f * decoder->adaptive.porch_level
                                                      + 0.25f * porch_now);
        }
    }
    int16_t blank_ref = decoder->adaptive.porch_valid
        ? decoder->adaptive.porch_level : detected.black_level;
    int16_t tip_ref = detected.sig_min;
    // The sync threshold is ALWAYS the tip-to-blank midpoint. The raw-domain
    // min is the one tip estimate that never fails (chroma never reaches the
    // sync tip), and the blank comes from the porch once it has data, from
    // the histogram's second peak before that. The picker's own
    // sync_threshold has been measured landing ABOVE true blanking on a real
    // capture (its two-peak "tip" landed on mid-level content: measured
    // threshold -9134 vs blank -10675 on the EBU PAL capture - blanking
    // itself then read LOW, the pulse classification degenerated into tens
    // of thousands of sub-us fragments, and every VBI train was rejected).
    // A threshold above blanking can never recover through the porch path
    // either (the PLL lock that gates the porch accumulation needs working
    // sync classification first), so the midpoint must not depend on
    // porch_valid - it must be correct from the very first commit.
    int16_t thresh_ref = (int16_t)(((int)tip_ref + (int)blank_ref) / 2);
    int16_t white_ref = detected.white_level;
    if (decoder->adaptive.porch_valid && have_luma_max) {
        int16_t lmax = decoder->adaptive.luma_max;
        if (lmax < blank_ref) lmax = blank_ref;
        white_ref = (int16_t)(blank_ref + (int)(((int)lmax - blank_ref) * 90) / 100);
    }
    // Reset the per-field accumulators for the next field.
    decoder->adaptive.porch_sum = 0;
    decoder->adaptive.porch_count = 0;
    decoder->adaptive.porch_sum_clean = 0;
    decoder->adaptive.porch_count_clean = 0;
    decoder->adaptive.luma_max_valid = false;

    if (!picker_ok && !(decoder->adaptive.porch_valid && have_luma_max)) {
        // No picker result and no porch data yet - keep previous levels.
        return;
    }

    bool first_levels = (decoder->adaptive.sync_tip == 0 && decoder->adaptive.white == 0);
    bool scene_change = false;
    float alpha_sync = 0.15f;
    float alpha_luma = 0.15f;

    if (!first_levels && picker_ok) {
        int black_delta = abs((int)detected.black_level - (int)decoder->adaptive.black);
        int white_delta = abs((int)detected.white_level - (int)decoder->adaptive.white);
        int threshold_delta = abs((int)detected.sync_threshold - (int)decoder->adaptive.threshold);
        int luma_delta = (black_delta > white_delta) ? black_delta : white_delta;
        // Scene changes mostly affect luma distribution. Keep sync adaptation conservative
        // while allowing black/white levels to catch up faster.
        scene_change = (luma_delta > 220 && threshold_delta < 220);
        if (scene_change) {
            alpha_luma = 0.45f;
            alpha_sync = 0.08f;
        }
    }

    if (first_levels) {
        // First time - initialize directly from the reference levels
        decoder->adaptive.sync_tip = tip_ref;
        decoder->adaptive.blanking = blank_ref;
        decoder->adaptive.black = blank_ref;
        decoder->adaptive.white = white_ref;
        decoder->adaptive.threshold = thresh_ref;
    } else {
        // Smooth update toward the reference levels.
        // Sync domain tracks slowly and is step-limited to avoid scene-cut jitter.
        int16_t target_threshold = thresh_ref;
        int threshold_step = (int)target_threshold - (int)decoder->adaptive.threshold;
        int max_threshold_step = scene_change ? 20 : 56;
        if (threshold_step > max_threshold_step) {
            target_threshold = (int16_t)(decoder->adaptive.threshold + max_threshold_step);
        } else if (threshold_step < -max_threshold_step) {
            target_threshold = (int16_t)(decoder->adaptive.threshold - max_threshold_step);
        }

        decoder->adaptive.sync_tip = (int16_t)(decoder->adaptive.sync_tip * (1.0f - alpha_sync) +
                                               tip_ref * alpha_sync);
        decoder->adaptive.blanking = (int16_t)(decoder->adaptive.blanking * (1.0f - alpha_sync) +
                                               blank_ref * alpha_sync);
        decoder->adaptive.black = (int16_t)(decoder->adaptive.black * (1.0f - alpha_luma) +
                                            blank_ref * alpha_luma);
        decoder->adaptive.white = (int16_t)(decoder->adaptive.white * (1.0f - alpha_luma) +
                                            white_ref * alpha_luma);
        decoder->adaptive.threshold = (int16_t)(decoder->adaptive.threshold * (1.0f - alpha_sync) +
                                                target_threshold * alpha_sync);
    }
    // Keep sync tracking adaptive in both modes.
    decoder->levels.sig_min = decoder->adaptive.sync_tip;
    decoder->levels.sig_max = decoder->adaptive.white;
    decoder->levels.sync_threshold = decoder->adaptive.threshold;

    // Fixed mode holds display black/white levels to avoid visible pumping.
    // Auto mode continuously updates display levels each field.
    if (decoder->level_mode == CVBS_LEVEL_MODE_FIXED) {
        if (!decoder->fixed_levels_valid) {
            decoder->fixed_black_level = decoder->adaptive.black;
            decoder->fixed_white_level = decoder->adaptive.white;
            decoder->fixed_levels_valid = true;
        }
        decoder->levels.black_level = decoder->fixed_black_level;
        decoder->levels.white_level = decoder->fixed_white_level;
    } else {
        decoder->fixed_levels_valid = false;
        decoder->levels.black_level = decoder->adaptive.black;
        decoder->levels.white_level = decoder->adaptive.white;
    }

    // Update compatibility range value (used as signal-validity guard).
    // Saturate: in the 16x s16 domain white - tip exceeds int16 and the
    // wrapped negative value used to clamp to 100 (measured: 38619 -> 100).
    int32_t range32 = (int32_t)decoder->levels.white_level
                      - (int32_t)decoder->levels.sig_min;
    if (range32 < 100) range32 = 100;
    if (range32 > 32767) range32 = 32767;
    decoder->levels.range = (int16_t)range32;

    // In FIXED sync mode, keep the lock threshold slowly tracking long-term gain shifts
    // (AGC/scene changes) without letting scene content cause abrupt threshold jumps.
    if (decoder->sync_mode == CVBS_SYNC_MODE_FIXED &&
        decoder->fixed_sync_locked &&
        decoder->adaptive.threshold != 0) {
        if (decoder->fixed_sync_threshold == 0) {
            decoder->fixed_sync_threshold = decoder->adaptive.threshold;
        } else {
            int delta = (int)decoder->adaptive.threshold - (int)decoder->fixed_sync_threshold;
            int max_step = scene_change ? 8 : 16;
            if (delta > max_step) delta = max_step;
            if (delta < -max_step) delta = -max_step;
            decoder->fixed_sync_threshold = (int16_t)(decoder->fixed_sync_threshold + delta);
        }
    }
}

//-----------------------------------------------------------------------------
// Software PLL-based CVBS Decoder
//
// This approach uses a software PLL to track H-sync timing:
// - PLL maintains a phase counter that represents position within a line
// - When H-sync edges are detected, PLL phase is corrected
// - Samples are written to frame buffer based on PLL line counter
// - V-sync detection resets the line counter to start a new field
// - Missing H-syncs don't break the display - PLL interpolates
//-----------------------------------------------------------------------------

// PLL tuning constants
#define PLL_LOCK_COUNT      10      // Good syncs needed to declare lock
#define PLL_UNLOCK_COUNT    5       // Bad syncs to lose lock
#define PLL_FIXED_UNLOCK_COUNT  20     // Bad syncs before fixed lock drops back to acquisition
#define PLL_FIXED_VSYNC_WINDOW  96     // Accept V-sync in fixed lock near expected line cadence
// Clamp the frozen fixed period to nominal ±5% (rate-relative, was a fixed
// 120-sample window that only made sense at 40 MSPS).
static inline double cvbs_fixed_period_clamp(const cvbs_decoder_t *decoder) {
    return cvbs_nominal_line_period(decoder) * 0.05;
}

// H-sync pulse validation is now profile-driven: pulses are classified in
// MICROSECONDS (EQ / H / broad) so any capture rate classifies correctly.
// The legacy fixed-sample HSYNC_MIN/MAX_WIDTH windows only worked at 40 MSPS
// (they would have treated EQ pulses as H-sync at higher rates).

// Lowpass filter coefficient (IIR single-pole) - Fixed-point Q8
// At 40 MSPS, a cutoff of ~500kHz gives alpha ≈ 0.08
// Lower alpha = more smoothing (removes HF noise while preserving sync edges)
// Fixed-point: alpha = 20/256 ≈ 0.078, (1-alpha) = 236/256 ≈ 0.922
#define LPF_ALPHA_FP        20      // 0.08 * 256 ≈ 20
#define LPF_ONE_MINUS_FP    236     // (1 - 0.08) * 256 ≈ 236
#define LPF_SHIFT           8       // Divide by 256

static inline double cvbs_nominal_line_period(const cvbs_decoder_t *decoder) {
    // Profile-derived at the live rate (default 40 MSPS before a rate arrives).
    if (!decoder) return cvbs_baseband_profiles[0].line_us * 40.0;  // 2560 samples
    return (double)cvbs_us_to_samples(decoder, cvbs_profile(decoder)->line_us);
}

static inline double cvbs_effective_line_period(const cvbs_decoder_t *decoder) {
    if (!decoder) return (double)CVBS_SAMPLES_PER_LINE_PAL;
    if (decoder->sync_mode == CVBS_SYNC_MODE_FIXED &&
        decoder->fixed_sync_locked &&
        decoder->fixed_line_period > 0.0) {
        // The frozen period is latched only AFTER the acquisition integral
        // has converged (see pll_process_hsync: ~1000 good syncs since lock,
        // leaving <0.01 samples of residual), so the frozen value itself is
        // sub-sample accurate - the wraps (and so every porch/burst/active
        // window) carry no systematic offset. An EMA correction fed back into
        // this value was measured UNSTABLE on the real EBU capture (the
        // wrap/EMA loop converged ~4 samples off, committing a 40.06 MHz
        // rate that scrambled every hue) - the residual is measured, not
        // fed back (see the rate refinement in pll_process_hsync).
        return decoder->fixed_line_period;
    }
    return decoder->pll.line_period + decoder->pll.freq_adjust;
}

static inline void cvbs_reset_fixed_sync_lock(cvbs_decoder_t *decoder) {
    if (!decoder) return;
    decoder->fixed_sync_locked = false;
    decoder->fixed_bad_syncs = 0;
    decoder->fixed_line_period = cvbs_nominal_line_period(decoder);
    decoder->fixed_sync_threshold = 0;
    decoder->fixed_period_ema = 0.0;
}

//-----------------------------------------------------------------------------
// Lowpass Filter for Sync Detection
//-----------------------------------------------------------------------------

// Apply IIR lowpass filter to a sample (fixed-point version)
// This smooths out high-frequency noise while preserving sync pulse edges
// Returns filtered value in original sample range (not shifted)
static inline int16_t apply_lowpass(cvbs_decoder_t *decoder, int16_t sample) {
    // Single-pole IIR: y[n] = alpha * x[n] + (1 - alpha) * y[n-1]
    // Fixed-point Q8: state is stored shifted left by 8
    // y[n] = (alpha * x[n] + (1-alpha) * y[n-1]) >> 8, but we keep state in Q8
    int32_t state = LPF_ALPHA_FP * (int32_t)sample + LPF_ONE_MINUS_FP * (decoder->lpf_state >> LPF_SHIFT);
    decoder->lpf_state = state;
    return (int16_t)(state >> LPF_SHIFT);
}

//-----------------------------------------------------------------------------
// Sync detector note
//-----------------------------------------------------------------------------

// Legacy standalone sync detector helpers were removed after the processing loop
// was consolidated into a single cadence-aware pass in gui_cvbs_process_buffer().

//-----------------------------------------------------------------------------
// Deinterlacer
//-----------------------------------------------------------------------------

// Deinterlace two fields into a full frame using weave with bob fallback
// - Weave: interleave even/odd fields (best quality when both present)
// - Bob: interpolate missing field lines (for partial frames)
//
// Field mapping (PAL/NTSC standard):
// - field_buffer[0] = odd field (field 1): contains lines 1, 3, 5... of the frame
// - field_buffer[1] = even field (field 2): contains lines 0, 2, 4... of the frame
// In the frame buffer, even field lines come first (0, 2, 4...), then odd (1, 3, 5...)
static void deinterlace_fields(cvbs_decoder_t *decoder) {
    if (!decoder || !decoder->field_buffer[0] || !decoder->field_buffer[1]) return;
    if (!decoder->frame_buffer) return;
    int frame_width = decoder->frame_width;
    int frame_height = decoder->frame_height;
    int max_field_height = (CVBS_MAX_HEIGHT + 1) / 2;
    int odd_height = decoder->field_line_count[0];
    int even_height = decoder->field_line_count[1];

    if (frame_width < 1 || frame_width > CVBS_MAX_WIDTH) return;
    if (frame_height < 1) return;
    if (frame_height > CVBS_MAX_HEIGHT) frame_height = CVBS_MAX_HEIGHT;
    if (odd_height < 0) odd_height = 0;
    if (even_height < 0) even_height = 0;
    if (odd_height > max_field_height) odd_height = max_field_height;
    if (even_height > max_field_height) even_height = max_field_height;

    uint8_t *odd_field = decoder->field_buffer[0];   // Odd field (frame lines 1, 3, 5...)
    uint8_t *even_field = decoder->field_buffer[1];  // Even field (frame lines 0, 2, 4...)
    uint8_t *frame = decoder->frame_buffer;

    // Check which fields are valid.
    // Weave only when both fields are temporally adjacent; otherwise one field is stale
    // and we should bob from the newest field to avoid old/offset field pairing.
    bool have_odd = decoder->field_ready[0];
    bool have_even = decoder->field_ready[1];
    if (have_odd && have_even) {
        uint64_t odd_seq = decoder->field_last_update_seq[0];
        uint64_t even_seq = decoder->field_last_update_seq[1];
        uint64_t seq_diff = (odd_seq > even_seq) ? (odd_seq - even_seq) : (even_seq - odd_seq);
        if (seq_diff > 1) {
            if (odd_seq >= even_seq) {
                have_even = false;
            } else {
                have_odd = false;
            }
        }
    }

    // RGB24 row stride (3 bytes per pixel since the colour decode).
    const size_t row_bytes = (size_t)frame_width * 3u;
    int generated_lines = 0;
    decoder->last_weave = have_odd && have_even;

    if (have_odd && have_even) {
        // Weave mode: keep each field's real row count so odd-height systems
        // render 625/525 rows correctly instead of collapsing to an even count.
        int max_rows = odd_height > even_height ? odd_height : even_height;
        for (int fl = 0; fl < max_rows; fl++) {
            int even_y = fl * 2;
            int odd_y = even_y + 1;
            if (fl < even_height && even_y < frame_height) {
                const uint8_t *src_even = even_field + (size_t)fl * row_bytes;
                uint8_t *dst_even = frame + (size_t)even_y * row_bytes;
                memcpy(dst_even, src_even, row_bytes);
                if (generated_lines < even_y + 1) generated_lines = even_y + 1;
            }
            if (fl < odd_height && odd_y < frame_height) {
                const uint8_t *src_odd = odd_field + (size_t)fl * row_bytes;
                uint8_t *dst_odd = frame + (size_t)odd_y * row_bytes;
                memcpy(dst_odd, src_odd, row_bytes);
                if (generated_lines < odd_y + 1) generated_lines = odd_y + 1;
            }
        }
    } else if (have_odd) {
        // Bob mode with odd field only: duplicate lines with interpolation.
        // Per-byte averaging keeps the RGB channels independent (the legacy
        // 32-bit word trick would now straddle pixel boundaries at x3).
        int field_height = odd_height;
        if (field_height < 1) return;
        if (frame_height > 0) memcpy(frame, odd_field, row_bytes);  // dst_even line 0
        if (frame_height > 1) memcpy(frame + row_bytes, odd_field, row_bytes);  // dst_odd line 0

        for (int fl = 1; fl < field_height; fl++) {
            const uint8_t *src = odd_field + (size_t)fl * row_bytes;
            const uint8_t *src_prev = odd_field + (size_t)(fl - 1) * row_bytes;
            int even_y = fl * 2;
            int odd_y = even_y + 1;

            if (odd_y < frame_height) {
                uint8_t *dst_odd = frame + (size_t)odd_y * row_bytes;
                memcpy(dst_odd, src, row_bytes);
            }
            if (even_y < frame_height) {
                uint8_t *dst_even = frame + (size_t)even_y * row_bytes;
                for (size_t x = 0; x < row_bytes; x++) {
                    dst_even[x] = (uint8_t)((src_prev[x] + src[x]) >> 1);
                }
            }
        }
        generated_lines = field_height * 2;
        if (generated_lines > frame_height) generated_lines = frame_height;
    } else if (have_even) {
        // Bob mode with even field only
        int field_height = even_height;
        if (field_height < 1) return;
        for (int fl = 0; fl < field_height - 1; fl++) {
            const uint8_t *src = even_field + (size_t)fl * row_bytes;
            const uint8_t *src_next = even_field + (size_t)(fl + 1) * row_bytes;
            int even_y = fl * 2;
            int odd_y = even_y + 1;

            if (even_y < frame_height) {
                uint8_t *dst_even = frame + (size_t)even_y * row_bytes;
                memcpy(dst_even, src, row_bytes);
            }
            if (odd_y < frame_height) {
                uint8_t *dst_odd = frame + (size_t)odd_y * row_bytes;
                for (size_t x = 0; x < row_bytes; x++) {
                    dst_odd[x] = (uint8_t)((src[x] + src_next[x]) >> 1);
                }
            }
        }
        // Last line: just duplicate
        int last_fl = field_height - 1;
        const uint8_t *src_last = even_field + (size_t)last_fl * row_bytes;
        int even_y = last_fl * 2;
        int odd_y = even_y + 1;
        if (even_y < frame_height) {
            uint8_t *dst_even_last = frame + (size_t)even_y * row_bytes;
            memcpy(dst_even_last, src_last, row_bytes);
        }
        if (odd_y < frame_height) {
            uint8_t *dst_odd_last = frame + (size_t)odd_y * row_bytes;
            memcpy(dst_odd_last, src_last, row_bytes);
        }
        generated_lines = field_height * 2;
        if (generated_lines > frame_height) generated_lines = frame_height;
    }

    // Full-frame line systems are odd-height (625/525). Duplicate final line if needed.
    if (generated_lines > 0 && frame_height > generated_lines) {
        const uint8_t *src_last = frame + (size_t)(generated_lines - 1) * row_bytes;
        for (int y = generated_lines; y < frame_height; y++) {
            memcpy(frame + (size_t)y * row_bytes, src_last, row_bytes);
        }
    }
    // If neither field ready, frame_buffer keeps its previous content
}

//-----------------------------------------------------------------------------
// Field Completion
//-----------------------------------------------------------------------------

// Complete a field - deinterlace and copy to display buffer
static void complete_field_pll(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    // Capture field decode timing and reset for next field
    g_field_decode_avg_ms = g_field_decode_us / 1000.0;
    g_field_decode_us = 0;

    int line_count = decoder->pll.current_line;

    // Expected lines for THIS field's parity (PAL 312/313, NTSC 263/262 -
    // the legacy code assumed one count for both parities).
    int expected_lines = cvbs_profile(decoder)->field_lines[
        decoder->state.current_field ? 1 : 0];

    // Only accept if we got a reasonable field (at least 80% of expected lines)
    // This prevents accepting partial/corrupted fields after format switch
    if (line_count < (expected_lines * 4 / 5)) {
        return;
    }

    // Mark this field as received
    int field_idx = decoder->state.current_field ? 1 : 0;
    if (line_count > decoder->field_height) line_count = decoder->field_height;
    decoder->field_ready[field_idx] = true;
    decoder->field_line_count[field_idx] = line_count;
    decoder->field_sequence++;
    decoder->field_last_update_seq[field_idx] = decoder->field_sequence;

    // Deinterlace and display - works with one or both fields
    // Bob mode is used until both fields are available, then weave takes over
    deinterlace_fields(decoder);

    // Copy deinterlaced frame to back buffer (display thread writes here)
    int frame_h = decoder->frame_height;
    int frame_w = decoder->frame_width;
    if (frame_h > CVBS_MAX_HEIGHT) frame_h = CVBS_MAX_HEIGHT;
    if (frame_w > CVBS_MAX_WIDTH) frame_w = CVBS_MAX_WIDTH;
    if (frame_w < 1) return;

    memcpy(decoder->display_back, decoder->frame_buffer,
           (size_t)frame_w * 3u * (size_t)frame_h);
    // Signal that new frame is ready for the render thread
    atomic_store(&decoder->display_ready, 1);
    /* No network or encoder work here: optional bounded frame handoff only.
     * The decoder owns display_back; callback must copy before returning. */
    if (decoder->monitor_frame_sink)
        decoder->monitor_frame_sink(decoder->monitor_frame_sink_context,
            decoder->display_back, (unsigned)frame_w, (unsigned)frame_h, get_time_us());
    decoder->state.frame_complete = true;
    decoder->state.frames_decoded++;
    decoder->debug.fields_decoded++;
#if CVBS_LIVE_DIAG
    if (decoder->state.frames_decoded % CVBS_LIVE_DIAG_EVERY == 0) {
        cvbs_diag_dump(decoder);
    }
#endif
}

// Commit a verified field boundary (the ported RF-decoder model; see
// cvbs_vbi_first_eq below). The previous field is completed, the new field
// is anchored at its exact boundary (first EQ - boundary*line), and the PLL
// phase / line counter are re-based so subsequent phase wraps number lines
// from the boundary - exactly the RF decoder's field_start + line*period map
// expressed in the PLL's streaming form.
// parity: 0 = first/odd field (buffer 0), 1 = second/even field (buffer 1).
static void cvbs_start_field_anchored(cvbs_decoder_t *decoder, int parity,
                                      size_t global_pos, double new_field_start) {
    cvbs_pll_state_t *pll = &decoder->pll;
    double period = pll->line_period + pll->freq_adjust;
    if (period < 1.0) period = cvbs_nominal_line_period(decoder);

    complete_field_pll(decoder);
    commit_adaptive_levels(decoder);
    // SECAM: lock the previous field's calibration/identification for the
    // new field (one field behind - the reference's last-good-reuse
    // semantics in a live stream).
    cvbs_secam_field_commit(decoder);

    // Anchor line 0 at the field start. The detection happens at the first
    // EQ pulse, which sits boundary lines (0.5/1.0) INTO the new field.
    double into_field = (double)global_pos - new_field_start;
    if (into_field < 0.0) into_field = 0.0;
    int line = (int)(into_field / period);
    double phase = into_field - (double)line * period;
    int field_idx = parity ? 1 : 0;
    if (decoder->field_buffer[field_idx]) {
        size_t clear_bytes = (size_t)decoder->frame_width * 3u * (size_t)decoder->field_height;
        memset(decoder->field_buffer[field_idx], 0, clear_bytes);
    }
    decoder->field_ready[field_idx] = false;
    decoder->field_line_count[field_idx] = 0;
    pll->current_line = line;
    pll->phase = phase;
    pll->samples_in_line = 0;
    decoder->state.current_field = parity ? 1 : 0;
    decoder->state.in_vsync = true;   // inside the vertical blanking interval
    decoder->lines_since_vsync = 0;
    decoder->debug.hsyncs_last_field = 0;
    decoder->debug.vsync_found++;
}

// The first EQ pulse of a pre-EQ train: derive the field parity from the
// EQ-to-last-H phase (half-line vs integer-line), place the field boundary
// exactly (first_eq - boundary*line), and validate the previous field's
// cadence (312.5/262.5 lines) with explicit reject reasons - the ported
// gui_rf_preview.c rf_vertical_start model that replaced the interval
// half-line counting and the >=15-half-lines parity guess, which mis-framed
// whenever the EQ train was noisy.
static void cvbs_vbi_first_eq(cvbs_decoder_t *decoder, size_t first_eq_pos,
                              size_t global_pos) {
    cvbs_vsync_state_t *vs = &decoder->vsync;
    cvbs_pll_state_t *pll = &decoder->pll;
    const cvbs_baseband_profile_t *profile = cvbs_profile(decoder);
    bool pal = (decoder->state.format != CVBS_FORMAT_NTSC);
    double period = pll->line_period + pll->freq_adjust;
    if (period < 1.0) period = cvbs_nominal_line_period(decoder);

    vs->vsync_candidates++;

    // Stream acquisition: no H pulse has ever been classified, so no parity
    // can be derived and no field is pending - this is not signal damage,
    // so it is not counted as a rejection. The next train establishes
    // everything (the RF decoder starts the same way).
    if (vs->last_h_pos == 0) {
        return;
    }
    // A valid first EQ needs a recent H reference (the pulse before the
    // train must be a normal H sync within ~2 lines).
    if ((double)(first_eq_pos - vs->last_h_pos) > period * 2.1) {
        vs->fields_rejected++;
        vs->reject_parity++;
        snprintf(vs->last_reject_reason, sizeof(vs->last_reject_reason),
                 "vertical EQ lacks recent H reference");
        // A stream jump (a display-buffer drop splicing the sample stream)
        // can land inside a train, leaving the last H reference across the
        // gap. Drop the stale boundary reference too, so the NEXT train
        // re-bootstraps the field sequence instead of failing its cadence
        // against a two-field-old reference (measured: two to three
        // rejected fields per gap before this - each reject anchoring via
        // the overflow fallback, shifting the picture's line numbering).
        vs->have_field = false;
        return;
    }

    // Parity from the EQ-to-last-H phase: half-line vs integer-line.
    double relative = (double)(first_eq_pos - vs->last_h_pos) / period;
    double half_distance = fabs(relative - (floor(relative) + 0.5));
    double integer_distance = fabs(relative - floor(relative + 0.5));
    bool half = half_distance < integer_distance;
    if (fmin(half_distance, integer_distance) > 0.18) {
        vs->fields_rejected++;
        vs->reject_parity++;
        snprintf(vs->last_reject_reason, sizeof(vs->last_reject_reason),
                 "vertical EQ/H parity phase uncertain");
        return;
    }

    int parity = pal ? (half ? 0 : 1) : (half ? 1 : 0);
    double boundary = pal ? (parity == 0 ? 0.5 : 1.0)
                          : (parity == 0 ? 1.0 : 0.5);
    double new_field_start = (double)first_eq_pos - boundary * period;

    if (vs->have_field) {
        double elapsed_lines =
            (new_field_start - (double)vs->field_start) / period;
        if (fabs(elapsed_lines - profile->field_cadence_lines) < 2.0 &&
            parity != vs->field_parity) {
            // Verified boundary: commit the completed field, anchor the new.
            cvbs_start_field_anchored(decoder, parity, global_pos, new_field_start);
            vs->fields_accepted++;
        } else {
            vs->fields_rejected++;
            if (fabs(elapsed_lines - profile->field_cadence_lines) >= 2.0) {
                vs->reject_cadence++;
                snprintf(vs->last_reject_reason, sizeof(vs->last_reject_reason),
                         "vertical field cadence outside profile");
                // RECOVERY ANCHOR: a cadence-off candidate passed the
                // recent-H and parity-phase gates, so it is a real train
                // whose SEQUENCE broke - a stream jump (a display-buffer
                // drop splicing the sample stream) shifted the elapsed
                // sample count by the gap. Anchor the new field at the
                // MEASURED boundary anyway (complete the old field short or
                // long): the overflow fallback would instead re-anchor at
                // the line-count cap with a GUESSED parity and misplaced
                // line numbering - measured on the gap tests as the
                // shifted-top/weaving picture for a full field per gap.
                // The reject stays counted for the diagnostics; the
                // sequence reference below still tracks this candidate, so
                // the next train's cadence validates normally.
                cvbs_start_field_anchored(decoder, parity, global_pos,
                                          new_field_start);
            } else {
                vs->reject_parity++;
                snprintf(vs->last_reject_reason, sizeof(vs->last_reject_reason),
                         "vertical fields repeat parity");
                // A parity-repeat candidate is ambiguous (a parity glitch
                // or a false candidate that passed the gates): do NOT
                // anchor on it - the old field continues to the overflow
                // fallback and the next train re-establishes.
            }
        }
    }
    // The boundary state tracks the newest candidate whether accepted or
    // rejected (the RF decoder's behavior): a bad candidate never poisons
    // the NEXT field's cadence reference.
    vs->field_start = (size_t)new_field_start;
    vs->field_parity = parity;
    vs->have_field = true;
}

// Process a detected H-sync edge - update PLL
static void pll_process_hsync(cvbs_decoder_t *decoder, double phase_at_sync) {
    cvbs_pll_state_t *pll = &decoder->pll;
    cvbs_pll_tuning_t tuning = cvbs_select_pll_tuning(decoder);
    bool fixed_mode = (decoder->sync_mode == CVBS_SYNC_MODE_FIXED);
    bool fixed_locked = fixed_mode && decoder->fixed_sync_locked && decoder->fixed_line_period > 0.0;
    double line_period = fixed_locked ? decoder->fixed_line_period : pll->line_period;
    if (line_period < 1.0) line_period = cvbs_nominal_line_period(decoder);

    // Calculate phase error: how far off was our prediction?
    // Ideal sync should happen at phase = 0 (or line_period)
    double phase_error = phase_at_sync;

    // Wrap to -half_period to +half_period
    if (phase_error > line_period / 2) {
        phase_error -= line_period;
    }

#if CVBS_LIVE_DIAG
    cvbs_diag_record(pll->current_line, decoder->state.current_field,
                     phase_at_sync, phase_error);
#endif

    // Update PLL lock status based on phase error magnitude.
    // good_sync is ALSO the frequency-measurement gate below: once locked
    // (snap each line) a good sync's phase_error is a clean (true - model)
    // period measurement, while an out-of-threshold error on a width-valid
    // H is a STREAM JUMP (a dropped/duplicated feed chunk), not a period
    // error. The snap absorbs the jump; the integrators must not see it -
    // measured on the live path: each drop kicked the frozen-period EMA by
    // up to 0.57 samples and the period walked ~3.5 samples (3000 ppm) off
    // within seconds, shifting every post-VBI line horizontally.
    bool good_sync = fabs(phase_error) < tuning.lock_threshold;
    if (good_sync) {
        pll->good_sync_count++;
        pll->bad_sync_count = 0;
        if (pll->good_sync_count >= PLL_LOCK_COUNT) {
            pll->locked = true;
        }
    } else {
        pll->bad_sync_count++;
        pll->good_sync_count = 0;
        if (pll->bad_sync_count >= PLL_UNLOCK_COUNT) {
            pll->locked = false;
        }
    }
    if (fixed_mode) {
        if (fixed_locked && pll->locked) {
            // Locked + measured sync: SNAP so the NEXT line wrap lands
            // exactly on the next sync START (the RF decoder's per-line
            // measured-H anchor). phase_at_sync is the phase at the pulse
            // start, so phase - phase_at_sync = the pulse width; with the
            // phase at width on the rising edge, the wrap fires precisely
            // period-width later = the next sync start. The legacy gentle
            // gain left up to lock_threshold samples (~2.5 us at 40 MSPS) of
            // per-line residual jitter, smearing the profile's burst/active
            // sample windows and turning the colour decode into noise.
            //
            // ALWAYS snap, whatever the phase error magnitude: the pulse
            // was classified H by WIDTH (3.5-8 us) - that classification is
            // the noise filter, and a width-valid H with a huge phase error
            // is a STREAM JUMP (a display-buffer drop splices the sample
            // stream), not noise. The old wild-sync guard (drop syncs with
            // > 0.4 line of error, never snap to them) deadlocked after a
            // jump: the phase stayed off, every sync stayed wild, the lock
            // dropped to the transitional hold which never re-anchored, and
            // the decoder never re-locked (measured: random 1-8 ms gaps in
            // the EBU capture left it stuck at a constant -504-sample error
            // for the rest of the stream, ~300 lines and counting). Snapping
            // absorbs a jump in ONE sync line; the bad-sync accounting below
            // still tracks genuinely noisy stretches.
            if (fabs(phase_error) < tuning.lock_threshold) {
                if (decoder->fixed_bad_syncs > 0) decoder->fixed_bad_syncs--;
            } else if (fabs(phase_error) > line_period * 0.30) {
                int expected_lines = cvbs_profile(decoder)->field_lines[
                    decoder->state.current_field ? 1 : 0];
                bool near_field_boundary = (pll->current_line < 8) ||
                                           (pll->current_line > (expected_lines - 8));
                if (!near_field_boundary) {
                    decoder->fixed_bad_syncs++;
                }
                decoder->stream_jumps++;
            }
            pll->phase -= phase_at_sync;
            pll->line_period = decoder->fixed_line_period;
            pll->freq_adjust = 0.0;
            pll->phase_integral = 0.0;

            // Slow FROZEN-PERIOD DRIFT TRACKING. The always-snap makes each
            // line's phase_error a clean measurement of (true - frozen)
            // period error (the snap removes the origin, the sub-sample
            // edges remove the crossing quantization). A slow EMA of it,
            // fed back into the frozen period at a tiny per-line gain, lets
            // the frozen value track the TRUE line period as the input
            // device's clock drifts (a live capture's crystal/TMDS relock
            // drifts tens to hundreds of ppm over minutes) - WITHOUT the
            // fixed mode's stability intent being lost: the per-line wrap
            // change from the correction is <= 0.0005 samples (invisible),
            // and the correction is clamped to the fixed-period band. This
            // also drains the EQ-train freewheel error (7.5 un-snapped lines
            // per field accumulate (frozen - true) and shift the FIRST
            // picture lines after each train - the drifting top-side
            // horizontal shift, growing as the clock drifts).
            if (good_sync) {
                decoder->fixed_period_ema =
                    decoder->fixed_period_ema * 0.999 + phase_error * 0.001;
                if (fabs(decoder->fixed_period_ema) < 8.0) {
                    double nominal = cvbs_nominal_line_period(decoder);
                    double clamp = cvbs_fixed_period_clamp(decoder);
                    double tracked = decoder->fixed_line_period
                                     + decoder->fixed_period_ema * 0.001;
                    if (tracked > nominal - clamp && tracked < nominal + clamp) {
                        decoder->fixed_line_period = tracked;
                        pll->line_period = decoder->fixed_line_period;
                    }
                }
            }

            if (decoder->fixed_bad_syncs >= PLL_FIXED_UNLOCK_COUNT) {
                cvbs_reset_fixed_sync_lock(decoder);
                pll->locked = false;
                pll->good_sync_count = 0;
                pll->bad_sync_count = 0;
                pll->phase_integral = 0.0;
                pll->freq_adjust = 0.0;
                decoder->sync_errors++;
            }
        } else if (!fixed_locked) {
            // Acquisition phase for fixed mode: mirror the AUTO path
            // exactly (per-line snap once locked so the windows carry no
            // residual jitter, integral converging the period - and ALWAYS
            // snapping to width-classified H syncs, see the locked branch:
            // a huge phase error on a width-valid H is a stream jump, not
            // noise) and FREEZE only after the integral has CONVERGED:
            // ~1000 good syncs since lock leaves <0.01 samples of residual.
            // The freeze at the FIRST lock (10 syncs) latched a ~1.3-sample
            // residual (measured on the real EBU capture): every wrap - and
            // so every porch/burst/active window - then ran the same ~1.3
            // samples late, the porch mean caught sync residue, the
            // committed black ran ~2 IRE high, the sync threshold landed
            // ~300 counts shallow, and the resulting false fragments rejected
            // every second VBI train for cadence. A converged freeze has
            // none of that, and the frozen value stays as stable as the
            // first-lock freeze ever was.
            if (pll->locked) {
                pll->phase -= phase_at_sync;
                if (good_sync) {
                    pll->phase_integral += phase_error * tuning.integral_gain;
                    if (pll->phase_integral > 50) pll->phase_integral = 50;
                    if (pll->phase_integral < -50) pll->phase_integral = -50;
                    pll->freq_adjust = pll->phase_integral;
                }
                if (pll->lines_since_lock >= 1000) {
                    double nominal = cvbs_nominal_line_period(decoder);
                    double fixed_period = pll->line_period + pll->freq_adjust;
                    double clamp = cvbs_fixed_period_clamp(decoder);
                    double min_period = nominal - clamp;
                    double max_period = nominal + clamp;
                    if (fixed_period < min_period) fixed_period = min_period;
                    if (fixed_period > max_period) fixed_period = max_period;
                    decoder->fixed_line_period = fixed_period;
                    decoder->fixed_sync_locked = true;
                    decoder->fixed_bad_syncs = 0;
                    decoder->fixed_sync_threshold = decoder->adaptive.threshold;
                    if (decoder->fixed_sync_threshold == 0) {
                        decoder->fixed_sync_threshold = decoder->levels.sync_threshold;
                    }
                    pll->line_period = fixed_period;
                    pll->freq_adjust = 0.0;
                    pll->phase_integral = 0.0;
                }
            } else {
                pll->phase -= phase_error * tuning.phase_gain;
                pll->phase_integral += phase_error * tuning.integral_gain;
                if (pll->phase_integral > 50) pll->phase_integral = 50;
                if (pll->phase_integral < -50) pll->phase_integral = -50;
                pll->freq_adjust = pll->phase_integral;
            }
        } else {
            // fixed_locked but not re-locked yet (transitional): hold the
            // frozen period and KEEP SNAPPING to every width-classified H.
            // The old hold-without-snap deadlocked after a stream jump: the
            // phase stayed off, every sync stayed bad, and the re-lock (10
            // consecutive good syncs) could never happen - measured: stuck
            // at a constant -504-sample error for hundreds of lines. With
            // the snap, the phase re-anchors immediately and the re-lock
            // follows within ~10 lines. The drift-tracking EMA keeps
            // running (the phase_error stays a clean period measurement).
            pll->phase -= phase_at_sync;
            pll->line_period = decoder->fixed_line_period;
            pll->freq_adjust = 0.0;
            if (good_sync) {
                decoder->fixed_period_ema =
                    decoder->fixed_period_ema * 0.999 + phase_error * 0.001;
            }
        }
    } else {
        // AUTO mode: while ACQUIRING, adapt phase and frequency gently. Once
        // LOCKED, snap the phase to the measured sync start (the RF decoder's
        // per-line measured-H anchor) so the profile's burst/active sample
        // windows carry no per-line residual jitter (the legacy gentle-gain
        // approach smeared the windows by up to lock_threshold samples and
        // turned the burst-locked colour decode into noise).
        // ALWAYS snap to width-classified H syncs (see the fixed-locked
        // branch: a huge phase error on a width-valid H is a stream jump,
        // not noise - the old wild-sync guard deadlocked after one).
        if (pll->locked) {
            // Snap so the NEXT line wrap lands exactly on the next sync
            // START (phase - phase_at_sync = the pulse width; the wrap then
            // fires period-width after the rising edge = the next sync
            // start) - the RF decoder's per-line measured-H anchor. Keep
            // the frequency tracking alive: the next line's phase_error
            // measures the per-line drift, so the integral still converges
            // the period for off-nominal capture rates.
            pll->phase -= phase_at_sync;
            if (good_sync) {
                pll->phase_integral += phase_error * tuning.integral_gain;
                if (pll->phase_integral > 50) pll->phase_integral = 50;
                if (pll->phase_integral < -50) pll->phase_integral = -50;
                pll->freq_adjust = pll->phase_integral;
            } else if (fabs(phase_error) > line_period * 0.30) {
                decoder->stream_jumps++;
            }
        } else {
            pll->phase -= phase_error * tuning.phase_gain;
            pll->phase_integral += phase_error * tuning.integral_gain;
            if (pll->phase_integral > 50) pll->phase_integral = 50;
            if (pll->phase_integral < -50) pll->phase_integral = -50;
            pll->freq_adjust = pll->phase_integral;
        }
    }

    // Store for derivative term (not currently used)
    pll->phase_error = phase_error;

    // Rate refinement from the PLL's sub-sample period estimate (skipped
    // entirely when the declared rate is TRUSTED - the panel plugin's exact
    // resampler output; see cvbs_measure_hh_spacing). The
    // measured_rate bootstrap median is edge-quantized: real-capture jitter
    // biased it 39.98 MHz against a true 40.000 on the EBU PAL capture, and
    // a 0.05% rate error sweeps the line-local carrier ~0.02 deg/sample,
    // rotating the late-active hues by up to ~10 deg - measured A/B: the red
    // bar at 104.8 deg with the exact rate vs 95.8 deg at the median rate).
    // AUTO mode: the phase integral converges line_period+freq_adjust onto
    // the TRUE spacing. FIXED lock: the frozen period's per-line residual
    // appears directly as each good sync's phase_error, so a slow EMA of it
    // recovers the same sub-sample truth. Committed only after a sustained
    // lock (the estimate needs ~1000 lines to settle) and bounded to +-10%
    // of the running rate (a real stream change re-bootstraps through the
    // median path's reject escape instead, never through this refinement).
    // lines_since_lock always counts (the FIXED-mode freeze gate depends
    // on it); the rate commit itself is skipped under a TRUSTED declared
    // rate. (The frozen-period DRIFT-tracking EMA now lives in the FIXED
    // branches above - it must run under a trusted rate too, since a
    // trusted rate does not make a drifting input clock exact.)
    if (pll->locked && good_sync) {
        if (pll->lines_since_lock < 1000000) pll->lines_since_lock++;
        if (pll->lines_since_lock > 1000 && !decoder->rate_trusted) {
            double effective;
            if (fixed_mode && decoder->fixed_sync_locked) {
                effective = decoder->fixed_line_period;
            } else {
                effective = pll->line_period + pll->freq_adjust;
            }
            if (effective > 100.0) {
                double refined = effective * 1000000.0
                                 / cvbs_profile(decoder)->line_us;
                double cur = cvbs_rate(decoder);
                if (refined > 1000000.0 && refined < 250000000.0 &&
                    fabs(refined - cur) < 0.10 * cur) {
                    decoder->measured_rate_hz = (uint32_t)(refined + 0.5);
                    decoder->measured_rate_valid = true;
                }
            }
        }
    } else if (!pll->locked) {
        pll->lines_since_lock = 0;
        decoder->fixed_period_ema = 0.0;
    }

    decoder->debug.hsyncs_last_field++;
}

// Decode current line from line buffer to field buffer
static void decode_current_line(cvbs_decoder_t *decoder) {
    if (!decoder || !decoder->line_buffer) return;

    cvbs_pll_state_t *pll = &decoder->pll;
    int line_num = pll->current_line;
    int frame_width = decoder->frame_width;

    int max_field_lines = decoder->field_height;
    if (frame_width < 1 || frame_width > CVBS_MAX_WIDTH) return;
    if (max_field_lines < 1) return;

    // Decode all lines into the field buffer (Full and Active share the same decode path).
    if (line_num >= 0 && line_num < max_field_lines) {
        int field_line = line_num;

        // Write to the appropriate field buffer (not directly to frame)
        int field_idx = decoder->state.current_field ? 1 : 0;
        uint8_t *field_buf = decoder->field_buffer[field_idx];

        if (field_buf && field_line >= 0 && field_line < max_field_lines) {
            // RGB24 rows (3 bytes per pixel) since the colour decode.
            uint8_t *row_ptr = field_buf + ((size_t)field_line * (size_t)frame_width * 3u);

            // Use samples from line buffer
            int samples_available = decoder->line_buffer_count;
            if (samples_available > 100) {  // Need minimum samples
                decode_line_to_pixels(decoder,
                                      decoder->line_buffer, samples_available,
                                      &decoder->levels, row_ptr, frame_width);
                if (decoder->field_line_count[field_idx] < field_line + 1) {
                    decoder->field_line_count[field_idx] = field_line + 1;
                }
            }
        }
    }

    // Clear line buffer for next line
    decoder->line_buffer_count = 0;
}

// Resolve sync threshold with safe fallbacks:
// 1) adaptive threshold (preferred, field-tracked)
// 2) legacy levels threshold
// 3) one-shot histogram bootstrap from current buffer (startup only)
static inline int16_t cvbs_get_sync_threshold(cvbs_decoder_t *decoder,
                                              const int16_t *buf, size_t count) {
    if (!decoder) return 0;
    if (decoder->sync_mode == CVBS_SYNC_MODE_FIXED &&
        decoder->fixed_sync_locked &&
        decoder->fixed_sync_threshold != 0) {
        return decoder->fixed_sync_threshold;
    }

    int16_t threshold = decoder->adaptive.threshold;

    // Bootstrap when decoder starts with no committed adaptive levels yet.
    // The bootstrap threshold is ALWAYS the raw min + 25% of the min..max
    // span of the current buffer - NO two-peak picker. The picker's
    // peak-based threshold has been measured landing ABOVE true blanking
    // on real captures (its "tip" peak mispicked onto mid-level content:
    // measured -9134 against a true blank of -10675 on the EBU PAL capture
    // - blanking itself then read LOW and the pulse classification
    // degenerated into tens of thousands of sub-us fragments, rejecting
    // every VBI train), and a threshold above blanking can NEVER recover
    // through the porch path either (the PLL lock that gates the porch
    // accumulation needs working sync classification first) - so the
    // bootstrap must land inside the tip..blank window on ANY content.
    // min+25% does: on a normal signal (tip -40 IRE, white +100 IRE) it
    // sits ~5 IRE below blanking, and even an all-dark or all-bright
    // buffer keeps it between tip and blank. The picker stays available to
    // the field-commit path (with the porch once lock establishes) where
    // its whole-field histogram and the porch cross-check it.
    //
    // The bootstrap must NOT seed the adaptive level trackers: a level
    // guessed from a one-buffer window then EMA-smooths over ~15 fields
    // toward the truth (measured: 75 IRE after 8 fields on bars). Leaving
    // adaptive at zero lets the FIRST REAL field commit initialize directly
    // from a whole-field window (the fast, accurate path). The decode
    // levels get a rough one-shot seed (black at the threshold, white at
    // the raw max) only until that first commit - a display-only transient.
    if (threshold == 0 && buf && count >= 64) {
        // The min..max span comes from a SINGLE-POLE LOWPASSED copy of the
        // buffer (the decoder's ~500kHz sync filter): the FILTERED max is
        // the LUMA peak (the subcarrier is filtered out), so a saturated
        // bar's chroma overshoot can never push the max above the white
        // level and lift min+25% above blanking (measured on the synthetic
        // 100% bars: the raw max rides the yellow bar's +44.7 IRE chroma
        // peak and the raw min+25% lands ABOVE the blanking level - every
        // pulse then fragments and nothing ever locks). The filtered min
        // is the sync tip, the deepest content on any legal signal (ITU
        // 100% bars' chroma dips stop ~7 IRE above the tip).
        // The local filter restarts from buf[0] (a RAW sample): a chunk
        // starting on a chroma peak would carry that peak as the "filtered"
        // max for the first ~3 tau (~36 samples) and lift the threshold
        // above blanking (measured on the synthetic bars: a chunk starting
        // on the cyan bar's raw peak gave mx=12306, threshold=8260 ABOVE the
        // blanking level 8192 - blanking then read LOW and the black bar +
        // porches + sync tip fused into one ~15us blob whose fragments
        // spawned false first-EQ candidates that rejected real trains for
        // cadence). Skip the warm-up region before tracking min/max.
        int32_t lpf = buf[0];
        int16_t mn = 0, mx = 0;
        bool minmax_warm = false;
        for (size_t i = 1; i < count; i++) {
            lpf = (20 * (int32_t)buf[i] + 236 * lpf) >> 8;
            int16_t f = (int16_t)lpf;
            if (i < 64) continue;  // filter warm-up (5.4 tau)
            if (!minmax_warm) {
                mn = mx = f;
                minmax_warm = true;
            } else {
                if (f < mn) mn = f;
                if (f > mx) mx = f;
            }
        }
        int32_t span = minmax_warm ? (int32_t)mx - (int32_t)mn : 0;
        if (span >= 100) {
            threshold = (int16_t)((int32_t)mn + span / 4);
            if (decoder->levels.range < 100) {
                decoder->levels.sig_min = mn;
                decoder->levels.sig_max = mx;
                decoder->levels.range = (int16_t)span;
                decoder->levels.sync_threshold = threshold;
                decoder->levels.black_level = threshold;
                decoder->levels.white_level = mx;
                if (decoder->level_mode == CVBS_LEVEL_MODE_FIXED) {
                    decoder->fixed_black_level = threshold;
                    decoder->fixed_white_level = mx;
                    decoder->fixed_levels_valid = true;
                }
            }
        }
    }

    // Degenerate-buffer fallback (span < 100): the last good bootstrap.
    if (threshold == 0) {
        threshold = decoder->levels.sync_threshold;
    }

    return threshold;
}

void gui_cvbs_process_buffer(cvbs_decoder_t *decoder,
                              const int16_t *buf, size_t count,
                              uint32_t sample_rate) {
    if (!decoder || !buf || count < 100) return;
    // The display thread passes the LIVE rate; 0/invalid keeps the previous
    // (or the 40 MSPS default). Everything downstream derives from it - and
    // the self-calibrated rate (cvbs_measure_hh_spacing) overrides it once
    // the signal's own sync geometry establishes the truth (the declared
    // value has been measured wrong on live paths: the app-level rate stuck
    // at the 40 MSPS default while the stream is 20 MSPS).
    if (sample_rate > 1000000u && decoder->sample_rate_hz != sample_rate) {
        decoder->sample_rate_hz = sample_rate;
        // A wrong declared scale can NEVER track to the truth through the
        // +-50-sample period integral cap (measured: PAL @ 25 MSPS stuck at
        // period 2560.5 samples/line vs the true 1600). Re-derive the
        // sample-domain state on any rate change; a stable capture only
        // fires this once, and acquisition re-locks within a few lines.
        cvbs_rederive_sample_domain(decoder);
    }
    double rate_hz = cvbs_rate(decoder);
    const cvbs_baseband_profile_t *profile = cvbs_profile(decoder);
    if (!decoder->line_buffer) return;

    // Start timing for this buffer processing
    uint64_t start_time = get_time_us();

    // Check for minimum signal strength (skip until first V-sync commits levels)
    if (decoder->levels.range < 100 && decoder->debug.vsync_found > 0) {
        decoder->sync_errors++;
        return;
    }

    // Cache frequently accessed values in local variables
    const int16_t base_threshold = cvbs_get_sync_threshold(decoder, buf, count);
    cvbs_pll_state_t *pll = &decoder->pll;
    int16_t *line_buffer = decoder->line_buffer;
    int line_buffer_count = decoder->line_buffer_count;
    size_t global_sample_pos = decoder->global_sample_pos;
    bool last_filtered_above = decoder->last_filtered_above;

    // Per-line DC restoration (back-porch clamp). The slicing threshold is
    // the committed tip/blank midpoint PLUS the latest line's measured DC
    // offset, so a baseline sag (AC-coupled source after the vertical broad
    // pulses) moves the threshold with the signal instead of leaving the
    // blanking below it. Measured mid-line from the raw line buffer the
    // moment the back porch is complete (dc_p1 samples into the line), so
    // the NEXT sync edge already slices at the corrected level and this
    // line's own luma black (decode_line_to_pixels) uses its own porch.
    // Windows relative to the line origin (the filtered falling-edge
    // crossing, CVBS_SYNC_EDGE_DELAY_SAMPLES after the true sync start):
    // tip interior 1.0..3.5 us into the 4.7 us pulse, porch = burst end +2
    // .. active start -2 (the field-commit porch window).
    const int dc_t0 = cvbs_us_to_samples(decoder, 1.0) - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    const int dc_t1 = cvbs_us_to_samples(decoder, 3.5) - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    const int dc_p0 = cvbs_us_to_samples(decoder, profile->burst_end_us) + 2
                      - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    const int dc_p1 = cvbs_us_to_samples(decoder, profile->active_start_us) - 2
                      - CVBS_SYNC_EDGE_DELAY_SAMPLES;
    const bool dc_windows_ok = dc_t0 >= 0 && dc_t1 > dc_t0 + 4 &&
                               dc_p0 > dc_t1 && dc_p1 > dc_p0 + 8 &&
                               dc_p1 < CVBS_LINE_BUFFER_SIZE;
    int16_t threshold = base_threshold;
    if (pll->locked && decoder->dc_offset_valid) {
        threshold = (int16_t)(base_threshold + lrint(decoder->dc_offset));
    }

    // Pre-calculate effective period (only changes on H-sync/state transitions)
    double effective_period = cvbs_effective_line_period(decoder);

    // Field-overflow cap: the current parity's line count plus VBI headroom
    // (a safety net - the verified VBI model normally re-anchors the field
    // long before this fires).
    int max_lines = profile->field_lines[decoder->state.current_field ? 1 : 0] + 24;

    // Cached VBI pulse state (same write-back pattern as the other locals).
    cvbs_vsync_state_t *vs = &decoder->vsync;

    // Process each sample
    for (size_t i = 0; i < count; i++) {
        int16_t sample = buf[i];

        // Apply lowpass filter for cleaner sync detection (fixed-point).
        // prev_filtered is the filter's PREVIOUS output, read from the Q8
        // state before this sample's update - it feeds the sub-sample edge
        // interpolation below.
        int16_t prev_filtered = (int16_t)(decoder->lpf_state >> LPF_SHIFT);
        int16_t filtered = apply_lowpass(decoder, sample);

        // Accumulate samples for histogram-based level detection (raw
        // domain - the peak picker's historical domain: sync tip + max).
        // Blank/black/threshold come from the per-line porch measurement
        // and the white from the filtered luma max (see commit_adaptive_
        // levels): the raw histogram's second-lowest peak runs below true
        // blank on saturated chroma, and the filtered histogram's edge-ramp
        // clusters mask the blank plateau from the picker.
        accumulate_level_sample(decoder, sample);

        // Track the per-field FILTERED (luma-domain) maximum: the ~500kHz
        // sync lowpass removes the chroma, so this is the true luma peak
        // (the white-bar plateau), not the raw max's chroma/noise overshoot.
        if (!decoder->adaptive.luma_max_valid ||
            filtered > decoder->adaptive.luma_max) {
            decoder->adaptive.luma_max = filtered;
            decoder->adaptive.luma_max_valid = true;
        }

        // Store UNFILTERED sample in line buffer (for video decoding)
        if (line_buffer_count < CVBS_LINE_BUFFER_SIZE) {
            line_buffer[line_buffer_count++] = sample;
        }

        // Back porch complete for this line: measure the line's DC (porch)
        // and sync tip from the raw samples and re-slice from here on. Gated
        // on PLL lock (an unlocked wrap lands anywhere) and on a plausible
        // tip-to-porch swing (a VBI broad-pulse line puts the porch window
        // on the tip: swing ~0 -> keep the previous offset).
        if (line_buffer_count == dc_p1 && dc_windows_ok && pll->locked &&
            decoder->adaptive.porch_valid) {
            int64_t ps = 0, ts = 0;
            for (int k = dc_p0; k < dc_p1; k++) ps += line_buffer[k];
            for (int k = dc_t0; k < dc_t1; k++) ts += line_buffer[k];
            double porch = (double)ps / (double)(dc_p1 - dc_p0);
            double tip = (double)ts / (double)(dc_t1 - dc_t0);
            // Nominal tip-to-blank swing from the committed levels (int32:
            // levels.range is an int16 that saturates in the 16x s16 domain).
            double swing_nom = (double)decoder->adaptive.porch_level
                               - (double)decoder->levels.sig_min;
            double range32 = (double)((int32_t)decoder->levels.white_level
                                      - (int32_t)decoder->levels.sig_min);
            if (swing_nom < 0.1 * range32) swing_nom = 0.4 * range32;
            double swing = porch - tip;
            double offset = porch - (double)decoder->adaptive.porch_level;
            // Accept a line whose tip window holds a real sync pulse: a full
            // H sync reads ~1.0x the nominal swing, a post-EQ line ~0.45-0.55x
            // (the 2.35 us EQ pulse half-fills the 1.0-3.5 us window), a
            // broad-pulse line ~0 (porch window on the tip) and a misaligned
            // window on picture content well above 1.6x. The offset bound is
            // a full tip-to-blank swing (~40 IRE) of baseline wander.
            bool accept = swing > 0.35 * swing_nom && swing < 1.6 * swing_nom &&
                          fabs(offset) < 1.0 * swing_nom;
            if (accept) {
                decoder->dc_offset = offset;
                decoder->dc_offset_valid = true;
                threshold = (int16_t)(base_threshold + lrint(offset));
            }
#if CVBS_LIVE_DIAG
            if (pll->current_line < 64) {
                if (pll->current_line == 0 || g_diag_dc_count >= 64 ||
                    (g_diag_dc_count > 0 &&
                     g_diag_dc[g_diag_dc_count - 1].line > pll->current_line)) {
                    g_diag_dc_count = 0;
                }
                if (g_diag_dc_count < 64) {
                    g_diag_dc[g_diag_dc_count++] = (cvbs_diag_dc_t){
                        pll->current_line, porch, tip, swing, offset, accept ? 1 : 0,
                        (int)threshold };
                }
            }
#endif
        }

        // Advance PLL phase
        pll->phase += 1.0;
        pll->samples_in_line++;
        pll->total_samples++;
        global_sample_pos++;

        // Check for edge transitions (pulse classifier + PLL)
        bool is_above = (filtered > threshold);
        bool falling_edge = last_filtered_above && !is_above;
        bool rising_edge = !last_filtered_above && is_above;

        // Falling edge: a low pulse begins. Record its start; the H-sync
        // phase anchor references the sync START (the profile's windows are
        // measured from it), reconstructed at the rising edge as the current
        // phase minus the pulse width (the PLL advances exactly one phase
        // count per sample and is only corrected at pulse ends).
        if (falling_edge) {
            vs->in_pulse = true;
            vs->pulse_start = global_sample_pos;
            // Sub-sample pulse start: linear interpolation of the threshold
            // crossing between the previous and current filtered samples.
            // The integer crossing quantizes +-0.5 samples per pulse, and
            // every sync edge feeds the PLL snap - the quantization bias
            // measurably corrupted the panel-rate decode chain (the frozen
            // period latched ~+0.5 samples, the refined rate ran ~0.04% off
            // and the carrier reference slipped against the real signal,
            // scrambling the hues; the 40 MSPS direct path hid it because a
            // half sample there is only ~20 degrees of carrier phase).
            double frac = (prev_filtered > filtered)
                ? (double)(prev_filtered - threshold) /
                  (double)(prev_filtered - filtered)
                : 0.0;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            vs->pulse_start_sub = (double)global_sample_pos - 1.0 + frac;
        }

        // Rising edge: a low pulse ends - classify it by WIDTH IN
        // MICROSECONDS and feed the ported VBI model. The legacy logic
        // (fixed-sample windows + interval half-line counting + a >=15
        // half-lines parity guess) only worked at 40 MSPS and mis-framed
        // whenever the EQ train was noisy; the pulse-width classification,
        // first-EQ parity and cadence validation below are the RF decoder's
        // verified model.
        if (rising_edge && vs->in_pulse) {
            vs->in_pulse = false;
            // Sub-sample pulse end (same interpolation as the start): the
            // width and the PLL snap both carry sub-sample resolution.
            double frac_end = (filtered > prev_filtered)
                ? (double)(threshold - prev_filtered) /
                  (double)(filtered - prev_filtered)
                : 0.0;
            if (frac_end < 0.0) frac_end = 0.0;
            if (frac_end > 1.0) frac_end = 1.0;
            double pulse_end_sub =
                (double)global_sample_pos - 1.0 + frac_end;
            double pulse_us = (pulse_end_sub - vs->pulse_start_sub) *
                              1000000.0 / rate_hz;
            double spacing = vs->last_pulse_pos
                ? (double)vs->pulse_start - (double)vs->last_pulse_pos
                : 0.0;
            bool half_line_cadence = vs->last_pulse_pos != 0 &&
                fabs(spacing - effective_period * 0.5) < effective_period * 0.12;
            // CADENCE-ASSISTED TRAIN RECLASSIFICATION (measured on the
            // real HackDAC captures, 2026-10-21, the classifier diag): the
            // AC-coupling baseline sag through the vertical blanking
            // (+19-26 IRE over the train lines) lifts the DC-clamped
            // slicing threshold 9-16 IRE above the pulse midpoint, so
            // measured widths inflate/shrink and the fixed WIDTH bands
            // misclassify train members:
            //  - the 2.35 us EQs read 3.4-3.7 us, crossing the 3.5 EQ/H
            //    edge (PAL survived by margin; the deeper-sagged SECAM
            //    reset eq_run on every train EQ and produced ZERO field
            //    candidates = permanent rolling);
            //  - the LATER broads of the 5-broad train read 0.8-0.9 us
            //    (the recovering baseline lifts their floors above the
            //    per-line threshold; the 3-broad handshake never
            //    completes; the second train died exactly here on both
            //    measured runs).
            // The SPACING is threshold-invariant (every pulse start shifts
            // by the same flank ascent), so the cadence recovers the true
            // classification:
            //  1. Inside a building BROAD run (last_kind == broad,
            //     v_run >= 1) a 0.6-8 us pulse at half-line cadence is a
            //     sag-shallow BROAD, not an EQ/H: continue the run so the
            //     handshake completes.
            //  2. Inside a building PRE-EQ run (eq_run >= 1) an H-band
            //     (3.5-8 us) pulse at half-line cadence is a sag-inflated
            //     EQ, not an H sync: continue the run.
            // Real H syncs sit at FULL-line cadence and arrive with
            // eq_run 0 outside trains, so both gates stay tight; the
            // first train EQ (measured at the settled pre-train
            // threshold, 2.3-2.9 us) stays width-classified, so
            // first_eq_pos and the parity math are unchanged. Rule 1
            // covers the post-broad EQ train too (last_kind stays broad
            // there): those EQs count as broad-run continuations, which
            // only increments the already-fired v_run (benign) and the
            // next field's pre-EQ train starts fresh off the picture Hs.
            if (pulse_us >= 0.6 && pulse_us < 8.0 &&
                vs->last_kind == 3 && vs->v_run >= 1 && half_line_cadence) {
                pulse_us = 27.0;   // reclassified: a sag-shallow broad
            } else if (pulse_us >= 3.5 && pulse_us < 8.0 &&
                       vs->eq_run >= 1 && half_line_cadence) {
                pulse_us = 2.5;   // reclassified: a sag-inflated train EQ
            }
            if (pulse_us >= 0.6 && pulse_us < 3.5) {
                // Equalizing pulse: part of a vertical-blanking train. The
                // FIRST Eq after a normal H pulse starts a candidate train,
                // but it is not trusted until the expected broad-pulse cadence
                // appears - mirroring the verified RF decoder path.
                if (vs->last_kind == 1 && half_line_cadence) {
                    vs->eq_run++;
                } else {
                    vs->eq_run = 1;
                    vs->first_eq_pos = vs->pulse_start;
                }
                vs->v_run = 0;
                vs->last_kind = 1;
                vs->last_pulse_pos = vs->pulse_start;
            } else if (pulse_us >= 3.5 && pulse_us <= 8.0) {
                // Normal H-sync tip: feeds the PLL (with the phase
                // reconstructed at the pulse START) and the VBI H reference.
                vs->eq_run = 0;
                vs->v_run = 0;
                double phase_at_start = pll->phase -
                    ((double)global_sample_pos - vs->pulse_start_sub);
                pll_process_hsync(decoder, phase_at_start);
                vs->last_h_pos = vs->pulse_start;
                vs->last_kind = 2;
                vs->last_pulse_pos = vs->pulse_start;
                // Update effective period after PLL adjustment
                effective_period = cvbs_effective_line_period(decoder);
            } else if (pulse_us >= 19.0 && pulse_us <= 36.0) {
                // Broad pulses confirm a real VBI train only after a valid
                // pre-EQ cadence. Trigger the field-boundary logic on the
                // SECOND broad pulse. The RF path fires on the third, but
                // the RF domain has no baseline sag; on the real BASEBAND
                // captures (measured, the classifier diag 2026-10-21) the
                // AC-coupling sag through the vertical blanking suppresses
                // the third and later broads ENTIRELY below the per-line
                // DC-clamped threshold on the deepest-sagged fields (the
                // next threshold crossing is a picture H six lines later,
                // halfcad=0 - no reclassification can recover it), so a
                // 3-broad handshake never completes on those fields and
                // the picture rolls permanently. Two broads at half-line
                // cadence after a >=3-EQ pre-train is still a signature
                // nothing outside the VBI produces (H syncs sit at
                // full-line cadence), and the anchor math is unchanged:
                // first_eq_pos (set at the pre-EQ train's FIRST EQ) and the
                // parity derivation use the pre-EQ train, not the broad
                // count. Exact-match == 2 fires once per train; later
                // broads (when they cross) only increment past it.
                if (vs->eq_run >= 3 && half_line_cadence) {
                    if (vs->last_kind == 3) {
                        vs->v_run++;
                    } else if (vs->last_kind == 1) {
                        vs->v_run = 1;
                    } else {
                        vs->v_run = 0;
                    }
                    if (vs->v_run == 2) {
                        decoder->line_buffer_count = line_buffer_count;
                        decoder->global_sample_pos = global_sample_pos;
                        decoder->last_filtered_above = is_above;
                        cvbs_vbi_first_eq(decoder, vs->first_eq_pos, global_sample_pos);
                        effective_period = cvbs_effective_line_period(decoder);
                        line_buffer_count = decoder->line_buffer_count;
                    }
                } else {
                    vs->v_run = 0;
                }
                vs->last_kind = 3;
                vs->last_pulse_pos = vs->pulse_start;
            }
            // Broad pulses (19-36us) and anything unclassified: neither an H
            // reference nor a PLL correction - exactly the RF decoder's model.

            // Rate self-calibration spacing (see cvbs_measure_hh_spacing):
            // fed from ANY EQ-or-H classified pulse, not just H - at a wrong
            // declared rate (e.g. 40 MSPS declared on a 20 MSPS stream, the
            // measured live-path failure) every pulse width halves in
            // microseconds, so the H syncs classify as EQ and an H-only feed
            // would never fire (chicken-and-egg: no H classification, no
            // measured rate). The active-area "EQs" still sit at FULL line
            // spacing, while the true train EQs sit at half-line spacing,
            // which the acceptance band rejects.
            if (pulse_us >= 0.6 && pulse_us <= 8.0) {
                if (vs->last_classified_pos != 0) {
                    double hh = (double)(vs->pulse_start)
                              - (double)(vs->last_classified_pos);
                    if (hh > 0.0) {
                        cvbs_measure_hh_spacing(decoder, hh);
                        // A fresh rate commit re-derives the PLL; refresh the
                        // cached effective period the loop uses below.
                        effective_period = cvbs_effective_line_period(decoder);
                    }
                }
                vs->last_classified_pos = vs->pulse_start;
            }
        }

        last_filtered_above = is_above;

        // Check if PLL indicates line complete (phase >= line_period)
        if (pll->phase >= effective_period) {
            // Write back line_buffer_count (and the global position, which
            // decode_line_to_pixels reads as the ABSOLUTE carrier anchor)
            // before decode.
            decoder->line_buffer_count = line_buffer_count;
            decoder->global_sample_pos = global_sample_pos;

            // Line complete - decode it
            {
                uint64_t t_line = get_time_us();
                decode_current_line(decoder);
                g_prof_lines_us += get_time_us() - t_line;
                g_prof_lines++;
            }

            // Refresh line_buffer_count (decode_current_line resets it)
            line_buffer_count = decoder->line_buffer_count;

            // Advance to next line
            pll->current_line++;
            pll->phase -= effective_period;
            pll->samples_in_line = 0;
            decoder->lines_since_vsync++;

            // Check for field overflow (safety net: the VBI model normally
            // re-anchors long before this fires; alternates parity as the
            // best fallback without a verified boundary)
            if (pll->current_line > max_lines) {
                int next_parity = decoder->state.current_field ? 0 : 1;
                decoder->global_sample_pos = global_sample_pos;
                decoder->last_filtered_above = last_filtered_above;
                cvbs_start_field_anchored(decoder, next_parity,
                                          global_sample_pos, (double)global_sample_pos);
                effective_period = cvbs_effective_line_period(decoder);
                line_buffer_count = decoder->line_buffer_count;
            }
        }
    }

    // Write back cached state
    decoder->line_buffer_count = line_buffer_count;
    decoder->global_sample_pos = global_sample_pos;
    decoder->last_filtered_above = last_filtered_above;

    // Accumulate total decode time for this field
    {
        uint64_t spent = get_time_us() - start_time;
        g_field_decode_us += spent;
        g_prof_process_us += spent;
    }
}

/* Install a sink before starting decode; caller owns the sink lifetime. */
void gui_cvbs_set_monitor_frame_sink(cvbs_decoder_t *decoder,
    void (*callback)(void *, const uint8_t *, unsigned, unsigned, uint64_t),
    void *context) {
    if (!decoder) return;
    decoder->monitor_frame_sink = callback;
    decoder->monitor_frame_sink_context = context;
}

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

// Swap buffers if new frame available (called from render thread before rendering)
// Copies back buffer to front buffer if display_ready flag is set.
void gui_cvbs_swap_buffers(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    // Check if new frame is available
    if (atomic_exchange(&decoder->display_ready, 0)) {
        // Copy back buffer to front buffer
        int frame_h = decoder->frame_height;
        int frame_w = decoder->frame_width;
        if (frame_h > CVBS_MAX_HEIGHT) frame_h = CVBS_MAX_HEIGHT;
        if (frame_w > CVBS_MAX_WIDTH) frame_w = CVBS_MAX_WIDTH;
        if (frame_w < 1) return;
        memcpy(decoder->display_front, decoder->display_back,
               (size_t)frame_w * 3u * (size_t)frame_h);
    }
}

// Draw on-screen status text depending on decoder->osd_mode.
static void gui_cvbs_render_osd(cvbs_decoder_t *decoder,
                                float draw_x, float draw_y,
                                float draw_w, float draw_h) {
    if (!decoder) return;

    if (decoder->osd_mode == CVBS_OSD_OFF) {
        return;
    }

    // The field being decoded has its ready flag cleared at its start, so
    // the flags alone always read as one field; report the mode the last
    // deinterlace pass actually used.
    const char *deint_mode = decoder->last_weave ? "weave" : "bob";

    float text_x = draw_x + 8.0f;
    float text_y = draw_y + draw_h - 24.0f;

    char osd_text[96];

    if (decoder->osd_mode == CVBS_OSD_MINIMAL) {
        // Minimal: deinterlacer + timing (existing behaviour)
        snprintf(osd_text, sizeof(osd_text), "%s | %.2f ms", deint_mode, g_field_decode_avg_ms);
        gui_text_draw_mono(osd_text, text_x + 1, text_y + 1, 14, BLACK);
        gui_text_draw_mono(osd_text, text_x, text_y, 14, (Color){255, 255, 100, 255});
        return;
    }

    // Detailed stats: include format, frames decoded and sync statistics.
    const char *fmt_name = gui_cvbs_get_format_name(decoder);

    // Line 1: format + deinterlacer + timing
    snprintf(osd_text, sizeof(osd_text), "%s | %s | %.2f ms",
             fmt_name, deint_mode, g_field_decode_avg_ms);
    gui_text_draw_mono(osd_text, text_x + 1, text_y + 1, 14, BLACK);
    gui_text_draw_mono(osd_text, text_x, text_y, 14, (Color){255, 255, 220, 255});

    // Line 2: frames + sync stats + the VBI model diagnostics. jumps = H
    // syncs that arrived > 0.3 line off the locked prediction = sample
    // stream discontinuities (feed-buffer drops); a clean feed reads 0.
    char osd_text2[128];
    snprintf(osd_text2, sizeof(osd_text2),
             "frames:%d sync_err:%d jumps:%d hsync/field:%d VBI %d/%d rej %d (cad %d/par %d)",
             decoder->state.frames_decoded,
             decoder->sync_errors,
             decoder->stream_jumps,
             decoder->debug.hsyncs_last_field,
             decoder->vsync.fields_accepted,
             decoder->vsync.vsync_candidates,
             decoder->vsync.fields_rejected,
             decoder->vsync.reject_cadence,
             decoder->vsync.reject_parity);

    // Line 3: live rate + the last VBI reject reason
    char osd_text3[112];
    snprintf(osd_text3, sizeof(osd_text3), "rate %.1f MHz | VBI rej: %s",
             cvbs_rate(decoder) / 1000000.0,
             decoder->vsync.last_reject_reason[0] ?
             decoder->vsync.last_reject_reason : "none");

    float text2_y = text_y - 18.0f;
    gui_text_draw_mono(osd_text2, text_x + 1, text2_y + 1, 14, BLACK);
    gui_text_draw_mono(osd_text2, text_x, text2_y, 14, (Color){200, 255, 200, 255});

    float text3_y = text2_y - 18.0f;
    gui_text_draw_mono(osd_text3, text_x + 1, text3_y + 1, 14, BLACK);
    gui_text_draw_mono(osd_text3, text_x, text3_y, 14, (Color){200, 200, 255, 255});

    // Line 4 (SECAM only): the FM-chroma diagnostics - the locked carrier
    // pair, the block offset, the line identification and its source, and
    // the cal/reuse/nominal field counters. All zeros until the first
    // field commits, so a freshly switched format reads cleanly.
    if (decoder->state.format == CVBS_FORMAT_SECAM) {
        double fob, forr, off, conf, mad;
        int dr_even, ident_src, fcal, freuse, fnom;
        gui_cvbs_get_secam_diag(decoder, &fob, &forr, &off, &dr_even,
                                &conf, &ident_src, &mad,
                                &fcal, &freuse, &fnom);
        static const char *ident_src_names[3] = {"porch", "content", "bottles"};
        int src_i = (ident_src >= 0 && ident_src < 3) ? ident_src : 0;
        char osd_text4[160];
        snprintf(osd_text4, sizeof(osd_text4),
                 "SECAM fB %.4f fR %.4f MHz off %+.1f kHz | D'R %s-even conf %.0f%% src %s cal/reuse/nom %d/%d/%d",
                 fob / 1000000.0, forr / 1000000.0, off / 1000.0,
                 dr_even ? "on" : "off", conf * 100.0,
                 ident_src_names[src_i], fcal, freuse, fnom);
        float text4_y = text3_y - 18.0f;
        gui_text_draw_mono(osd_text4, text_x + 1, text4_y + 1, 14, BLACK);
        gui_text_draw_mono(osd_text4, text_x, text4_y, 14, (Color){255, 200, 255, 255});
    }
}

// Pure RGB24 -> raylib Color blit for the render path. Kept a standalone
// pure function (no raylib drawing dependencies) so the test harness can
// contract-test its byte layout - the 2026-10-19 incident: a hand-packed
// uint32 conversion here (0xFF000000 | r<<16 | g<<8 | b, whose little-endian
// memory is b,g,r,a against raylib Color's r,g,b,a) displayed the decoded
// BLUE as red and RED as blue while every headless verification path -
// which read the RGB24 buffers directly, BYPASSING this conversion - was
// green. Byte-layout contract: test/gui_cvbs_test.c
// (run_render_conversion_contract); HARD rules:
// misrc_gui/dev/gui_output_verification_rules.md. dst_pitch is in Color
// pixels (the frame image's row pitch - MAX_WIDTH - so NTSC (width <
// MAX_WIDTH) does not shear).
static void cvbs_blit_rgb24_to_color(const uint8_t *src, Color *dst,
                                      int width, int height, int dst_pitch) {
    for (int py = 0; py < height; py++) {
        const uint8_t *src_row = src + (size_t)py * (size_t)width * 3u;
        Color *dst_row = dst + (size_t)py * (size_t)dst_pitch;
        for (int px = 0; px < width; px++) {
            dst_row[px].r = src_row[px * 3 + 0];
            dst_row[px].g = src_row[px * 3 + 1];
            dst_row[px].b = src_row[px * 3 + 2];
            dst_row[px].a = 255;
        }
    }
}

// Render core shared by the public render (always with OSD) and the panel
// chrome-hide path: draw_osd=false keeps the decoded picture but hides the
// OSD text chrome with the rest of the controls.
static void gui_cvbs_render_frame_impl(cvbs_decoder_t *decoder,
                                       float x, float y, float width, float height,
                                       bool draw_osd) {
    if (!decoder || !decoder->display_front) {
        // No frame available - draw placeholder
        DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){20, 20, 30, 255});
        DrawText("No Signal", (int)(x + width/2 - 40), (int)(y + height/2 - 10),
                 20, (Color){100, 100, 100, 255});
        return;
    }

    // Update texture if needed
    if (!decoder->texture_valid) {
        decoder->frame_texture = LoadTextureFromImage(decoder->frame_image);
        SetTextureFilter(decoder->frame_texture, TEXTURE_FILTER_BILINEAR);
        decoder->texture_valid = true;
    }

    // Get frame height
    int field_h = decoder->frame_height;
    int frame_w = decoder->frame_width;
    if (field_h > CVBS_MAX_HEIGHT) field_h = CVBS_MAX_HEIGHT;
    if (frame_w > CVBS_MAX_WIDTH) frame_w = CVBS_MAX_WIDTH;
    if (frame_w < 1) return;

    // Active picture crop, derived from the SAME profile geometry the decode
    // windows use (including the sync-edge-delay compensation), so the crop
    // tracks the decoded content exactly. The legacy hardcoded 4fsc-ish
    // offsets (PAL x=120 w=900 of 1135 px) were up to ~90 px away from the
    // real active window - measured as a visibly off-center picture on the
    // EBU bars reference. Vertical: the weave maps field line fl to frame
    // rows 2*fl (+1), so the picture rows are twice the profile's picture
    // line range.
    const cvbs_baseband_profile_t *profile = cvbs_profile(decoder);
    double delay_us = (double)CVBS_SYNC_EDGE_DELAY_SAMPLES * 1000000.0
                      / cvbs_rate(decoder);
    int ax0 = (int)((profile->active_start_us - delay_us)
                    / profile->line_us * (double)frame_w + 0.5);
    int ax1 = (int)((profile->active_end_us - delay_us)
                    / profile->line_us * (double)frame_w + 0.5);
    int active_x = ax0;
    int active_w = ax1 - ax0;
    int active_y = profile->picture_start_line * 2;
    int active_h = profile->picture_lines * 2;
    if (active_x < 0) active_x = 0;
    if (active_y < 0) active_y = 0;
    if (active_w < 1) active_w = 1;
    if (active_h < 1) active_h = 1;
    if (active_x + active_w > frame_w) active_w = frame_w - active_x;
    if (active_y + active_h > field_h) active_h = field_h - active_y;

    // Convert the decoded RGB24 frame to the image through the pure,
    // contract-tested blit (see cvbs_blit_rgb24_to_color): the GUI-consumed
    // conversion is code under test, not glue - the 2026-10-19 R/B swap
    // incident (dev/gui_output_verification_rules.md). The row pitch is the
    // image width (MAX_WIDTH) so NTSC (frame_w < MAX_WIDTH) does not shear.
    // Grayscale mode decoded R=G=B so the layout is shared.
    cvbs_blit_rgb24_to_color(decoder->display_front,
                             (Color *)decoder->frame_image.data,
                             frame_w, field_h, decoder->frame_image.width);

    // Upload to GPU
    UpdateTexture(decoder->frame_texture, decoder->frame_image.data);

    // Calculate aspect-correct rectangle (4:3 aspect ratio for CVBS)
    float aspect = 4.0f / 3.0f;
    float display_aspect = width / height;

    float draw_w, draw_h;
    if (display_aspect > aspect) {
        // Height limited
        draw_h = height;
        draw_w = height * aspect;
    } else {
        // Width limited
        draw_w = width;
        draw_h = width / aspect;
    }

    float draw_x = x + (width - draw_w) / 2;
    float draw_y = y + (height - draw_h) / 2;

    // Active mode crops to the picture area only. Full mode draws the
    // entire frame: the tbc-tools-style hybrid active/background split is
    // baked into the decode (see decode_line_to_pixels) - the leveled
    // studio-swing mapping on the active window, the hybrid background
    // mapping (with decoded chroma) everywhere else.
    Rectangle src;
    if (decoder->frame_view_mode == CVBS_FRAME_VIEW_ACTIVE) {
        src = (Rectangle){(float)active_x, (float)active_y,
                          (float)active_w, (float)active_h};
    } else {
        src = (Rectangle){0.0f, 0.0f, (float)frame_w, (float)field_h};
    }
    Rectangle dst = {draw_x, draw_y, draw_w, draw_h};
    DrawTexturePro(decoder->frame_texture, src, dst, (Vector2){0, 0}, 0, WHITE);

    // Draw OSD overlay (mode-controlled)
    if (draw_osd) gui_cvbs_render_osd(decoder, draw_x, draw_y, draw_w, draw_h);
}

void gui_cvbs_render_frame(cvbs_decoder_t *decoder,
                            float x, float y, float width, float height) {
    gui_cvbs_render_frame_impl(decoder, x, y, width, height, true);
}

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

void gui_cvbs_get_secam_diag(cvbs_decoder_t *decoder,
                             double *fob_hz, double *for_hz,
                             double *carrier_offset_hz, int *dr_on_even,
                             double *ident_confidence, int *ident_source,
                             double *noise_mad_hz,
                             int *fields_calibrated, int *fields_reused,
                             int *fields_nominal) {
    const cvbs_secam_state_t zero = {0};
    const cvbs_secam_state_t *s = decoder ? &decoder->secam : &zero;
    if (fob_hz) *fob_hz = s->fob;
    if (for_hz) *for_hz = s->for_;
    if (carrier_offset_hz) *carrier_offset_hz = s->carrier_offset;
    if (dr_on_even) *dr_on_even = s->dr_on_even ? 1 : 0;
    if (ident_confidence) *ident_confidence = s->ident_confidence;
    if (ident_source) *ident_source = s->ident_source;
    if (noise_mad_hz) *noise_mad_hz = s->noise_mad_hz;
    if (fields_calibrated) *fields_calibrated = s->fields_calibrated;
    if (fields_reused) *fields_reused = s->fields_reused;
    if (fields_nominal) *fields_nominal = s->fields_nominal;
}

cvbs_format_t gui_cvbs_get_format(cvbs_decoder_t *decoder) {
    if (!decoder) return CVBS_FORMAT_UNKNOWN;
    return decoder->state.format;
}

const char *gui_cvbs_get_format_name(cvbs_decoder_t *decoder) {
    if (!decoder) return "Unknown";

    switch (decoder->state.format) {
        case CVBS_FORMAT_PAL:   return "PAL 625-line";
        case CVBS_FORMAT_NTSC:  return "NTSC 525-line";
        case CVBS_FORMAT_SECAM: return "SECAM 625-line";
        default:                return "Detecting...";
    }
}
static void gui_cvbs_apply_geometry(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    bool ntsc = (decoder->state.format == CVBS_FORMAT_NTSC);
    const cvbs_baseband_profile_t *profile = cvbs_profile(decoder);
    decoder->state.total_lines = ntsc ? CVBS_NTSC_TOTAL_LINES : CVBS_PAL_TOTAL_LINES;
    decoder->state.active_lines = ntsc ? CVBS_NTSC_ACTIVE_LINES : CVBS_PAL_ACTIVE_LINES;
    decoder->pll.line_period = cvbs_nominal_line_period(decoder);

    // Always allocate/decode at full-frame dimensions.
    // Active vs Full is purely a render-time crop.
    decoder->frame_width = ntsc ? CVBS_NTSC_FULL_WIDTH_4FSC : CVBS_PAL_FULL_WIDTH_4FSC;
    decoder->frame_height = ntsc ? CVBS_NTSC_FULL_HEIGHT : CVBS_PAL_FULL_HEIGHT;
    // The taller parity's line count (PAL 313, NTSC 263): one buffer serves
    // both parities, matching the RF decoder's field line counts by parity
    // (the legacy code used 312/262 and silently dropped the taller parity's
    // last line).
    decoder->field_height = profile->field_lines[1] > profile->field_lines[0]
                            ? profile->field_lines[1] : profile->field_lines[0];

    if (decoder->frame_width > CVBS_MAX_WIDTH) decoder->frame_width = CVBS_MAX_WIDTH;
    if (decoder->frame_height > CVBS_MAX_HEIGHT) decoder->frame_height = CVBS_MAX_HEIGHT;
}

// TODO(dev): Add progressive scan display modes:
// - NTSC 240p single-field mode
// - PAL 288p single-field mode
// This requires a non-deinterlaced single-field display path plus dedicated mode controls.
static void gui_cvbs_set_frame_view_mode(cvbs_decoder_t *decoder, cvbs_frame_view_t mode) {
    if (!decoder) return;
    if (mode != CVBS_FRAME_VIEW_ACTIVE && mode != CVBS_FRAME_VIEW_FULL) {
        mode = CVBS_FRAME_VIEW_ACTIVE;
    }
    if (decoder->frame_view_mode == mode) return;

    // Purely a render-time crop: the decode always runs at full-frame
    // geometry (see gui_cvbs_apply_geometry - "Active vs Full is purely a
    // render-time crop"), so there is NO decoder reset here. The legacy
    // reset threw away the PLL/VBI/level lock on every switch and the
    // picture rolled through re-acquisition (real-GUI user report,
    // 2026-10-20). The next render simply crops the src rectangle
    // differently; the lock and the decoded frame carry over untouched.
    decoder->frame_view_mode = mode;
}

void gui_cvbs_set_format(cvbs_decoder_t *decoder, int format_select) {
    if (!decoder) return;

    // format_select: 0=PAL, 1=NTSC, 2=SECAM
    cvbs_format_t new_format = CVBS_FORMAT_NTSC;
    if (format_select == 0) new_format = CVBS_FORMAT_PAL;
    else if (format_select == 2) new_format = CVBS_FORMAT_SECAM;

    if (decoder->state.format != new_format) {
        decoder->state.format = new_format;
        gui_cvbs_apply_geometry(decoder);

        // Reset decode state after changing system
        gui_cvbs_reset(decoder);
    }
}

//-----------------------------------------------------------------------------
// CVBS Panel Rendering (for panel system integration)
//-----------------------------------------------------------------------------

// Forward declaration needed for dropdown API (from gui_dropdown.h)
#include "../ui/gui_dropdown.h"
#include "../ui/gui_ui.h"

// Render the CVBS overlay controls in the top-right corner of the panel
// Uses the same style as sidebar dropdowns (FONT_SIZE_DROPDOWN_OPT, COLOR_BUTTON, etc.)
// Overlay hitbox state is stored in decoder->overlay for click detection.
// Layout (right-aligned vertical): [Decoder]
//                                  [OSD]
//                                  [Frame]
//                                  [Levels]
//                                  [Sync]
//                                  [System]
static void render_cvbs_system_overlay(cvbs_decoder_t *decoder,
                                        float panel_x, float panel_y, float panel_w) {
    if (!decoder) return;

    decoder->overlay.is_visible = true;
    bool show_tape_controls = (decoder->decoder_mode == CVBS_DECODER_MODE_TAPE);
    if (!show_tape_controls) {
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.tape_format_dropdown_open = false;
    }

    // Clear optional rects so hidden controls don't capture stale clicks.
    decoder->overlay.tape_btn_rect = (Rectangle){0};
    decoder->overlay.tape_format_btn_rect = (Rectangle){0};
    decoder->overlay.tape_mode_btn_rect = (Rectangle){0};

    // Button dimensions
    float btn_h = 18.0f;
    float gap = 6.0f;

    // System selector label (PAL/NTSC/SECAM). In CVBS mode the button shows
    // the ACTUAL decoder format (it tracks manual selection here and any
    // future auto-detection alike); in Tape mode it shows the tape-side
    // system pick (kept in sync with the tape format by the click handler
    // and video_tape_apply_runtime_args).
    const char *system_labels[] = {"PAL", "NTSC", "SECAM"};
    int sys;
    if (show_tape_controls) {
        sys = decoder->overlay.selected_system; // 0=PAL, 1=NTSC, 2=SECAM
    } else {
        sys = (decoder->state.format == CVBS_FORMAT_NTSC) ? 1 :
              (decoder->state.format == CVBS_FORMAT_SECAM) ? 2 : 0;
    }
    if (sys < 0 || sys > 2) sys = 0;
    const char *sys_name = system_labels[sys];
    const char *tape_format_labels[] = {"PAL", "NTSC", "SECAM"};
    int tape_fmt = decoder->overlay.selected_tape_format;
    if (tape_fmt < 0 || tape_fmt > 2) tape_fmt = 0;
    const char *tape_fmt_name = tape_format_labels[tape_fmt];

    char dec_label[32];
    snprintf(dec_label, sizeof(dec_label), "Decoder: %s", cvbs_decoder_mode_name(decoder->decoder_mode));
    char tape_label[40];
    snprintf(tape_label, sizeof(tape_label), "Tape: %s", cvbs_tape_format_name(decoder->tape_format));
    char tape_format_label[32];
    snprintf(tape_format_label, sizeof(tape_format_label), "Format: %s", tape_fmt_name);
    char tape_mode_label[24];
    snprintf(tape_mode_label, sizeof(tape_mode_label), "Mode: %s", cvbs_tape_mode_name(decoder->tape_mode));
    const char *frame_label = (decoder->frame_view_mode == CVBS_FRAME_VIEW_FULL)
                              ? "Frame: Full"
                              : "Frame: Active";
    char level_label[40];
    char sync_label[40];
    char colour_label[40];
    cvbs_build_level_label(decoder, level_label, sizeof(level_label));
    cvbs_build_sync_label(decoder, sync_label, sizeof(sync_label));
    cvbs_build_colour_label(decoder, colour_label, sizeof(colour_label));

    // Compute button width large enough for all three labels so the box
    // does not change size when switching between PAL, NTSC and SECAM.
    // Floor 110 keeps it in line with the other boxes (Frame/OSD/Levels
    // land 110-136): the old 140 floor was sized for the former long
    // "PAL/SECAM" label and left the short 3-way labels swimming in a
    // too-wide box (user feedback, real-GUI 2026-10-20).
    float sys_text_w_max = 0.0f;
    for (int i = 0; i < 3; i++) {
        float w = (float)gui_text_measure(system_labels[i], FONT_SIZE_DROPDOWN_OPT);
        if (w > sys_text_w_max) sys_text_w_max = w;
    }
    float sys_btn_w = sys_text_w_max + 28.0f; // padding + arrow
    if (sys_btn_w < 110.0f) sys_btn_w = 110.0f;

    const char *osd_labels[] = {"OSD: Off", "OSD: Minimal", "OSD: Stats"};
    const char *osd_label = osd_labels[decoder->osd_mode];
    float osd_text_w = (float)gui_text_measure(osd_label, FONT_SIZE_DROPDOWN_OPT);
    float osd_btn_w = osd_text_w + 16.0f;
    if (osd_btn_w < 90.0f) osd_btn_w = 90.0f;

    float frame_text_w = (float)gui_text_measure(frame_label, FONT_SIZE_DROPDOWN_OPT);
    float frame_btn_w = frame_text_w + 16.0f;
    if (frame_btn_w < 110.0f) frame_btn_w = 110.0f;
    float level_text_w = (float)gui_text_measure(level_label, FONT_SIZE_DROPDOWN_OPT);
    float level_btn_w = level_text_w + 16.0f;
    if (level_btn_w < 120.0f) level_btn_w = 120.0f;
    float sync_text_w = (float)gui_text_measure(sync_label, FONT_SIZE_DROPDOWN_OPT);
    float sync_btn_w = sync_text_w + 16.0f;
    if (sync_btn_w < 120.0f) sync_btn_w = 120.0f;
    float colour_text_w = (float)gui_text_measure(colour_label, FONT_SIZE_DROPDOWN_OPT);
    float colour_btn_w = colour_text_w + 16.0f;
    if (colour_btn_w < 120.0f) colour_btn_w = 120.0f;

    float dec_text_w = (float)gui_text_measure(dec_label, FONT_SIZE_DROPDOWN_OPT);
    float dec_btn_w = dec_text_w + 16.0f;
    if (dec_btn_w < 128.0f) dec_btn_w = 128.0f;

    float tape_text_w = (float)gui_text_measure(tape_label, FONT_SIZE_DROPDOWN_OPT);
    float tape_btn_w = tape_text_w + 28.0f;  // include dropdown arrow
    if (tape_btn_w < 130.0f) tape_btn_w = 130.0f;
    float tape_format_text_w = (float)gui_text_measure(tape_format_label, FONT_SIZE_DROPDOWN_OPT);
    float tape_format_btn_w = tape_format_text_w + 28.0f;  // include dropdown arrow
    if (tape_format_btn_w < 130.0f) tape_format_btn_w = 130.0f;
    float tape_mode_text_w = (float)gui_text_measure(tape_mode_label, FONT_SIZE_DROPDOWN_OPT);
    float tape_mode_btn_w = tape_mode_text_w + 16.0f;
    if (tape_mode_btn_w < 100.0f) tape_mode_btn_w = 100.0f;

    // Position buttons in a right-aligned vertical stack.
    float x_right = panel_x + panel_w - 8.0f;
    float btn_y0 = panel_y + 8.0f;
    float row_step = btn_h + gap;

    float col_btn_w = dec_btn_w;
    if (show_tape_controls && tape_btn_w > col_btn_w) col_btn_w = tape_btn_w;
    if (show_tape_controls && tape_format_btn_w > col_btn_w) col_btn_w = tape_format_btn_w;
    if (show_tape_controls && tape_mode_btn_w > col_btn_w) col_btn_w = tape_mode_btn_w;
    if (osd_btn_w > col_btn_w) col_btn_w = osd_btn_w;
    if (frame_btn_w > col_btn_w) col_btn_w = frame_btn_w;
    if (level_btn_w > col_btn_w) col_btn_w = level_btn_w;
    if (sync_btn_w > col_btn_w) col_btn_w = sync_btn_w;
    if (colour_btn_w > col_btn_w) col_btn_w = colour_btn_w;
    if (sys_btn_w > col_btn_w) col_btn_w = sys_btn_w;

    float col_x = x_right - col_btn_w;

    int row = 0;
    float dec_btn_x = col_x + (col_btn_w - dec_btn_w);
    float dec_btn_y = btn_y0 + row_step * row++;

    float tape_btn_x = col_x + (col_btn_w - tape_btn_w);
    float tape_btn_y = btn_y0 + row_step * row;
    if (show_tape_controls) row++;

    float tape_format_btn_x = col_x + (col_btn_w - tape_format_btn_w);
    float tape_format_btn_y = btn_y0 + row_step * row;
    if (show_tape_controls) row++;

    float tape_mode_btn_x = col_x + (col_btn_w - tape_mode_btn_w);
    float tape_mode_btn_y = btn_y0 + row_step * row;
    if (show_tape_controls) row++;

    float osd_btn_x = col_x + (col_btn_w - osd_btn_w);
    float osd_btn_y = btn_y0 + row_step * row++;
    float frame_btn_x = col_x + (col_btn_w - frame_btn_w);
    float frame_btn_y = btn_y0 + row_step * row++;
    float level_btn_x = col_x + (col_btn_w - level_btn_w);
    float level_btn_y = btn_y0 + row_step * row++;
    float sync_btn_x = col_x + (col_btn_w - sync_btn_w);
    float sync_btn_y = btn_y0 + row_step * row++;
    float colour_btn_x = col_x + (col_btn_w - colour_btn_w);
    float colour_btn_y = btn_y0 + row_step * row++;
    float sys_btn_x = col_x + (col_btn_w - sys_btn_w);
    float sys_btn_y = btn_y0 + row_step * row++;

    // Store hit boxes for click detection
    decoder->overlay.decoder_btn_rect = (Rectangle){dec_btn_x, dec_btn_y, dec_btn_w, btn_h};
    if (show_tape_controls) {
        decoder->overlay.tape_btn_rect = (Rectangle){tape_btn_x, tape_btn_y, tape_btn_w, btn_h};
        decoder->overlay.tape_format_btn_rect = (Rectangle){tape_format_btn_x, tape_format_btn_y, tape_format_btn_w, btn_h};
        decoder->overlay.tape_mode_btn_rect = (Rectangle){tape_mode_btn_x, tape_mode_btn_y, tape_mode_btn_w, btn_h};
    }
    decoder->overlay.osd_btn_rect     = (Rectangle){osd_btn_x, osd_btn_y, osd_btn_w, btn_h};
    decoder->overlay.frame_btn_rect   = (Rectangle){frame_btn_x, frame_btn_y, frame_btn_w, btn_h};
    decoder->overlay.level_btn_rect   = (Rectangle){level_btn_x, level_btn_y, level_btn_w, btn_h};
    decoder->overlay.sync_btn_rect    = (Rectangle){sync_btn_x, sync_btn_y, sync_btn_w, btn_h};
    decoder->overlay.colour_btn_rect  = (Rectangle){colour_btn_x, colour_btn_y, colour_btn_w, btn_h};
    decoder->overlay.system_btn_rect  = (Rectangle){sys_btn_x, sys_btn_y, sys_btn_w, btn_h};
    // Decoder mode button (CVBS <-> Tape)
    Color dec_bg = (decoder->decoder_mode == CVBS_DECODER_MODE_TAPE) ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    DrawRectangleRounded(decoder->overlay.decoder_btn_rect, 0.15f, 4, dec_bg);
    float dec_text_x = dec_btn_x + (dec_btn_w - dec_text_w) / 2.0f;
    float dec_text_y = dec_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
    gui_text_draw(dec_label, dec_text_x, dec_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    if (show_tape_controls) {
        // Tape family selector (dropdown)
        bool tape_open = decoder->overlay.tape_dropdown_open;
        Rectangle tape_btn_rect = decoder->overlay.tape_btn_rect;
        Color tape_bg = tape_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
        DrawRectangleRounded(tape_btn_rect, 0.15f, 4, tape_bg);

        int tape_label_w = gui_text_measure(tape_label, FONT_SIZE_DROPDOWN_OPT);
        float tape_arrow_w = 8.0f;
        float tape_total_w = (float)tape_label_w + tape_arrow_w + 4.0f;
        float tape_text_x = tape_btn_rect.x + (tape_btn_rect.width - tape_total_w) / 2.0f;
        float tape_text_y = tape_btn_rect.y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
        gui_text_draw(tape_label, tape_text_x, tape_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

        float arrow_size = 5.0f;
        float arrow_x = tape_text_x + tape_label_w + 6.0f;
        float arrow_cy = tape_btn_rect.y + btn_h / 2.0f;
        if (tape_open) {
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

        // Format selector (PAL/NTSC/SECAM dropdown)
        bool tape_fmt_open = decoder->overlay.tape_format_dropdown_open;
        Rectangle tape_format_btn_rect = decoder->overlay.tape_format_btn_rect;
        Color tape_fmt_bg = tape_fmt_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
        DrawRectangleRounded(tape_format_btn_rect, 0.15f, 4, tape_fmt_bg);

        int tape_fmt_label_w = gui_text_measure(tape_format_label, FONT_SIZE_DROPDOWN_OPT);
        float tape_fmt_total_w = (float)tape_fmt_label_w + tape_arrow_w + 4.0f;
        float tape_fmt_text_x = tape_format_btn_rect.x + (tape_format_btn_rect.width - tape_fmt_total_w) / 2.0f;
        float tape_fmt_text_y = tape_format_btn_rect.y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
        gui_text_draw(tape_format_label, tape_fmt_text_x, tape_fmt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

        float fmt_arrow_x = tape_fmt_text_x + tape_fmt_label_w + 6.0f;
        float fmt_arrow_cy = tape_format_btn_rect.y + btn_h / 2.0f;
        if (tape_fmt_open) {
            Vector2 top = { fmt_arrow_x + arrow_size/2, fmt_arrow_cy - arrow_size/2 };
            Vector2 left = { fmt_arrow_x, fmt_arrow_cy + arrow_size/2 };
            Vector2 right = { fmt_arrow_x + arrow_size, fmt_arrow_cy + arrow_size/2 };
            DrawTriangle(top, left, right, COLOR_TEXT);
        } else {
            Vector2 bottom = { fmt_arrow_x + arrow_size/2, fmt_arrow_cy + arrow_size/2 };
            Vector2 left = { fmt_arrow_x, fmt_arrow_cy - arrow_size/2 };
            Vector2 right = { fmt_arrow_x + arrow_size, fmt_arrow_cy - arrow_size/2 };
            DrawTriangle(bottom, right, left, COLOR_TEXT);
        }

        // Tape speed mode button (SP/LP/ELP cycle)
        DrawRectangleRounded(decoder->overlay.tape_mode_btn_rect, 0.15f, 4, COLOR_BUTTON);
        float tape_mode_text_x = tape_mode_btn_x + (tape_mode_btn_w - tape_mode_text_w) / 2.0f;
        float tape_mode_text_y = tape_mode_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
        gui_text_draw(tape_mode_label, tape_mode_text_x, tape_mode_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    }

    // OSD mode button (cycles Off/Minimal/Stats)
    Color osd_bg = COLOR_BUTTON;
    DrawRectangleRounded(decoder->overlay.osd_btn_rect, 0.15f, 4, osd_bg);
    float osd_text_x = osd_btn_x + (osd_btn_w - osd_text_w) / 2.0f;
    float osd_text_y = osd_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
    gui_text_draw(osd_label, osd_text_x, osd_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Frame mode button (Active/Full)
    Color frame_bg = COLOR_BUTTON;
    DrawRectangleRounded(decoder->overlay.frame_btn_rect, 0.15f, 4, frame_bg);
    float frame_text_x = frame_btn_x + (frame_btn_w - frame_text_w) / 2.0f;
    float frame_text_y = frame_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
    gui_text_draw(frame_label, frame_text_x, frame_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Levels mode button (Auto/Fixed)
    Color level_bg = COLOR_BUTTON;
    DrawRectangleRounded(decoder->overlay.level_btn_rect, 0.15f, 4, level_bg);
    float level_text_x = level_btn_x + (level_btn_w - level_text_w) / 2.0f;
    float level_text_y = level_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
    gui_text_draw(level_label, level_text_x, level_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Sync mode button (Auto/Fixed)
    Color sync_bg = COLOR_BUTTON;
    DrawRectangleRounded(decoder->overlay.sync_btn_rect, 0.15f, 4, sync_bg);
    float sync_text_x = sync_btn_x + (sync_btn_w - sync_text_w) / 2.0f;
    float sync_text_y = sync_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
    gui_text_draw(sync_label, sync_text_x, sync_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Colour mode button (Auto/Off): burst-gated baseband chroma vs legacy
    // grayscale. Same chrome-gating as every other control.
    DrawRectangleRounded(decoder->overlay.colour_btn_rect, 0.15f, 4, COLOR_BUTTON);
    float colour_text_x = colour_btn_x + (colour_btn_w - colour_text_w) / 2.0f;
    float colour_text_y = colour_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
    gui_text_draw(colour_label, colour_text_x, colour_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // System selector button with dropdown arrow
    bool is_open = decoder->overlay.system_dropdown_open;
    Color sys_bg = is_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    Rectangle sys_btn_rect = decoder->overlay.system_btn_rect;
    DrawRectangleRounded(sys_btn_rect, 0.15f, 4, sys_bg);

    int sys_label_w = gui_text_measure(sys_name, FONT_SIZE_DROPDOWN_OPT);
    float arrow_w = 8.0f;
    float total_w = (float)sys_label_w + arrow_w + 4.0f;
    float sys_text_x = sys_btn_rect.x + (sys_btn_rect.width - total_w) / 2.0f;
    float sys_text_y = sys_btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
    gui_text_draw(sys_name, sys_text_x, sys_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Arrow
    float arrow_size = 5.0f;
    float arrow_x = sys_text_x + sys_label_w + 6.0f;
    float arrow_cy = sys_btn_y + btn_h / 2.0f;
    if (is_open) {
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

    // Draw dropdown options for system if open (same 0/1/2 value space as
    // gui_cvbs_set_format, so an option click maps directly)
    if (is_open) {
        float opt_y = sys_btn_y + btn_h;
        const char *options[] = {"PAL", "NTSC", "SECAM"};
        int sys_values[] = {0, 1, 2};
        float opt_h = 20.0f;

        // Background for dropdown container
        DrawRectangleRounded((Rectangle){sys_btn_rect.x, opt_y, sys_btn_rect.width, opt_h * 3.0f},
                             0.1f, 4, COLOR_PANEL_BG);

        for (int i = 0; i < 3; i++) {
            Rectangle opt_rect = {sys_btn_rect.x, opt_y + i * opt_h, sys_btn_rect.width, opt_h};
            decoder->overlay.system_options_rect[i] = opt_rect;

            bool is_selected = (sys == sys_values[i]);
            Vector2 mouse = gui_ui_get_mouse_position();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            Color opt_bg = gui_dropdown_option_color(is_selected, hover);
            DrawRectangleRec(opt_rect, opt_bg);

            int opt_text_w = gui_text_measure(options[i], FONT_SIZE_DROPDOWN_OPT);
            float opt_text_x = sys_btn_rect.x + sys_btn_rect.width/2.0f - opt_text_w/2.0f;
            float opt_text_y = opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
            gui_text_draw(options[i], opt_text_x, opt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }

    // Tape family dropdown options
    if (show_tape_controls && decoder->overlay.tape_dropdown_open) {
        Rectangle tape_btn_rect = decoder->overlay.tape_btn_rect;
        float opt_y = tape_btn_rect.y + btn_h;
        float opt_h = 20.0f;
        const char *options[] = {"VHS", "S-VHS", "Betamax", "Video8", "Hi8", "U-matic"};
        cvbs_tape_format_t values[] = {
            CVBS_TAPE_FORMAT_VHS,
            CVBS_TAPE_FORMAT_SVHS,
            CVBS_TAPE_FORMAT_BETAMAX,
            CVBS_TAPE_FORMAT_VIDEO8,
            CVBS_TAPE_FORMAT_HI8,
            CVBS_TAPE_FORMAT_UMATIC
        };
        DrawRectangleRounded((Rectangle){tape_btn_rect.x, opt_y, tape_btn_rect.width, opt_h * 6.0f},
                             0.1f, 4, COLOR_PANEL_BG);

        for (int i = 0; i < 6; i++) {
            Rectangle opt_rect = {tape_btn_rect.x, opt_y + i * opt_h, tape_btn_rect.width, opt_h};
            decoder->overlay.tape_options_rect[i] = opt_rect;

            bool is_selected = (decoder->tape_format == values[i]);
            Vector2 mouse = gui_ui_get_mouse_position();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            Color opt_bg = gui_dropdown_option_color(is_selected, hover);
            DrawRectangleRec(opt_rect, opt_bg);

            int opt_text_w = gui_text_measure(options[i], FONT_SIZE_DROPDOWN_OPT);
            float opt_text_x = tape_btn_rect.x + tape_btn_rect.width/2.0f - opt_text_w/2.0f;
            float opt_text_y = opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
            gui_text_draw(options[i], opt_text_x, opt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }

    // Tape format dropdown options (PAL/NTSC/SECAM)
    if (show_tape_controls && decoder->overlay.tape_format_dropdown_open) {
        Rectangle fmt_btn_rect = decoder->overlay.tape_format_btn_rect;
        float opt_y = fmt_btn_rect.y + btn_h;
        float opt_h = 20.0f;
        const char *options[] = {"PAL", "NTSC", "SECAM"};

        DrawRectangleRounded((Rectangle){fmt_btn_rect.x, opt_y, fmt_btn_rect.width, opt_h * 3.0f},
                             0.1f, 4, COLOR_PANEL_BG);

        for (int i = 0; i < 3; i++) {
            Rectangle opt_rect = {fmt_btn_rect.x, opt_y + i * opt_h, fmt_btn_rect.width, opt_h};
            decoder->overlay.tape_format_options_rect[i] = opt_rect;

            bool is_selected = (tape_fmt == i);
            Vector2 mouse = gui_ui_get_mouse_position();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            Color opt_bg = gui_dropdown_option_color(is_selected, hover);
            DrawRectangleRec(opt_rect, opt_bg);

            int opt_text_w = gui_text_measure(options[i], FONT_SIZE_DROPDOWN_OPT);
            float opt_text_x = fmt_btn_rect.x + fmt_btn_rect.width/2.0f - opt_text_w/2.0f;
            float opt_text_y = opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2.0f;
            gui_text_draw(options[i], opt_text_x, opt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
}

//=============================================================================
// Panel Interface (vtable) Implementation
//=============================================================================
typedef enum {
    VIDEO_PLUGIN_CVBS = 0,
    VIDEO_PLUGIN_TAPE_DECODE = 1,
    VIDEO_PLUGIN_VHS_FM = 2
} video_plugin_kind_t;

typedef struct {
    cvbs_decoder_t decoder;
    // Fixed-rate decode input resampler (see cvbs_panel_target_rate and
    // video_cvbs_plugin_process): streaming soxr state + scratch.
    soxr_t resample;
    uint32_t resample_in_rate;
    uint32_t resample_out_rate;
    bool resample_failed;
    int16_t *resample_scratch;
    size_t resample_scratch_cap;
    // FLOAT staging for the resampler output: soxr's INT16 output path
    // is NONDETERMINISTIC (its dither differs run to run on identical
    // input - measured on the MSYS2 build 2026-10-21, per-chunk checksums:
    // the decode outcome flipped 0 vs 282 accepted fields between runs),
    // so the resampler outputs FLOAT32 and the int16 conversion happens
    // here, deterministically (cvbs_plugin_resample_block).
    float *resample_f32;
    size_t resample_f32_cap;
} video_cvbs_plugin_t;

typedef struct {
    cvbs_decoder_t decoder;
    char active_profile[32];
    float active_frequency_mhz;
} video_tape_plugin_t;
typedef struct {
    vhs_fm_decoder_t decoder;
} video_vhs_fm_plugin_t;

typedef struct {
    video_cvbs_plugin_t cvbs;
    video_tape_plugin_t tape_decode;
    video_vhs_fm_plugin_t vhs_fm;
    atomic_int active_plugin;
    // Panel-wide chrome-hide shared by all three decoder modes (moved from
    // the VHS FM overlay): one flag + one double-click tracker, stored on
    // the panel itself so the hidden state survives decoder-mode cycling.
    struct {
        bool controls_hidden;      // Double-click toggles ALL overlay chrome.
        uint64_t last_click_ms;   // Double-click detection (ms + position).
        float last_click_x, last_click_y;
    } chrome;
} video_panel_state_t;

static const char *video_plugin_name(video_plugin_kind_t plugin) {
    switch (plugin) {
        case VIDEO_PLUGIN_TAPE_DECODE: return "Tape Decode";
        case VIDEO_PLUGIN_VHS_FM:      return "RF Preview";
        case VIDEO_PLUGIN_CVBS:
        default:                       return "CVBS";
    }
}

// Tape Decode is hidden from the GUI until it works: set this to 1 to
// re-expose it in the Decoder cycle. All plugin code stays compiled (the
// vtable process/render paths and the mode's state remain); this is the
// single UI entry point - the Decoder button cycle - so gating it here
// makes the mode unreachable while keeping every line of the decoder.
#define VIDEO_TAPE_DECODE_UI_ENABLED 0

static inline video_plugin_kind_t video_panel_next_plugin(video_plugin_kind_t plugin) {
#if VIDEO_TAPE_DECODE_UI_ENABLED
    switch (plugin) {
        case VIDEO_PLUGIN_CVBS:        return VIDEO_PLUGIN_TAPE_DECODE;
        case VIDEO_PLUGIN_TAPE_DECODE: return VIDEO_PLUGIN_VHS_FM;
        case VIDEO_PLUGIN_VHS_FM:
        default:                       return VIDEO_PLUGIN_CVBS;
    }
#else
    switch (plugin) {
        case VIDEO_PLUGIN_CVBS:        return VIDEO_PLUGIN_VHS_FM;
        case VIDEO_PLUGIN_VHS_FM:
        default:                       return VIDEO_PLUGIN_CVBS;
    }
#endif
}

static inline cvbs_decoder_t *video_panel_decoder(video_panel_state_t *panel, video_plugin_kind_t plugin) {
    if (!panel) return NULL;
    if (plugin == VIDEO_PLUGIN_TAPE_DECODE) return &panel->tape_decode.decoder;
    if (plugin == VIDEO_PLUGIN_CVBS) return &panel->cvbs.decoder;
    return NULL;
}

static inline cvbs_decoder_t *video_panel_active_decoder(video_panel_state_t *panel) {
    if (!panel) return NULL;
    return video_panel_decoder(panel, panel->active_plugin);
}

// Persisted default system (0=PAL, 1=NTSC, 2=SECAM): misrc_gui pushes the
// loaded app setting here once at startup (gui_cvbs_set_default_system)
// and every panel decoder is created in it. The System dropdown's click
// handler writes the user's pick back to app->settings.video_system so
// the exit-time settings save persists it across launches (user request,
// real-GUI 2026-10-20: "the system config should be persistent based off
// what the user sets not just PAL always").
static int s_cvbs_default_system = 0;

void gui_cvbs_set_default_system(int system_select) {
    if (system_select >= 0 && system_select <= 2) {
        s_cvbs_default_system = system_select;
    }
}

static bool video_panel_init_decoder_common(cvbs_decoder_t *decoder,
                                            cvbs_decoder_mode_t mode,
                                            const char *trace_source) {
    if (!decoder) return false;
    if (!gui_cvbs_init(decoder)) return false;

    decoder->osd_mode = CVBS_OSD_MINIMAL;
    decoder->frame_view_mode = CVBS_FRAME_VIEW_ACTIVE;
    decoder->decoder_mode = mode;
    decoder->tape_format = CVBS_TAPE_FORMAT_VHS;
    decoder->tape_mode = CVBS_TAPE_MODE_SP;
    decoder->level_mode = CVBS_LEVEL_MODE_AUTO;
    decoder->sync_mode = CVBS_SYNC_MODE_FIXED;
    decoder->fixed_levels_valid = false;
    cvbs_reset_fixed_sync_lock(decoder);

    // Start in the USER'S persisted system pick, not hardcoded PAL (the
    // tape plugin's apply_runtime_args overrides its own decoder right
    // after this, so this only governs the CVBS-mode decoders).
    decoder->overlay.selected_system = s_cvbs_default_system;
    decoder->overlay.selected_tape_format = 0;  // 0=PAL, 1=NTSC, 2=SECAM
    decoder->overlay.system_dropdown_open = false;
    decoder->overlay.tape_dropdown_open = false;
    decoder->overlay.tape_format_dropdown_open = false;
    gui_cvbs_set_format(decoder, s_cvbs_default_system);
    cvbs_trace_mode_watch(decoder, trace_source);
    return true;
}

static bool video_cvbs_plugin_init(video_cvbs_plugin_t *plugin) {
    if (!plugin) return false;
    return video_panel_init_decoder_common(&plugin->decoder, CVBS_DECODER_MODE_CVBS, "create_plugin_cvbs");
}

static bool video_vhs_fm_plugin_init(video_vhs_fm_plugin_t *plugin) {
    if (!plugin) return false;
    if (!gui_vhs_fm_init(&plugin->decoder)) return false;
    gui_vhs_fm_set_format(&plugin->decoder, 0);
    plugin->decoder.overlay.is_visible = false;
    plugin->decoder.overlay.dropdown_open = false;
    return true;
}

static const char *video_tape_profile_from_selection(const video_tape_plugin_t *plugin) {
    if (!plugin) return "PAL_VHS";
    const cvbs_decoder_t *decoder = &plugin->decoder;
    int system = decoder->overlay.selected_tape_format;  // 0=PAL 1=NTSC 2=SECAM
    cvbs_tape_format_t family = decoder->tape_format;
    cvbs_tape_mode_t mode = decoder->tape_mode;

    if (system == 1) { // NTSC
        switch (family) {
            case CVBS_TAPE_FORMAT_HI8:
                if (mode == CVBS_TAPE_MODE_LP) return "NTSC_HI8_LP";
                if (mode == CVBS_TAPE_MODE_ELP) return "NTSC_HI8_EP";
                return "NTSC_HI8";
            case CVBS_TAPE_FORMAT_VIDEO8:
                return "NTSC_VIDEO8";
            case CVBS_TAPE_FORMAT_SVHS:
                return "NTSC_SVHS";
            case CVBS_TAPE_FORMAT_BETAMAX:
                return "NTSC_BETAMAX";
            case CVBS_TAPE_FORMAT_UMATIC:
                return "NTSC_UMATIC";
            case CVBS_TAPE_FORMAT_VHS:
            default:
                if (mode == CVBS_TAPE_MODE_LP) return "NTSC_VHS_LP";
                if (mode == CVBS_TAPE_MODE_ELP) return "NTSC_VHS_EP";
                return "NTSC_VHS";
        }
    }

    if (system == 2) { // SECAM / MESECAM fallbacks
        if (family == CVBS_TAPE_FORMAT_VHS) {
            if (mode == CVBS_TAPE_MODE_LP) return "MESECAM_VHS_LP";
            if (mode == CVBS_TAPE_MODE_ELP) return "MESECAM_VHS_EP";
            return "MESECAM_VHS";
        }
        return "MESECAM_VHS";
    }

    // PAL
    switch (family) {
        case CVBS_TAPE_FORMAT_HI8:
            if (mode == CVBS_TAPE_MODE_LP) return "PAL_HI8_LP";
            if (mode == CVBS_TAPE_MODE_ELP) return "PAL_HI8_EP";
            return "PAL_HI8";
        case CVBS_TAPE_FORMAT_VIDEO8:
            return "PAL_VIDEO8";
        case CVBS_TAPE_FORMAT_SVHS:
            return "PAL_SVHS";
        case CVBS_TAPE_FORMAT_BETAMAX:
            return "PAL_BETAMAX";
        case CVBS_TAPE_FORMAT_UMATIC:
            return "PAL_UMATIC";
        case CVBS_TAPE_FORMAT_VHS:
        default:
            if (mode == CVBS_TAPE_MODE_LP) return "PAL_VHS_LP";
            if (mode == CVBS_TAPE_MODE_ELP) return "PAL_VHS_EP";
            return "PAL_VHS";
    }
}

static void video_tape_apply_runtime_args(video_tape_plugin_t *plugin, const char *source) {
    if (!plugin) return;
    cvbs_decoder_t *decoder = &plugin->decoder;
    int selected_format = decoder->overlay.selected_tape_format;
    if (selected_format < 0 || selected_format > 2) selected_format = 0;
    decoder->overlay.selected_tape_format = selected_format;

    // The system selector shares the tape format's 0=PAL/1=NTSC/2=SECAM
    // value space (the Tape-mode System dropdown syncs selected_tape_format
    // directly), so one mapping keeps both selectors and the decoder format
    // consistent - including SECAM, which the old 2-way system space
    // collapsed onto PAL.
    decoder->overlay.selected_system = selected_format;
    gui_cvbs_set_format(decoder, selected_format);

    const char *profile = video_tape_profile_from_selection(plugin);
    snprintf(plugin->active_profile, sizeof(plugin->active_profile), "%s", profile);
    plugin->active_frequency_mhz = 40.0f;
    bool ire0_adjust = (decoder->level_mode == CVBS_LEVEL_MODE_FIXED);
    const char *arg_ire0_adjust = ire0_adjust ? "--ire0-adjust" : "(disabled)";
    const char *system_name = (selected_format == 1) ? "NTSC"
                             : (selected_format == 2) ? "SECAM"
                             : "PAL";

    TraceLog(LOG_INFO,
             "TAPE DECODE ARGS: source=%s system=%s profile=%s frequency=%.1f input_format=s16le ire0_adjust=%d arg=%s",
             (source && source[0]) ? source : "unknown",
             system_name,
             plugin->active_profile,
             (double)plugin->active_frequency_mhz,
             ire0_adjust ? 1 : 0,
             arg_ire0_adjust);
}

static bool video_tape_plugin_init(video_tape_plugin_t *plugin) {
    if (!plugin) return false;
    if (!video_panel_init_decoder_common(&plugin->decoder, CVBS_DECODER_MODE_TAPE, "create_plugin_tape_decode")) {
        return false;
    }
    plugin->active_profile[0] = '\0';
    plugin->active_frequency_mhz = 40.0f;
    video_tape_apply_runtime_args(plugin, "init");
    return true;
}

static void video_panel_close_all_dropdowns(video_panel_state_t *panel) {
    if (!panel) return;
    cvbs_decoder_t *cvbs = &panel->cvbs.decoder;
    cvbs_decoder_t *tape = &panel->tape_decode.decoder;
    vhs_fm_decoder_t *vhs = &panel->vhs_fm.decoder;
    cvbs->overlay.system_dropdown_open = false;
    cvbs->overlay.tape_dropdown_open = false;
    cvbs->overlay.tape_format_dropdown_open = false;
    tape->overlay.system_dropdown_open = false;
    tape->overlay.tape_dropdown_open = false;
    tape->overlay.tape_format_dropdown_open = false;
    vhs->overlay.dropdown_open = false;
    vhs->overlay.is_visible = false;
}

// Shared double-click chrome toggle (350 ms / 6 px, as first shipped in the
// VHS FM overlay, now panel-wide): every non-control click feeds this
// tracker; completing a double-click flips controls_hidden for ALL three
// decoder modes and closes any open dropdown so no stale options survive.
static bool video_panel_chrome_toggle_on_click(video_panel_state_t *panel, Vector2 mouse_pos) {
    if (!panel) return false;
    uint64_t now_ms = get_time_ms();
    bool double_click =
        panel->chrome.last_click_ms != 0 &&
        now_ms >= panel->chrome.last_click_ms &&
        now_ms - panel->chrome.last_click_ms <= 350 &&
        fabsf(mouse_pos.x - panel->chrome.last_click_x) <= 6.0f &&
        fabsf(mouse_pos.y - panel->chrome.last_click_y) <= 6.0f;
    panel->chrome.last_click_ms = now_ms;
    panel->chrome.last_click_x = mouse_pos.x;
    panel->chrome.last_click_y = mouse_pos.y;
    if (!double_click) return false;
    panel->chrome.controls_hidden = !panel->chrome.controls_hidden;
    video_panel_close_all_dropdowns(panel);
    panel->chrome.last_click_ms = 0;   // require two fresh clicks to toggle back
    return true;
}

static void video_detach_rf(vhs_fm_decoder_t *decoder);

static void video_panel_activate_plugin(video_panel_state_t *panel, video_plugin_kind_t plugin) {
    if (!panel) return;
    if (atomic_load(&panel->active_plugin) == (int)plugin) return;
    video_plugin_kind_t old_plugin = atomic_load(&panel->active_plugin);
    if (old_plugin == VIDEO_PLUGIN_VHS_FM) video_detach_rf(&panel->vhs_fm.decoder);
    video_panel_close_all_dropdowns(panel);
    gui_vhs_fm_reset(&panel->vhs_fm.decoder);
    panel->active_plugin = plugin;
    TraceLog(LOG_INFO,
             "VIDEO DECODER SWITCH: old=%s new=%s",
             video_plugin_name(old_plugin),
             video_plugin_name(panel->active_plugin));
}

//-----------------------------------------------------------------------------
// Lifecycle Functions
//-----------------------------------------------------------------------------

// UI reference counts own pooled worker lifetime. The producer only loads
// published workers under reader registration and never waits for the UI.
static _Atomic(gui_rf_preview_worker_t *) s_rf_routes[2][GUI_RF_PROFILE_COUNT];
static unsigned s_rf_references[2][GUI_RF_PROFILE_COUNT];
static vhs_fm_decoder_t *s_rf_bindings[4];
static gui_rf_preview_worker_t *s_rf_audio_source;
static void video_rf_audio(void *context, const float *stereo, size_t frames, uint64_t end_sample) {
    (void)end_sample; // Audio and video consume the same continuous RF worker.
    if (stereo && frames) gui_audio_push_hifi(context, stereo, frames);
    else gui_audio_reset_hifi(context);
}
static atomic_uint s_rf_route_readers;
static atomic_bool s_rf_pressure_paused;
static bool s_rf_input_supported = true; // Extraction producer owns this.
static gui_rf_preview_pressure_t s_rf_pressure;
static void video_detach_rf(vhs_fm_decoder_t *decoder) {
    if (!decoder || !decoder->worker) return;
    int ch=decoder->bound_channel, sys=decoder->bound_profile;
    if (ch>=0 && ch<2 && sys>=0 && sys<GUI_RF_PROFILE_COUNT && s_rf_references[ch][sys]) {
        if (--s_rf_references[ch][sys]==0) {
            gui_rf_preview_worker_t *worker=atomic_exchange(&s_rf_routes[ch][sys],NULL);
            if (s_rf_audio_source == worker) {
                gui_rf_preview_worker_set_audio_enabled(worker,false);
                gui_audio_set_hifi_source(NULL);
                s_rf_audio_source = NULL;
            }
            while (atomic_load(&s_rf_route_readers)) thrd_sleep_ms(1);
            gui_rf_preview_worker_destroy(worker);
        }
    }
    for (int i=0;i<4;++i) if (s_rf_bindings[i]==decoder) s_rf_bindings[i]=NULL;
    gui_vhs_fm_bind_worker(decoder,NULL);
    decoder->bound_channel=-1;
}
static vhs_fm_decoder_t *video_config_rf(panel_view_type_t view, void *state) {
    if (view!=PANEL_VIEW_CVBS || !state) return NULL;
    video_panel_state_t *panel=state;
    return atomic_load(&panel->active_plugin)==VIDEO_PLUGIN_VHS_FM ? &panel->vhs_fm.decoder : NULL;
}
// Shared CPU budget values (percent of one logical core). Above one core,
// bounded DSP lanes may run concurrently within the aggregate allowance.
static int vhs_budget_clamp(int percent) {
    switch (percent) {
        case 50: case 75: case 100: case 150: case 200: case 300:
            return percent;
        default:
            return 100;
    }
}
static int vhs_budget_next(int percent) {
    if (percent < 50) return 50;
    if (percent < 75) return 75;
    if (percent < 100) return 100;
    if (percent < 150) return 150;
    if (percent < 200) return 200;
    if (percent < 300) return 300;
    return 50;
}
void gui_video_update_rf_routes(gui_app_t *app) {
    if (!app) return;
    int budget=vhs_budget_clamp(app->settings.rf_preview_budget_percent);
    static int applied_budget;
    if (budget!=applied_budget) {
        gui_rf_preview_worker_set_budget(budget/100.0);
        applied_budget=budget;
    }
    channel_panel_config_t *configs[2]={&app->panel_config_a,&app->panel_config_b};
    vhs_fm_decoder_t *desired[4]={0};
    for (int ch=0;ch<2;++ch) {
        desired[ch*2]=video_config_rf(configs[ch]->left_view,configs[ch]->left_state);
        desired[ch*2+1]=configs[ch]->split ? video_config_rf(configs[ch]->right_view,configs[ch]->right_state) : NULL;
    }
    for (int i=0;i<4;++i) {
        vhs_fm_decoder_t *old=s_rf_bindings[i];
        if (!old) continue;
        bool present=false;
        for (int j=0;j<4;++j) if (desired[j]==old) present=true;
        if (!present) video_detach_rf(old);
    }
    memset(s_rf_bindings,0,sizeof(s_rf_bindings));
    for (int i=0;i<4;++i) {
        vhs_fm_decoder_t *decoder=desired[i];
        if (!decoder) continue;
        int ch=i/2, sys=(int)gui_vhs_fm_get_profile(decoder);
        if (decoder->worker && (decoder->bound_channel!=ch || decoder->bound_profile!=sys))
            video_detach_rf(decoder);
        if (!decoder->worker) {
            gui_rf_preview_worker_t *worker=atomic_load(&s_rf_routes[ch][sys]);
            bool created=worker==NULL;
            if (created) {
                worker=gui_rf_preview_worker_create_profile((gui_rf_profile_t)sys);
                if (worker) {
                    gui_rf_preview_worker_set_audio_callback(worker, video_rf_audio, worker);
                    gui_rf_preview_worker_set_audio_enabled(worker,false);
                }
            }
            if (!worker) continue;
            if (!gui_vhs_fm_bind_worker(decoder,worker)) {
                if (created) gui_rf_preview_worker_destroy(worker);
                continue;
            }
            decoder->bound_channel=ch; decoder->bound_profile=sys;
            decoder->bound_system=gui_rf_profile_format((gui_rf_profile_t)sys)==GUI_RF_NTSC;
            s_rf_references[ch][sys]++;
            gui_rf_preview_worker_set_paused(worker,atomic_load(&s_rf_pressure_paused));
            if (created) atomic_store(&s_rf_routes[ch][sys],worker);
        }
        s_rf_bindings[i]=decoder;
    }
    // One monitor source: first visible Hi-Fi-capable panel (A before B).
    gui_rf_preview_worker_t *audio_source = NULL;
    for (int i=0; i<4; ++i) if (desired[i] && desired[i]->worker &&
        gui_rf_profile_has_hifi(gui_vhs_fm_get_profile(desired[i]))) {
        audio_source=desired[i]->worker;
        break;
    }
    if (audio_source != s_rf_audio_source) {
        if (s_rf_audio_source) gui_rf_preview_worker_set_audio_enabled(s_rf_audio_source,false);
        gui_audio_set_hifi_source(audio_source);
        s_rf_audio_source=audio_source;
    }
    // Decode audio only for the selected, audible live source. Keep the Hi-Fi
    // video profile when muted; its FM carriers and RF filtering still apply.
    bool audible=app->is_capturing && app->settings.audio_monitor_playback;
    for (int ch=0;ch<2;++ch) for (int profile=0;profile<GUI_RF_PROFILE_COUNT;++profile) {
        if (!gui_rf_profile_has_hifi((gui_rf_profile_t)profile)) continue;
        gui_rf_preview_worker_t *worker=atomic_load(&s_rf_routes[ch][profile]);
        if (worker) gui_rf_preview_worker_set_audio_enabled(worker,audible && worker==audio_source);
    }
}
static unsigned video_buffer_percent(gui_app_t *app, buffer_id_t id) {
    if (!app->buffers.initialized[id] || !app->buffers.buffers[id].buffer_size) return 0;
    uint64_t read=atomic_load(&app->buffers.stats[id].bytes_read);
    uint64_t written=atomic_load(&app->buffers.stats[id].bytes_written);
    return gui_rf_preview_pressure_fill_percent(written,read,app->buffers.buffers[id].buffer_size);
}
void gui_video_submit_rf(gui_app_t *app, const int16_t *samples_a, const int16_t *samples_b,
                          size_t count_a, size_t count_b, const gui_rf_block_info_t info[2],
                          bool input_supported) {
    if (!app || !info) return;
    unsigned raw=video_buffer_percent(app,BUF_CAPTURE_RF), record=0;
    // Keep the drop snapshot independent of recording toggles: re-enabling a
    // recording must not count its previous session history as a new drop.
    uint64_t drops=atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].write_drops) +
                   atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops) +
                   atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops);
    if (app->is_recording) {
        for (int ch=0;ch<2;++ch) {
            if (!(ch ? app->settings.capture_b : app->settings.capture_a)) continue;
            buffer_id_t id=ch ? BUF_RECORD_B : BUF_RECORD_A;
            unsigned fill=video_buffer_percent(app,id);
            if (fill>record) record=fill;
        }
    }
    bool paused=gui_rf_preview_pressure_update(&s_rf_pressure,get_time_ms(),raw,record,drops,app->is_capturing);
    atomic_store(&s_rf_pressure_paused,paused);
    const int16_t *samples[2]={samples_a,samples_b};
    size_t counts[2]={count_a,count_b};
    bool support_changed=s_rf_input_supported!=input_supported;
    s_rf_input_supported=input_supported;
    atomic_fetch_add(&s_rf_route_readers,1);
    for (int ch=0;ch<2;++ch) for (int sys=0;sys<GUI_RF_PROFILE_COUNT;++sys) {
        gui_rf_preview_worker_t *worker=atomic_load(&s_rf_routes[ch][sys]);
        if (!worker) continue;
        gui_rf_preview_worker_set_paused(worker,paused);
        if (support_changed || (info[ch].discontinuity && !counts[ch])) gui_rf_preview_worker_reset(worker);
        if (input_supported && samples[ch] && counts[ch])
            gui_rf_preview_worker_submit(worker,samples[ch],counts[ch],&info[ch]);
    }
    atomic_fetch_sub(&s_rf_route_readers,1);
}
static void *cvbs_vtable_create(void) {
    video_panel_state_t *panel = calloc(1, sizeof(video_panel_state_t));
    if (!panel) return NULL;

    if (!video_cvbs_plugin_init(&panel->cvbs)) {
        free(panel);
        return NULL;
    }
    if (!video_tape_plugin_init(&panel->tape_decode)) {
        gui_cvbs_cleanup(&panel->cvbs.decoder);
        free(panel);
        return NULL;
    }
    if (!video_vhs_fm_plugin_init(&panel->vhs_fm)) {
        gui_cvbs_cleanup(&panel->tape_decode.decoder);
        gui_cvbs_cleanup(&panel->cvbs.decoder);
        free(panel);
        return NULL;
    }

    atomic_init(&panel->active_plugin, VIDEO_PLUGIN_CVBS);
    return panel;
}

static void cvbs_vtable_destroy(void *state) {
    if (!state) return;
    video_panel_state_t *panel = (video_panel_state_t *)state;
    video_detach_rf(&panel->vhs_fm.decoder);
#if LIBSOXR_ENABLED
    if (panel->cvbs.resample) {
        soxr_delete(panel->cvbs.resample);
        panel->cvbs.resample = NULL;
    }
    free(panel->cvbs.resample_scratch);
    panel->cvbs.resample_scratch = NULL;
    panel->cvbs.resample_scratch_cap = 0;
    free(panel->cvbs.resample_f32);
    panel->cvbs.resample_f32 = NULL;
    panel->cvbs.resample_f32_cap = 0;
#endif
    gui_cvbs_cleanup(&panel->cvbs.decoder);
    gui_cvbs_cleanup(&panel->tape_decode.decoder);
    gui_vhs_fm_cleanup(&panel->vhs_fm.decoder);
    free(panel);
}

static void cvbs_vtable_clear(void *state) {
    if (!state) return;
    video_panel_state_t *panel = (video_panel_state_t *)state;
    gui_cvbs_reset(&panel->cvbs.decoder);
    gui_cvbs_reset(&panel->tape_decode.decoder);
    gui_vhs_fm_reset(&panel->vhs_fm.decoder);
    video_tape_apply_runtime_args(&panel->tape_decode, "clear");
    video_panel_close_all_dropdowns(panel);
}

//-----------------------------------------------------------------------------
// Processing Function (called from display thread)
//-----------------------------------------------------------------------------

// Fixed decode-rate targets (the bandwidth spec): just above 4x fsc, the
// classic composite-decoder sample rate. Decoding at a FIXED target makes
// the decoder's declared rate the ACTUAL rate by construction (no dependence
// on the input path's rate plumbing, which has been measured stuck at the
// legacy 40 MSPS default), halves+ the decode bandwidth vs a 40 MSPS
// native stream, and keeps the display thread ahead of the lossy display
// buffer - drops there tear the decoder's stream and were measured
// destabilizing the sync of a pure reference source.
static uint32_t cvbs_panel_target_rate(const cvbs_decoder_t *decoder) {
    return (decoder && decoder->state.format == CVBS_FORMAT_NTSC)
               ? 14300000u : 17900000u;
}

#if LIBSOXR_ENABLED
// (Re)create the streaming resampler when either rate changes (a new input
// rate from the app, or a PAL/NTSC format flip changing the target).
static bool cvbs_plugin_resampler_ensure(video_cvbs_plugin_t *plugin,
                                         uint32_t in_rate, uint32_t out_rate) {
    if (plugin->resample && !plugin->resample_failed &&
        plugin->resample_in_rate == in_rate &&
        plugin->resample_out_rate == out_rate) {
        return true;
    }
    if (plugin->resample) {
        soxr_delete(plugin->resample);
        plugin->resample = NULL;
    }
    plugin->resample_failed = false;
    plugin->resample_in_rate = in_rate;
    plugin->resample_out_rate = out_rate;
    // int16 input keeps the panel path allocation-free; FLOAT32 output
    // (converted to int16 deterministically in resample_block - soxr's
    // INT16 output dithers with a run-varying seed); linear phase (50)
    // preserves the sync edges; passband 95% of the output Nyquist retains
    // the subcarrier (4.43/3.58 MHz) plus luma detail (up to ~8.5 MHz).
    soxr_io_spec_t io = soxr_io_spec(SOXR_INT16_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t quality = soxr_quality_spec(SOXR_HQ, 0);
    quality.passband_end = 0.95;
    quality.stopband_begin = 1.0;
    quality.phase_response = 50.0;
    soxr_runtime_spec_t runtime = soxr_runtime_spec(1);
    soxr_error_t error = NULL;
    plugin->resample = soxr_create(in_rate, out_rate, 1,
                                   &error, &io, &quality, &runtime);
    if (error || !plugin->resample) {
        plugin->resample_failed = true;
        return false;
    }
    return true;
}

static bool cvbs_plugin_resample_grow(video_cvbs_plugin_t *plugin, size_t need) {
    if (plugin->resample_scratch_cap >= need &&
        plugin->resample_f32_cap >= need) return true;
    size_t cap = plugin->resample_scratch_cap ? plugin->resample_scratch_cap : 4096;
    while (cap < need) cap *= 2;
    int16_t *grown = (int16_t *)realloc(plugin->resample_scratch,
                                        cap * sizeof(int16_t));
    float *grown_f = (float *)realloc(plugin->resample_f32,
                                      cap * sizeof(float));
    if (!grown || !grown_f) {
        // realloc leaves the original block (and the plugin pointer)
        // intact on failure; free only what this call allocated.
        free(grown);
        free(grown_f);
        return false;
    }
    plugin->resample_scratch = grown;
    plugin->resample_scratch_cap = cap;
    plugin->resample_f32 = grown_f;
    plugin->resample_f32_cap = cap;
    return true;
}

static bool cvbs_plugin_resample_block(video_cvbs_plugin_t *plugin,
                                       const int16_t *input, size_t input_count,
                                       size_t *out_count) {
    if (!plugin->resample) return false;
    size_t consumed = 0, produced = 0;
    while (consumed < input_count) {
        size_t in_done = 0, out_done = 0;
        soxr_error_t error = soxr_process(plugin->resample,
            (void *)(input + consumed), input_count - consumed, &in_done,
            plugin->resample_f32 + produced,
            plugin->resample_f32_cap - produced, &out_done);
        if (error || (!in_done && !out_done)) {
            plugin->resample_failed = true;
            return false;
        }
        consumed += in_done;
        produced += out_done;
    }
    // Deterministic float -> int16: soxr's INT16 output path dithers with
    // a run-varying seed (measured 2026-10-21), which flipped VBI pulse
    // classifications between runs on identical input. soxr's FLOAT32
    // domain is NORMALIZED +-1.0 (measured: chunk absum 0.16/sample vs
    // 8012 on the int16 path - the first FLOAT32 build rounded the whole
    // stream to 0/+-1 and the decoder saw a dead signal), so the
    // conversion rescales by 32768. lrintf (default round-to-nearest-even)
    // + a range clamp is deterministic.
    for (size_t k = 0; k < produced; k++) {
        long v = lrintf(plugin->resample_f32[k] * 32768.0f);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        plugin->resample_scratch[k] = (int16_t)v;
    }
    *out_count = produced;
    return true;
}
#endif  // LIBSOXR_ENABLED

static void video_cvbs_plugin_process(video_cvbs_plugin_t *plugin,
                                      const int16_t *samples,
                                      size_t count, uint32_t sample_rate) {
    if (!plugin || !samples || count == 0) return;
    cvbs_decoder_t *decoder = &plugin->decoder;
    cvbs_trace_mode_watch(decoder, "process_enter_cvbs");
#if LIBSOXR_ENABLED
    uint32_t in_rate = (sample_rate > 1000000u) ? sample_rate : 40000000u;
    uint32_t out_rate = cvbs_panel_target_rate(decoder);
    bool resampled = false;
    if (in_rate != out_rate &&
        cvbs_plugin_resampler_ensure(plugin, in_rate, out_rate)) {
        // Output bound: the ratio-scaled count plus soxr's delayed-output
        // headroom (its HQ filter buffers whole DFT blocks).
        size_t need = (size_t)((double)count * (double)out_rate /
                               (double)in_rate) + 8192u;
        size_t out_count = 0;
        uint64_t t_rs = get_time_us();
        bool rs_ok = cvbs_plugin_resample_grow(plugin, need) &&
                     cvbs_plugin_resample_block(plugin, samples, count, &out_count);
        uint64_t rs_spent = get_time_us() - t_rs;
        g_prof_resample_us += rs_spent;
        // The OSD per-field "ms" is the WHOLE panel cost against the field
        // budget (PAL 20 ms / NTSC 16.7 ms): the resampler is part of it.
        g_field_decode_us += rs_spent;
        if (rs_ok) {
            // The resampler's OUTPUT rate is exact by construction: mark the
            // declared rate TRUSTED so the decoder's rate self-calibration
            // (the H-spacing median + the PLL refinement, both ~0.01-0.06%
            // error) never overrides it - at this 4x-fsc decode rate that
            // error would slip the carrier reference tens of degrees per
            // line and scramble the hues (see cvbs_measure_hh_spacing).
            decoder->rate_trusted = true;
            gui_cvbs_process_buffer(decoder, plugin->resample_scratch,
                                    out_count, out_rate);
            resampled = true;
        }
    }
    if (!resampled) {
        // Passthrough (already at the target) or resampler failure: the
        // direct feed at the INPUT path's own rate claim (not exact by
        // construction) - clear the trust flag so a stale one from an
        // earlier resample cannot disable the self-calibration here. On
        // failure the decoder's rate self-calibration recovers whatever
        // rate the stream really is.
        decoder->rate_trusted = false;
        gui_cvbs_process_buffer(decoder, samples, count,
                                in_rate == out_rate ? out_rate : sample_rate);
    }
#else
    gui_cvbs_process_buffer(decoder, samples, count, sample_rate);
#endif
    cvbs_trace_mode_watch(decoder, "process_exit_cvbs");
}

static void video_tape_plugin_process(video_tape_plugin_t *plugin,
                                      const int16_t *samples,
                                      size_t count, uint32_t sample_rate) {
    if (!plugin || !samples || count == 0) return;
    cvbs_decoder_t *decoder = &plugin->decoder;
    if (plugin->active_profile[0] == '\0') {
        video_tape_apply_runtime_args(plugin, "process_bootstrap");
    }
    cvbs_trace_mode_watch(decoder, "process_enter_tape_decode");
    gui_cvbs_process_buffer(decoder, samples, count, sample_rate);
    cvbs_trace_mode_watch(decoder, "process_exit_tape_decode");
}

static void cvbs_vtable_process(void *state, const int16_t *samples, size_t count, uint32_t sample_rate) {
    if (!state || !samples || count == 0) return;
    video_panel_state_t *panel = (video_panel_state_t *)state;
    // The LIVE rate flows through to the decoders: every timing window in
    // the baseband profile derives from it (the legacy path discarded it and
    // hardcoded 40 MSPS).
    switch (atomic_load(&panel->active_plugin)) {
        case VIDEO_PLUGIN_CVBS: video_cvbs_plugin_process(&panel->cvbs, samples, count, sample_rate); break;
        case VIDEO_PLUGIN_TAPE_DECODE: video_tape_plugin_process(&panel->tape_decode, samples, count, sample_rate); break;
        default: break; // RF receives continuous input directly from extraction.
    }
}

//-----------------------------------------------------------------------------
// Rendering Functions
//-----------------------------------------------------------------------------

static void render_vhs_decoder_overlay(video_panel_state_t *panel, gui_app_t *app, Rectangle bounds) {
    if (!panel || !app) return;
    vhs_fm_decoder_t *decoder = &panel->vhs_fm.decoder;
    decoder->overlay.is_visible = !panel->chrome.controls_hidden;
    // Double-click on the panel (cvbs_vtable_handle_click) toggles ALL the
    // overlay chrome — buttons + stats text — for a clean video preview.
    if (panel->chrome.controls_hidden) return;

    decoder->overlay.tape_rect = decoder->overlay.beta_preset_rect = (Rectangle){0};
    decoder->overlay.mesecam_rect = (Rectangle){0};
    for (int i = 0; i < 3; ++i) decoder->overlay.stats_opts_rect[i] = (Rectangle){0};
    if (bounds.width <= 0 || bounds.height <= 0) {
        decoder->overlay.is_visible = false;
        return;
    }

    const float btn_h = 18.0f;
    const char *label = "Decoder: RF Preview";
    float text_w = (float)gui_text_measure(label, FONT_SIZE_DROPDOWN_OPT);
    float btn_w = text_w + 16.0f;
    if (btn_w < 128.0f) btn_w = 128.0f;

    float x_right = bounds.x + bounds.width - 8.0f;
    float btn_x = x_right - btn_w;
    float btn_y = bounds.y + 8.0f;
    decoder->overlay.button_rect = (Rectangle){btn_x, btn_y, btn_w, btn_h};

    DrawRectangleRounded(decoder->overlay.button_rect, 0.15f, 4, COLOR_BUTTON_HOVER);
    float text_x = btn_x + (btn_w - text_w) * 0.5f;
    float text_y = btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f;
    gui_text_draw(label, text_x, text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    float control_y = btn_y + btn_h + 6;
    // One compact control per row, ordered from recording format to options.
    const char *tape_label = decoder->overlay.selected_tape == 1 ? "Tape: Betamax" : "Tape: VHS";
    float tape_w = (float)gui_text_measure(tape_label, FONT_SIZE_DROPDOWN_OPT) + 16;
    float tape_y = control_y;
    decoder->overlay.tape_rect = (Rectangle){x_right - tape_w, tape_y, tape_w, btn_h};
    DrawRectangleRounded(decoder->overlay.tape_rect, 0.15f, 4, COLOR_BUTTON_HOVER);
    gui_text_draw(tape_label, decoder->overlay.tape_rect.x + 8,
                  tape_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f,
                  FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    control_y += btn_h + 6;
    const char *format_label = decoder->overlay.selected_tape ? "NTSC (fixed)"
        : decoder->overlay.selected_system == 2 ? "System: SECAM"
        : decoder->overlay.selected_system == 1 ? "System: NTSC" : "System: PAL";
    float fmt_w = (float)gui_text_measure(format_label, FONT_SIZE_DROPDOWN_OPT) + 16;
    decoder->overlay.format_rect = (Rectangle){x_right - fmt_w, control_y, fmt_w, btn_h};
    DrawRectangleRounded(decoder->overlay.format_rect, 0.15f, 4, COLOR_BUTTON_HOVER);
    text_y = control_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f;
    gui_text_draw(format_label, decoder->overlay.format_rect.x + 8, text_y,
                   FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    control_y += btn_h + 6;
    if (!decoder->overlay.selected_tape && decoder->overlay.selected_system == 2) {
        const char *mesecam_label = decoder->overlay.secam_mesecam ? "MESECAM: On" : "MESECAM: Off";
        float mesecam_w = (float)gui_text_measure(mesecam_label, FONT_SIZE_DROPDOWN_OPT) + 16;
        decoder->overlay.mesecam_rect =
            (Rectangle){x_right - mesecam_w, control_y, mesecam_w, btn_h};
        DrawRectangleRounded(decoder->overlay.mesecam_rect, 0.15f, 4,
                             decoder->overlay.secam_mesecam ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
        gui_text_draw(mesecam_label, decoder->overlay.mesecam_rect.x + 8,
                      control_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        control_y += btn_h + 6;
    }
    if (decoder->overlay.selected_tape) {
        char preset_label[40];
        snprintf(preset_label, sizeof(preset_label), "Preset: %s", gui_vhs_fm_get_beta_preset_name(decoder));
        float preset_w = (float)gui_text_measure(preset_label, FONT_SIZE_DROPDOWN_OPT) + 16;
        decoder->overlay.beta_preset_rect =
            (Rectangle){x_right - preset_w, control_y, preset_w, btn_h};
        DrawRectangleRounded(decoder->overlay.beta_preset_rect, 0.15f, 4, COLOR_BUTTON_HOVER);
        gui_text_draw(preset_label, decoder->overlay.beta_preset_rect.x + 8,
                      control_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f,
                      FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        control_y += btn_h + 6;
    }
    // Frame display mode toggle (Active / Full): Full widens the horizontal
    // decode window to the whole line so VBI + sync tip + colour burst render
    // with decoded chroma instead of only the active picture area.
    const char *frame_label = decoder->overlay.frame_mode == 1 ? "Frame: Full" : "Frame: Active";
    float frame_w = (float)gui_text_measure(frame_label, FONT_SIZE_DROPDOWN_OPT) + 24;
    if (frame_w < 118.0f) frame_w = 118.0f;
    decoder->overlay.frame_rect =
        (Rectangle){x_right - frame_w, control_y, frame_w, btn_h};
    DrawRectangleRounded(decoder->overlay.frame_rect, 0.15f, 4, COLOR_BUTTON_HOVER);
    {
        text_y = control_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f;
        float fw = (float)gui_text_measure(frame_label, FONT_SIZE_DROPDOWN_OPT);
        gui_text_draw(frame_label,
                      decoder->overlay.frame_rect.x + (frame_w - fw) * 0.5f,
                      text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    }
    control_y += btn_h + 6;
    int budget=vhs_budget_clamp(app->settings.rf_preview_budget_percent);
    char budget_label[40]; snprintf(budget_label,sizeof(budget_label),"Budget: %.2f core (shared)",budget/100.0);
    float budget_w=(float)gui_text_measure(budget_label,FONT_SIZE_DROPDOWN_OPT)+16;
    decoder->overlay.budget_rect=(Rectangle){x_right-budget_w,control_y,budget_w,btn_h};
    DrawRectangleRounded(decoder->overlay.budget_rect,0.15f,4,COLOR_BUTTON_HOVER);
    gui_text_draw(budget_label,decoder->overlay.budget_rect.x+8,control_y+(btn_h-FONT_SIZE_DROPDOWN_OPT)*.5f,
                  FONT_SIZE_DROPDOWN_OPT,COLOR_TEXT);
    control_y += btn_h + 6;
    // Stats view selector (All / Basic / Off) — a real dropdown so the stats
    // readout is user-selectable. Its popup follows the final control row.
    static const char *stats_names[3] = { "All", "Basic", "Off" };
    int stats_view = decoder->overlay.stats_view;
    if (stats_view < 0 || stats_view > 2) stats_view = 0;
    char stats_label[40];
    snprintf(stats_label, sizeof(stats_label), "Stats: %s", stats_names[stats_view]);
    float stats_w = (float)gui_text_measure(stats_label, FONT_SIZE_DROPDOWN_OPT) + 16;
    if (stats_w < 96.0f) stats_w = 96.0f;
    decoder->overlay.stats_btn_rect =
        (Rectangle){x_right - stats_w, control_y, stats_w, btn_h};
    Color stats_bg = decoder->overlay.stats_dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    DrawRectangleRounded(decoder->overlay.stats_btn_rect, 0.15f, 4, stats_bg);
    // No dropdown arrow: it overlapped the centered label; the open state
    // is already indicated by the button highlight (stats_bg) above.
    {
        text_y = control_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f;
        float sw = (float)gui_text_measure(stats_label, FONT_SIZE_DROPDOWN_OPT);
        gui_text_draw(stats_label,
                      decoder->overlay.stats_btn_rect.x + (stats_w - sw) * 0.5f,
                      text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
    }
    // Keep the popup inside shorter panels; hit-testing gives it priority.
    if (decoder->overlay.stats_dropdown_open) {
        float opt_h = 20.0f;
        float menu_y = decoder->overlay.stats_btn_rect.y + btn_h;
        if (menu_y + 3 * opt_h > bounds.y + bounds.height - 8)
            menu_y = decoder->overlay.stats_btn_rect.y - 3 * opt_h;
        for (int i = 0; i < 3; i++) {
            Rectangle opt = {
                decoder->overlay.stats_btn_rect.x,
                menu_y + (float)i * opt_h,
                decoder->overlay.stats_btn_rect.width,
                opt_h
            };
            decoder->overlay.stats_opts_rect[i] = opt;
            bool selected = (stats_view == i);
            bool hover = CheckCollisionPointRec(gui_ui_get_mouse_position(), opt);
            DrawRectangleRec(opt, gui_dropdown_option_color(selected, hover));
            const char *opt_lbl = stats_names[i];
            float ow = (float)gui_text_measure(opt_lbl, FONT_SIZE_DROPDOWN_OPT);
            gui_text_draw(opt_lbl,
                          opt.x + (opt.width - ow) * 0.5f,
                          opt.y + (opt_h - FONT_SIZE_DROPDOWN_OPT) * 0.5f,
                          FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
}

static void cvbs_vtable_render(void *state, gui_app_t *app, int channel,
                                Rectangle bounds, Color channel_color) {
    (void)app;
    (void)channel;
    (void)channel_color;

    video_panel_state_t *panel = (video_panel_state_t *)state;
    if (!panel) return;

    // Keep all decoders updated while preserving independent state.
    gui_cvbs_swap_buffers(&panel->cvbs.decoder);
    gui_cvbs_swap_buffers(&panel->tape_decode.decoder);
    atomic_store(&panel->vhs_fm.decoder.input_supported,atomic_load(&app->rf_preview_input_supported));
    gui_vhs_fm_swap_buffers(&panel->vhs_fm.decoder);

    if (panel->active_plugin == VIDEO_PLUGIN_VHS_FM) {
        gui_vhs_fm_render_frame(&panel->vhs_fm.decoder, bounds.x, bounds.y,
                                bounds.width, bounds.height, panel->chrome.controls_hidden);
        render_vhs_decoder_overlay(panel, app, bounds);
        return;
    }

    cvbs_decoder_t *decoder = video_panel_active_decoder(panel);
    if (!decoder) {
        const char *msg = "Video Decoder Not Available";
        int w = MeasureText(msg, 12);
        DrawRectangleRec(bounds, (Color){20, 20, 20, 255});
        DrawText(msg, (int)(bounds.x + bounds.width/2 - w/2),
                 (int)(bounds.y + bounds.height/2 - 6), 12, (Color){150, 150, 150, 255});
        return;
    }

    // Chrome-hide (shared panel state): the decoded picture stays, but the
    // OSD text and the control buttons hide until a double-click restores.
    gui_cvbs_render_frame_impl(decoder, bounds.x, bounds.y, bounds.width, bounds.height,
                               !panel->chrome.controls_hidden);
    if (panel->chrome.controls_hidden) {
        // No controls rendered this frame: keep click gating consistent.
        decoder->overlay.is_visible = false;
    } else {
        render_cvbs_system_overlay(decoder, bounds.x, bounds.y, bounds.width);
    }
}

//-----------------------------------------------------------------------------
// Interaction Functions
//-----------------------------------------------------------------------------

static bool cvbs_vtable_handle_click(void *state, gui_app_t *app, int channel,
                                      Vector2 mouse_pos, Rectangle bounds) {
    (void)app;
    (void)channel;
    video_panel_state_t *panel = (video_panel_state_t *)state;
    if (!panel) return false;

    if (panel->active_plugin == VIDEO_PLUGIN_VHS_FM) {
        vhs_fm_decoder_t *vhs = &panel->vhs_fm.decoder;
        if (!CheckCollisionPointRec(mouse_pos, bounds)) {
            // Click outside the panel resets double-click tracking.
            panel->chrome.last_click_ms = 0;
            return false;
        }

        bool chrome_visible = vhs->overlay.is_visible && !panel->chrome.controls_hidden;
        // An open popup owns clicks, including when it opens above the button.
        if (chrome_visible && vhs->overlay.stats_dropdown_open) {
            panel->chrome.last_click_ms = 0;
            for (int i = 0; i < 3; i++) {
                if (CheckCollisionPointRec(mouse_pos, vhs->overlay.stats_opts_rect[i])) {
                    vhs->overlay.stats_view = i;
                    vhs->overlay.stats_dropdown_open = false;
                    return true;
                }
            }
            // Button or outside click closes the dropdown.
            vhs->overlay.stats_dropdown_open = false;
            return true;
        }
        // Control clicks first (only while the controls are shown) so a
        // double-click ON a control never toggles the chrome.
        if (chrome_visible && CheckCollisionPointRec(mouse_pos, vhs->overlay.button_rect)) {
            panel->chrome.last_click_ms = 0;
            video_panel_activate_plugin(panel, video_panel_next_plugin(panel->active_plugin));
            return true;
        }

        if (chrome_visible && CheckCollisionPointRec(mouse_pos,vhs->overlay.tape_rect)) {
            panel->chrome.last_click_ms=0;
            vhs->overlay.selected_tape=(vhs->overlay.selected_tape+1)%2;
            if (vhs->overlay.selected_tape) gui_vhs_fm_set_format(vhs,1);
            return true;
        }
        if (chrome_visible && vhs->overlay.selected_tape &&
            CheckCollisionPointRec(mouse_pos,vhs->overlay.beta_preset_rect)) {
            panel->chrome.last_click_ms=0;
            gui_vhs_fm_set_beta_preset(vhs, (vhs->overlay.beta_preset + 1) % GUI_BETA_PRESET_COUNT);
            return true;
        }
        if (chrome_visible && !vhs->overlay.selected_tape && vhs->overlay.selected_system == 2 &&
            CheckCollisionPointRec(mouse_pos,vhs->overlay.mesecam_rect)) {
            panel->chrome.last_click_ms=0;
            gui_vhs_fm_set_mesecam(vhs, !vhs->overlay.secam_mesecam);
            return true;
        }
        if (chrome_visible && CheckCollisionPointRec(mouse_pos,vhs->overlay.budget_rect)) {
            panel->chrome.last_click_ms = 0;
            app->settings.rf_preview_budget_percent=
                vhs_budget_next(vhs_budget_clamp(app->settings.rf_preview_budget_percent));
            return true;
        }
        if (chrome_visible && CheckCollisionPointRec(mouse_pos, vhs->overlay.format_rect)) {
            panel->chrome.last_click_ms = 0;
            gui_vhs_fm_set_format(vhs, (vhs->overlay.selected_system + 1) % 3);
            return true;
        }
        if (chrome_visible && CheckCollisionPointRec(mouse_pos, vhs->overlay.stats_btn_rect)) {
            panel->chrome.last_click_ms = 0;
            vhs->overlay.stats_dropdown_open = true;
            return true;
        }
        // Frame display mode: cycle Active <-> Full (whole-line decode window
        // for VBI + sync tip + colour burst).
        if (chrome_visible && CheckCollisionPointRec(mouse_pos, vhs->overlay.frame_rect)) {
            panel->chrome.last_click_ms = 0;
            gui_vhs_fm_set_frame_mode(vhs, vhs->overlay.frame_mode == 1 ? 0 : 1);
            return true;
        }

        // Double-click anywhere else on the panel toggles ALL overlay
        // chrome (buttons + stats) for a clean video preview — and restores
        // it while hidden, since the control checks above are skipped then.
        return video_panel_chrome_toggle_on_click(panel, mouse_pos);
    }
    cvbs_decoder_t *decoder = video_panel_active_decoder(panel);
    if (!decoder) return false;
    // Click outside the panel resets double-click tracking (VHS FM parity).
    if (!CheckCollisionPointRec(mouse_pos, bounds)) {
        panel->chrome.last_click_ms = 0;
        return false;
    }
    bool tape_ui = (panel->active_plugin == VIDEO_PLUGIN_TAPE_DECODE);
    video_tape_plugin_t *tape_plugin = &panel->tape_decode;
    // Control clicks only while the overlay chrome is shown: is_visible is
    // set by the render path and cleared while hidden, and controls_hidden
    // closes the stale-rect window between a hide toggle and the next
    // render. Every control hit resets the double-click tracker so a
    // double-click ON a control never toggles the chrome; while hidden the
    // guards skip all control checks and the click falls through to the
    // shared double-click restore at the bottom.
    bool chrome_visible = decoder->overlay.is_visible && !panel->chrome.controls_hidden;
    // Decoder button: cycle between independent plugin decoders.
    if (chrome_visible && CheckCollisionPointRec(mouse_pos, decoder->overlay.decoder_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        video_plugin_kind_t next = video_panel_next_plugin(panel->active_plugin);
        video_panel_activate_plugin(panel, next);
        decoder = video_panel_active_decoder(panel);
        if (decoder) {
            cvbs_trace_mode_watch(decoder, "overlay_decoder_switch");
        }
        return true;
    }

    // Tape family dropdown button (Tape mode only)
    if (chrome_visible && tape_ui && CheckCollisionPointRec(mouse_pos, decoder->overlay.tape_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        decoder->overlay.tape_dropdown_open = !decoder->overlay.tape_dropdown_open;
        decoder->overlay.tape_format_dropdown_open = false;
        decoder->overlay.system_dropdown_open = false;
        return true;
    }

    // Tape format dropdown button (PAL/NTSC/SECAM, Tape mode only)
    if (chrome_visible && tape_ui && CheckCollisionPointRec(mouse_pos, decoder->overlay.tape_format_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        decoder->overlay.tape_format_dropdown_open = !decoder->overlay.tape_format_dropdown_open;
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.system_dropdown_open = false;
        return true;
    }

    // Tape speed mode button (SP/LP/ELP, Tape mode only)
    if (chrome_visible && tape_ui && CheckCollisionPointRec(mouse_pos, decoder->overlay.tape_mode_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        cvbs_tape_mode_t old_mode = decoder->tape_mode;
        if (decoder->tape_mode == CVBS_TAPE_MODE_SP) {
            decoder->tape_mode = CVBS_TAPE_MODE_LP;
        } else if (decoder->tape_mode == CVBS_TAPE_MODE_LP) {
            decoder->tape_mode = CVBS_TAPE_MODE_ELP;
        } else {
            decoder->tape_mode = CVBS_TAPE_MODE_SP;
        }
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.tape_format_dropdown_open = false;
        decoder->overlay.system_dropdown_open = false;
        if (old_mode != decoder->tape_mode) {
            TraceLog(LOG_INFO,
                     "CVBS MODE TRACE: source=overlay_click field=tape_mode old=%s new=%s",
                     cvbs_tape_mode_name(old_mode),
                     cvbs_tape_mode_name(decoder->tape_mode));
        }
        video_tape_apply_runtime_args(tape_plugin, "overlay_tape_mode_click");
        cvbs_trace_mode_watch(decoder, "overlay_tape_mode_click");
        return true;
    }

    // Colour mode button: toggle Auto <-> Off (burst-gated baseband chroma
    // vs the exact legacy grayscale)
    if (chrome_visible && CheckCollisionPointRec(mouse_pos, decoder->overlay.colour_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        decoder->colour_mode = (decoder->colour_mode == CVBS_COLOUR_AUTO)
                                ? CVBS_COLOUR_OFF : CVBS_COLOUR_AUTO;
        decoder->prev_burst_valid = 0;  // mode switch breaks the V-switch chain
        return true;
    }

    // OSD mode button: cycle OFF -> MINIMAL -> STATS -> OFF
    if (chrome_visible && CheckCollisionPointRec(mouse_pos, decoder->overlay.osd_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        decoder->overlay.system_dropdown_open = false;
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.tape_format_dropdown_open = false;
        if (decoder->osd_mode == CVBS_OSD_OFF) {
            decoder->osd_mode = CVBS_OSD_MINIMAL;
        } else if (decoder->osd_mode == CVBS_OSD_MINIMAL) {
            decoder->osd_mode = CVBS_OSD_STATS;
        } else {
            decoder->osd_mode = CVBS_OSD_OFF;
        }
        return true;
    }

    // Tape family dropdown options
    if (chrome_visible && tape_ui && decoder->overlay.tape_dropdown_open) {
        cvbs_tape_format_t values[] = {
            CVBS_TAPE_FORMAT_VHS,
            CVBS_TAPE_FORMAT_SVHS,
            CVBS_TAPE_FORMAT_BETAMAX,
            CVBS_TAPE_FORMAT_VIDEO8,
            CVBS_TAPE_FORMAT_HI8,
            CVBS_TAPE_FORMAT_UMATIC
        };
        for (int i = 0; i < 6; i++) {
            if (CheckCollisionPointRec(mouse_pos, decoder->overlay.tape_options_rect[i])) {
                panel->chrome.last_click_ms = 0;
                cvbs_tape_format_t old_format = decoder->tape_format;
                decoder->tape_format = values[i];
                decoder->overlay.tape_dropdown_open = false;
                if (old_format != decoder->tape_format) {
                    TraceLog(LOG_INFO,
                             "CVBS MODE TRACE: source=overlay_click field=tape_format old=%s new=%s",
                             cvbs_tape_format_name(old_format),
                             cvbs_tape_format_name(decoder->tape_format));
                }
                video_tape_apply_runtime_args(tape_plugin, "overlay_tape_format_click");
                cvbs_trace_mode_watch(decoder, "overlay_tape_format_click");
                return true;
            }
        }
        // Click outside dropdown closes it
        decoder->overlay.tape_dropdown_open = false;
    }

    // Tape format dropdown options (PAL/NTSC/SECAM)
    if (chrome_visible && tape_ui && decoder->overlay.tape_format_dropdown_open) {
        for (int i = 0; i < 3; i++) {
            if (CheckCollisionPointRec(mouse_pos, decoder->overlay.tape_format_options_rect[i])) {
                panel->chrome.last_click_ms = 0;
                decoder->overlay.selected_tape_format = i;
                decoder->overlay.system_dropdown_open = false;
                decoder->overlay.tape_format_dropdown_open = false;
                video_tape_apply_runtime_args(tape_plugin, "overlay_tape_system_click");
                cvbs_trace_mode_watch(decoder, "overlay_tape_system_click");
                return true;
            }
        }
        // Click outside dropdown closes it
        decoder->overlay.tape_format_dropdown_open = false;
    }

    // Frame mode button: toggle Active <-> Full
    if (chrome_visible && CheckCollisionPointRec(mouse_pos, decoder->overlay.frame_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        cvbs_frame_view_t next_mode = (decoder->frame_view_mode == CVBS_FRAME_VIEW_ACTIVE)
                                      ? CVBS_FRAME_VIEW_FULL
                                      : CVBS_FRAME_VIEW_ACTIVE;
        decoder->overlay.system_dropdown_open = false;
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.tape_format_dropdown_open = false;
        gui_cvbs_set_frame_view_mode(decoder, next_mode);
        return true;
    }

    // Levels mode button: toggle Auto <-> Fixed
    if (chrome_visible && CheckCollisionPointRec(mouse_pos, decoder->overlay.level_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        decoder->overlay.system_dropdown_open = false;
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.tape_format_dropdown_open = false;
        const char *source = tape_ui
                             ? "overlay_tape_level_click"
                             : "overlay_level_click";
        cvbs_decoder_cycle_level_mode(decoder, source);
        if (tape_ui) {
            video_tape_apply_runtime_args(tape_plugin, source);
        }
        return true;
    }

    // Sync mode button: toggle Auto <-> Fixed
    if (chrome_visible && CheckCollisionPointRec(mouse_pos, decoder->overlay.sync_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        decoder->overlay.system_dropdown_open = false;
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.tape_format_dropdown_open = false;
        const char *source = tape_ui
                             ? "overlay_tape_sync_click"
                             : "overlay_sync_click";
        cvbs_decoder_cycle_sync_mode(decoder, source);
        return true;
    }

    // System button: toggle dropdown
    if (chrome_visible && CheckCollisionPointRec(mouse_pos, decoder->overlay.system_btn_rect)) {
        panel->chrome.last_click_ms = 0;
        decoder->overlay.system_dropdown_open = !decoder->overlay.system_dropdown_open;
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.tape_format_dropdown_open = false;
        return true;
    }

    // System dropdown options (0=PAL, 1=NTSC, 2=SECAM - the same value
    // space as gui_cvbs_set_format and the tape format selector)
    if (chrome_visible && decoder->overlay.system_dropdown_open) {
        int sys_values[] = {0, 1, 2};
        for (int i = 0; i < 3; i++) {
            if (CheckCollisionPointRec(mouse_pos, decoder->overlay.system_options_rect[i])) {
                panel->chrome.last_click_ms = 0;
                decoder->overlay.selected_system = sys_values[i];
                if (tape_ui) {
                    // Keep the tape format selector in sync (identical value
                    // space); apply_runtime_args re-derives everything.
                    decoder->overlay.selected_tape_format = sys_values[i];
                    video_tape_apply_runtime_args(tape_plugin, "overlay_system_click");
                } else {
                    gui_cvbs_set_format(decoder, sys_values[i]);
                    // Persist the user's pick BOTH ways: the app setting
                    // (the budget button's pattern - the exit-time save
                    // writes it, and the next launch's panels start in this
                    // system) and the live session default (so a panel
                    // re-created mid-session - e.g. a genuinely fresh view
                    // state after a cache eviction - also starts in it).
                    if (app) app->settings.video_system = sys_values[i];
                    gui_cvbs_set_default_system(sys_values[i]);
                }
                decoder->overlay.system_dropdown_open = false;
                return true;
            }
        }
        // Click outside dropdown closes it
        decoder->overlay.system_dropdown_open = false;
    }

    // Double-click anywhere else on the panel toggles ALL overlay chrome
    // (buttons + OSD text) for a clean video preview — and restores it while
    // hidden, since the control checks above are skipped when hidden.
    return video_panel_chrome_toggle_on_click(panel, mouse_pos);
}

//-----------------------------------------------------------------------------
// Vtable Definition
//-----------------------------------------------------------------------------

static const panel_vtable_t s_cvbs_vtable = {
    .name = "Video",

    // Lifecycle
    .create = cvbs_vtable_create,
    .destroy = cvbs_vtable_destroy,
    .clear = cvbs_vtable_clear,

    // Processing
    .process = cvbs_vtable_process,

    // Rendering
    .render = cvbs_vtable_render,
    .render_overlay = NULL,  // Overlay is rendered in cvbs_vtable_render

    // Interaction
    .handle_click = cvbs_vtable_handle_click,
    .handle_scroll = NULL,

    // Menus (TODO: implement panel-owned system selector)
    .get_menu_count = NULL,
    .get_menu = NULL,
};

//-----------------------------------------------------------------------------
// Registration
//-----------------------------------------------------------------------------

void gui_cvbs_panel_register(void) {
    panel_register(PANEL_VIEW_CVBS, &s_cvbs_vtable);
}

