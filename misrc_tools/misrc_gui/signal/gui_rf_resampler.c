#include "gui_rf_resampler.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef LIBSOXR_ENABLED
#define LIBSOXR_ENABLED 0
#endif
#if LIBSOXR_ENABLED
#include <soxr.h>
#endif

/* Rate provisions (see dev/output_stdio_buffer_and_rf_rate_provisions.md):
 * 20 MSPS output keeps its exact 20/40/80 input fast paths. The wide 40 MSPS
 * output uses exact 40/80/160 paths; other ratios use soxr up to 200 MSPS. */
#define RF_OUTPUT_RATE_HZ 20000000u
#define RF_WIDE_OUTPUT_RATE_HZ 40000000u
#define RF_MIN_INPUT_RATE_HZ 16000000u
#define RF_MAX_INPUT_RATE_HZ 200000000u
#define RF_SOXR_BLOCK_SAMPLES 2048u
/* The converters use DFT blocks capped at 4096 samples. Leave room for
 * delayed output from preceding calls as well as the current input block. */
#define RF_SOXR_OUTPUT_HEADROOM 8192u

typedef struct {
    float history[32];
    unsigned write_index;
    unsigned phase;
} rf_halfband_t;

struct gui_rf_resampler {
    uint32_t input_rate_hz, output_rate_hz;
    unsigned factor;
    bool ed_beta;
    rf_halfband_t stages[2];
#if LIBSOXR_ENABLED
    soxr_t soxr;
    bool failed;
#endif
};

/* Ported from VHSDecode.Core/Rf/HalfRateSampleLoader.cs in vhs-decode-dotnet
 * (GPL-3.0), revision f381ffb. These are the 15 nonzero coefficients of its
 * symmetric halfband FIR. Unlike the seekable loader, this implementation is
 * causal: the centre is 15 source samples behind the newest sample. Keeping
 * the same centre preserves a delay of 7.5 output samples at 40 -> 20 MSPS. */
static const float rf_halfband_coefficients[8] = {
    0.5000046374907835f,
    0.3126333216205309f,
    -0.09010692207961689f,
    0.040107417651266825f,
    -0.017917030421899405f,
    0.007100857132041433f,
    -0.0022302855185240434f,
    0.00041032287080936366f
};

static bool halfband_push(rf_halfband_t *stage, float sample, float *output) {
    const unsigned newest = stage->write_index;
    stage->history[newest] = sample;
    stage->write_index = (newest + 1u) & 31u;
    const bool emit = stage->phase == 0u;
    stage->phase ^= 1u;
    if (!emit) return false;

    float value = rf_halfband_coefficients[0]
        * stage->history[(newest + 32u - 15u) & 31u];
    for (unsigned tap = 1u; tap < 8u; ++tap) {
        const unsigned offset = 2u * tap - 1u;
        value += rf_halfband_coefficients[tap]
            * (stage->history[(newest + 32u - (15u - offset)) & 31u]
                + stage->history[(newest + 32u - (15u + offset)) & 31u]);
    }
    *output = value;
    return true;
}

static bool input_rate_valid(uint32_t rate) {
    return rate >= RF_MIN_INPUT_RATE_HZ && rate <= RF_MAX_INPUT_RATE_HZ;
}

static bool output_rate_valid(uint32_t rate) {
    return rate == RF_OUTPUT_RATE_HZ || rate == RF_WIDE_OUTPUT_RATE_HZ;
}

gui_rf_resampler_t *gui_rf_resampler_create(uint32_t input_rate_hz) {
    return gui_rf_resampler_create_rate(input_rate_hz, RF_OUTPUT_RATE_HZ);
}

static gui_rf_resampler_t *rf_resampler_create(uint32_t input_rate_hz,
                                               uint32_t output_rate_hz, bool ed_beta) {
    if (!input_rate_valid(input_rate_hz) || !output_rate_valid(output_rate_hz) ||
        (ed_beta && input_rate_hz < RF_OUTPUT_RATE_HZ)) return NULL;
    gui_rf_resampler_t *resampler = calloc(1, sizeof(*resampler));
    if (!resampler) return NULL;
    resampler->input_rate_hz = input_rate_hz;
    resampler->output_rate_hz = output_rate_hz;
    resampler->ed_beta = ed_beta;
    if (input_rate_hz == output_rate_hz) {
        resampler->factor = 1u;
    } else if (!ed_beta && input_rate_hz == output_rate_hz * 2u) {
        resampler->factor = 2u;
    } else if (!ed_beta && input_rate_hz == output_rate_hz * 4u) {
        resampler->factor = 4u;
    } else {
#if LIBSOXR_ENABLED
        soxr_error_t error = NULL;
        soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        soxr_quality_spec_t quality = soxr_quality_spec(ed_beta ? SOXR_16_BITQ : SOXR_HQ, SOXR_HI_PREC_CLOCK);
        const double nyquist = 0.5 * fmin((double)input_rate_hz, output_rate_hz);
        /* ED preserves its 9.3 MHz white carrier with a 9.5/10 MHz transition.
         * Keep legacy 20 MSPS and explicit 40 MSPS responses unchanged. */
        const double passband_hz = output_rate_hz == RF_WIDE_OUTPUT_RATE_HZ
            ? 16000000.0 : ed_beta ? 9500000.0 : 8000000.0;
        quality.passband_end = fmin(0.95, passband_hz / nyquist);
        quality.stopband_begin = 1.0;
        quality.phase_response = 50.0;
        soxr_runtime_spec_t runtime = soxr_runtime_spec(1);
        runtime.log2_min_dft_size = 8u;
        runtime.log2_large_dft_size = ed_beta ? 12u : 10u;
        resampler->soxr = soxr_create(input_rate_hz, output_rate_hz, 1,
                                      &error, &io, &quality, &runtime);
        if (error || !resampler->soxr) {
            gui_rf_resampler_destroy(resampler);
            return NULL;
        }
#else
        free(resampler);
        return NULL;
#endif
    }
    return resampler;
}

gui_rf_resampler_t *gui_rf_resampler_create_rate(uint32_t input_rate_hz,
                                               uint32_t output_rate_hz) {
    return rf_resampler_create(input_rate_hz, output_rate_hz, false);
}

gui_rf_resampler_t *gui_rf_resampler_create_ed_beta(uint32_t input_rate_hz) {
    return rf_resampler_create(input_rate_hz, RF_OUTPUT_RATE_HZ, true);
}

void gui_rf_resampler_destroy(gui_rf_resampler_t *resampler) {
    if (!resampler) return;
#if LIBSOXR_ENABLED
    if (resampler->soxr) soxr_delete(resampler->soxr);
#endif
    free(resampler);
}

void gui_rf_resampler_reset(gui_rf_resampler_t *resampler) {
    if (!resampler) return;
    memset(resampler->stages, 0, sizeof(resampler->stages));
#if LIBSOXR_ENABLED
    if (resampler->soxr) resampler->failed = soxr_clear(resampler->soxr) != NULL;
#endif
}

static size_t rf_output_capacity(uint32_t input_rate_hz, uint32_t output_rate_hz,
                                 size_t input_count, bool ed_beta) {
    if (!input_rate_valid(input_rate_hz) || !output_rate_valid(output_rate_hz) || input_count == 0 ||
        (ed_beta && input_rate_hz < RF_OUTPUT_RATE_HZ)) return 0;
    if (input_rate_hz == output_rate_hz) return input_count;
    if (!ed_beta && input_rate_hz == output_rate_hz * 2u) return input_count / 2u + (input_count % 2u != 0);
    if (!ed_beta && input_rate_hz == output_rate_hz * 4u) return input_count / 4u + (input_count % 4u != 0);
#if LIBSOXR_ENABLED
    /* Divide before multiplying, so even a caller's oversized count cannot
     * wrap this allocation bound. All rates here are expressed in Hz. */
    const size_t whole = input_count / input_rate_hz;
    const uint64_t remainder = input_count % input_rate_hz;
    if (whole > (SIZE_MAX - RF_SOXR_OUTPUT_HEADROOM) / output_rate_hz) return 0;
    const size_t base = whole * output_rate_hz;
    const size_t tail = (size_t)((remainder * output_rate_hz + input_rate_hz - 1u)
                                / input_rate_hz);
    if (tail > SIZE_MAX - RF_SOXR_OUTPUT_HEADROOM - base) return 0;
    return base + tail + RF_SOXR_OUTPUT_HEADROOM;
#else
    return 0;
#endif
}

size_t gui_rf_resampler_output_capacity(uint32_t input_rate_hz, size_t input_count) {
    return rf_output_capacity(input_rate_hz, RF_OUTPUT_RATE_HZ, input_count, false);
}

size_t gui_rf_resampler_output_capacity_rate(uint32_t input_rate_hz,
                                            uint32_t output_rate_hz, size_t input_count) {
    return rf_output_capacity(input_rate_hz, output_rate_hz, input_count, false);
}

size_t gui_rf_resampler_output_capacity_ed_beta(uint32_t input_rate_hz, size_t input_count) {
    return rf_output_capacity(input_rate_hz, RF_OUTPUT_RATE_HZ, input_count, true);
}

bool gui_rf_resampler_process(gui_rf_resampler_t *resampler,
                              const int16_t *input, size_t input_count,
                              float *output, size_t output_capacity,
                              size_t *output_count) {
    if (!output_count) return false;
    *output_count = 0;
    if (!resampler || (input_count && (!input || !output))) return false;
    if (!input_count) return true; /* A capture gap is not end-of-input. */
    const size_t capacity = rf_output_capacity(resampler->input_rate_hz,
        resampler->output_rate_hz, input_count, resampler->ed_beta);
    if (!capacity || output_capacity < capacity) return false;

    if (resampler->factor) {
        size_t produced = 0;
        for (size_t index = 0; index < input_count; ++index) {
            float value = (float)input[index] * (1.0f / 32768.0f);
            if (resampler->factor >= 2u && !halfband_push(&resampler->stages[0], value, &value))
                continue;
            if (resampler->factor == 4u && !halfband_push(&resampler->stages[1], value, &value))
                continue;
            output[produced++] = value;
        }
        *output_count = produced;
        return true;
    }
#if LIBSOXR_ENABLED
    if (!resampler->soxr || resampler->failed) return false;
    float normalized[RF_SOXR_BLOCK_SAMPLES];
    size_t source_offset = 0;
    size_t produced = 0;
    while (source_offset < input_count) {
        size_t block = input_count - source_offset;
        if (block > RF_SOXR_BLOCK_SAMPLES) block = RF_SOXR_BLOCK_SAMPLES;
        for (size_t index = 0; index < block; ++index)
            normalized[index] = (float)input[source_offset + index] * (1.0f / 32768.0f);
        size_t consumed = 0;
        while (consumed < block) {
            size_t input_done = 0;
            size_t output_done = 0;
            soxr_error_t error = soxr_process(resampler->soxr, normalized + consumed,
                block - consumed, &input_done, output + produced,
                output_capacity - produced, &output_done);
            if (error || (!input_done && !output_done)) {
                resampler->failed = true;
                return false;
            }
            consumed += input_done;
            produced += output_done;
        }
        source_offset += block;
    }
    *output_count = produced;
    return true;
#else
    return false;
#endif
}

bool gui_rf_resampler_finish(gui_rf_resampler_t *resampler, float *output,
                             size_t output_capacity, size_t *output_count) {
    if (!output_count) return false;
    *output_count = 0;
    if (!resampler || !output || !output_capacity) return false;
    if (resampler->factor) return true;
#if LIBSOXR_ENABLED
    if (!resampler->soxr || resampler->failed) return false;
    resampler->failed = soxr_process(resampler->soxr, NULL, 0, NULL, output,
                                     output_capacity, output_count) != NULL;
    return !resampler->failed;
#else
    return false;
#endif
}

double gui_rf_resampler_delay_samples(const gui_rf_resampler_t *resampler) {
    if (!resampler) return 0.0;
    if (resampler->factor == 2u) return 7.5;
    if (resampler->factor == 4u) return 11.25;
    /* soxr aligns output sample zero to input sample zero. Its filter
     * look-ahead delays availability, already reflected by output_count;
     * adding soxr_delay() here would count that buffered latency twice. */
    return 0.0;
}
