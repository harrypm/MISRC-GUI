/*
 * Dedicated Hi-Fi 5 MSPS resampling (gui_rf_resampler_create_hifi_narrow):
 * the Hi-Fi carrier band (1.2 - 2.0 MHz) passes flat, and video-FM energy
 * that would fold onto the carriers at 5 MSPS (3.0 - 4.4 MHz -> 0.6 - 2.0
 * MHz) is rejected. 20 -> 5 and 40 -> 5 use halfband stages; 28.636 -> 5
 * (CXADC) uses soxr when the build has it.
 * harrypm 2026-11-25: dedicated Hi-Fi formats sample at 5 MSPS, not 20.
 */
#include "../misrc_gui/signal/gui_rf_resampler.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int s_fail;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); s_fail++; } } while (0)

// Output amplitude of a full-scale/2 tone after resampling (steady state).
static double tone_gain(uint32_t in_rate, double hz) {
    gui_rf_resampler_t *r = gui_rf_resampler_create_hifi_narrow(in_rate);
    if (!r) return -1.0;
    const size_t n = 1u << 18;
    int16_t *in = malloc(n * sizeof(*in));
    const size_t cap = gui_rf_resampler_output_capacity_hifi_narrow(in_rate, n);
    float *out = malloc(cap * sizeof(*out));
    for (size_t i = 0; i < n; i++) in[i] = (int16_t)lrint(16384.0 * sin(2.0 * M_PI * hz * (double)i / in_rate));
    size_t oc = 0;
    const bool ok = gui_rf_resampler_process(r, in, n, out, cap, &oc);
    double peak = 0.0;
    for (size_t i = oc / 4; ok && i < oc; i++) if (fabs(out[i]) > peak) peak = fabs(out[i]);
    free(in);
    free(out);
    gui_rf_resampler_destroy(r);
    return ok ? peak / 0.5 : -1.0;
}

// Bounds are the previous path's own response (the decoder's 47-tap Kaiser
// front end at 20 MSPS): -0.4 dB at 1.95 MHz, -30 dB at the 3.1 MHz
// transition edge, -73 dB or better across the 3.4 - 4.4 MHz luma FM band.
// Measured 2026-11-25 for the halfband cascade: -0.3 / -33 / -71..-91 dB.
static void check_rate(uint32_t rate) {
    const double pass[] = { 1200000.0, 1300000.0, 1400000.0, 1700000.0, 1800000.0, 1950000.0 };
    const double luma[] = { 3400000.0, 3800000.0, 4400000.0 };
    double worst_pass = 0.0, worst_luma = -400.0;
    for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); i++) {
        const double db = 20.0 * log10(tone_gain(rate, pass[i]));
        if (fabs(db) > fabs(worst_pass)) worst_pass = db;
    }
    for (size_t i = 0; i < sizeof(luma) / sizeof(luma[0]); i++) {
        const double db = 20.0 * log10(tone_gain(rate, luma[i]) + 1e-12);
        if (db > worst_luma) worst_luma = db;
    }
    const double edge = 20.0 * log10(tone_gain(rate, 3100000.0) + 1e-12);
    printf("%6.3f -> 5 MSPS: carrier band worst %+.2f dB, 3.1 MHz edge %.1f dB, luma FM band worst %.1f dB\n",
           rate / 1e6, worst_pass, edge, worst_luma);
    CHECK(fabs(worst_pass) < 0.5, "Hi-Fi carrier band passes flat (within 0.5 dB)");
    CHECK(edge < -30.0, "3.1 MHz (folds to 1.9 MHz) at least as low as the old 20 MSPS front end");
    CHECK(worst_luma < -60.0, "3.4 - 4.4 MHz luma FM (folds onto the carriers) rejected");
}

int main(void) {
    CHECK(gui_rf_resampler_create_hifi_narrow(5000000u) == NULL, "input below 8 MSPS is refused");
    check_rate(20000000u);
    check_rate(40000000u);
    if (gui_rf_resampler_output_capacity_hifi_narrow(28636363u, 1)) check_rate(28636363u);
    else printf("28.636 -> 5 MSPS: soxr not in this build (skipped)\n");
    CHECK(gui_rf_resampler_output_capacity_hifi_narrow(40000000u, 65536) == 8192, "40 -> 5 capacity is exact 1/8");
    if (s_fail) return 1;
    printf("Hi-Fi narrow 5 MSPS: carriers flat, folding video band rejected\n");
    return 0;
}
