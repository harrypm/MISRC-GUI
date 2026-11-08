/* MISRC GUI LaserDisc analog FM audio preview. GPL-3.0-or-later. */
#include "gui_ld_rf_analog.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LD_AUDIO_PI 3.14159265358979323846
#define LD_AUDIO_RF_RATE (GUI_LD_RF_INPUT_RATE_HZ / 2u)
#define LD_AUDIO_IQ_RATE 625000u
#define LD_AUDIO_DECIMATION (GUI_LD_RF_INPUT_RATE_HZ / LD_AUDIO_IQ_RATE)

enum {
    LD_AUDIO_RF_HISTORY = 32,
    LD_AUDIO_IQ_HISTORY = 256,
    LD_AUDIO_IQ_HALF = 80,
    LD_AUDIO_LOCK_WINDOW = LD_AUDIO_IQ_RATE / 100,
    LD_AUDIO_PCM_BATCH = 128
};

typedef struct {
    float b0, b1, b2, a1, a2;
} ld_audio_sos_t;

/* Elliptic 19.5 kHz passband, 0.25 dB ripple, 55 dB above 24 kHz at 625 kHz. */
static const ld_audio_sos_t ld_audio_antialias[] = {
    {0.00103665888f, 0.00103665888f, 0.0f, -0.921102253f, 0.0f},
    {1.0f, -1.81679765f, 1.0f, -1.87139197f, 0.887700025f},
    {1.0f, -1.92632668f, 1.0f, -1.91705133f, 0.948473884f},
    {1.0f, -1.94358771f, 1.0f, -1.94745285f, 0.986498507f}
};

typedef struct {
    float tap_i[LD_AUDIO_IQ_HALF + 1], tap_q[LD_AUDIO_IQ_HALF + 1];
    float center_i, center_q, prev_i, prev_q;
    float deemphasis, dc, filtered, fade;
    float antialias[4][2];
    double power_sum, power_squared_sum, detune_sum;
    float power_reference, previous_detune, previous_delta;
    unsigned valid, good_windows, valid_streak, rough;
    bool have_iq, locked;
} ld_audio_channel_t;

struct gui_ld_rf_analog {
    gui_ld_rf_system_t system;
    ld_audio_channel_t channel[2];
    float rf[2 * LD_AUDIO_RF_HISTORY], iq[2 * LD_AUDIO_IQ_HISTORY];
    float rf_taps[8];
    float pcm[2 * LD_AUDIO_PCM_BATCH];
    uint64_t next_sample;
    double rf_power_sum;
    unsigned rf_cursor, iq_cursor, window_samples, pcm_phase, pcm_count;
    bool have_input;
};

static double ld_audio_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k <= 12; ++k) {
        term *= (x * x) / (4.0 * k * k);
        sum += term;
    }
    return sum;
}

static double ld_audio_window(int offset, int half, double beta, double normalization) {
    double x = (double)offset / half;
    return ld_audio_i0(beta * sqrt(1.0 - x * x)) / normalization;
}

void gui_ld_rf_analog_reset(gui_ld_rf_analog_t *demod, gui_ld_rf_system_t system) {
    if (!demod) return;
    memset(demod, 0, sizeof(*demod));
    demod->system = system;

    /* Sparse 31-tap halfband. For NTSC, retain both sound carriers below
     * 3 MHz while keeping the 7–9 MHz video carrier out of the decimated RF. */
    const double halfband_beta = 6.5;
    double normalization = ld_audio_i0(halfband_beta);
    for (int k = 0; k < 8; ++k) {
        int offset = 2 * k + 1;
        double sinc = (k & 1 ? -1.0 : 1.0) / (LD_AUDIO_PI * offset);
        demod->rf_taps[k] = (float)(sinc * ld_audio_window(offset, 15, halfband_beta, normalization));
    }

    /* IEC 60857/60856 channel carriers. NTSC derives both from exact fH;
     * PAL digital-audio discs omit these two analog subcarriers. */
    const bool ntsc = system == GUI_LD_RF_NTSC;
    const double ntsc_fh = 315000000.0 / 88.0 / 227.5;
    const double carriers[2] = {
        ntsc ? 146.25 * ntsc_fh : 683593.75,
        ntsc ? 178.75 * ntsc_fh : 1066406.25
    };
    const double cutoff = (ntsc ? 180000.0 : 170000.0) / LD_AUDIO_RF_RATE;
    const double beta = 5.5;
    normalization = ld_audio_i0(beta);
    for (int c = 0; c < 2; ++c) {
        ld_audio_channel_t *ch = &demod->channel[c];
        double step = 2.0 * LD_AUDIO_PI * carriers[c];
        ch->center_i = (float)cos(step / LD_AUDIO_IQ_RATE);
        ch->center_q = (float)sin(step / LD_AUDIO_IQ_RATE);
        ch->tap_i[0] = (float)(2.0 * cutoff);
        for (int k = 1; k <= LD_AUDIO_IQ_HALF; ++k) {
            double lowpass = sin(2.0 * LD_AUDIO_PI * cutoff * k) / (LD_AUDIO_PI * k);
            double tap = lowpass * ld_audio_window(k, LD_AUDIO_IQ_HALF, beta, normalization);
            double angle = step * k / LD_AUDIO_RF_RATE;
            ch->tap_i[k] = (float)(tap * cos(angle));
            ch->tap_q[k] = (float)(tap * sin(angle));
        }
    }
}

gui_ld_rf_analog_t *gui_ld_rf_analog_create(void) {
    gui_ld_rf_analog_t *demod = malloc(sizeof(*demod));
    gui_ld_rf_analog_reset(demod, GUI_LD_RF_NTSC);
    return demod;
}

void gui_ld_rf_analog_destroy(gui_ld_rf_analog_t *demod) {
    free(demod);
}

bool gui_ld_rf_analog_locked(const gui_ld_rf_analog_t *demod) {
    return demod && (demod->channel[0].locked || demod->channel[1].locked);
}

static inline float ld_audio_rf_value(float sample) {
    uint32_t bits;
    memcpy(&bits, &sample, sizeof(bits));
    /* Fast-math can optimize away isfinite(). */
    return (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) ? 0.0f : sample;
}

static inline float ld_audio_discriminator(ld_audio_channel_t *ch, float i, float q) {
    float power = i*i + q*q;
    float detune = 0.0f;
    bool usable = power > 2e-9f && power > 0.06f * ch->power_reference;
    bool valid = usable && ch->have_iq;
    if (valid) {
        float cross = ch->prev_i * q - ch->prev_q * i;
        float dot = ch->prev_i * i + ch->prev_q * q;
        float re = dot * ch->center_i + cross * ch->center_q;
        float im = cross * ch->center_i - dot * ch->center_q;
        detune = atan2f(im, re) * (float)(LD_AUDIO_IQ_RATE / (2.0 * LD_AUDIO_PI));
        valid = fabsf(detune) <= 165000.0f;
    }
    ch->prev_i = i;
    ch->prev_q = q;
    ch->have_iq = usable;
    ch->power_sum += power;
    ch->power_squared_sum += (double)power * power;
    if (valid) {
        float delta = detune - ch->previous_detune;
        /* A 20 kHz tone at 150 kHz deviation changes its per-sample
         * frequency slope by less than 6 kHz on the 625 kHz grid. */
        if (ch->valid_streak >= 2u && fabsf(delta - ch->previous_delta) > 22000.0f)
            ++ch->rough;
        ch->previous_detune = detune;
        ch->previous_delta = delta;
        if (ch->valid_streak < 2u) ++ch->valid_streak;
        ch->detune_sum += detune;
        ++ch->valid;
    } else {
        ch->valid_streak = 0;
    }
    return valid ? detune : 0.0f;
}

static void ld_audio_update_lock(gui_ld_rf_analog_t *demod) {
    double rf_power = demod->rf_power_sum / LD_AUDIO_LOCK_WINDOW;
    const ld_audio_channel_t *left = &demod->channel[0];
    double left_detune = left->valid > 0.9 * LD_AUDIO_LOCK_WINDOW ? left->detune_sum / left->valid : 0.0;
    for (int c = 0; c < 2; ++c) {
        ld_audio_channel_t *ch = &demod->channel[c];
        double power = ch->power_sum / LD_AUDIO_LOCK_WINDOW;
        double mean_detune = ch->valid ? ch->detune_sum / ch->valid : 0.0;
        double power_variance = ch->power_squared_sum / LD_AUDIO_LOCK_WINDOW - power * power;
        /* A valid FM carrier has a persistent envelope, a center near its
         * specified frequency and bounded deviation. */
        bool good = power > 2e-8 && power > 1e-5 * rf_power &&
                    power_variance < 0.6 * power * power &&
                    fabs(mean_detune) < 40000.0 && ch->valid > 0.88 * LD_AUDIO_LOCK_WINDOW;
        /* On NTSC AC-3 discs the right channel can contain QPSK at 2.88 MHz.
         * Mute when the residual upward frequency shift, envelope modulation
         * and rapid changes of frequency slope agree. Speed error moves both
         * analog carriers in proportion to their nominal frequencies. */
        if (c == 1 && demod->system == GUI_LD_RF_NTSC &&
            mean_detune - left_detune * (178.75 / 146.25) > 14000.0 &&
            power_variance > 0.12 * power * power && ch->rough > LD_AUDIO_LOCK_WINDOW / 4u)
            good = false;
        if (good) {
            if (ch->good_windows < 2u) ++ch->good_windows;
            ch->locked = ch->good_windows == 2u;
            ch->power_reference = (float)power;
        } else {
            ch->good_windows = 0;
            ch->locked = false;
            ch->power_reference = 0.0f;
        }
        ch->power_sum = ch->power_squared_sum = ch->detune_sum = 0.0;
        ch->valid = ch->rough = 0;
    }
    demod->rf_power_sum = 0.0;
    demod->window_samples = 0;
}

static inline float ld_audio_condition(ld_audio_channel_t *ch, float deviation) {
    /* IEC 60856/60857 75 us de-emphasis, then remove carrier/DC offset.
     * Use ld-decode's signed PCM normalization to match its analog track level. */
    const float emphasis_alpha = 0.021107f;
    const float dc_alpha = 0.0002513f;
    const float hz_to_pcm = -1.0f / 371081.0f;
    ch->deemphasis += emphasis_alpha * (deviation * hz_to_pcm - ch->deemphasis);
    ch->dc += dc_alpha * (ch->deemphasis - ch->dc);
    float y = ch->deemphasis - ch->dc;
    for (unsigned k = 0; k < sizeof(ld_audio_antialias) / sizeof(ld_audio_antialias[0]); ++k) {
        const ld_audio_sos_t *f = &ld_audio_antialias[k];
        float *z = ch->antialias[k];
        float next = f->b0 * y + z[0];
        z[0] = f->b1 * y - f->a1 * next + z[1];
        z[1] = f->b2 * y - f->a2 * next;
        y = next;
    }
    return y;
}

static void ld_audio_emit(gui_ld_rf_analog_t *demod, uint64_t end_sample,
                          gui_ld_rf_analog_pcm_callback_t callback, void *context) {
    if (callback && demod->pcm_count) callback(context, demod->pcm, demod->pcm_count, end_sample);
    demod->pcm_count = 0;
}

static void ld_audio_iq_tick(gui_ld_rf_analog_t *demod, const float *center, float rf,
                             uint64_t end_sample, gui_ld_rf_analog_pcm_callback_t callback,
                             void *context) {
    ld_audio_channel_t *left = &demod->channel[0], *right = &demod->channel[1];
    float li = center[0] * left->tap_i[0], lq = 0.0f;
    float ri = center[0] * right->tap_i[0], rq = 0.0f;
    for (int k = 1; k <= LD_AUDIO_IQ_HALF; ++k) {
        float older = center[-k], newer = center[k];
        float sum = older + newer, difference = older - newer;
        li += left->tap_i[k] * sum;
        lq += left->tap_q[k] * difference;
        ri += right->tap_i[k] * sum;
        rq += right->tap_q[k] * difference;
    }
    float audio[2] = {
        ld_audio_condition(left, ld_audio_discriminator(left, li, lq)),
        ld_audio_condition(right, ld_audio_discriminator(right, ri, rq))
    };
    demod->rf_power_sum += (double)rf * rf;
    if (++demod->window_samples == LD_AUDIO_LOCK_WINDOW) ld_audio_update_lock(demod);

    /* 48/625 of the 625 kHz grid, with interpolation at each exact tick. */
    demod->pcm_phase += 48u;
    if (demod->pcm_phase >= 625u) {
        demod->pcm_phase -= 625u;
        float t = 1.0f - (float)demod->pcm_phase / 48.0f;
        for (int c = 0; c < 2; ++c) {
            ld_audio_channel_t *ch = &demod->channel[c];
            float pcm = ch->filtered + t * (audio[c] - ch->filtered);
            const float step = 1.0f / 96.0f;
            ch->fade = ch->locked ? fminf(1.0f, ch->fade + step) : fmaxf(0.0f, ch->fade - step);
            demod->pcm[2 * demod->pcm_count + c] = ch->fade * fmaxf(-1.0f, fminf(1.0f, pcm));
        }
        if (++demod->pcm_count == LD_AUDIO_PCM_BATCH) ld_audio_emit(demod, end_sample, callback, context);
    }
    left->filtered = audio[0];
    right->filtered = audio[1];
}

void gui_ld_rf_analog_process(gui_ld_rf_analog_t *demod, const float *samples, size_t count,
                              uint64_t first_sample, gui_ld_rf_analog_pcm_callback_t callback,
                              void *context) {
    if (!demod || !samples || !count || count > UINT64_MAX - first_sample) return;
    if (demod->have_input && first_sample != demod->next_sample)
        gui_ld_rf_analog_reset(demod, demod->system);
    if (!demod->have_input) {
        uint64_t first_iq = first_sample / LD_AUDIO_DECIMATION + (first_sample % LD_AUDIO_DECIMATION != 0u);
        demod->pcm_phase = (unsigned)(first_iq % 625u) * 48u % 625u;
        demod->have_input = true;
    }
    demod->next_sample = first_sample + count;
    for (size_t k = 0; k < count; ++k) {
        uint64_t tick = first_sample + k;
        float raw = ld_audio_rf_value(samples[k]);
        unsigned cursor = demod->rf_cursor;
        demod->rf[cursor] = raw;
        demod->rf[cursor + LD_AUDIO_RF_HISTORY] = raw;
        demod->rf_cursor = (cursor + 1u) & (LD_AUDIO_RF_HISTORY - 1u);
        if (tick & 1u) continue;

        const float *center = &demod->rf[cursor + LD_AUDIO_RF_HISTORY - 15];
        float rf = 0.5f * center[0];
        for (int i = 0; i < 8; ++i) {
            int offset = 2 * i + 1;
            rf += demod->rf_taps[i] * (center[-offset] + center[offset]);
        }
        cursor = demod->iq_cursor;
        demod->iq[cursor] = rf;
        demod->iq[cursor + LD_AUDIO_IQ_HISTORY] = rf;
        demod->iq_cursor = (cursor + 1u) & (LD_AUDIO_IQ_HISTORY - 1u);
        if (tick % LD_AUDIO_DECIMATION == 0u) {
            center = &demod->iq[cursor + LD_AUDIO_IQ_HISTORY - LD_AUDIO_IQ_HALF];
            ld_audio_iq_tick(demod, center, raw, tick + 1u, callback, context);
        }
    }
    ld_audio_emit(demod, demod->next_sample, callback, context);
}
