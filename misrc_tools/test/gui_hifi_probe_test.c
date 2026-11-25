/*
 * Hi-Fi carrier probe (signal/gui_hifi_probe.c): a VHS Hi-Fi FM pair is
 * found and told apart (NTSC 1.3 / 1.7 MHz vs PAL 1.4 / 1.8 MHz) at 20 and
 * 40 MSPS; video-like RF, noise and a lone tone are not Hi-Fi.
 * Real-data check (dev/prompt_video_field_view_readme.md): a Hi-Fi RF file
 * scored NTSC 0.86 mean, the Daimler video RF 0.10.
 */
#include "../misrc_gui/signal/gui_hifi_probe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int s_fail;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); s_fail++; } } while (0)

static double s_seed = 12345.0;
static double noise(void) {   // deterministic uniform -1..1
    s_seed = fmod(s_seed * 16807.0, 2147483647.0);
    return s_seed / 1073741823.5 - 1.0;
}

// FM carriers (each +-dev Hz at tone_hz), optional extra tone / noise.
static void synth(int16_t *out, size_t n, double rate, const double *carriers, int nc,
                  double dev, double extra_hz, double extra_amp, double noise_amp) {
    double ph[4] = {0};
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        double v = 0.0;
        for (int c = 0; c < nc; c++) {
            const double f = carriers[c] + dev * sin(2.0 * M_PI * (1000.0 + 300.0 * c) * t);
            ph[c] += 2.0 * M_PI * f / rate;
            v += 0.4 * cos(ph[c]);
        }
        if (extra_amp > 0.0) v += extra_amp * cos(2.0 * M_PI * extra_hz * t);
        v += noise_amp * noise();
        double s = v * 12000.0;
        if (s > 32767.0) s = 32767.0;
        if (s < -32768.0) s = -32768.0;
        out[i] = (int16_t)s;
    }
}

int main(void) {
    static int16_t buf[GUI_HIFI_PROBE_SAMPLES];
    gui_hifi_probe_result_t r;
    const double ntsc[2] = { 1300000.0, 1700000.0 }, pal[2] = { 1400000.0, 1800000.0 };
    const double rates[2] = { 20e6, 40e6 };
    for (int k = 0; k < 2; k++) {
        synth(buf, GUI_HIFI_PROBE_SAMPLES, rates[k], ntsc, 2, 30000.0, 0, 0, 0.05);
        CHECK(gui_hifi_probe_block(buf, GUI_HIFI_PROBE_SAMPLES, rates[k], &r), "probe runs");
        printf("NTSC pair @%.0f MSPS: ntsc %.3f pal %.3f beta %.3f\n", rates[k] / 1e6,
               r.score[GUI_HIFI_FMT_VHS_NTSC], r.score[GUI_HIFI_FMT_VHS_PAL], r.score[GUI_HIFI_FMT_BETA_NTSC]);
        CHECK(r.score[GUI_HIFI_FMT_VHS_NTSC] >= GUI_HIFI_PROBE_PRESENT, "NTSC pair found");
        CHECK(r.score[GUI_HIFI_FMT_VHS_PAL] < GUI_HIFI_PROBE_PRESENT, "NTSC pair is not PAL");

        synth(buf, GUI_HIFI_PROBE_SAMPLES, rates[k], pal, 2, 30000.0, 0, 0, 0.05);
        CHECK(gui_hifi_probe_block(buf, GUI_HIFI_PROBE_SAMPLES, rates[k], &r), "probe runs");
        printf("PAL pair  @%.0f MSPS: ntsc %.3f pal %.3f\n", rates[k] / 1e6,
               r.score[GUI_HIFI_FMT_VHS_NTSC], r.score[GUI_HIFI_FMT_VHS_PAL]);
        CHECK(r.score[GUI_HIFI_FMT_VHS_PAL] >= GUI_HIFI_PROBE_PRESENT, "PAL pair found");
        CHECK(r.score[GUI_HIFI_FMT_VHS_NTSC] < GUI_HIFI_PROBE_PRESENT, "PAL pair is not NTSC");
    }
    // Video-like RF: FM luma carrier at 3.8 MHz (+-500 kHz) + colour-under 629 kHz.
    {
        const double luma[1] = { 3800000.0 };
        synth(buf, GUI_HIFI_PROBE_SAMPLES, 20e6, luma, 1, 500000.0, 629000.0, 0.3, 0.2);
        CHECK(gui_hifi_probe_block(buf, GUI_HIFI_PROBE_SAMPLES, 20e6, &r), "probe runs");
        printf("video-like: ntsc %.3f pal %.3f\n", r.score[GUI_HIFI_FMT_VHS_NTSC], r.score[GUI_HIFI_FMT_VHS_PAL]);
        CHECK(r.score[GUI_HIFI_FMT_VHS_NTSC] < GUI_HIFI_PROBE_PRESENT &&
              r.score[GUI_HIFI_FMT_VHS_PAL] < GUI_HIFI_PROBE_PRESENT, "video RF is not Hi-Fi");
    }
    // White noise and a lone 1.3 MHz tone (one carrier of the pair).
    {
        synth(buf, GUI_HIFI_PROBE_SAMPLES, 20e6, NULL, 0, 0, 0, 0, 1.0);
        CHECK(gui_hifi_probe_block(buf, GUI_HIFI_PROBE_SAMPLES, 20e6, &r), "probe runs");
        printf("noise: ntsc %.3f pal %.3f\n", r.score[GUI_HIFI_FMT_VHS_NTSC], r.score[GUI_HIFI_FMT_VHS_PAL]);
        CHECK(r.score[GUI_HIFI_FMT_VHS_NTSC] < 0.3f && r.score[GUI_HIFI_FMT_VHS_PAL] < 0.3f, "noise is not Hi-Fi");
        synth(buf, GUI_HIFI_PROBE_SAMPLES, 20e6, NULL, 0, 0, 1300000.0, 0.5, 0.02);
        CHECK(gui_hifi_probe_block(buf, GUI_HIFI_PROBE_SAMPLES, 20e6, &r), "probe runs");
        printf("lone tone: ntsc %.3f\n", r.score[GUI_HIFI_FMT_VHS_NTSC]);
        CHECK(r.score[GUI_HIFI_FMT_VHS_NTSC] < GUI_HIFI_PROBE_PRESENT, "one carrier is not a pair");
    }
    // Contract: short blocks and rates that cannot hold the band are refused.
    CHECK(!gui_hifi_probe_block(buf, GUI_HIFI_PROBE_SAMPLES - 1, 20e6, &r), "short block refused");
    CHECK(!gui_hifi_probe_block(buf, GUI_HIFI_PROBE_SAMPLES, 4e6, &r), "low rate refused");
    if (s_fail) return 1;
    printf("Hi-Fi probe: pairs found + told apart at 20 / 40 MSPS, video / noise / tone rejected\n");
    return 0;
}
