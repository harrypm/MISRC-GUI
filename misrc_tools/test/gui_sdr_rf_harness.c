/* Synthetic numerical/streaming fixtures, not tuner or tape-decoding tests. */
#include <stdio.h>
#include <string.h>

// Include production code to cover the saturating quantizer's overflow limits.
#include "../misrc_gui/processing/gui_sdr_rf.c"

static int checks, failures;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

#if LIBSOXR_ENABLED
typedef struct {
    int16_t *samples;
    size_t count, capacity;
    bool fail;
} sample_sink_t;

static bool collect(void *opaque, const int16_t *samples, size_t count)
{
    sample_sink_t *sink = opaque;
    if (sink->fail) return false;
    if (count > SIZE_MAX / sizeof(int16_t) - sink->count) return false;
    size_t needed = sink->count + count;
    if (needed > sink->capacity) {
        size_t capacity = needed * 2;
        int16_t *buffer = realloc(sink->samples, capacity * sizeof(*buffer));
        if (!buffer) return false;
        sink->samples = buffer;
        sink->capacity = capacity;
    }
    memcpy(sink->samples + sink->count, samples, count * sizeof(*samples));
    sink->count += count;
    return true;
}

static uint8_t *make_tone(uint32_t rate, size_t pairs, double offset, double phase)
{
    uint8_t *iq = malloc(pairs * 2);
    if (!iq) return NULL;
    for (size_t i = 0; i < pairs; i++) {
        double angle = SDR_RF_TWO_PI * offset * (double)i / rate + phase;
        iq[2 * i] = (uint8_t)lround(128.0 + 64.0 * cos(angle));
        iq[2 * i + 1] = (uint8_t)lround(128.0 + 64.0 * sin(angle));
    }
    return iq;
}

static sample_sink_t convert(uint32_t rate, uint64_t center,
                             const uint8_t *iq, size_t pairs, bool fragmented)
{
    sample_sink_t sink = {0};
    char error[192];
    gui_sdr_rf_t *state = gui_sdr_rf_create(rate, center, error, sizeof(error));
    expect_true(state != NULL, "valid stream creates");
    if (!state) {
        fprintf(stderr, "create: %s\n", error);
        return sink;
    }
    const size_t chunks[] = {1, 17, 4095, 8193, 3, 32771, 1024};
    size_t offset = 0, n = 0;
    bool success = true;
    while (offset < pairs && success) {
        size_t count = fragmented ? chunks[n++ % (sizeof(chunks) / sizeof(chunks[0]))] : pairs;
        if (count > pairs - offset) count = pairs - offset;
        success = gui_sdr_rf_process(state, iq + offset * 2, count, collect, &sink);
        offset += count;
    }
    expect_true(success, "all input pairs accepted");
    size_t before_finish = sink.count;
    success = gui_sdr_rf_finish(state, collect, &sink);
    expect_true(success, "normal EOF drains successfully");
    expect_true(sink.count == (size_t)llround((double)pairs * GUI_SDR_RF_OUTPUT_RATE_HZ / rate),
                "EOF output count is the rounded exact sample-rate ratio");
    if (pairs > 8192) expect_true(sink.count > before_finish, "EOF emits the buffered filter tail");
    size_t final_count = sink.count;
    expect_true(gui_sdr_rf_finish(state, collect, &sink) && sink.count == final_count,
                "repeated finish does not duplicate samples");
    expect_true(!gui_sdr_rf_process(state, iq, 0, collect, &sink), "closed stream rejects new input");
    expect_true(gui_sdr_rf_error(state)[0] != '\0', "closed stream reports an error");
    gui_sdr_rf_destroy(state);
    return sink;
}

static double tone_amplitude(const sample_sink_t *sink, double frequency, double *phase)
{
    const size_t margin = GUI_SDR_RF_OUTPUT_RATE_HZ / 1000u;
    if (sink->count <= 2 * margin) return 0.0;
    size_t count = sink->count - 2 * margin;
    // Whole 1 kHz periods prevent leakage between the test's integer-kHz tones.
    count -= count % margin;
    double real = 0, imag = 0;
    for (size_t j = 0; j < count; j++) {
        size_t i = margin + j;
        double angle = SDR_RF_TWO_PI * frequency * (double)i / GUI_SDR_RF_OUTPUT_RATE_HZ;
        real += sink->samples[i] * cos(angle);
        imag -= sink->samples[i] * sin(angle);
    }
    if (phase) *phase = atan2(imag, real);
    return count ? 2.0 * hypot(real, imag) / count : 0;
}

static double input_tone_amplitude(const uint8_t *iq, size_t pairs, uint32_t rate, double offset)
{
    double real = 0, imag = 0;
    for (size_t i = 0; i < pairs; i++) {
        double angle = SDR_RF_TWO_PI * offset * (double)i / rate;
        double a = ((int)iq[2 * i] - 128) / 128.0;
        double b = ((int)iq[2 * i + 1] - 128) / 128.0;
        real += a * cos(angle) + b * sin(angle);
        imag += b * cos(angle) - a * sin(angle);
    }
    return hypot(real, imag) / pairs * (32768.0 * GUI_SDR_RF_OUTPUT_GAIN);
}

static void check_tone(uint32_t rate, uint64_t center, double offset)
{
    const size_t pairs = rate / 50;
    const double input_phase = 0.37;
    uint8_t *iq = make_tone(rate, pairs, offset, input_phase);
    expect_true(iq != NULL, "tone allocation");
    if (!iq) return;
    sample_sink_t whole = convert(rate, center, iq, pairs, false);
    sample_sink_t pieces = convert(rate, center, iq, pairs, true);
    expect_true(whole.count == pieces.count, "fragmentation preserves output sample count");
    if (whole.count && whole.count == pieces.count) {
        expect_true(memcmp(whole.samples, pieces.samples, whole.count * sizeof(int16_t)) == 0,
                    "fragmentation preserves every signed16 output sample");
        double phase = 0;
        double wanted = tone_amplitude(&whole, (double)center + offset, &phase);
        double image = tone_amplitude(&whole, (double)center - offset, NULL);
        double input_image = input_tone_amplitude(iq, pairs, rate, -offset);
        expect_true(fabs(wanted - 8192.0) < 60.0, "expected RF carrier has the fixed -6 dB gain");
        // Quantizing a six-sample complex period to RTL's byte grid already
        // introduces a small image; conversion must not add another one.
        expect_true(image < wanted * 0.01 && image <= input_image + 8.0,
                    "I/Q pairing adds no mirrored carrier beyond native byte quantization");
        expect_true(fabs(phase - input_phase) < 0.01, "complex frequency shift preserves carrier phase");
        double resample_image = tone_amplitude(&whole, fabs((double)center + offset - rate), NULL);
        if (fabs((double)center + offset - rate) > 1000.0) {
            expect_true(resample_image < wanted * 0.003, "interpolation suppresses input-rate images");
        }
    }
    // A fresh state must restart the oscillator and filter, not reuse old phase.
    sample_sink_t restart = convert(rate, center, iq, pairs, false);
    expect_true(whole.count == restart.count &&
                (!whole.count || memcmp(whole.samples, restart.samples, whole.count * sizeof(int16_t)) == 0),
                "fresh recording resets filter and oscillator state");
    free(iq);
    free(whole.samples);
    free(pieces.samples);
    free(restart.samples);
}

static void check_noise_boundaries(void)
{
    const size_t pairs = 50003;
    uint8_t *iq = malloc(pairs * 2);
    expect_true(iq != NULL, "noise allocation");
    if (!iq) return;
    uint32_t random = 0x41346a3bu;
    for (size_t i = 0; i < pairs * 2; i++) {
        random = random * 1664525u + 1013904223u;
        iq[i] = (uint8_t)(random >> 24);
    }
    sample_sink_t whole = convert(1536000, 1600321, iq, pairs, false);
    sample_sink_t pieces = convert(1536000, 1600321, iq, pairs, true);
    expect_true(whole.count == pieces.count &&
                (!whole.count || memcmp(whole.samples, pieces.samples, whole.count * sizeof(int16_t)) == 0),
                "broadband rail-to-rail input is independent of caller block boundaries");
    free(iq);
    free(whole.samples);
    free(pieces.samples);
}

static bool count_silence(void *opaque, const int16_t *samples, size_t count)
{
    uint64_t *total = opaque;
    for (size_t i = 0; i < count; i++) if (samples[i]) return false;
    *total += count;
    return true;
}

static void check_bounded_stream(uint32_t rate)
{
    char error[192];
    uint8_t iq[8192];
    memset(iq, 128, sizeof(iq));
    uint64_t total = 0;
    gui_sdr_rf_t *state = gui_sdr_rf_create(rate, 1600000, error, sizeof(error));
    expect_true(state != NULL, "bounded streaming fixture creates");
    if (!state) return;
    bool success = true;
    for (unsigned i = 0; i < 128 && success; i++) {
        success = gui_sdr_rf_process(state, iq, sizeof(iq) / 2, count_silence, &total);
        expect_true(soxr_delay(state->resampler) < 32768.0,
                    "resampler backlog stays bounded while accepting a continuing stream");
    }
    expect_true(success, "continuing stream accepts every full block");
    expect_true(gui_sdr_rf_finish(state, count_silence, &total), "continuing stream drains");
    expect_true(total == (uint64_t)llround(128.0 * (sizeof(iq) / 2) * GUI_SDR_RF_OUTPUT_RATE_HZ / rate),
                "continuing stream does not duplicate or omit output at EOF");
    gui_sdr_rf_destroy(state);
}

static void check_stream_edges(void)
{
    char error[192];
    uint8_t iq[32];
    memset(iq, 128, sizeof(iq));
    sample_sink_t sink = {0};
    gui_sdr_rf_t *state = gui_sdr_rf_create(2400000, 1600000, error, sizeof(error));
    expect_true(state != NULL, "empty stream creates");
    expect_true(gui_sdr_rf_process(state, NULL, 0, collect, &sink), "zero input is not EOF");
    expect_true(gui_sdr_rf_finish(state, collect, &sink) && sink.count == 0, "empty EOF has no samples");
    gui_sdr_rf_destroy(state);

    for (size_t pairs = 1; pairs <= 16; pairs++) {
        sample_sink_t short_stream = convert(2048000, 1500000, iq, pairs, true);
        bool silent = true;
        for (size_t i = 0; i < short_stream.count; i++) silent &= short_stream.samples[i] == 0;
        expect_true(silent, "unsigned midpoint maps to exactly silent RF");
        free(short_stream.samples);
    }

    state = gui_sdr_rf_create(2400000, 1600000, error, sizeof(error));
    expect_true(!gui_sdr_rf_process(state, NULL, 1, collect, &sink), "NULL nonempty input is rejected");
    expect_true(!gui_sdr_rf_finish(state, collect, &sink), "invalid input prevents a clean EOF");
    expect_true(gui_sdr_rf_error(state)[0] != '\0', "input failure has a diagnostic");
    gui_sdr_rf_destroy(state);

    state = gui_sdr_rf_create(2400000, 1600000, error, sizeof(error));
    expect_true(!gui_sdr_rf_process(state, iq, SIZE_MAX, collect, &sink), "oversized pair count is rejected");
    gui_sdr_rf_destroy(state);

    state = gui_sdr_rf_create(2400000, 1600000, error, sizeof(error));
    expect_true(!gui_sdr_rf_process(state, iq, 1, NULL, NULL), "missing output callback is rejected");
    gui_sdr_rf_destroy(state);

    state = gui_sdr_rf_create(2400000, 1600000, error, sizeof(error));
    sink.fail = true;
    bool success = gui_sdr_rf_process(state, iq, 16, collect, &sink);
    if (success) success = gui_sdr_rf_finish(state, collect, &sink);
    expect_true(!success, "writer failure terminates the stream");
    expect_true(strstr(gui_sdr_rf_error(state), "write failed") != NULL, "writer failure diagnostic");
    expect_true(!gui_sdr_rf_process(state, iq, 1, collect, &sink), "writer failure is sticky");
    expect_true(!gui_sdr_rf_finish(state, collect, &sink), "writer failure cannot be flushed as success");
    gui_sdr_rf_destroy(state);

    expect_true(sdr_rf_quantize(0) == 0, "quantizer zero");
    expect_true(sdr_rf_quantize(1) == 16384 && sdr_rf_quantize(-1) == -16384, "quantizer headroom");
    expect_true(sdr_rf_quantize(10) == 32767, "positive overshoot saturates instead of wrapping");
    expect_true(sdr_rf_quantize(-10) == -32768, "negative overshoot saturates instead of wrapping");
    expect_true(sdr_rf_quantize(32767.75 / 16384.0) == 32767, "rounding cannot overflow positive int16");
    free(sink.samples);
}
#endif

int main(void)
{
    char error[192];
    const uint32_t invalid_input_rates[] = {0, 3000000u, 3200000u, UINT32_MAX};
    for (size_t i = 0; i < sizeof(invalid_input_rates) / sizeof(invalid_input_rates[0]); i++) {
        uint32_t input_rate = invalid_input_rates[i];
        expect_true(!gui_sdr_rf_validate(input_rate, 1500000u,
                                         error, sizeof(error)),
                    "reject zero input or an input band touching or crossing DC");
        expect_true(error[0] != '\0', "input rejection explains why");
        expect_true(!gui_sdr_rf_create(input_rate, 1500000u,
                                       error, sizeof(error)),
                    "invalid input does not create stream");
    }
    const uint32_t valid_input_rates[] = {250000u, 960000u, 2560000u};
    for (size_t i = 0; i < sizeof(valid_input_rates) / sizeof(valid_input_rates[0]); i++) {
        expect_true(gui_sdr_rf_validate(valid_input_rates[i], 1500000u, error, sizeof(error)),
                    "accept any positive input whose full translated band fits");
    }
    expect_true(gui_sdr_rf_validate(3200000u, 1800000u,
                                    error, sizeof(error)),
                "the same 3.2 MSPS input fits at a higher center");

    expect_true(!gui_sdr_rf_validate(2400000u, 3000000u, error, sizeof(error)),
                "reject a high-center band crossing the fixed 8 MSPS output Nyquist");
    const uint64_t invalid_centers[] = {0, 1200000, 2800000, 4000000, 100000000, UINT64_MAX};
    for (size_t i = 0; i < sizeof(invalid_centers) / sizeof(invalid_centers[0]); i++) {
        expect_true(!gui_sdr_rf_validate(2400000, invalid_centers[i], error, sizeof(error)), "reject RF band crossing DC or Nyquist");
        expect_true(error[0] != '\0', "RF geometry rejection explains why");
    }
    expect_true(gui_sdr_rf_validate(2400000, 1200001, NULL, 0), "valid band just above DC");
    expect_true(gui_sdr_rf_validate(2400000, 2799999, error, sizeof(error)), "valid band just below Nyquist");
    expect_true(error[0] == '\0', "validation success clears old error");
    expect_true(!gui_sdr_rf_process(NULL, NULL, 0, NULL, NULL), "NULL state rejects process");
    expect_true(!gui_sdr_rf_finish(NULL, NULL, NULL), "NULL state rejects finish");
    expect_true(gui_sdr_rf_error(NULL)[0] != '\0', "NULL state has a diagnostic");
    gui_sdr_rf_destroy(NULL);
    expect_true(gui_sdr_rf_passband_hz(0) == 0.0, "no passband for zero input rate");

#if LIBSOXR_ENABLED
    const double passband = gui_sdr_rf_passband_hz(2400000u);
    expect_true(passband >= 1070000.0 && passband < 1180000.0,
                "2.4 MSPS passband covers the accepted PAL edge but excludes the transition band");
    check_tone(2400000u, 2300000u, -1070000.0);
    const uint32_t rates[] = {1024000, 1200000, 1536000, 2048000, 2400000};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        check_tone(rates[i], 1600000, 200000.0);
        check_tone(rates[i], 1500000, -200000.0);
        check_bounded_stream(rates[i]);
    }
    check_tone(250000, 1500000, 40000.0);
    check_tone(960000, 1500000, -200000.0);
    check_tone(2560000, 1500000, 200000.0);
    check_tone(2399998, 1600123, 200000.0);
    check_noise_boundaries();
    check_stream_edges();
#else
    expect_true(!gui_sdr_rf_create(2400000, 1600000, error, sizeof(error)), "no-libsoxr build cannot create export");
    expect_true(strstr(error, "libsoxr") != NULL, "no-libsoxr build explains missing dependency");
#endif
    printf("gui_sdr_rf: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
