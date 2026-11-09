/* MISRC GUI LaserDisc AC-3 RF preview. GPL-3.0-or-later.
 * Differential symbol mapping and timing recovery adapted from ld-decode
 * lddecode/ac3rf.py (Copyright 2025–2026 Staffan Ulfberg, GPL-3.0-or-later).
 */
#include "gui_ld_ac3_rf.h"

#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LD_AC3_PI 3.14159265358979323846

enum {
    AC3_RF_RATE = 20000000,
    AC3_SYMBOL_RATE = 288000,
    AC3_CHUNK = 1024,
    AC3_NCO_PERIOD = 125, /* 2.88 MHz is 36/125 of the 10 MSPS mix rate. */
    AC3_IQ_DIVISOR = 8,
    AC3_IQ_RATE = AC3_RF_RATE / AC3_IQ_DIVISOR,
    AC3_HALF_BAND_1 = 9,
    AC3_HALF_BAND_2 = 11,
    AC3_RRC_HALF = 27, /* Three symbols either side at 2.5 MSPS. */
    AC3_DIFFERENTIAL_DELAY = (AC3_IQ_RATE + AC3_SYMBOL_RATE / 2) / AC3_SYMBOL_RATE,
    AC3_FILTER_DELAY = AC3_HALF_BAND_1 + 2 * AC3_HALF_BAND_1 +
                       4 * AC3_HALF_BAND_2 + AC3_IQ_DIVISOR * AC3_RRC_HALF,
    AC3_HALF_SYMBOL = (AC3_RF_RATE + AC3_SYMBOL_RATE) / (2 * AC3_SYMBOL_RATE),
    AC3_WARMUP = 2 * AC3_FILTER_DELAY + AC3_IQ_DIVISOR * AC3_DIFFERENTIAL_DELAY,
    AC3_CLOCK_MODULUS = 1024,
    AC3_CLOCK_STEP = (AC3_CLOCK_MODULUS * AC3_SYMBOL_RATE + AC3_IQ_RATE / 2) / AC3_IQ_RATE
};

typedef struct {
    float rf[2 * AC3_HALF_BAND_1 + AC3_CHUNK];
    float mix_i[2 * AC3_HALF_BAND_1 + AC3_CHUNK / 2];
    float mix_q[2 * AC3_HALF_BAND_1 + AC3_CHUNK / 2];
    float half_i[2 * AC3_HALF_BAND_2 + AC3_CHUNK / 4];
    float half_q[2 * AC3_HALF_BAND_2 + AC3_CHUNK / 4];
    float base_i[2 * AC3_RRC_HALF + AC3_CHUNK / 8];
    float base_q[2 * AC3_RRC_HALF + AC3_CHUNK / 8];
    float delay_i[AC3_DIFFERENTIAL_DELAY], delay_q[AC3_DIFFERENTIAL_DELAY];
    size_t rf_phase, mix_phase, half_phase;
    unsigned nco, delay_cursor, previous_symbol, transitions, transition_clock;
    unsigned clock;
    int clock_error, clock_adjust;
    uint64_t next_sample, samples_seen;
    bool started;
} ld_ac3_rf_state_t;

struct gui_ld_ac3_rf {
    float hb1[(AC3_HALF_BAND_1 + 1) / 2], hb2[(AC3_HALF_BAND_2 + 1) / 2];
    float rrc[AC3_RRC_HALF + 1];
    float cos_lut[AC3_NCO_PERIOD], sin_lut[AC3_NCO_PERIOD];
    ld_ac3_rf_state_t stream;
};

static double ac3_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k <= 12; ++k) {
        term *= (x * x) / (4.0 * k * k);
        sum += term;
    }
    return sum;
}

static void ac3_halfband_taps(float *taps, int half) {
    const double beta = 7.857;
    const double norm = ac3_i0(beta);
    for (int k = 0; k <= half / 2; ++k) {
        const int offset = 2 * k + 1;
        const double x = (double)offset / half;
        const double window = ac3_i0(beta * sqrt(1.0 - x * x)) / norm;
        taps[k] = (float)((k & 1 ? -window : window) / (LD_AC3_PI * offset));
    }
}

gui_ld_ac3_rf_t *gui_ld_ac3_rf_create(void) {
    gui_ld_ac3_rf_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    ac3_halfband_taps(d->hb1, AC3_HALF_BAND_1);
    ac3_halfband_taps(d->hb2, AC3_HALF_BAND_2);
    for (unsigned k = 0; k < AC3_NCO_PERIOD; ++k) {
        const double angle = 2.0 * LD_AC3_PI * 36.0 * k / AC3_NCO_PERIOD;
        d->cos_lut[k] = (float)cos(angle);
        d->sin_lut[k] = (float)sin(angle);
    }
    /* Matched root-raised cosine, 0.7 rolloff as used by ld-decode. */
    const double beta = 0.7;
    d->rrc[0] = (float)(1.0 + beta * (4.0 / LD_AC3_PI - 1.0));
    for (int k = 1; k <= AC3_RRC_HALF; ++k) {
        const double t = (double)k * AC3_SYMBOL_RATE / AC3_IQ_RATE;
        const double a = 4.0 * beta * t;
        d->rrc[k] = (float)((sin(LD_AC3_PI * t * (1.0 - beta)) +
                            a * cos(LD_AC3_PI * t * (1.0 + beta))) /
                           (LD_AC3_PI * t * (1.0 - a * a)));
    }
    return d;
}

void gui_ld_ac3_rf_destroy(gui_ld_ac3_rf_t *d) {
    free(d);
}

void gui_ld_ac3_rf_reset(gui_ld_ac3_rf_t *d) {
    if (d) memset(&d->stream, 0, sizeof(d->stream));
}

static inline float ac3_halfband_at(const float *center, const float *taps, int half) {
    float value = 0.5f * center[0];
    for (int k = 0; k <= half / 2; ++k) {
        const int offset = 2 * k + 1;
        value += taps[k] * (center[-offset] + center[offset]);
    }
    return value;
}

static inline float ac3_rrc_at(const float *center, const float *taps) {
    float value = taps[0] * center[0];
    for (int k = 1; k <= AC3_RRC_HALF; ++k)
        value += taps[k] * (center[-k] + center[k]);
    return value;
}

static inline bool ac3_reclock(ld_ac3_rf_state_t *s, float i, float q) {
    const unsigned cursor = s->delay_cursor;
    const float pi = s->delay_i[cursor], pq = s->delay_q[cursor];
    const float dot = i * pi + q * pq, cross = q * pi - i * pq;
    const unsigned symbol = fabsf(dot) >= fabsf(cross) ? (dot >= 0.0f ? 0u : 3u)
                                                     : (cross >= 0.0f ? 1u : 2u);
    s->delay_i[cursor] = i;
    s->delay_q[cursor] = q;
    s->delay_cursor = cursor + 1 == AC3_DIFFERENTIAL_DELAY ? 0 : cursor + 1;

    if (symbol != s->previous_symbol) {
        s->transition_clock = s->clock;
        ++s->transitions;
    }
    s->previous_symbol = symbol;

    /* The wrap samples the symbol center. A single observed transition
     * should be near half-scale, between adjacent sampling instants. */
    int advance = AC3_CLOCK_STEP + s->clock_adjust;
    if (advance < 1) advance = 1;
    s->clock += (unsigned)advance;
    s->clock_adjust = 0;
    if (s->clock < AC3_CLOCK_MODULUS) return false;

    s->clock -= AC3_CLOCK_MODULUS;
    const int error = s->transitions == 1 ?
                      AC3_CLOCK_MODULUS / 2 - (int)s->transition_clock : 0;
    s->transitions = 0;
    s->clock_error += error;
    if (s->clock_error > 16383) s->clock_error = 16383;
    if (s->clock_error < -16384) s->clock_error = -16384;
    /* Second-order loop at 288 kbaud, 1.8 kHz natural frequency, damping 0.6. */
    s->clock_adjust = (int)(0.15343599f * error + 0.00502054f * s->clock_error);
    return true;
}

static void ac3_process_chunk(gui_ld_ac3_rf_t *d, const float *samples,
                              size_t count, uint64_t first_sample,
                              gui_ld_ac3_rf_symbol_t emit, void *context) {
    ld_ac3_rf_state_t *s = &d->stream;
    memcpy(s->rf + 2 * AC3_HALF_BAND_1, samples, count * sizeof(*samples));

    /* Only calculate retained samples. Contiguous histories let the
     * compiler vectorize the short FIRs without ring-index arithmetic. */
    size_t mixed = 0;
    for (size_t n = s->rf_phase; n < count; n += 2) {
        const float rf = ac3_halfband_at(s->rf + n + AC3_HALF_BAND_1,
                                         d->hb1, AC3_HALF_BAND_1);
        s->mix_i[2 * AC3_HALF_BAND_1 + mixed] = rf * d->cos_lut[s->nco];
        s->mix_q[2 * AC3_HALF_BAND_1 + mixed] = rf * d->sin_lut[s->nco];
        if (++s->nco == AC3_NCO_PERIOD) s->nco = 0;
        ++mixed;
    }
    size_t half = 0;
    for (size_t n = s->mix_phase; n < mixed; n += 2) {
        s->half_i[2 * AC3_HALF_BAND_2 + half] = ac3_halfband_at(
            s->mix_i + n + AC3_HALF_BAND_1, d->hb1, AC3_HALF_BAND_1);
        s->half_q[2 * AC3_HALF_BAND_2 + half] = ac3_halfband_at(
            s->mix_q + n + AC3_HALF_BAND_1, d->hb1, AC3_HALF_BAND_1);
        ++half;
    }
    size_t base = 0;
    for (size_t n = s->half_phase; n < half; n += 2) {
        s->base_i[2 * AC3_RRC_HALF + base] = ac3_halfband_at(
            s->half_i + n + AC3_HALF_BAND_2, d->hb2, AC3_HALF_BAND_2);
        s->base_q[2 * AC3_RRC_HALF + base] = ac3_halfband_at(
            s->half_q + n + AC3_HALF_BAND_2, d->hb2, AC3_HALF_BAND_2);
        ++base;
    }
    const size_t first = s->rf_phase + 2 * s->mix_phase + 4 * s->half_phase;
    for (size_t n = 0; n < base; ++n) {
        const float i = ac3_rrc_at(s->base_i + n + AC3_RRC_HALF, d->rrc);
        const float q = ac3_rrc_at(s->base_q + n + AC3_RRC_HALF, d->rrc);
        const size_t position = first + AC3_IQ_DIVISOR * n;
        if (ac3_reclock(s, i, q) && s->samples_seen + position >= AC3_WARMUP)
            emit(context, (uint8_t)s->previous_symbol,
                 first_sample + position + 1 - AC3_FILTER_DELAY + AC3_HALF_SYMBOL);
    }
    memmove(s->rf, s->rf + count, 2 * AC3_HALF_BAND_1 * sizeof(float));
    memmove(s->mix_i, s->mix_i + mixed, 2 * AC3_HALF_BAND_1 * sizeof(float));
    memmove(s->mix_q, s->mix_q + mixed, 2 * AC3_HALF_BAND_1 * sizeof(float));
    memmove(s->half_i, s->half_i + half, 2 * AC3_HALF_BAND_2 * sizeof(float));
    memmove(s->half_q, s->half_q + half, 2 * AC3_HALF_BAND_2 * sizeof(float));
    memmove(s->base_i, s->base_i + base, 2 * AC3_RRC_HALF * sizeof(float));
    memmove(s->base_q, s->base_q + base, 2 * AC3_RRC_HALF * sizeof(float));
    s->rf_phase ^= count & 1;
    s->mix_phase ^= mixed & 1;
    s->half_phase ^= half & 1;
    s->samples_seen += count;
}

void gui_ld_ac3_rf_process(gui_ld_ac3_rf_t *d, const float *samples,
                           size_t count, uint64_t first_sample,
                           gui_ld_ac3_rf_symbol_t emit, void *context) {
    if (!d || !samples || !count || !emit) return;
    if (d->stream.started && d->stream.next_sample != first_sample)
        gui_ld_ac3_rf_reset(d);
    d->stream.started = true;
    d->stream.next_sample = first_sample + count;
    while (count) {
        const size_t chunk = count < AC3_CHUNK ? count : AC3_CHUNK;
        ac3_process_chunk(d, samples, chunk, first_sample, emit, context);
        count -= chunk;
        samples += chunk;
        first_sample += chunk;
    }
}
