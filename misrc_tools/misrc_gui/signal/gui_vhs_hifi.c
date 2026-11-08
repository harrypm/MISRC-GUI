/* MISRC GUI PAL/NTSC VHS Hi-Fi preview. GPL-3.0-or-later.
 *
 * VHS carrier frequencies, FM deviation and noise reduction time constants:
 * JVC Video Technical Guide VTG82063, section 7.3. Shelving and expander
 * defaults follow the vhs-decode / vhs-decode-dotnet VHS Hi-Fi post path.
 * This is a causal approximation for monitoring. No whole-file gain
 * normalization, spectral noise reduction or video head timing is used.
 */
#include "gui_vhs_hifi.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VHS_HIFI_PI 3.14159265358979323846

enum {
    VHS_RF_RATE = 5000000,
    VHS_IQ_RATE = 500000,
    VHS_RF_HALF = 23,
    VHS_RF_RING = 64,
    VHS_IQ_HALF = 75,
    VHS_IQ_MAX_HALF = 123,
    VHS_IQ_FILTER_HZ = 170000,
    VHS_IQ_EDGE_GUARD_HZ = 20000,
    VHS_IQ_RING = 256,
    VHS_REPAIR_RING = 128,
    VHS_REPAIR_MAX = 56,
    VHS_AUDIO_HALF = 160,
    VHS_AUDIO_TAPS = 2 * VHS_AUDIO_HALF + 1,
    VHS_AUDIO_RING = 512,
    VHS_AUDIO_MAX_PHASES = 12,
    VHS_PCM_BATCH = 128
};

typedef struct { float b0, b1, a1; } vhs_shelf_t;

typedef struct {
    double carrier_hz[2];
    const char *searching, *left_locked, *right_locked, *stereo_locked;
} vhs_hifi_profile_t;

/* JVC VTG82063 §7.3.1 specifies the same deviation and emphasis for
 * these systems. Discontinuity repair follows the RF phase, independent
 * of the field cadence. */
static const vhs_hifi_profile_t vhs_hifi_profiles[] = {
    [GUI_RF_PAL] = {
        {1400000, 1800000},
        "Searching for PAL VHS Hi-Fi carriers; muted",
        "PAL VHS Hi-Fi left carrier locked",
        "PAL VHS Hi-Fi right carrier locked",
        "PAL VHS Hi-Fi stereo locked"
    },
    [GUI_RF_NTSC] = {
        {1300000, 1700000},
        "Searching for NTSC VHS Hi-Fi carriers; muted",
        "NTSC VHS Hi-Fi left carrier locked",
        "NTSC VHS Hi-Fi right carrier locked",
        "NTSC VHS Hi-Fi stereo locked"
    }
};

typedef struct {
    float previous_i, previous_q, power_reference;
    float dc, previous_hz, previous_delta;
    double power_sum, squared_power_sum, detune_sum;
    unsigned valid, rough, streak, good_windows, bad_windows;
    bool have_iq, locked;

    float pending[VHS_REPAIR_RING], previous_good;
    unsigned bad_run;
    bool have_anchor;

    float audio[VHS_AUDIO_RING * 2];
    float deemphasis, weighting, nr_shelf, envelope, fade;
} vhs_hifi_channel_t;

/* Coefficients are independent of stream position and survive seeks. */
struct gui_vhs_hifi_decoder {
    const vhs_hifi_profile_t *profile;
    unsigned rf_decimation, iq_decimation, lock_window, audio_phases, audio_step;
    int iq_half;
    bool complex_input;
    float rf_taps[VHS_RF_HALF + 1];
    float iq_i[2][VHS_IQ_MAX_HALF + 1], iq_q[2][VHS_IQ_MAX_HALF + 1];
    float center_i[2], center_q[2], demod_hz_per_rad;
    float audio_taps[VHS_AUDIO_MAX_PHASES][VHS_AUDIO_TAPS];
    vhs_shelf_t deemphasis, weighting, nr_shelf;
    float attack, release, dc_step;

    struct {
        gui_beta_hifi_status_t status;
        vhs_hifi_channel_t channel[2];
        float rf[VHS_RF_RING * 2], iq_i[VHS_IQ_RING * 2], iq_q[VHS_IQ_RING * 2];
        float pcm[VHS_PCM_BATCH * 2];
        uint64_t next_sample;
        unsigned rf_pos, iq_pos, iq_phase, repair_pos, audio_pos, audio_phase;
        unsigned window_count, pcm_count;
        double rf_power_sum;
        bool have_input;
    } stream;
};

static double vhs_i0(double value) {
    double sum = 1, term = 1;
    for (unsigned i = 1; i <= 12; ++i) {
        term *= (value * value) / (4.0 * i * i);
        sum += term;
    }
    return sum;
}

static double vhs_lowpass(double offset, double half, double cutoff, double beta) {
    double n = offset / (half + 1.0);
    double ideal = offset == 0 ? 2.0 * cutoff :
                   sin(2.0 * VHS_HIFI_PI * cutoff * offset) / (VHS_HIFI_PI * offset);
    return ideal * vhs_i0(beta * sqrt(1.0 - n * n)) / vhs_i0(beta);
}

static vhs_shelf_t vhs_shelf(double zero, double pole) {
    const double step = 2.0 * GUI_BETA_HIFI_AUDIO_RATE_HZ;
    double den = 1 + step * pole;
    return (vhs_shelf_t){ (float)((1 + step * zero) / den),
                         (float)((1 - step * zero) / den),
                         (float)((1 - step * pole) / den) };
}

static inline float vhs_filter(vhs_shelf_t filter, float *state, float sample) {
    float output = filter.b0 * sample + *state;
    *state = filter.b1 * sample - filter.a1 * output;
    return output;
}

static unsigned vhs_sdr_decimation(uint32_t rate) {
    switch (rate) {
        case 1024000: return 2;
        case 1200000:
        case 1536000: return 3;
        case 2048000: return 4;
        case 2400000: return 5;
        default: return 0;
    }
}

bool gui_vhs_hifi_iq_supported(gui_rf_format_t format, uint32_t iq_rate_hz,
                              uint64_t center_hz) {
    if (format != GUI_RF_PAL && format != GUI_RF_NTSC) return false;
    if (!vhs_sdr_decimation(iq_rate_hz)) return false;
    const double *carrier = vhs_hifi_profiles[format].carrier_hz;
    double span = iq_rate_hz / 2.0 - VHS_IQ_FILTER_HZ - VHS_IQ_EDGE_GUARD_HZ;
    return fabs(carrier[0] - (double)center_hz) <= span &&
           fabs(carrier[1] - (double)center_hz) <= span;
}

static gui_vhs_hifi_decoder_t *vhs_create(gui_rf_format_t format, uint32_t rf_rate_hz,
                                         uint64_t iq_center_hz) {
    gui_vhs_hifi_decoder_t *decoder = malloc(sizeof(*decoder));
    if (!decoder) return NULL;
    decoder->profile = &vhs_hifi_profiles[format];
    /* The real RF path uses absolute carrier frequencies (zero center). */
    decoder->complex_input = iq_center_hz != 0;

    /* Use the 5 MSPS front end for 20 and 10 MSPS. At 8/8.192 MSPS
     * filter the carriers at the input rate to avoid folding nearby RF
     * into the upper Hi-Fi sideband with a preliminary 2:1 decimation. */
    unsigned rf_factor = rf_rate_hz == GUI_RF_DECODE_RATE_HZ ? 4 :
                         rf_rate_hz == 10000000u ? 2 : 1;
    decoder->rf_decimation = rf_factor;
    unsigned iq_input_rate = rf_rate_hz / rf_factor;
    decoder->iq_decimation = decoder->complex_input ? vhs_sdr_decimation(iq_input_rate) :
                                                       iq_input_rate / VHS_IQ_RATE;
    unsigned iq_rate = iq_input_rate / decoder->iq_decimation;
    decoder->iq_half = (VHS_IQ_HALF * iq_input_rate + VHS_RF_RATE / 2) / VHS_RF_RATE;
    decoder->lock_window = iq_rate / 100;
    decoder->demod_hz_per_rad = (float)(iq_rate / (2 * VHS_HIFI_PI));

    if (rf_factor > 1) {
        for (unsigned k = 0; k <= VHS_RF_HALF; ++k)
            decoder->rf_taps[k] = k && k % rf_factor == 0 ? 0 :
                (float)vhs_lowpass(k, VHS_RF_HALF, 0.5 / rf_factor, 6.5);
    }

    for (unsigned c = 0; c < 2; ++c) {
        double frequency = decoder->profile->carrier_hz[c] - (double)iq_center_hz;
        double rotation = 2 * VHS_HIFI_PI * frequency / iq_rate;
        decoder->center_i[c] = (float)cos(rotation);
        decoder->center_q[c] = (float)sin(rotation);
        for (int k = 0; k <= decoder->iq_half; ++k) {
            double tap = vhs_lowpass(k, decoder->iq_half, (double)VHS_IQ_FILTER_HZ / iq_input_rate, 6.4);
            double angle = 2 * VHS_HIFI_PI * frequency * k / iq_input_rate;
            decoder->iq_i[c][k] = (float)(tap * cos(angle));
            decoder->iq_q[c][k] = (float)(tap * sin(angle));
        }
    }

    switch (iq_rate) {
        case 400000: decoder->audio_phases = 3; decoder->audio_step = 25; break;
        case 480000: decoder->audio_phases = 1; decoder->audio_step = 10; break;
        case 512000: decoder->audio_phases = 3; decoder->audio_step = 32; break;
        default: decoder->audio_phases = VHS_AUDIO_MAX_PHASES; decoder->audio_step = 125; break;
    }
    for (unsigned phase = 0; phase < decoder->audio_phases; ++phase) {
        float *taps = decoder->audio_taps[phase];
        double gain = 0;
        for (unsigned i = 0; i < VHS_AUDIO_TAPS; ++i) {
            double offset = (double)i - VHS_AUDIO_HALF + (double)phase / decoder->audio_phases;
            taps[i] = (float)vhs_lowpass(offset, VHS_AUDIO_HALF, 21000.0 / iq_rate, 6);
            gain += taps[i];
        }
        for (unsigned i = 0; i < VHS_AUDIO_TAPS; ++i) taps[i] /= (float)gain;
    }

    decoder->deemphasis = vhs_shelf(20e-6, 56e-6);
    decoder->weighting = vhs_shelf(240e-6, 24e-6);
    decoder->nr_shelf = vhs_shelf(56e-6, 240e-6);
    decoder->attack = (float)(1 - exp(-1 / (6.5e-3 * GUI_BETA_HIFI_AUDIO_RATE_HZ)));
    decoder->release = (float)(1 - exp(-1 / (70e-3 * GUI_BETA_HIFI_AUDIO_RATE_HZ)));
    decoder->dc_step = (float)(1 - exp(-2 * VHS_HIFI_PI * 12 / iq_rate));
    memset(&decoder->stream, 0, sizeof(decoder->stream));
    gui_vhs_hifi_reset(decoder);
    return decoder;
}

gui_vhs_hifi_decoder_t *gui_vhs_hifi_create(gui_rf_format_t format, uint32_t rf_rate_hz) {
    if (format != GUI_RF_PAL && format != GUI_RF_NTSC) return NULL;
    if (rf_rate_hz != GUI_RF_DECODE_RATE_HZ && !gui_vhs_hifi_is_native_low_rate(rf_rate_hz))
        return NULL;
    return vhs_create(format, rf_rate_hz, 0);
}

gui_vhs_hifi_decoder_t *gui_vhs_hifi_create_iq(gui_rf_format_t format, uint32_t iq_rate_hz,
                                             uint64_t center_hz) {
    if (!gui_vhs_hifi_iq_supported(format, iq_rate_hz, center_hz)) return NULL;
    return vhs_create(format, iq_rate_hz, center_hz);
}

void gui_vhs_hifi_destroy(gui_vhs_hifi_decoder_t *decoder) {
    free(decoder);
}

void gui_vhs_hifi_reset(gui_vhs_hifi_decoder_t *decoder) {
    if (!decoder) return;
    uint64_t resets = decoder->stream.status.resets + 1;
    memset(&decoder->stream, 0, sizeof(decoder->stream));
    decoder->stream.status.resets = resets;
    strcpy(decoder->stream.status.reason, decoder->profile->searching);
}

static void vhs_update_lock(gui_vhs_hifi_decoder_t *decoder) {
    unsigned window = decoder->lock_window;
    double rf_power = decoder->stream.rf_power_sum / window;
    for (unsigned c = 0; c < 2; ++c) {
        vhs_hifi_channel_t *channel = &decoder->stream.channel[c];
        double mean = channel->valid ? channel->detune_sum / channel->valid : 0;
        double power = channel->power_sum / window;
        double variance = channel->squared_power_sum / window - power * power;
        /* An FM carrier has a steady envelope even when its phase is
         * changing. Power alone would lock to filtered broadband noise. */
        bool good = power > 1e-10 && power > 0.004 * rf_power &&
            variance < 0.75 * power * power && fabs(mean) < 45000 &&
            channel->valid > 0.85 * window && channel->rough < window / 8;
        if (good) {
            channel->bad_windows = 0;
            channel->power_reference = (float)power;
            if (channel->good_windows < 2) ++channel->good_windows;
            if (!channel->locked && channel->good_windows >= 2) {
                channel->dc = (float)mean;
                channel->locked = true;
            }
        } else {
            channel->good_windows = 0;
            if (channel->bad_windows < 2) ++channel->bad_windows;
            if (channel->bad_windows >= 2) {
                channel->locked = false;
                channel->power_reference = 0;
            }
        }
        channel->power_sum = channel->squared_power_sum = channel->detune_sum = 0;
        channel->valid = channel->rough = 0;
    }
    bool left = decoder->stream.channel[0].locked;
    bool right = decoder->stream.channel[1].locked;
    decoder->stream.status.locked = left || right;
    const vhs_hifi_profile_t *profile = decoder->profile;
    strcpy(decoder->stream.status.reason, left && right ? profile->stereo_locked :
           left ? profile->left_locked : right ? profile->right_locked : profile->searching);
    decoder->stream.rf_power_sum = 0;
    decoder->stream.window_count = 0;
}

static float vhs_repair(vhs_hifi_channel_t *channel, unsigned pos, float hz, bool valid,
                        gui_beta_hifi_status_t *status) {
    channel->pending[pos] = valid ? hz - channel->dc : 0;
    if (!valid) {
        if (!channel->bad_run && channel->locked) ++status->dropouts;
        if (channel->bad_run <= VHS_REPAIR_MAX) ++channel->bad_run;
        if (channel->bad_run > VHS_REPAIR_MAX) channel->have_anchor = false;
    } else {
        float sample = channel->pending[pos];
        if (channel->bad_run && channel->have_anchor) {
            unsigned start = pos - channel->bad_run;
            float step = (sample - channel->previous_good) / (channel->bad_run + 1);
            for (unsigned i = 0; i < channel->bad_run; ++i)
                channel->pending[(start + i) & (VHS_REPAIR_RING - 1)] =
                    channel->previous_good + step * (i + 1);
        }
        channel->previous_good = sample;
        channel->have_anchor = true;
        channel->bad_run = 0;
    }
    return channel->pending[(pos + 1) & (VHS_REPAIR_RING - 1)];
}

static float vhs_demod(gui_vhs_hifi_decoder_t *decoder, unsigned c, float re, float im) {
    vhs_hifi_channel_t *channel = &decoder->stream.channel[c];
    float power = re * re + im * im;
    bool usable = power > 1e-11f && power > 0.025f * channel->power_reference;
    bool valid = usable && channel->have_iq;
    float hz = 0;
    if (valid) {
        float dot = channel->previous_i * re + channel->previous_q * im;
        float cross = channel->previous_i * im - channel->previous_q * re;
        float real = dot * decoder->center_i[c] + cross * decoder->center_q[c];
        float imag = cross * decoder->center_i[c] - dot * decoder->center_q[c];
        hz = atan2f(imag, real) * decoder->demod_hz_per_rad;
        valid = fabsf(hz) <= 175000;
    }
    channel->previous_i = re;
    channel->previous_q = im;
    channel->have_iq = usable;
    channel->power_sum += power;
    channel->squared_power_sum += (double)power * power;
    if (valid) {
        float delta = hz - channel->previous_hz;
        bool abrupt = channel->streak >= 2 && fabsf(delta - channel->previous_delta) > 30000;
        if (abrupt) ++channel->rough;
        channel->previous_hz = hz;
        channel->previous_delta = delta;
        if (channel->streak < 2) ++channel->streak;
        channel->detune_sum += hz;
        ++channel->valid;
        valid = !abrupt;
    } else {
        channel->streak = 0;
    }
    if (valid) channel->dc += decoder->dc_step * (hz - channel->dc);
    return vhs_repair(channel, decoder->stream.repair_pos, hz, valid,
                      &decoder->stream.status) * (1.0f / 150000.0f);
}

static float vhs_condition(gui_vhs_hifi_decoder_t *decoder, unsigned c, float sample) {
    vhs_hifi_channel_t *channel = &decoder->stream.channel[c];
    float de = vhs_filter(decoder->deemphasis, &channel->deemphasis, sample);
    float side = fabsf(vhs_filter(decoder->weighting, &channel->weighting, de));
    float step = side > channel->envelope ? decoder->attack : decoder->release;
    channel->envelope += step * (side - channel->envelope);
    /* 2:1 logarithmic expansion gives a linear multiplier of the peak
     * envelope. Reference level corresponds to operating FM deviation. */
    float gain = fminf(3.0f * channel->envelope, 2.0f);
    float audio = vhs_filter(decoder->nr_shelf, &channel->nr_shelf, de) * gain;
    if (!channel->locked) audio = 0;
    channel->fade += 0.012f * ((channel->locked ? 1.0f : 0.0f) - channel->fade);
    return fminf(1.0f, fmaxf(-1.0f, audio * channel->fade));
}

static void vhs_emit(gui_vhs_hifi_decoder_t *decoder, uint64_t sample,
                     gui_beta_hifi_pcm_callback_t callback, void *context) {
    if (decoder->stream.pcm_count && callback)
        callback(context, decoder->stream.pcm, decoder->stream.pcm_count, sample,
                 &decoder->stream.status);
    decoder->stream.pcm_count = 0;
}

static void vhs_iq_step(gui_vhs_hifi_decoder_t *decoder, uint64_t end_sample,
                        gui_beta_hifi_pcm_callback_t callback, void *context) {
    unsigned center = decoder->stream.iq_pos + VHS_IQ_RING - 1 - decoder->iq_half;
    const float *i = decoder->stream.iq_i + center;
    const float *q = decoder->stream.iq_q + center;
    decoder->stream.rf_power_sum += (double)i[0] * i[0];
    if (decoder->complex_input) decoder->stream.rf_power_sum += (double)q[0] * q[0];
    unsigned pos = decoder->stream.audio_pos;
    for (unsigned c = 0; c < 2; ++c) {
        const float *tap_i = decoder->iq_i[c], *tap_q = decoder->iq_q[c];
        float re = i[0] * tap_i[0], im = 0;
        if (decoder->complex_input) {
            im = q[0] * tap_i[0];
            for (int k = 1; k <= decoder->iq_half; ++k) {
                re += (i[-k] + i[k]) * tap_i[k] - (q[-k] - q[k]) * tap_q[k];
                im += (q[-k] + q[k]) * tap_i[k] + (i[-k] - i[k]) * tap_q[k];
            }
        } else {
            for (int k = 1; k <= decoder->iq_half; ++k) {
                re += (i[-k] + i[k]) * tap_i[k];
                im += (i[-k] - i[k]) * tap_q[k];
            }
        }
        float sample = vhs_demod(decoder, c, re, im);
        float *history = decoder->stream.channel[c].audio;
        history[pos] = history[pos + VHS_AUDIO_RING] = sample;
    }
    decoder->stream.repair_pos = (decoder->stream.repair_pos + 1) & (VHS_REPAIR_RING - 1);
    if (++decoder->stream.window_count == decoder->lock_window) vhs_update_lock(decoder);
    decoder->stream.audio_pos = (pos + 1) & (VHS_AUDIO_RING - 1);
    decoder->stream.audio_phase += decoder->audio_phases;
    if (decoder->stream.audio_phase < decoder->audio_step) return;
    decoder->stream.audio_phase -= decoder->audio_step;
    const float *taps = decoder->audio_taps[decoder->stream.audio_phase];
    unsigned start = pos + VHS_AUDIO_RING - VHS_AUDIO_TAPS + 1;
    unsigned pcm_pos = 2 * decoder->stream.pcm_count;
    for (unsigned c = 0; c < 2; ++c) {
        const float *history = decoder->stream.channel[c].audio + start;
        float sample = 0;
        for (unsigned i = 0; i < VHS_AUDIO_TAPS; ++i) sample += history[i] * taps[i];
        decoder->stream.pcm[pcm_pos + c] = vhs_condition(decoder, c, sample);
    }
    ++decoder->stream.status.pcm_frames;
    if (++decoder->stream.pcm_count == VHS_PCM_BATCH) vhs_emit(decoder, end_sample, callback, context);
}

static void vhs_begin_input(gui_vhs_hifi_decoder_t *decoder, size_t count, uint64_t first_sample) {
    if (decoder->stream.have_input && first_sample != decoder->stream.next_sample)
        gui_vhs_hifi_reset(decoder);
    decoder->stream.have_input = true;
    decoder->stream.status.input_samples += count;
}

static inline float vhs_finite_sample(float sample) {
    uint32_t bits;
    memcpy(&bits, &sample, sizeof(bits));
    /* The GUI builds with fast-math, so use the IEEE exponent bits. */
    return (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) ? 0 : sample;
}

static inline void vhs_feed(gui_vhs_hifi_decoder_t *decoder, float sample, uint64_t end_sample,
                            gui_beta_hifi_pcm_callback_t callback, void *context) {
    unsigned pos = decoder->stream.iq_pos;
    decoder->stream.iq_i[pos] = decoder->stream.iq_i[pos + VHS_IQ_RING] = sample;
    decoder->stream.iq_pos = (pos + 1) & (VHS_IQ_RING - 1);
    if (++decoder->stream.iq_phase == decoder->iq_decimation) {
        decoder->stream.iq_phase = 0;
        vhs_iq_step(decoder, end_sample, callback, context);
    }
}

void gui_vhs_hifi_process(gui_vhs_hifi_decoder_t *decoder, const float *samples,
                          size_t count, uint64_t first_sample,
                          gui_beta_hifi_pcm_callback_t callback, void *context) {
    if (!decoder || decoder->complex_input || !samples || !count) return;
    vhs_begin_input(decoder, count, first_sample);
    const unsigned rf_factor = decoder->rf_decimation;
    const int rf_tap_step = rf_factor == 2 ? 2 : 1;
    for (size_t i = 0; i < count; ++i) {
        float rf = vhs_finite_sample(samples[i]);
        if (rf_factor > 1) {
            unsigned pos = decoder->stream.rf_pos;
            decoder->stream.rf[pos] = decoder->stream.rf[pos + VHS_RF_RING] = rf;
            decoder->stream.rf_pos = (pos + 1) & (VHS_RF_RING - 1);
            if ((decoder->stream.rf_pos & (rf_factor - 1)) != 0) continue;
            const float *input = decoder->stream.rf + pos + VHS_RF_RING - VHS_RF_HALF;
            rf = input[0] * decoder->rf_taps[0];
            for (int k = 1; k <= VHS_RF_HALF; k += rf_tap_step)
                rf += (input[-k] + input[k]) * decoder->rf_taps[k];
        }
        vhs_feed(decoder, rf, first_sample + i + 1, callback, context);
    }
    decoder->stream.next_sample = first_sample + count;
    vhs_emit(decoder, decoder->stream.next_sample, callback, context);
}

void gui_vhs_hifi_process_iq(gui_vhs_hifi_decoder_t *decoder, const float *i, const float *q,
                            size_t count, uint64_t first_sample,
                            gui_beta_hifi_pcm_callback_t callback, void *context) {
    if (!decoder || !decoder->complex_input || !i || !q || !count) return;
    vhs_begin_input(decoder, count, first_sample);
    for (size_t n = 0; n < count; ++n) {
        unsigned pos = decoder->stream.iq_pos;
        decoder->stream.iq_q[pos] = decoder->stream.iq_q[pos + VHS_IQ_RING] = vhs_finite_sample(q[n]);
        vhs_feed(decoder, vhs_finite_sample(i[n]), first_sample + n + 1, callback, context);
    }
    decoder->stream.next_sample = first_sample + count;
    vhs_emit(decoder, decoder->stream.next_sample, callback, context);
}
