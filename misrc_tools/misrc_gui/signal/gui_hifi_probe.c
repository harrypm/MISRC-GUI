#include "gui_hifi_probe.h"

#include <math.h>

#define PROBE_LO_HZ 900000.0
#define PROBE_HI_HZ 2200000.0
#define PROBE_HALF_WINDOW_HZ 45000.0
#define PROBE_MAX_CARRIERS 4

static const struct {
    int count;
    double hz[PROBE_MAX_CARRIERS];
} s_carriers[GUI_HIFI_FMT_COUNT] = {
    [GUI_HIFI_FMT_VHS_NTSC]  = { 2, { 1300000.0, 1700000.0 } },
    [GUI_HIFI_FMT_VHS_PAL]   = { 2, { 1400000.0, 1800000.0 } },
    [GUI_HIFI_FMT_BETA_NTSC] = { 4, { 1380000.0, 1530000.0, 1680000.0, 1830000.0 } },
};

const char *gui_hifi_fmt_name(gui_hifi_fmt_t fmt) {
    switch (fmt) {
        case GUI_HIFI_FMT_VHS_NTSC: return "VHS NTSC";
        case GUI_HIFI_FMT_VHS_PAL: return "VHS PAL";
        case GUI_HIFI_FMT_BETA_NTSC: return "Beta NTSC";
        default: return "?";
    }
}

bool gui_hifi_probe_block(const int16_t *samples, size_t count, double rate_hz,
                          gui_hifi_probe_result_t *out) {
    if (!samples || !out || count < GUI_HIFI_PROBE_SAMPLES || !(rate_hz >= 2.0 * PROBE_HI_HZ * 1.05))
        return false;
    const size_t n = GUI_HIFI_PROBE_SAMPLES;
    static float x[GUI_HIFI_PROBE_SAMPLES];   // single caller thread (the extraction thread / tests)
    double mean = 0.0;
    for (size_t i = 0; i < n; i++) mean += samples[i];
    mean /= (double)n;
    const double two_pi = 6.283185307179586;
    for (size_t i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * cos(two_pi * (double)i / (double)(n - 1));
        x[i] = (float)(((double)samples[i] - mean) * w);
    }
    const double df = rate_hz / (double)n;
    const int k0 = (int)ceil(PROBE_LO_HZ / df), k1 = (int)floor(PROBE_HI_HZ / df);
    double total = 0.0;
    double in_win[GUI_HIFI_FMT_COUNT][PROBE_MAX_CARRIERS] = {{0}};
    for (int k = k0; k <= k1; k++) {
        const double f = (double)k * df;
        const float coeff = (float)(2.0 * cos(two_pi * (double)k / (double)n));
        float s1 = 0.0f, s2 = 0.0f;
        for (size_t i = 0; i < n; i++) {
            const float s = x[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        const double p = (double)s1 * s1 + (double)s2 * s2 - (double)coeff * s1 * s2;
        total += p;
        for (int fmt = 0; fmt < GUI_HIFI_FMT_COUNT; fmt++)
            for (int c = 0; c < s_carriers[fmt].count; c++)
                if (fabs(f - s_carriers[fmt].hz[c]) <= PROBE_HALF_WINDOW_HZ) in_win[fmt][c] += p;
    }
    out->band_power = (float)(total / (double)(k1 - k0 + 1));
    for (int fmt = 0; fmt < GUI_HIFI_FMT_COUNT; fmt++) {
        double sum = 0.0, lo = INFINITY, hi = 0.0;
        for (int c = 0; c < s_carriers[fmt].count; c++) {
            sum += in_win[fmt][c];
            if (in_win[fmt][c] < lo) lo = in_win[fmt][c];
            if (in_win[fmt][c] > hi) hi = in_win[fmt][c];
        }
        double score = total > 0.0 ? sum / total : 0.0;
        // Every carrier of the set must be there: one strong tone (a spur,
        // the colour-under's harmonic) is not a Hi-Fi pair.
        if (hi > 0.0 && lo / hi < 0.05) score *= 0.25;
        out->score[fmt] = (float)score;
    }
    return true;
}
