/*
 * MISRC GUI - CVBS Video Decoder Module
 *
 * Decodes composite video (PAL/NTSC) from raw ADC samples.
 * Uses software PLL for H-sync tracking and provides frame buffer display.
 */

#include "gui_cvbs.h"
#include "gui_trigger.h"
#include "../visualization/gui_text.h"
#include "../../common/threading.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Debug timing for luma filter performance (per-field accumulation)
static uint64_t g_field_decode_us = 0;      // Accumulated decode time for current field
static double g_field_decode_avg_ms = 0.0;  // Last completed field's decode time (for OSD)

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
// Internal Constants
//-----------------------------------------------------------------------------

// Back porch offset (after H-sync, before active video)
#define BACK_PORCH_SAMPLES      228  // ~7µs - slightly more than standard to skip color burst

// Field detection constants
#define PAL_FIELD_LINES         312  // Lines per PAL field (312.5 rounded)
#define NTSC_FIELD_LINES        262  // Lines per NTSC field (262.5 rounded)
#define PAL_ACTIVE_START        23   // First active line in PAL field
#define NTSC_ACTIVE_START       21   // First active line in NTSC field

// Field heights (half of full frame)
#define PAL_FIELD_HEIGHT        288  // 576/2
#define NTSC_FIELD_HEIGHT       243  // 486/2

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
// Must cover full PAL line period at 40 MSPS when full-frame mode is enabled.
#define FILTERED_LINE_MAX 2600
static int16_t g_filtered_line[FILTERED_LINE_MAX];

// Decode a single video line from samples to grayscale pixels
// Optimized: pre-filters entire line, then resamples to output width.
static void decode_line_to_pixels(const cvbs_decoder_t *decoder,
                                  const int16_t *samples, size_t sample_count,
                                  const cvbs_levels_t *levels,
                                  uint8_t *pixels, int pixel_width) {
    if (!decoder || !samples || !levels || !pixels || pixel_width <= 0) return;

    // Always decode the full line (sync + blanking + active).
    // Active vs Full is decided at render time by cropping the src rectangle.
    if (sample_count < 16) return;

    const int16_t *src = samples;
    int n = (int)sample_count;

    int full_line_samples = (decoder->state.format == CVBS_FORMAT_NTSC)
                            ? CVBS_SAMPLES_PER_LINE_NTSC
                            : CVBS_SAMPLES_PER_LINE_PAL;
    if (n > full_line_samples) n = full_line_samples;
    if (n > FILTERED_LINE_MAX) n = FILTERED_LINE_MAX;
    if (n < 16) return;

    // Pre-calculate level normalization (fixed-point: multiply by 255, divide by range)
    int32_t black = levels->black_level;
    int32_t range = levels->white_level - black;
    if (range < 1) range = 1;
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

    // Main body - compiler will unroll the small fixed-size loop
    int end = n - half_taps;
    for (int i = half_taps; i < end; i++) {
        const int16_t *p = src + i - half_taps;
        int32_t sum = 0;
        for (int k = 0; k < kernel.taps; k++) {
            sum += p[k] * kernel.coeffs[k];
        }
        g_filtered_line[i] = (int16_t)(sum >> kernel.shift);
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
    // Pass 2: Resample filtered line to output pixels (simple lookup)
    // =========================================================================

    // Fixed-point position tracking (16.16 format)
    int32_t pos = 0;
    int32_t step = (n << 16) / pixel_width;

    for (int px = 0; px < pixel_width; px++) {
        int idx = pos >> 16;
        if (idx >= n) idx = n - 1;

        // Normalize to 0-255
        int32_t val = ((g_filtered_line[idx] - black) * 255) / range;

        // Clamp
        if (val < 0) val = 0;
        else if (val > 255) val = 255;

        pixels[px] = (uint8_t)val;
        pos += step;
    }

}

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

bool gui_cvbs_init(cvbs_decoder_t *decoder) {
    if (!decoder) return false;

    memset(decoder, 0, sizeof(cvbs_decoder_t));

    // Allocate field buffers (each field is half the frame height)
    // Max field height is PAL: 288 lines
    decoder->field_height = PAL_FIELD_HEIGHT;
    size_t field_size = (size_t)CVBS_MAX_WIDTH * (size_t)((CVBS_MAX_HEIGHT + 1) / 2);

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

    // Allocate deinterlaced frame buffer at full-frame resolution (D1)
    decoder->frame_width = CVBS_FRAME_WIDTH;
    decoder->frame_height = CVBS_PAL_HEIGHT;  // Start with PAL full-frame height
    decoder->frame_buffer = (uint8_t *)calloc((size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT, 1);
    if (!decoder->frame_buffer) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        return false;
    }

    // Allocate double-buffered display buffers for thread safety
    // Display thread writes to back buffer, render thread reads from front
    decoder->display_front = (uint8_t *)calloc((size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT, 1);
    decoder->display_back = (uint8_t *)calloc((size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT, 1);
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

    // Clear field buffers
    size_t field_size = (size_t)CVBS_MAX_WIDTH * (size_t)((CVBS_MAX_HEIGHT + 1) / 2);
    if (decoder->field_buffer[0]) {
        memset(decoder->field_buffer[0], 0, field_size);
    }
    if (decoder->field_buffer[1]) {
        memset(decoder->field_buffer[1], 0, field_size);
    }

    // Clear frame buffers
    if (decoder->frame_buffer) {
        memset(decoder->frame_buffer, 0, (size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT);
    }
    // Clear both display buffers
    if (decoder->display_front) {
        memset(decoder->display_front, 0, (size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT);
    }
    if (decoder->display_back) {
        memset(decoder->display_back, 0, (size_t)CVBS_MAX_WIDTH * (size_t)CVBS_MAX_HEIGHT);
    }
    atomic_store(&decoder->display_ready, 0);

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
    decoder->field_sequence = 0;
    decoder->field_last_update_seq[0] = 0;
    decoder->field_last_update_seq[1] = 0;
    decoder->fixed_levels_valid = false;
    decoder->fixed_black_level = 0;
    decoder->fixed_white_level = 0;
    cvbs_reset_fixed_sync_lock(decoder);

    // Reset legacy state
    decoder->lines_since_vsync = 0;

    // Reset frame state (preserve decoder->state.format set by gui_cvbs_set_format)
    decoder->state.current_line = 0;
    decoder->state.current_field = 0;
    decoder->state.in_vsync = false;
    decoder->state.frame_complete = false;
    // Note: display_ready already cleared atomically above

    // Reset statistics
    decoder->sync_errors = 0;
    decoder->format_changes = 0;

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

    // Check if detection succeeded (range > 0 means valid peaks found)
    if (detected.range < 100) {
        // Detection failed - keep previous levels
        return;
    }


    bool first_levels = (decoder->adaptive.sync_tip == 0 && decoder->adaptive.white == 0);
    bool scene_change = false;
    float alpha_sync = 0.15f;
    float alpha_luma = 0.15f;

    if (!first_levels) {
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
        // First time - initialize directly from detected levels
        decoder->adaptive.sync_tip = detected.sig_min;
        decoder->adaptive.blanking = detected.black_level;  // Histogram finds actual blanking
        decoder->adaptive.black = detected.black_level;
        decoder->adaptive.white = detected.white_level;
        decoder->adaptive.threshold = detected.sync_threshold;
    } else {
        // Smooth update using detected histogram peaks.
        // Sync domain tracks slowly and is step-limited to avoid scene-cut jitter.
        int16_t target_threshold = detected.sync_threshold;
        int threshold_step = (int)target_threshold - (int)decoder->adaptive.threshold;
        int max_threshold_step = scene_change ? 20 : 56;
        if (threshold_step > max_threshold_step) {
            target_threshold = (int16_t)(decoder->adaptive.threshold + max_threshold_step);
        } else if (threshold_step < -max_threshold_step) {
            target_threshold = (int16_t)(decoder->adaptive.threshold - max_threshold_step);
        }

        decoder->adaptive.sync_tip = (int16_t)(decoder->adaptive.sync_tip * (1.0f - alpha_sync) +
                                               detected.sig_min * alpha_sync);
        decoder->adaptive.blanking = (int16_t)(decoder->adaptive.blanking * (1.0f - alpha_sync) +
                                               detected.black_level * alpha_sync);
        decoder->adaptive.black = (int16_t)(decoder->adaptive.black * (1.0f - alpha_luma) +
                                            detected.black_level * alpha_luma);
        decoder->adaptive.white = (int16_t)(decoder->adaptive.white * (1.0f - alpha_luma) +
                                            detected.white_level * alpha_luma);
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
    int16_t range = decoder->levels.white_level - decoder->levels.sig_min;
    if (range < 100) range = 100;
    decoder->levels.range = range;

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
#define PLL_FIXED_PERIOD_CLAMP  120.0  // Clamp fixed period to nominal ± this many samples

// H-sync pulse validation (aligned with gui_trigger.h constants)
#define HSYNC_MIN_WIDTH     CVBS_HSYNC_MIN_WIDTH  // 100 samples (~2.5µs minimum)
#define HSYNC_MAX_WIDTH     CVBS_HSYNC_MAX_WIDTH  // 280 samples (~7µs maximum)

// Lowpass filter coefficient (IIR single-pole) - Fixed-point Q8
// At 40 MSPS, a cutoff of ~500kHz gives alpha ≈ 0.08
// Lower alpha = more smoothing (removes HF noise while preserving sync edges)
// Fixed-point: alpha = 20/256 ≈ 0.078, (1-alpha) = 236/256 ≈ 0.922
#define LPF_ALPHA_FP        20      // 0.08 * 256 ≈ 20
#define LPF_ONE_MINUS_FP    236     // (1 - 0.08) * 256 ≈ 236
#define LPF_SHIFT           8       // Divide by 256

static inline double cvbs_nominal_line_period(const cvbs_decoder_t *decoder) {
    if (!decoder) return (double)CVBS_SAMPLES_PER_LINE_PAL;
    return (decoder->state.format == CVBS_FORMAT_NTSC)
           ? (double)CVBS_SAMPLES_PER_LINE_NTSC
           : (double)CVBS_SAMPLES_PER_LINE_PAL;
}

static inline double cvbs_effective_line_period(const cvbs_decoder_t *decoder) {
    if (!decoder) return (double)CVBS_SAMPLES_PER_LINE_PAL;
    if (decoder->sync_mode == CVBS_SYNC_MODE_FIXED &&
        decoder->fixed_sync_locked &&
        decoder->fixed_line_period > 0.0) {
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
    int field_height = decoder->field_height;
    int frame_height = decoder->frame_height;
    int max_field_height = (CVBS_MAX_HEIGHT + 1) / 2;

    if (frame_width < 1 || frame_width > CVBS_MAX_WIDTH) return;
    if (field_height < 1) return;
    if (field_height > max_field_height) field_height = max_field_height;
    if (frame_height < 1) return;
    if (frame_height > CVBS_MAX_HEIGHT) frame_height = CVBS_MAX_HEIGHT;

    int generated_lines = field_height * 2;
    if (generated_lines > frame_height) {
        field_height = frame_height / 2;
        generated_lines = field_height * 2;
    }

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

    if (have_odd && have_even) {
        // Weave mode: interleave both fields for full vertical resolution
        for (int fl = 0; fl < field_height; fl++) {
            uint8_t *src_odd = odd_field + (size_t)fl * (size_t)frame_width;
            uint8_t *src_even = even_field + (size_t)fl * (size_t)frame_width;

            // Even frame lines (0, 2, 4...) from even field
            // Odd frame lines (1, 3, 5...) from odd field
            uint8_t *dst_even = frame + (size_t)(fl * 2) * (size_t)frame_width;
            uint8_t *dst_odd = frame + (size_t)(fl * 2 + 1) * (size_t)frame_width;

            memcpy(dst_even, src_even, (size_t)frame_width);
            memcpy(dst_odd, src_odd, (size_t)frame_width);
        }
    } else if (have_odd) {
        // Bob mode with odd field only: duplicate lines with interpolation
        // First line: just duplicate (no previous line to interpolate from)
        memcpy(frame + frame_width, odd_field, (size_t)frame_width);  // dst_odd line 0
        memcpy(frame, odd_field, (size_t)frame_width);  // dst_even line 0

        // Remaining lines: interpolate even lines from adjacent odd lines
        for (int fl = 1; fl < field_height; fl++) {
            uint8_t *src = odd_field + (size_t)fl * (size_t)frame_width;
            uint8_t *src_prev = odd_field + (size_t)(fl - 1) * (size_t)frame_width;
            uint8_t *dst_even = frame + (size_t)(fl * 2) * (size_t)frame_width;
            uint8_t *dst_odd = frame + (size_t)(fl * 2 + 1) * (size_t)frame_width;

            memcpy(dst_odd, src, (size_t)frame_width);

            // Interpolate using integer average: (a + b) >> 1
            // Process 4 bytes at a time using 32-bit operations
            int x = 0;
            for (; x <= frame_width - 4; x += 4) {
                uint32_t a = *(uint32_t *)(src_prev + x);
                uint32_t b = *(uint32_t *)(src + x);
                // Average without overflow: (a & b) + ((a ^ b) >> 1)
                uint32_t avg = (a & b) + (((a ^ b) & 0xFEFEFEFE) >> 1);
                *(uint32_t *)(dst_even + x) = avg;
            }
            // Handle remaining pixels
            for (; x < frame_width; x++) {
                dst_even[x] = (uint8_t)((src_prev[x] + src[x]) >> 1);
            }
        }
    } else if (have_even) {
        // Bob mode with even field only
        // Process all but last line with interpolation
        for (int fl = 0; fl < field_height - 1; fl++) {
            uint8_t *src = even_field + (size_t)fl * (size_t)frame_width;
            uint8_t *src_next = even_field + (size_t)(fl + 1) * (size_t)frame_width;
            uint8_t *dst_even = frame + (size_t)(fl * 2) * (size_t)frame_width;
            uint8_t *dst_odd = frame + (size_t)(fl * 2 + 1) * (size_t)frame_width;

            memcpy(dst_even, src, (size_t)frame_width);

            // Interpolate using 32-bit operations
            int x = 0;
            for (; x <= frame_width - 4; x += 4) {
                uint32_t a = *(uint32_t *)(src + x);
                uint32_t b = *(uint32_t *)(src_next + x);
                uint32_t avg = (a & b) + (((a ^ b) & 0xFEFEFEFE) >> 1);
                *(uint32_t *)(dst_odd + x) = avg;
            }
            for (; x < frame_width; x++) {
                dst_odd[x] = (uint8_t)((src[x] + src_next[x]) >> 1);
            }
        }
        // Last line: just duplicate
        int last_fl = field_height - 1;
        uint8_t *src_last = even_field + (size_t)last_fl * (size_t)frame_width;
        uint8_t *dst_even_last = frame + (size_t)(last_fl * 2) * (size_t)frame_width;
        uint8_t *dst_odd_last = frame + (size_t)(last_fl * 2 + 1) * (size_t)frame_width;
        memcpy(dst_even_last, src_last, (size_t)frame_width);
        memcpy(dst_odd_last, src_last, (size_t)frame_width);
    }

    // Full-frame line systems are odd-height (625/525). Duplicate final line if needed.
    if (generated_lines > 0 && frame_height > generated_lines) {
        uint8_t *src_last = frame + (size_t)(generated_lines - 1) * (size_t)frame_width;
        for (int y = generated_lines; y < frame_height; y++) {
            memcpy(frame + (size_t)y * (size_t)frame_width, src_last, (size_t)frame_width);
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

    // Get expected field parameters
    int expected_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                         NTSC_FIELD_LINES : PAL_FIELD_LINES;

    // Only accept if we got a reasonable field (at least 80% of expected lines)
    // This prevents accepting partial/corrupted fields after format switch
    if (line_count < (expected_lines * 4 / 5)) {
        return;
    }

    // Mark this field as received
    int field_idx = decoder->state.current_field ? 1 : 0;
    decoder->field_ready[field_idx] = true;
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
           (size_t)frame_w * (size_t)frame_h);
    // Signal that new frame is ready for the render thread
    atomic_store(&decoder->display_ready, 1);
    decoder->state.frame_complete = true;
    decoder->state.frames_decoded++;
    decoder->debug.fields_decoded++;
}

// Start a new field after V-sync
// is_odd_field: true for odd field (buffer 0), false for even field (buffer 1)
static void start_new_field_pll(cvbs_decoder_t *decoder, bool is_odd_field) {
    complete_field_pll(decoder);
    commit_adaptive_levels(decoder);

    decoder->pll.current_line = 0;
    decoder->state.current_field = is_odd_field ? 0 : 1;
    decoder->lines_since_vsync = 0;
    decoder->debug.hsyncs_last_field = 0;
    decoder->debug.vsync_found++;
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

    // Update PLL lock status based on phase error magnitude
    if (fabs(phase_error) < tuning.lock_threshold) {
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
        if (!fixed_locked) {
            // Acquisition phase for fixed mode: use AUTO-style PLL long enough to
            // find a stable cadence, then freeze line period.
            if (fabs(phase_error) <= line_period * 0.4) {
                pll->phase -= phase_error * tuning.phase_gain;
                pll->phase_integral += phase_error * tuning.integral_gain;
                if (pll->phase_integral > 50) pll->phase_integral = 50;
                if (pll->phase_integral < -50) pll->phase_integral = -50;
                pll->freq_adjust = pll->phase_integral;
            }

            if (pll->locked) {
                double nominal = cvbs_nominal_line_period(decoder);
                double fixed_period = pll->line_period + pll->freq_adjust;
                double min_period = nominal - PLL_FIXED_PERIOD_CLAMP;
                double max_period = nominal + PLL_FIXED_PERIOD_CLAMP;
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
            // Locked fixed mode: hold frequency constant, only apply a tiny
            // phase correction to avoid long-term drift.
            pll->line_period = decoder->fixed_line_period;
            pll->freq_adjust = 0.0;
            pll->phase_integral = 0.0;
            int expected_lines = (decoder->state.format == CVBS_FORMAT_NTSC)
                                 ? NTSC_FIELD_LINES
                                 : PAL_FIELD_LINES;
            bool near_field_boundary = (pll->current_line < 8) ||
                                       (pll->current_line > (expected_lines - 8));
            bool in_vsync_region = decoder->vsync.in_vsync;
            if (fabs(phase_error) < tuning.lock_threshold) {
                pll->phase -= phase_error * tuning.fixed_phase_gain;
                if (decoder->fixed_bad_syncs > 0) decoder->fixed_bad_syncs--;
            } else if (fabs(phase_error) > line_period * 0.30) {
                if (!in_vsync_region && !near_field_boundary) {
                    decoder->fixed_bad_syncs++;
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
        }
    } else {
        // AUTO mode: keep adapting both phase and frequency.
        if (fabs(phase_error) > line_period * 0.4) {
            // This sync is way off - probably noise or V-sync region.
            decoder->debug.hsyncs_last_field++;
            return;
        }
        pll->phase -= phase_error * tuning.phase_gain;
        pll->phase_integral += phase_error * tuning.integral_gain;
        if (pll->phase_integral > 50) pll->phase_integral = 50;
        if (pll->phase_integral < -50) pll->phase_integral = -50;
        pll->freq_adjust = pll->phase_integral;
    }

    // Store for derivative term (not currently used)
    pll->phase_error = phase_error;

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
            uint8_t *row_ptr = field_buf + ((size_t)field_line * (size_t)frame_width);

            // Use samples from line buffer
            int samples_available = decoder->line_buffer_count;
            if (samples_available > 100) {  // Need minimum samples
                decode_line_to_pixels(decoder,
                                      decoder->line_buffer, samples_available,
                                      &decoder->levels, row_ptr, frame_width);
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
    if (threshold == 0) {
        threshold = decoder->levels.sync_threshold;
    }

    // Bootstrap when decoder starts with no committed adaptive levels yet.
    if (threshold == 0 && buf && count >= 64) {
        cvbs_levels_t bootstrap = {0};
        trigger_analyze_cvbs_levels(buf, count, &bootstrap);
        if (bootstrap.range >= 100) {
            decoder->levels = bootstrap;
            if (decoder->adaptive.sync_tip == 0 && decoder->adaptive.white == 0) {
                decoder->adaptive.sync_tip = bootstrap.sig_min;
                decoder->adaptive.blanking = bootstrap.black_level;
                decoder->adaptive.black = bootstrap.black_level;
                decoder->adaptive.white = bootstrap.white_level;
                decoder->adaptive.threshold = bootstrap.sync_threshold;
            }
            if (decoder->level_mode == CVBS_LEVEL_MODE_FIXED) {
                decoder->fixed_black_level = bootstrap.black_level;
                decoder->fixed_white_level = bootstrap.white_level;
                decoder->fixed_levels_valid = true;
            }
            threshold = bootstrap.sync_threshold;
        }
    }

    return threshold;
}

void gui_cvbs_process_buffer(cvbs_decoder_t *decoder,
                              const int16_t *buf, size_t count) {
    if (!decoder || !buf || count < 100) return;
    if (!decoder->line_buffer) return;

    // Start timing for this buffer processing
    uint64_t start_time = get_time_us();

    // Check for minimum signal strength (skip until first V-sync commits levels)
    if (decoder->levels.range < 100 && decoder->debug.vsync_found > 0) {
        decoder->sync_errors++;
        return;
    }

    // Cache frequently accessed values in local variables
    int16_t threshold = cvbs_get_sync_threshold(decoder, buf, count);
    cvbs_pll_state_t *pll = &decoder->pll;
    int16_t *line_buffer = decoder->line_buffer;
    int line_buffer_count = decoder->line_buffer_count;
    size_t global_sample_pos = decoder->global_sample_pos;
    bool last_filtered_above = decoder->last_filtered_above;

    // Pre-calculate effective period (only changes on H-sync/state transitions)
    double effective_period = cvbs_effective_line_period(decoder);

    // Pre-calculate max lines for field overflow check
    int max_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                   NTSC_FIELD_LINES + 20 : PAL_FIELD_LINES + 20;

    // Process each sample
    for (size_t i = 0; i < count; i++) {
        int16_t sample = buf[i];

        // Apply lowpass filter for cleaner sync detection (fixed-point)
        int16_t filtered = apply_lowpass(decoder, sample);

        // Accumulate samples for histogram-based level detection
        // Use raw sample (not filtered) to capture full signal distribution
        accumulate_level_sample(decoder, sample);

        // Store UNFILTERED sample in line buffer (for video decoding)
        if (line_buffer_count < CVBS_LINE_BUFFER_SIZE) {
            line_buffer[line_buffer_count++] = sample;
        }

        // Advance PLL phase
        pll->phase += 1.0;
        pll->samples_in_line++;
        pll->total_samples++;
        global_sample_pos++;

        // Check for edge transitions (computed once, used by both detectors)
        bool is_above = (filtered > threshold);
        bool falling_edge = last_filtered_above && !is_above;
        bool rising_edge = !last_filtered_above && is_above;

        // Run V-sync detector on falling edges only
        if (falling_edge) {
            size_t interval = global_sample_pos - decoder->vsync_last_edge_pos;
            decoder->vsync_last_edge_pos = global_sample_pos;
            cvbs_vsync_state_t *vs = &decoder->vsync;

            // Half-line interval: 1000-1600 samples (vs 2200-3000 for full line)
            bool is_half_line = (interval >= 1000 && interval <= 1600);

            if (is_half_line) {
                vs->half_line_count++;
                vs->total_half_lines++;
                if (vs->half_line_count >= 6 && !vs->in_vsync) {
                    vs->in_vsync = true;
                    vs->total_half_lines = vs->half_line_count;
                }
            } else if (vs->in_vsync && interval >= 2200 && interval <= 3000) {
                // V-sync complete
                vs->in_vsync = false;
                int vsync_half_lines = vs->total_half_lines;
                decoder->debug.last_half_line_count = vsync_half_lines;
                bool is_odd_field = (vsync_half_lines >= 15);
                bool strong_vsync_signature = (vsync_half_lines >= 10);
                vs->half_line_count = 0;
                vs->total_half_lines = 0;
                int expected_lines = (decoder->state.format == CVBS_FORMAT_NTSC)
                                     ? NTSC_FIELD_LINES
                                     : PAL_FIELD_LINES;
                int line_delta = pll->current_line - expected_lines;
                if (line_delta < 0) line_delta = -line_delta;
                bool accept_vsync = true;
                if (decoder->sync_mode == CVBS_SYNC_MODE_FIXED &&
                    decoder->fixed_sync_locked) {
                    int allowed_window = PLL_FIXED_VSYNC_WINDOW;
                    if (decoder->fixed_bad_syncs > (PLL_FIXED_UNLOCK_COUNT / 2)) {
                        allowed_window += 32;
                    }
                    if (line_delta > allowed_window) {
                        // Allow re-anchoring when the V-sync signature is strong,
                        // even if line counting drifted.
                        bool can_reanchor = strong_vsync_signature &&
                                            (line_delta <= (expected_lines / 2));
                        accept_vsync = can_reanchor;
                    }
                }

                if (accept_vsync) {
                    if (decoder->sync_mode == CVBS_SYNC_MODE_FIXED &&
                        decoder->fixed_sync_locked &&
                        !strong_vsync_signature) {
                        // Weak V-sync signature: keep field cadence deterministic.
                        is_odd_field = (decoder->state.current_field == 1);
                    }
                    // Write back before calling start_new_field_pll
                    decoder->line_buffer_count = line_buffer_count;
                    decoder->global_sample_pos = global_sample_pos;
                    decoder->last_filtered_above = is_above;

                    start_new_field_pll(decoder, is_odd_field);

                    // Refresh cached values that may have changed
                    effective_period = cvbs_effective_line_period(decoder);
                    line_buffer_count = decoder->line_buffer_count;
                } else {
                    decoder->sync_errors++;
                }
            } else {
                vs->half_line_count = 0;
            }

            // H-sync falling edge - start pulse measurement
            decoder->in_hsync_pulse = true;
            decoder->hsync_pulse_start = global_sample_pos;
        }

        // H-sync rising edge - check pulse width
        if (rising_edge && decoder->in_hsync_pulse) {
            decoder->in_hsync_pulse = false;
            size_t pulse_width = global_sample_pos - decoder->hsync_pulse_start;

            if (pulse_width >= HSYNC_MIN_WIDTH && pulse_width <= HSYNC_MAX_WIDTH) {
                pll_process_hsync(decoder, pll->phase);
                // Update effective period after PLL adjustment
                effective_period = cvbs_effective_line_period(decoder);
            }
        }

        last_filtered_above = is_above;

        // Check if PLL indicates line complete (phase >= line_period)
        if (pll->phase >= effective_period) {
            // Write back line_buffer_count before decode
            decoder->line_buffer_count = line_buffer_count;

            // Line complete - decode it
            decode_current_line(decoder);

            // Refresh line_buffer_count (decode_current_line resets it)
            line_buffer_count = decoder->line_buffer_count;

            // Advance to next line
            pll->current_line++;
            pll->phase -= effective_period;
            pll->samples_in_line = 0;
            decoder->lines_since_vsync++;

            // Check for field overflow
            if (pll->current_line > max_lines) {
                bool next_is_odd = (decoder->state.current_field == 1);
                decoder->global_sample_pos = global_sample_pos;
                decoder->last_filtered_above = last_filtered_above;
                start_new_field_pll(decoder, next_is_odd);
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
    g_field_decode_us += get_time_us() - start_time;
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
               (size_t)frame_w * (size_t)frame_h);
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

    const char *deint_mode = (decoder->field_ready[0] && decoder->field_ready[1])
                             ? "weave" : "bob";

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

    // Line 2: frames + sync stats
    char osd_text2[96];
    snprintf(osd_text2, sizeof(osd_text2),
             "frames:%d sync_err:%d vsync:%d hsync/field:%d",
             decoder->state.frames_decoded,
             decoder->sync_errors,
             decoder->debug.vsync_found,
             decoder->debug.hsyncs_last_field);

    float text2_y = text_y - 18.0f;
    gui_text_draw_mono(osd_text2, text_x + 1, text2_y + 1, 14, BLACK);
    gui_text_draw_mono(osd_text2, text_x, text2_y, 14, (Color){200, 255, 200, 255});
}

void gui_cvbs_render_frame(cvbs_decoder_t *decoder,
                            float x, float y, float width, float height) {
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

    // Active picture boundaries measured from the decoded full frame.
    // The decoder resamples sync-to-sync across the full frame width, so the
    // active picture is offset from theoretical 4fsc positions.
    bool ntsc = (decoder->state.format == CVBS_FORMAT_NTSC);
    int active_x = ntsc ? 100 : 120;
    int active_y = ntsc ? 25  : 35;
    int active_w = ntsc ? 720 : 900;
    int active_h = ntsc ? 470 : 560;
    if (active_x < 0) active_x = 0;
    if (active_y < 0) active_y = 0;
    if (active_x + active_w > frame_w) active_w = frame_w - active_x;
    if (active_y + active_h > field_h) active_h = field_h - active_y;

    // Convert grayscale to RGBA for the image.
    // Write with the image row pitch (MAX_WIDTH) so NTSC (frame_w < MAX_WIDTH)
    // does not shear.
    uint8_t *gray_src = decoder->display_front;
    uint32_t *rgba_dst = (uint32_t *)decoder->frame_image.data;
    int img_pitch = decoder->frame_image.width;  // CVBS_MAX_WIDTH (1135)

    for (int py = 0; py < field_h; py++) {
        int src_row = py * frame_w;
        int dst_row = py * img_pitch;
        for (int px = 0; px < frame_w; px++) {
            uint8_t gray = gray_src[src_row + px];
            rgba_dst[dst_row + px] = 0xFF000000 |
                                     ((uint32_t)gray << 16) |
                                     ((uint32_t)gray << 8)  |
                                     (uint32_t)gray;
        }
    }

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

    // Active mode crops to the picture area only.
    // Full mode draws everything with the contrast offset baked in above.
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
    gui_cvbs_render_osd(decoder, draw_x, draw_y, draw_w, draw_h);
}

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

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
    decoder->state.total_lines = ntsc ? CVBS_NTSC_TOTAL_LINES : CVBS_PAL_TOTAL_LINES;
    decoder->state.active_lines = ntsc ? CVBS_NTSC_ACTIVE_LINES : CVBS_PAL_ACTIVE_LINES;
    decoder->pll.line_period = ntsc ? CVBS_NTSC_LINE_SAMPLES : CVBS_PAL_LINE_SAMPLES;

    // Always allocate/decode at full-frame dimensions.
    // Active vs Full is purely a render-time crop.
    decoder->frame_width = ntsc ? CVBS_NTSC_FULL_WIDTH_4FSC : CVBS_PAL_FULL_WIDTH_4FSC;
    decoder->frame_height = ntsc ? CVBS_NTSC_FULL_HEIGHT : CVBS_PAL_FULL_HEIGHT;
    decoder->field_height = ntsc ? NTSC_FIELD_LINES : PAL_FIELD_LINES;

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

    decoder->frame_view_mode = mode;
    gui_cvbs_apply_geometry(decoder);
    gui_cvbs_reset(decoder);
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

    // System selector label (PAL/SECAM vs NTSC)
    const char *system_labels[] = {"PAL/SECAM", "NTSC"};
    int sys = decoder->overlay.selected_system; // 0=PAL/SECAM, 1=NTSC
    if (sys < 0 || sys > 1) sys = 0;
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
    cvbs_build_level_label(decoder, level_label, sizeof(level_label));
    cvbs_build_sync_label(decoder, sync_label, sizeof(sync_label));

    // Compute button width large enough for both labels so the box
    // does not change size when switching between PAL and NTSC.
    float sys_text_w0 = (float)gui_text_measure(system_labels[0], FONT_SIZE_DROPDOWN_OPT);
    float sys_text_w1 = (float)gui_text_measure(system_labels[1], FONT_SIZE_DROPDOWN_OPT);
    float sys_text_w_max = (sys_text_w0 > sys_text_w1) ? sys_text_w0 : sys_text_w1;
    float sys_btn_w = sys_text_w_max + 28.0f; // padding + arrow
    if (sys_btn_w < 140.0f) sys_btn_w = 140.0f;

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

    // Draw dropdown options for system if open
    if (is_open) {
        float opt_y = sys_btn_y + btn_h;
        const char *options[] = {"PAL/SECAM", "NTSC"};
        int sys_values[] = {0, 1};
        float opt_h = 20.0f;

        // Background for dropdown container
        DrawRectangleRounded((Rectangle){sys_btn_rect.x, opt_y, sys_btn_rect.width, opt_h * 2.0f},
                             0.1f, 4, COLOR_PANEL_BG);

        for (int i = 0; i < 2; i++) {
            Rectangle opt_rect = {sys_btn_rect.x, opt_y + i * opt_h, sys_btn_rect.width, opt_h};
            decoder->overlay.system_options_rect[i] = opt_rect;

            bool is_selected = (sys == sys_values[i]);
            Vector2 mouse = GetMousePosition();
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
            Vector2 mouse = GetMousePosition();
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
            Vector2 mouse = GetMousePosition();
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
    VIDEO_PLUGIN_TAPE_DECODE = 1
} video_plugin_kind_t;

typedef struct {
    cvbs_decoder_t decoder;
} video_cvbs_plugin_t;

typedef struct {
    cvbs_decoder_t decoder;
    char active_profile[32];
    float active_frequency_mhz;
} video_tape_plugin_t;

typedef struct {
    video_cvbs_plugin_t cvbs;
    video_tape_plugin_t tape_decode;
    video_plugin_kind_t active_plugin;
} video_panel_state_t;

static const char *video_plugin_name(video_plugin_kind_t plugin) {
    return (plugin == VIDEO_PLUGIN_TAPE_DECODE) ? "Tape Decode" : "CVBS";
}

static inline cvbs_decoder_t *video_panel_decoder(video_panel_state_t *panel, video_plugin_kind_t plugin) {
    if (!panel) return NULL;
    return (plugin == VIDEO_PLUGIN_TAPE_DECODE)
        ? &panel->tape_decode.decoder
        : &panel->cvbs.decoder;
}

static inline cvbs_decoder_t *video_panel_active_decoder(video_panel_state_t *panel) {
    if (!panel) return NULL;
    return video_panel_decoder(panel, panel->active_plugin);
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

    decoder->overlay.selected_system = 0;       // 0=PAL/SECAM
    decoder->overlay.selected_tape_format = 0;  // 0=PAL, 1=NTSC, 2=SECAM
    decoder->overlay.system_dropdown_open = false;
    decoder->overlay.tape_dropdown_open = false;
    decoder->overlay.tape_format_dropdown_open = false;
    gui_cvbs_set_format(decoder, 0);
    cvbs_trace_mode_watch(decoder, trace_source);
    return true;
}

static bool video_cvbs_plugin_init(video_cvbs_plugin_t *plugin) {
    if (!plugin) return false;
    return video_panel_init_decoder_common(&plugin->decoder, CVBS_DECODER_MODE_CVBS, "create_plugin_cvbs");
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

    if (selected_format == 1) {
        decoder->overlay.selected_system = 1;
        gui_cvbs_set_format(decoder, 1); // NTSC
    } else if (selected_format == 2) {
        decoder->overlay.selected_system = 0;
        gui_cvbs_set_format(decoder, 2); // SECAM
    } else {
        decoder->overlay.selected_system = 0;
        gui_cvbs_set_format(decoder, 0); // PAL
    }

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
    cvbs->overlay.system_dropdown_open = false;
    cvbs->overlay.tape_dropdown_open = false;
    cvbs->overlay.tape_format_dropdown_open = false;
    tape->overlay.system_dropdown_open = false;
    tape->overlay.tape_dropdown_open = false;
    tape->overlay.tape_format_dropdown_open = false;
}

static void video_panel_activate_plugin(video_panel_state_t *panel, video_plugin_kind_t plugin) {
    if (!panel) return;
    if (panel->active_plugin == plugin) return;
    video_plugin_kind_t old_plugin = panel->active_plugin;
    video_panel_close_all_dropdowns(panel);
    panel->active_plugin = plugin;
    TraceLog(LOG_INFO,
             "VIDEO DECODER SWITCH: old=%s new=%s",
             video_plugin_name(old_plugin),
             video_plugin_name(panel->active_plugin));
}

//-----------------------------------------------------------------------------
// Lifecycle Functions
//-----------------------------------------------------------------------------

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

    panel->active_plugin = VIDEO_PLUGIN_CVBS;
    return panel;
}

static void cvbs_vtable_destroy(void *state) {
    if (!state) return;
    video_panel_state_t *panel = (video_panel_state_t *)state;
    gui_cvbs_cleanup(&panel->cvbs.decoder);
    gui_cvbs_cleanup(&panel->tape_decode.decoder);
    free(panel);
}

static void cvbs_vtable_clear(void *state) {
    if (!state) return;
    video_panel_state_t *panel = (video_panel_state_t *)state;
    gui_cvbs_reset(&panel->cvbs.decoder);
    gui_cvbs_reset(&panel->tape_decode.decoder);
    video_tape_apply_runtime_args(&panel->tape_decode, "clear");
    panel->active_plugin = VIDEO_PLUGIN_CVBS;
    video_panel_close_all_dropdowns(panel);
}

//-----------------------------------------------------------------------------
// Processing Function (called from display thread)
//-----------------------------------------------------------------------------
static void video_cvbs_plugin_process(video_cvbs_plugin_t *plugin,
                                      const int16_t *samples,
                                      size_t count) {
    if (!plugin || !samples || count == 0) return;
    cvbs_decoder_t *decoder = &plugin->decoder;
    cvbs_trace_mode_watch(decoder, "process_enter_cvbs");
    gui_cvbs_process_buffer(decoder, samples, count);
    cvbs_trace_mode_watch(decoder, "process_exit_cvbs");
}

static void video_tape_plugin_process(video_tape_plugin_t *plugin,
                                      const int16_t *samples,
                                      size_t count) {
    if (!plugin || !samples || count == 0) return;
    cvbs_decoder_t *decoder = &plugin->decoder;
    if (plugin->active_profile[0] == '\0') {
        video_tape_apply_runtime_args(plugin, "process_bootstrap");
    }
    cvbs_trace_mode_watch(decoder, "process_enter_tape_decode");
    gui_cvbs_process_buffer(decoder, samples, count);
    cvbs_trace_mode_watch(decoder, "process_exit_tape_decode");
}

static void cvbs_vtable_process(void *state, const int16_t *samples, size_t count, uint32_t sample_rate) {
    (void)sample_rate;  // CVBS uses its own sample rate detection
    if (!state || !samples || count == 0) return;
    video_panel_state_t *panel = (video_panel_state_t *)state;
    video_cvbs_plugin_process(&panel->cvbs, samples, count);
    video_tape_plugin_process(&panel->tape_decode, samples, count);
}

//-----------------------------------------------------------------------------
// Rendering Functions
//-----------------------------------------------------------------------------

static void cvbs_vtable_render(void *state, gui_app_t *app, int channel,
                                Rectangle bounds, Color channel_color) {
    (void)app;
    (void)channel;
    (void)channel_color;

    video_panel_state_t *panel = (video_panel_state_t *)state;
    cvbs_decoder_t *decoder = video_panel_active_decoder(panel);

    if (!decoder) {
        // No decoder state - show message
        const char *msg = "Video Decoder Not Available";
        int w = MeasureText(msg, 12);
        DrawRectangleRec(bounds, (Color){20, 20, 20, 255});
        DrawText(msg, (int)(bounds.x + bounds.width/2 - w/2),
                 (int)(bounds.y + bounds.height/2 - 6), 12, (Color){150, 150, 150, 255});
        return;
    }

    // Swap buffers for both plugin decoders to keep independent feeds fresh.
    gui_cvbs_swap_buffers(&panel->cvbs.decoder);
    gui_cvbs_swap_buffers(&panel->tape_decode.decoder);

    // Render the decoded frame
    gui_cvbs_render_frame(decoder, bounds.x, bounds.y, bounds.width, bounds.height);

    // Render the system selector overlay (uses decoder->overlay for state)
    render_cvbs_system_overlay(decoder, bounds.x, bounds.y, bounds.width);
}

//-----------------------------------------------------------------------------
// Interaction Functions
//-----------------------------------------------------------------------------

static bool cvbs_vtable_handle_click(void *state, gui_app_t *app, int channel,
                                      Vector2 mouse_pos, Rectangle bounds) {
    (void)app;
    (void)channel;
    (void)bounds;
    video_panel_state_t *panel = (video_panel_state_t *)state;
    if (!panel) return false;
    cvbs_decoder_t *decoder = video_panel_active_decoder(panel);
    if (!decoder) return false;
    if (!decoder->overlay.is_visible) return false;
    bool tape_ui = (panel->active_plugin == VIDEO_PLUGIN_TAPE_DECODE);
    video_tape_plugin_t *tape_plugin = &panel->tape_decode;
    // Decoder button: switch between independent plugin decoders (CVBS/Tape).
    if (CheckCollisionPointRec(mouse_pos, decoder->overlay.decoder_btn_rect)) {
        video_plugin_kind_t next = (panel->active_plugin == VIDEO_PLUGIN_CVBS)
            ? VIDEO_PLUGIN_TAPE_DECODE
            : VIDEO_PLUGIN_CVBS;
        video_panel_activate_plugin(panel, next);
        decoder = video_panel_active_decoder(panel);
        if (decoder) {
            cvbs_trace_mode_watch(decoder, "overlay_decoder_switch");
        }
        return true;
    }

    // Tape family dropdown button (Tape mode only)
    if (tape_ui && CheckCollisionPointRec(mouse_pos, decoder->overlay.tape_btn_rect)) {
        decoder->overlay.tape_dropdown_open = !decoder->overlay.tape_dropdown_open;
        decoder->overlay.tape_format_dropdown_open = false;
        decoder->overlay.system_dropdown_open = false;
        return true;
    }

    // Tape format dropdown button (PAL/NTSC/SECAM, Tape mode only)
    if (tape_ui && CheckCollisionPointRec(mouse_pos, decoder->overlay.tape_format_btn_rect)) {
        decoder->overlay.tape_format_dropdown_open = !decoder->overlay.tape_format_dropdown_open;
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.system_dropdown_open = false;
        return true;
    }

    // Tape speed mode button (SP/LP/ELP, Tape mode only)
    if (tape_ui && CheckCollisionPointRec(mouse_pos, decoder->overlay.tape_mode_btn_rect)) {
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

    // OSD mode button: cycle OFF -> MINIMAL -> STATS -> OFF
    if (CheckCollisionPointRec(mouse_pos, decoder->overlay.osd_btn_rect)) {
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
    if (tape_ui && decoder->overlay.tape_dropdown_open) {
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
    if (tape_ui && decoder->overlay.tape_format_dropdown_open) {
        for (int i = 0; i < 3; i++) {
            if (CheckCollisionPointRec(mouse_pos, decoder->overlay.tape_format_options_rect[i])) {
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
    if (CheckCollisionPointRec(mouse_pos, decoder->overlay.frame_btn_rect)) {
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
    if (CheckCollisionPointRec(mouse_pos, decoder->overlay.level_btn_rect)) {
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
    if (CheckCollisionPointRec(mouse_pos, decoder->overlay.sync_btn_rect)) {
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
    if (CheckCollisionPointRec(mouse_pos, decoder->overlay.system_btn_rect)) {
        decoder->overlay.system_dropdown_open = !decoder->overlay.system_dropdown_open;
        decoder->overlay.tape_dropdown_open = false;
        decoder->overlay.tape_format_dropdown_open = false;
        return true;
    }

    // System dropdown options
    if (decoder->overlay.system_dropdown_open) {
        int sys_values[] = {0, 1};  // 0=625-line PAL/SECAM, 1=525-line NTSC
        for (int i = 0; i < 2; i++) {
            if (CheckCollisionPointRec(mouse_pos, decoder->overlay.system_options_rect[i])) {
                decoder->overlay.selected_system = sys_values[i];
                if (tape_ui) {
                    if (sys_values[i] == 1) {
                        decoder->overlay.selected_tape_format = 1; // NTSC
                    } else if (decoder->overlay.selected_tape_format == 1) {
                        decoder->overlay.selected_tape_format = 0; // PAL fallback from NTSC
                    }
                    video_tape_apply_runtime_args(tape_plugin, "overlay_system_click");
                } else {
                    gui_cvbs_set_format(decoder, sys_values[i]);
                }
                decoder->overlay.system_dropdown_open = false;
                return true;
            }
        }
        // Click outside dropdown closes it
        decoder->overlay.system_dropdown_open = false;
    }

    return false;
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

