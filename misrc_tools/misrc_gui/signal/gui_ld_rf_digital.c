/* MISRC GUI LaserDisc digital audio preview. GPL-3.0-or-later.
 * EFM framing and CIRC layout follow ECMA-130 and tbc-tools
 * (Copyright (C) 2025 Simon Inns, GPL-3.0-or-later).
 */
#include "gui_ld_rf_digital.h"
#include "gui_ld_efm_frontend.h"
#include "gui_ld_rf_efm_codes.h"

#include <stdlib.h>
#include <string.h>

enum {
    LD_EFM_INPUT = 20000000,
    LD_EFM_FRAME_BITS = 588,
    LD_EFM_FRAME_RATE = 7350,
    LD_EFM_AUDIO_PER_FRAME = 6,
    LD_EFM_CIRC_DELAY = 111,
    LD_EFM_DTS_LOOKAHEAD = 4096,
    LD_EFM_PCM_BATCH = 96
};

typedef struct {
    uint8_t bytes[32];
    uint32_t erasures;
} ld_efm_frame_t;

struct gui_ld_rf_digital {
    uint16_t symbols[1u << 14];
    uint8_t gf_log[256], gf_exp[512];
    gui_ld_efm_frontend_t frontend;
    uint64_t next_sample;
    bool have_rf;
    uint8_t channel[3 * LD_EFM_FRAME_BITS + 22];
    unsigned channel_bits, previous_run;
    uint8_t q_subcode[12];
    unsigned q_bits;
    bool q_audio, q_preemphasis;
    ld_efm_frame_t last_f2, c1[128];
    ld_efm_frame_t c2[4];
    unsigned circ_position, circ_ready;
    unsigned good_pcm;
    int16_t lookahead[LD_EFM_DTS_LOOKAHEAD][2];
    uint64_t lookahead_end[LD_EFM_DTS_LOOKAHEAD];
    unsigned pcm_head, pcm_fill;
    uint64_t dts_window;
    bool encoded, locked;
    float previous_pcm[2], pcm[LD_EFM_PCM_BATCH * 2];
    float emphasis_input[2], emphasis_output[2];
    unsigned audio_phase, pcm_count;
    bool have_pcm;
};

static void ld_reset_pcm(gui_ld_rf_digital_t *d) {
    d->pcm_head = d->pcm_fill = d->pcm_count = 0;
    d->audio_phase = 0;
    d->dts_window = 0;
    d->locked = d->have_pcm = false;
    memset(d->emphasis_input, 0, sizeof(d->emphasis_input));
    memset(d->emphasis_output, 0, sizeof(d->emphasis_output));
}

static void ld_reset_audio(gui_ld_rf_digital_t *d) {
    d->circ_position = d->circ_ready = d->good_pcm = 0;
    ld_reset_pcm(d);
}

static void ld_reset_channel(gui_ld_rf_digital_t *d) {
    d->channel_bits = d->previous_run = 0;
    d->q_bits = 98;
    d->q_audio = d->q_preemphasis = false;
    ld_reset_audio(d);
}

void gui_ld_rf_digital_reset(gui_ld_rf_digital_t *d) {
    if (!d) return;
    ld_reset_channel(d);
    d->encoded = d->have_rf = false;
    gui_ld_efm_frontend_reset(&d->frontend);
}

gui_ld_rf_digital_t *gui_ld_rf_digital_create(void) {
    gui_ld_rf_digital_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    for (unsigned symbol = 0; symbol < 258; ++symbol)
        d->symbols[ld_efm_codes[symbol]] = (uint16_t)(symbol + 1);
    unsigned element = 1;
    for (unsigned i = 0; i < 255; ++i) {
        d->gf_exp[i] = (uint8_t)element;
        d->gf_log[element] = (uint8_t)i;
        element <<= 1;
        if (element & 256u) element ^= 0x11du;
    }
    for (unsigned i = 255; i < 512; ++i) d->gf_exp[i] = d->gf_exp[i-255];
    gui_ld_rf_digital_reset(d);
    return d;
}

void gui_ld_rf_digital_destroy(gui_ld_rf_digital_t *d) {
    free(d);
}

bool gui_ld_rf_digital_locked(const gui_ld_rf_digital_t *d) {
    return d && d->locked;
}

static uint8_t ld_gf_mul(const gui_ld_rf_digital_t *d, uint8_t a, uint8_t b) {
    return a && b ? d->gf_exp[d->gf_log[a] + d->gf_log[b]] : 0;
}

/* For a known set of error locations, solve the four parity equations for
 * symbol magnitudes. Verify unused equations before committing a repair. */
static bool ld_rs_repair(const gui_ld_rf_digital_t *d, uint8_t *data, unsigned length,
                         const uint8_t syndrome[4], const unsigned *positions, unsigned count) {
    uint8_t matrix[4][5];
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned col = 0; col < count; ++col)
            matrix[row][col] = d->gf_exp[row * (length - 1 - positions[col])];
        matrix[row][count] = syndrome[row];
    }
    for (unsigned col = 0; col < count; ++col) {
        unsigned pivot = col;
        while (pivot < count && !matrix[pivot][col]) ++pivot;
        if (pivot == count) return false;
        for (unsigned j = col; j <= count; ++j) {
            uint8_t temp = matrix[col][j];
            matrix[col][j] = matrix[pivot][j];
            matrix[pivot][j] = temp;
        }
        uint8_t inverse = d->gf_exp[255 - d->gf_log[matrix[col][col]]];
        for (unsigned j = col; j <= count; ++j)
            matrix[col][j] = ld_gf_mul(d, matrix[col][j], inverse);
        for (unsigned row = 0; row < 4; ++row) {
            if (row == col) continue;
            uint8_t factor = matrix[row][col];
            for (unsigned j = col; j <= count; ++j)
                matrix[row][j] ^= ld_gf_mul(d, matrix[col][j], factor);
        }
    }
    for (unsigned row = count; row < 4; ++row) if (matrix[row][count]) return false;
    for (unsigned i = 0; i < count; ++i) data[positions[i]] ^= matrix[i][count];
    return true;
}

/* Shortened GF(256) RS(32,28) / RS(28,24), roots alpha^0 through alpha^3.
 * At most two unknown errors, four erasures or 2*errors+erasures <= 4. */
static bool ld_rs_decode(const gui_ld_rf_digital_t *d, ld_efm_frame_t *frame, unsigned length) {
    unsigned known = 0, locations[4];
    uint8_t syndrome[4] = {0};
    for (unsigned i = 0; i < length; ++i) {
        if ((frame->erasures >> i) & 1u) {
            if (known == 4) return false;
            locations[known++] = i;
        }
        for (unsigned row = 0; row < 4; ++row) {
            uint8_t prior = syndrome[row];
            syndrome[row] = (prior ? d->gf_exp[d->gf_log[prior] + row] : 0) ^ frame->bytes[i];
        }
    }
    if (!(syndrome[0] | syndrome[1] | syndrome[2] | syndrome[3])) return true;
    if (known && ld_rs_repair(d, frame->bytes, length, syndrome, locations, known)) return true;
    if (known > 2) return false;
    for (unsigned i = 0; i < length; ++i) {
        if ((frame->erasures >> i) & 1u) continue;
        locations[known] = i;
        if (ld_rs_repair(d, frame->bytes, length, syndrome, locations, known + 1)) return true;
        if (known == 0) for (unsigned j = i + 1; j < length; ++j) {
            locations[1] = j;
            if (ld_rs_repair(d, frame->bytes, length, syndrome, locations, 2)) return true;
        }
    }
    return false;
}

static void ld_output_pcm(gui_ld_rf_digital_t *d, const int16_t pair[2], uint64_t end,
                          gui_ld_rf_digital_pcm_t pcm, void *context) {
    float current[2] = { pair[0] / 32768.0f, pair[1] / 32768.0f };
    if (d->q_preemphasis) {
        /* 50/15 us audio de-emphasis, bilinear at 44.1 kHz. Keep the raw
         * words intact until after the DTS signature check. */
        for (unsigned c = 0; c < 2; ++c) {
            float sample = current[c];
            current[c] = (2.323f*sample - .323f*d->emphasis_input[c] +
                          3.41f*d->emphasis_output[c])/5.41f;
            d->emphasis_input[c] = sample;
            d->emphasis_output[c] = current[c];
        }
    }
    if (d->have_pcm) {
        /* 44.1 -> 48 kHz. Interpolate only between adjacent validated samples
         * and preserve the rational phase across all RF blocks. */
        while (d->audio_phase < 160) {
            float fraction = d->audio_phase / 160.0f;
            for (unsigned c = 0; c < 2; ++c)
                d->pcm[d->pcm_count*2+c] = d->previous_pcm[c] + fraction * (current[c] - d->previous_pcm[c]);
            d->audio_phase += 147;
            if (++d->pcm_count == LD_EFM_PCM_BATCH) {
                pcm(context, d->pcm, d->pcm_count, end);
                d->pcm_count = 0;
            }
        }
        d->audio_phase -= 160;
    }
    d->previous_pcm[0] = current[0];
    d->previous_pcm[1] = current[1];
    d->have_pcm = true;
}

void gui_ld_rf_digital_finish(gui_ld_rf_digital_t *d,
                              gui_ld_rf_digital_pcm_t pcm, void *context) {
    if (!d || !pcm || d->encoded || !d->pcm_fill) return;
    unsigned index = (d->pcm_head + LD_EFM_DTS_LOOKAHEAD - d->pcm_fill) % LD_EFM_DTS_LOOKAHEAD;
    uint64_t end = 0;
    for (unsigned i = 0; i < d->pcm_fill; ++i) {
        end = d->lookahead_end[index];
        ld_output_pcm(d, d->lookahead[index], end, pcm, context);
        index = (index + 1) % LD_EFM_DTS_LOOKAHEAD;
    }
    d->pcm_fill = 0;
    if (d->pcm_count) {
        pcm(context, d->pcm, d->pcm_count, end);
        d->pcm_count = 0;
    }
}

static bool ld_dts_word(gui_ld_rf_digital_t *d, uint16_t word) {
    d->dts_window = (d->dts_window << 16) | ((word & 255u) << 8) | (word >> 8);
    uint32_t sync = (uint32_t)(d->dts_window >> 16);
    uint16_t header = (uint16_t)d->dts_window;
    /* Require aligned DTS core sync and the following fixed header bits.
     * 16-bit and 14-bit forms in either byte order. */
    return (sync == UINT32_C(0x7ffe8001) && (header & 0xfc00u) == 0xfc00u) ||
           (sync == UINT32_C(0xfe7f0180) && (header & 0x00fcu) == 0x00fcu) ||
           (sync == UINT32_C(0x1fffe800) && (header & 0xfff0u) == 0x07f0u) ||
           (sync == UINT32_C(0xff1f00e8) && (header & 0xf0ffu) == 0xf007u);
}

static void ld_queue_pcm(gui_ld_rf_digital_t *d, const int16_t pair[2], uint64_t end,
                         gui_ld_rf_digital_pcm_t pcm, void *context) {
    if (d->encoded) return;
    for (unsigned channel = 0; channel < 2; ++channel) {
        if (ld_dts_word(d, (uint16_t)pair[channel])) {
            d->encoded = true;
            ld_reset_audio(d);
            return;
        }
    }
    unsigned head = d->pcm_head;
    if (d->pcm_fill == LD_EFM_DTS_LOOKAHEAD) {
        d->locked = true;
        ld_output_pcm(d, d->lookahead[head], d->lookahead_end[head], pcm, context);
    } else {
        ++d->pcm_fill;
    }
    memcpy(d->lookahead[head], pair, sizeof(d->lookahead[head]));
    d->lookahead_end[head] = end;
    d->pcm_head = (head + 1) % LD_EFM_DTS_LOOKAHEAD;
}

static void ld_circ_frame(gui_ld_rf_digital_t *d, const ld_efm_frame_t *f2, uint64_t end,
                          gui_ld_rf_digital_pcm_t pcm, void *context) {
    unsigned frame = d->circ_position++;
    if (d->circ_ready < LD_EFM_CIRC_DELAY + 1) ++d->circ_ready;
    ld_efm_frame_t c1 = *f2;
    for (unsigned i = 1; i < 32; i += 2) c1.bytes[i] = d->last_f2.bytes[i];
    c1.erasures = (f2->erasures & UINT32_C(0x55555555)) |
                  (d->last_f2.erasures & UINT32_C(0xaaaaaaaa));
    d->last_f2 = *f2;
    if (d->circ_ready < 2) return;
    for (unsigned i = 12; i < 16; ++i) c1.bytes[i] ^= 255u;
    for (unsigned i = 28; i < 32; ++i) c1.bytes[i] ^= 255u;
    c1.erasures = ld_rs_decode(d, &c1, 32) ? 0 : UINT32_MAX;
    d->c1[frame % 128] = c1;
    if (d->circ_ready < 110) return;

    ld_efm_frame_t c2 = {0};
    for (unsigned i = 0; i < 28; ++i) {
        const ld_efm_frame_t *old = &d->c1[(frame - 4*(27-i)) % 128];
        c2.bytes[i] = old->bytes[i];
        c2.erasures |= old->erasures & (UINT32_C(1) << i);
    }
    static const uint8_t deinterleave[24] = {
        0,1,8,9,16,17,2,3,10,11,18,19,4,5,12,13,20,21,6,7,14,15,22,23
    };
    ld_efm_frame_t output = {0};
    output.erasures = ld_rs_decode(d, &c2, 28) ? 0 : UINT32_MAX;
    for (unsigned i = 0; i < 24; ++i)
        output.bytes[deinterleave[i]] = c2.bytes[i + (i >= 12 ? 4 : 0)];
    /* Use a power-of-two position ring so wrap at UINT_MAX preserves order. */
    unsigned slot = frame % 4;
    d->c2[slot] = output;
    if (d->circ_ready <= LD_EFM_CIRC_DELAY) return;
    const ld_efm_frame_t *delayed = &d->c2[(slot+2) % 4];

    bool valid = !output.erasures && !delayed->erasures;
    if (valid) {
        if (d->good_pcm < 8) ++d->good_pcm;
    } else if (d->good_pcm) {
        --d->good_pcm;
    }
    if (!d->good_pcm) {
        ld_reset_pcm(d);
        return;
    }
    const uint64_t delay = (uint64_t)LD_EFM_CIRC_DELAY * LD_EFM_INPUT / LD_EFM_FRAME_RATE;
    if (end < delay) return;
    end -= delay;
    for (unsigned i = 0; i < LD_EFM_AUDIO_PER_FRAME; ++i) {
        unsigned at = 4*i;
        const ld_efm_frame_t *source = i & 1u ? delayed : &output;
        int16_t pair[2] = {0};
        if (!source->erasures)
            for (unsigned c = 0; c < 2; ++c) {
                unsigned byte = at+2*c;
                pair[c] = (int16_t)((source->bytes[byte] << 8) | source->bytes[byte+1]);
            }
        uint64_t remaining = (uint64_t)(5-i) * LD_EFM_INPUT / 44100u;
        ld_queue_pcm(d, pair, end >= remaining ? end - remaining : 0, pcm, context);
        if (d->encoded) break;
    }
}

static unsigned ld_channel_symbol(const gui_ld_rf_digital_t *d, const uint8_t *bits) {
    unsigned code = 0;
    for (unsigned b = 0; b < 14; ++b) code = (code << 1) | bits[b];
    return d->symbols[code];
}

static void ld_subcode(gui_ld_rf_digital_t *d, unsigned symbol) {
    if (symbol == 257) {
        d->q_bits = 97;
        return;
    }
    if (symbol == 258 && d->q_bits == 97) {
        d->q_bits = 0;
        return;
    }
    if (!symbol || symbol > 256 || d->q_bits >= 96) {
        d->q_bits = 98;
        return;
    }
    unsigned position = d->q_bits++;
    unsigned byte = position / 8;
    if (position % 8 == 0) d->q_subcode[byte] = 0;
    d->q_subcode[byte] = (uint8_t)((d->q_subcode[byte] << 1) | (((symbol-1) >> 6) & 1u));
    if (d->q_bits != 96) return;

    uint16_t crc = 0;
    for (unsigned i = 0; i < 10; ++i) {
        crc ^= (uint16_t)(d->q_subcode[i] << 8);
        for (unsigned b = 0; b < 8; ++b)
            crc = (uint16_t)((crc << 1) ^ (crc & 0x8000u ? 0x1021u : 0));
    }
    uint16_t recorded = (uint16_t)((d->q_subcode[10] << 8) | d->q_subcode[11]);
    if ((uint16_t)(crc ^ 0xffffu) != recorded || (d->q_subcode[0] & 15u) > 4) return;
    unsigned control = d->q_subcode[0] >> 4;
    bool audio = control < 4;
    bool preemphasis = audio && (control & 1u);
    if (d->q_audio != audio || d->q_preemphasis != preemphasis) ld_reset_audio(d);
    d->q_audio = audio;
    d->q_preemphasis = preemphasis;
}

static void ld_decode_channel(gui_ld_rf_digital_t *d, const uint8_t *bits, uint64_t end,
                              gui_ld_rf_digital_pcm_t pcm, void *context) {
    ld_subcode(d, ld_channel_symbol(d, bits+27));
    if (!d->q_audio) return;
    ld_efm_frame_t frame = {0};
    for (unsigned i = 0; i < 32; ++i) {
        unsigned symbol = ld_channel_symbol(d, bits+44+i*17);
        if (!symbol || symbol > 256) frame.erasures |= UINT32_C(1) << i;
        else frame.bytes[i] = (uint8_t)(symbol - 1);
    }
    ld_circ_frame(d, &frame, end, pcm, context);
}

static void ld_efm_run(gui_ld_rf_digital_t *d, unsigned run, uint64_t tick,
                       gui_ld_rf_digital_pcm_t pcm, void *context) {
    if (d->encoded) return;
    if (run < 3 || run > 11) {
        ld_reset_channel(d);
        return;
    }
    bool sync = run == 11 && d->previous_run == 11;
    d->previous_run = run;
    if (d->channel_bits) {
        if (d->channel_bits + run > sizeof(d->channel)) {
            ld_reset_channel(d);
            d->previous_run = run;
        } else {
            d->channel[d->channel_bits] = 1;
            memset(d->channel + d->channel_bits + 1, 0, run - 1);
            d->channel_bits += run;
        }
    }
    if (!sync) return;
    if (d->channel_bits > 22) {
        unsigned span = d->channel_bits - 22;
        if (span < LD_EFM_FRAME_BITS - 8) return;
        unsigned frames = (span + LD_EFM_FRAME_BITS/2) / LD_EFM_FRAME_BITS;
        bool exact = span == frames*LD_EFM_FRAME_BITS;
        const ld_efm_frame_t missing = {.erasures=UINT32_MAX};
        for (unsigned i = 0; i < frames && !d->encoded; ++i) {
            uint64_t delay = 22u * LD_EFM_INPUT / 4321800u +
                (uint64_t)(frames-1-i)*LD_EFM_INPUT / LD_EFM_FRAME_RATE;
            uint64_t end = tick > delay ? tick-delay : 0;
            if (exact) ld_decode_channel(d, d->channel+i*LD_EFM_FRAME_BITS, end, pcm, context);
            else if (d->circ_ready) {
                d->q_bits = 98;
                ld_circ_frame(d, &missing, end, pcm, context);
            }
        }
    }
    memset(d->channel, 0, 22);
    d->channel[0] = d->channel[11] = 1;
    d->channel_bits = 22;
}

typedef struct {
    gui_ld_rf_digital_t *decoder;
    gui_ld_rf_digital_pcm_t pcm;
    void *context;
} ld_efm_process_t;

static void ld_efm_tvalue(void *context, unsigned run, uint64_t tick) {
    ld_efm_process_t *process = context;
    ld_efm_run(process->decoder, run, tick, process->pcm, process->context);
}

void gui_ld_rf_digital_process(gui_ld_rf_digital_t *d, const float *samples,
                               size_t count, uint64_t first, gui_ld_rf_digital_pcm_t pcm,
                               void *context) {
    if (!d || !samples || !count || !pcm || count > UINT64_MAX-first) return;
    if (d->have_rf && first != d->next_sample) gui_ld_rf_digital_reset(d);
    d->have_rf = true;
    d->next_sample = first + count;
    ld_efm_process_t process = { d, pcm, context };
    if (!d->encoded)
        gui_ld_efm_frontend_process(&d->frontend, samples, count, first, ld_efm_tvalue, &process);
}
