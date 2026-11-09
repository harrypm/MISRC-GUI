/* MISRC GUI LaserDisc RF preview. GPL-3.0-or-later. */
#include "gui_ld_rf_demod.h"

#include <math.h>
#include <string.h>

#define LD_PI 3.14159265358979323846
#define LD_FM_RATE_HZ (GUI_LD_RF_INPUT_RATE_HZ / 2u)

/* Analytic FIR center plus the half-interval phase discriminator delay,
 * measured in 5 MSPS output samples. */
static const double ld_fm_delay = (GUI_LD_RF_COLOR_FIR_HALF + 1.0) / GUI_LD_RF_DECIMATION;

static const double ld_butterworth6_q[] = {0.5176380902, 0.7071067812, 1.931851653};

static gui_ld_rf_biquad_t ld_lowpass(double rate, double cutoff, double q) {
    double angle = 2.0 * LD_PI * cutoff / rate;
    double cosine = cos(angle);
    double alpha = sin(angle) / (2.0 * q);
    double divisor = 1.0 + alpha;
    float b = (float)((1.0 - cosine) / (2.0 * divisor));
    gui_ld_rf_biquad_t result = {
        b, 2.0f * b, b,
        (float)(-2.0 * cosine / divisor), (float)((1.0 - alpha) / divisor)
    };
    return result;
}

static inline float ld_filter(const gui_ld_rf_biquad_t *filter, float state[2], float sample) {
    float value = filter->b0 * sample + state[0];
    state[0] = filter->b1 * sample - filter->a1 * value + state[1];
    state[1] = filter->b2 * sample - filter->a2 * value;
    return value;
}

static double ld_filter_delay(const gui_ld_rf_biquad_t *filter) {
    double numerator = filter->b0 + filter->b1 + filter->b2;
    double denominator = 1.0 + filter->a1 + filter->a2;
    return (filter->b1 + 2.0 * filter->b2) / numerator -
           (filter->a1 + 2.0 * filter->a2) / denominator;
}

static inline float ld_phase(float y, float x) {
    /* Range-reduced degree-11 atan approximation. Both FM callers have
     * nonzero carrier pairs, so the larger component is safe to divide by. */
    const float ay = fabsf(y), ax = fabsf(x);
    const bool steep = ay > ax;
    const float r = steep ? ax / ay : ay / ax;
    const float t = r * r;
    float angle = r * (0.99997726f + t * (-0.33262347f + t * (0.19354346f +
                  t * (-0.11643287f + t * (0.05265332f - t * 0.01172120f)))));
    if (steep) angle = 1.57079632679f - angle;
    if (x < 0.0f) angle = 3.14159265359f - angle;
    return copysignf(angle, y);
}

static inline float ld_finite_rf(float sample) {
    /* Fast-math builds can assume isfinite() is always true. */
    uint32_t bits;
    memcpy(&bits, &sample, sizeof(bits));
    return (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) ? 0.0f : sample;
}

static void ld_analytic_reset(gui_ld_rf_analytic_t *color, bool ntsc, double blank, double hz_per_ire,
                           double zero, double pole) {
    static const float window[] = {
        1.000000000f, 0.996128517f, 0.984586396f, 0.965588772f, 0.939488108f,
        0.906765228f, 0.868017159f, 0.823942119f, 0.775322104f, 0.723003598f,
        0.667876949f, 0.610855027f, 0.552851770f, 0.494761220f, 0.437437637f,
        0.381677226f, 0.328201957f, 0.277645894f, 0.230544334f, 0.187326003f,
        0.148308430f, 0.113696521f, 0.083584271f, 0.057959430f, 0.036710892f
    };
    const double fsc = ntsc ? 315000000.0 / 88.0 : 4433618.75;
    const double low = ntsc ? 3450000.0 : 2100000.0;
    const double high = ntsc ? 9750000.0 : 9600000.0;
    const double width = (high - low) / GUI_LD_RF_INPUT_RATE_HZ;
    const double center = (high + low) * LD_PI / GUI_LD_RF_INPUT_RATE_HZ;
    /* A 49-tap Kaiser-windowed analytic bandpass preserves the lower chroma
     * sideband while rejecting audio and the conjugate RF image. */
    color->taps_i[0] = (float)width;
    for (unsigned k = 1; k <= GUI_LD_RF_COLOR_FIR_HALF; ++k) {
        double tap = window[k] * sin(LD_PI * width * k) / (LD_PI * k);
        color->taps_i[k] = (float)(tap * cos(center * k));
        color->taps_q[k] = (float)(tap * sin(center * k));
    }
    const double blank_phase = 2.0 * LD_PI * blank / LD_FM_RATE_HZ;
    color->blank_i = (float)cos(blank_phase);
    color->blank_q = (float)sin(blank_phase);
    color->ire_per_radian = (float)(LD_FM_RATE_HZ / (2.0 * LD_PI * hz_per_ire));

    const double sc_phase = 2.0 * LD_PI * fsc / LD_FM_RATE_HZ;
    color->step_i = cos(sc_phase);
    color->step_q = sin(sc_phase);
    /* Match the center of the chroma passband after de-emphasis and compensate
     * the finite interval of the 10 MSPS phase discriminator. */
    const double p = 2.0 * LD_PI * fsc * pole;
    const double z = 2.0 * LD_PI * fsc * zero;
    const double gain = 0.02 * (sc_phase / 2.0) / (sin(sc_phase / 2.0) * (1.0 + p * p));
    color->scale_i = (float)(gain * (1.0 + p * z));
    color->scale_q = (float)(gain * (z - p));
    color->delay = ld_fm_delay;
    for (int k = 0; k < 3; ++k) {
        color->filter[k] = ld_lowpass(LD_FM_RATE_HZ, ntsc ? 550000.0 : 460000.0,
                                      ld_butterworth6_q[k]);
        color->delay += ld_filter_delay(&color->filter[k]) * GUI_LD_RF_LUMA_RATE_HZ / LD_FM_RATE_HZ;
    }
}

void gui_ld_rf_demod_reset(gui_ld_rf_demod_t *demod, gui_ld_rf_system_t system) {
    if (!demod) return;
    memset(demod, 0, sizeof(*demod));
    demod->system = system;
    bool ntsc = system == GUI_LD_RF_NTSC;
    /* IEC 60856 PAL: 7.1 MHz blank, 800 kHz to +100 IRE.
     * IEC 60857 NTSC: 8.1 MHz blank, 1.7 MHz from -40 to +100 IRE. */
    double blank = ntsc ? 8100000.0 : 7100000.0;
    double hz_per_ire = ntsc ? 1700000.0 / 140.0 : 8000.0;
    /* The analytic discriminator already runs at 10 MSPS for color. Use
     * it for Y as well, with a 1.8 MHz low-pass before the 5 MSPS picture
     * grid. This recovers lower RF sideband detail and rejects composite
     * chroma before decimation. */
    demod->luma_delay = ld_fm_delay;
    for (int i = 0; i < 3; ++i) {
        demod->luma_filter[i] = ld_lowpass(LD_FM_RATE_HZ, 1800000.0, ld_butterworth6_q[i]);
        demod->luma_delay += ld_filter_delay(&demod->luma_filter[i]) * GUI_LD_RF_LUMA_RATE_HZ / LD_FM_RATE_HZ;
    }

    /* Bilinear first-order video de-emphasis from the LD profile time constants. */
    double zero = ntsc ? 120e-9 : 100e-9;
    double pole = ntsc ? 320e-9 : 400e-9;
    double scale = 2.0 * LD_FM_RATE_HZ;
    double divisor = 1.0 + scale * pole;
    demod->deemph_b0 = (float)((1.0 + scale * zero) / divisor);
    demod->deemph_b1 = (float)((1.0 - scale * zero) / divisor);
    demod->deemph_a1 = (float)((1.0 - scale * pole) / divisor);
    demod->luma_delay += (pole - zero) * GUI_LD_RF_LUMA_RATE_HZ;
    ld_analytic_reset(&demod->analytic, ntsc, blank, hz_per_ire, zero, pole);
}

double gui_ld_rf_demod_luma_delay(const gui_ld_rf_demod_t *demod) {
    return demod ? demod->luma_delay : 0.0;
}

double gui_ld_rf_demod_chroma_delay(const gui_ld_rf_demod_t *demod) {
    return demod ? demod->analytic.delay : 0.0;
}

static void ld_analytic_start(gui_ld_rf_analytic_t *color, gui_ld_rf_system_t system, uint64_t first) {
    bool ntsc = system == GUI_LD_RF_NTSC;
    uint64_t period = ntsc ? 352u : 3200000u;
    uint64_t cycles = ntsc ? 63u : 709379u;
    /* The FIR center and half-interval discriminator delay are both fixed.
     * Seed the subcarrier on the corresponding absolute source tick. */
    uint64_t tick = (first % period + (first & 1u) + period - GUI_LD_RF_COLOR_FIR_HALF - 1u) % period;
    double angle = 2.0 * LD_PI * (double)(tick * cycles % period) / (double)period;
    color->oscillator_i = cos(angle);
    color->oscillator_q = sin(angle);
    color->warmup = 2u * GUI_LD_RF_COLOR_FIR_HALF;
}

static size_t ld_decode_block(gui_ld_rf_demod_t *demod, const float *rf, size_t count,
                              uint64_t first, float *luma, float *chroma_i, float *chroma_q) {
    gui_ld_rf_analytic_t *color = &demod->analytic;
    double osc_i = color->oscillator_i;
    double osc_q = color->oscillator_q;
    size_t produced = 0;
    for (size_t n = 0; n < count; ++n) {
        unsigned cursor = color->cursor;
        float sample = ld_finite_rf(rf[n]);
        color->history[cursor] = sample;
        color->history[cursor + GUI_LD_RF_COLOR_HISTORY] = sample;
        color->cursor = (cursor + 1u) & (GUI_LD_RF_COLOR_HISTORY - 1u);
        bool warming = color->warmup != 0;
        if (warming) --color->warmup;
        uint64_t tick = first + n;
        if (tick & 1u) continue;

        const float *center = &color->history[cursor + GUI_LD_RF_COLOR_HISTORY - GUI_LD_RF_COLOR_FIR_HALF];
        float i = center[0] * color->taps_i[0];
        float q = 0.0f;
        for (int k = 1; k <= GUI_LD_RF_COLOR_FIR_HALF; ++k) {
            float older = center[-k], newer = center[k];
            i += color->taps_i[k] * (older + newer);
            q += color->taps_q[k] * (older - newer);
        }
        float ire = 0.0f;
        bool carrier = !warming && i*i + q*q > 1e-10f;
        if (carrier && color->have_iq) {
            float cross = color->prev_i * q - color->prev_q * i;
            float dot = color->prev_i * i + color->prev_q * q;
            float relative_i = dot * color->blank_i + cross * color->blank_q;
            float relative_q = cross * color->blank_i - dot * color->blank_q;
            ire = ld_phase(relative_q, relative_i) * color->ire_per_radian;
            ire = fmaxf(-110.0f, fminf(150.0f, ire));
        }
        color->prev_i = i;
        color->prev_q = q;
        color->have_iq = carrier;

        float value = demod->deemph_b0 * ire + demod->deemph_b1 * demod->deemph_input -
                      demod->deemph_a1 * demod->deemph_output;
        demod->deemph_input = ire;
        demod->deemph_output = value;
        for (int stage = 0; stage < 3; ++stage)
            value = ld_filter(&demod->luma_filter[stage], demod->luma_state[stage], value);

        float c_i = (float)(ire * osc_i);
        float c_q = (float)(-ire * osc_q);
        for (int k = 0; k < 3; ++k) {
            c_i = ld_filter(&color->filter[k], color->filter_i[k], c_i);
            c_q = ld_filter(&color->filter[k], color->filter_q[k], c_q);
        }
        if (!(tick % GUI_LD_RF_DECIMATION)) {
            luma[produced] = value;
            chroma_i[produced] = color->scale_i * c_i - color->scale_q * c_q;
            chroma_q[produced++] = color->scale_i * c_q + color->scale_q * c_i;
        }
        double next_i = osc_i * color->step_i - osc_q * color->step_q;
        osc_q = osc_i * color->step_q + osc_q * color->step_i;
        osc_i = next_i;
        if ((tick & 4095u) == 0u) {
            double norm = 1.0 / sqrt(osc_i * osc_i + osc_q * osc_q);
            osc_i *= norm;
            osc_q *= norm;
        }
    }
    color->oscillator_i = osc_i;
    color->oscillator_q = osc_q;
    return produced;
}

size_t gui_ld_rf_demod_process(gui_ld_rf_demod_t *demod, const float *rf, size_t count,
                              uint64_t first, float *luma, float *chroma_i,
                              float *chroma_q, size_t capacity) {
    if (!demod || !rf || !count || !chroma_i || !chroma_q || count > UINT64_MAX - first) return 0;
    unsigned phase = (unsigned)(first % GUI_LD_RF_DECIMATION);
    unsigned first_tick = (GUI_LD_RF_DECIMATION - phase) % GUI_LD_RF_DECIMATION;
    size_t required = count > first_tick ? 1 + (count - 1 - first_tick) / GUI_LD_RF_DECIMATION : 0;
    if (required > capacity || (required && !luma)) return 0;

    if (demod->have_input && first != demod->next_sample)
        gui_ld_rf_demod_reset(demod, demod->system);
    if (!demod->have_input) ld_analytic_start(&demod->analytic, demod->system, first);
    demod->have_input = true;
    demod->next_sample = first + count;
    return ld_decode_block(demod, rf, count, first, luma, chroma_i, chroma_q);
}
