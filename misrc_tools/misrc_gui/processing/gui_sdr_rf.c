/* Streaming conversion for the optional RTL-SDR real-RF recording path. */
#include "gui_sdr_rf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#if LIBSOXR_ENABLED
#include <soxr.h>

static soxr_quality_spec_t sdr_rf_quality_spec(void)
{
    return soxr_quality_spec(SOXR_VHQ, SOXR_HI_PREC_CLOCK);
}
#endif

#define SDR_RF_INPUT_PAIRS 4096
#define SDR_RF_OUTPUT_SAMPLES 8192
#define SDR_RF_TWO_PI 6.283185307179586476925286766559

struct gui_sdr_rf {
    char error[192];
    bool finished;
#if LIBSOXR_ENABLED
    soxr_t resampler;
    uint32_t center_hz;
    uint32_t phase;
    uint64_t output_samples;
    double osc_i, osc_q;
    double step_i, step_q;
    float input[SDR_RF_INPUT_PAIRS * 2];
    float interpolated[SDR_RF_OUTPUT_SAMPLES * 2];
    int16_t output[SDR_RF_OUTPUT_SAMPLES];
#endif
};

static void sdr_rf_set_error(char *error, size_t error_size, const char *message)
{
    if (error && error_size) snprintf(error, error_size, "%s", message);
}

double gui_sdr_rf_passband_hz(uint32_t input_rate_hz)
{
#if LIBSOXR_ENABLED
    return sdr_rf_quality_spec().passband_end * (input_rate_hz / 2.0);
#else
    (void)input_rate_hz;
    return 0.0;
#endif
}

bool gui_sdr_rf_validate(uint32_t input_rate_hz, uint64_t center_hz,
                         char *error, size_t error_size)
{
    if (input_rate_hz == 0) {
        sdr_rf_set_error(error, error_size, "Hi-Fi RF requires a positive input sample rate");
        return false;
    }
    // Bound the center before doubling it, and widen the rate before adding.
    if (center_hz >= GUI_SDR_RF_OUTPUT_RATE_HZ / 2u ||
        2u * center_hz <= input_rate_hz ||
        2u * center_hz + (uint64_t)input_rate_hz >= GUI_SDR_RF_OUTPUT_RATE_HZ) {
        sdr_rf_set_error(error, error_size,
                         "Hi-Fi RF requires the tuned band to lie between DC and output Nyquist");
        return false;
    }
    sdr_rf_set_error(error, error_size, "");
    return true;
}

gui_sdr_rf_t *gui_sdr_rf_create(uint32_t input_rate_hz, uint64_t center_hz,
                               char *error, size_t error_size)
{
    if (!gui_sdr_rf_validate(input_rate_hz, center_hz, error, error_size))
        return NULL;
#if LIBSOXR_ENABLED
    gui_sdr_rf_t *state = calloc(1, sizeof(*state));
    if (!state) {
        sdr_rf_set_error(error, error_size, "Cannot allocate Hi-Fi RF converter");
        return NULL;
    }
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t quality = sdr_rf_quality_spec();
    soxr_runtime_spec_t runtime = soxr_runtime_spec(1);
    soxr_error_t soxr_error = NULL;
    state->resampler = soxr_create(input_rate_hz, GUI_SDR_RF_OUTPUT_RATE_HZ, 2,
                                  &soxr_error, &io_spec, &quality, &runtime);
    if (!state->resampler || soxr_error) {
        sdr_rf_set_error(error, error_size,
                         soxr_error ? soxr_error : "Cannot create Hi-Fi RF resampler");
        gui_sdr_rf_destroy(state);
        return NULL;
    }
    state->center_hz = (uint32_t)center_hz;
    state->osc_i = 1.0;
    double step = SDR_RF_TWO_PI * (double)center_hz / GUI_SDR_RF_OUTPUT_RATE_HZ;
    state->step_i = cos(step);
    state->step_q = sin(step);
    return state;
#else
    sdr_rf_set_error(error, error_size, "Hi-Fi RF export requires libsoxr");
    return NULL;
#endif
}

#if LIBSOXR_ENABLED
static int16_t sdr_rf_quantize(double value)
{
    double scaled = value * (32768.0 * GUI_SDR_RF_OUTPUT_GAIN);
    if (scaled >= 32767.0) return 32767;
    if (scaled <= -32768.0) return -32768;
    return (int16_t)lround(scaled);
}

static bool sdr_rf_emit(gui_sdr_rf_t *state, size_t samples,
                        gui_sdr_rf_emit_t emit, void *opaque)
{
    for (size_t i = 0; i < samples; i++) {
        double rf = state->interpolated[2 * i] * state->osc_i -
                    state->interpolated[2 * i + 1] * state->osc_q;
        if (!isfinite(rf)) {
            sdr_rf_set_error(state->error, sizeof(state->error),
                             "Hi-Fi RF conversion produced a non-finite sample");
            return false;
        }
        state->output[i] = sdr_rf_quantize(rf);

        // Keep the oscillator independent of input/output block boundaries.
        double next_i = state->osc_i * state->step_i - state->osc_q * state->step_q;
        state->osc_q = state->osc_i * state->step_q + state->osc_q * state->step_i;
        state->osc_i = next_i;
        state->phase += state->center_hz;
        if (state->phase >= GUI_SDR_RF_OUTPUT_RATE_HZ) state->phase -= GUI_SDR_RF_OUTPUT_RATE_HZ;
        state->output_samples++;
        // Re-anchor to an exact integer-Hz phase every 4096 samples, bounding
        // floating-point drift even during recordings lasting many hours.
        if ((state->output_samples & 4095u) == 0) {
            double phase = SDR_RF_TWO_PI * state->phase / GUI_SDR_RF_OUTPUT_RATE_HZ;
            state->osc_i = cos(phase);
            state->osc_q = sin(phase);
        }
    }
    if (samples && !emit(opaque, state->output, samples)) {
        sdr_rf_set_error(state->error, sizeof(state->error), "Hi-Fi RF output write failed");
        return false;
    }
    return true;
}
#endif

bool gui_sdr_rf_process(gui_sdr_rf_t *state, const uint8_t *iq, size_t pairs,
                        gui_sdr_rf_emit_t emit, void *opaque)
{
    if (!state || state->error[0]) return false;
    if (state->finished || !emit || (!iq && pairs) || pairs > SIZE_MAX / 2) {
        sdr_rf_set_error(state->error, sizeof(state->error), "Invalid Hi-Fi RF input or closed stream");
        return false;
    }
#if LIBSOXR_ENABLED
    while (pairs) {
        size_t chunk = pairs < SDR_RF_INPUT_PAIRS ? pairs : SDR_RF_INPUT_PAIRS;
        for (size_t i = 0; i < 2 * chunk; i++) state->input[i] = ((int)iq[i] - 128) / 128.0f;
        size_t used = 0;
        while (used < chunk) {
            size_t in_done = 0, out_done = 0;
            soxr_error_t error = soxr_process(state->resampler, state->input + 2 * used,
                                             chunk - used, &in_done, state->interpolated,
                                             SDR_RF_OUTPUT_SAMPLES, &out_done);
            if (error || (!in_done && !out_done)) {
                sdr_rf_set_error(state->error, sizeof(state->error),
                                 error ? error : "Hi-Fi RF resampler made no progress");
                return false;
            }
            used += in_done;
            if (!sdr_rf_emit(state, out_done, emit, opaque)) return false;
        }
        iq += chunk * 2;
        pairs -= chunk;
    }
    return true;
#else
    (void)opaque;
    return false;
#endif
}

bool gui_sdr_rf_finish(gui_sdr_rf_t *state, gui_sdr_rf_emit_t emit, void *opaque)
{
    if (!state || state->error[0]) return false;
    if (state->finished) return true;
    if (!emit) {
        sdr_rf_set_error(state->error, sizeof(state->error), "Missing Hi-Fi RF output callback");
        return false;
    }
#if LIBSOXR_ENABLED
    size_t out_done;
    do {
        out_done = 0;
        soxr_error_t error = soxr_process(state->resampler, NULL, 0, NULL,
                                         state->interpolated, SDR_RF_OUTPUT_SAMPLES, &out_done);
        if (error) {
            sdr_rf_set_error(state->error, sizeof(state->error), error);
            return false;
        }
        if (!sdr_rf_emit(state, out_done, emit, opaque)) return false;
    } while (out_done);
    state->finished = true;
    return true;
#else
    (void)opaque;
    return false;
#endif
}

const char *gui_sdr_rf_error(const gui_sdr_rf_t *state)
{
    return state ? state->error : "Hi-Fi RF converter is not initialized";
}

void gui_sdr_rf_destroy(gui_sdr_rf_t *state)
{
    if (!state) return;
#if LIBSOXR_ENABLED
    if (state->resampler) soxr_delete(state->resampler);
#endif
    free(state);
}
