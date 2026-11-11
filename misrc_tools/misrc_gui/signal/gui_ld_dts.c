#include "gui_ld_dts.h"

#include <stdlib.h>

#if MISRC_HAVE_LD_DTS
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <string.h>
#endif

enum {
    LD_DTS_RF_RATE = 20000000,
    LD_DTS_PCM_RATE = 44100,
    LD_DTS_HEADER_BYTES = 12,
    LD_DTS_MAX_SAMPLES = 4096,
    LD_DTS_MAX_BYTES = LD_DTS_MAX_SAMPLES * 4,
#if MISRC_HAVE_LD_DTS
    LD_DTS_MIX_CHANNELS = AV_CHAN_TOP_BACK_RIGHT + 1,
#endif
};

struct gui_ld_dts {
    uint32_t sync;
    uint64_t previous_end;
    uint64_t packet_start;
    uint64_t lock_until;
    unsigned received;
    unsigned packet_bytes;
    unsigned packet_samples;
    bool little_endian;
    bool packed14;
    bool encoded;
    bool locked;
#if MISRC_HAVE_LD_DTS
    AVCodecContext *codec;
    AVFrame *frame;
    bool decoder_tried;
    uint8_t packet[LD_DTS_MAX_BYTES + AV_INPUT_BUFFER_PADDING_SIZE];
#else
    uint8_t packet[LD_DTS_HEADER_BYTES];
#endif
};

gui_ld_dts_t *gui_ld_dts_create(void) {
    return calloc(1, sizeof(gui_ld_dts_t));
}

void gui_ld_dts_destroy(gui_ld_dts_t *d) {
    if (!d) return;
#if MISRC_HAVE_LD_DTS
    av_frame_free(&d->frame);
    avcodec_free_context(&d->codec);
#endif
    free(d);
}

static void ld_dts_discard(gui_ld_dts_t *d) {
    d->sync = 0;
    d->received = 0;
    d->packet_bytes = 0;
#if MISRC_HAVE_LD_DTS
    if (d->locked) avcodec_flush_buffers(d->codec);
#endif
    d->locked = false;
}

void gui_ld_dts_reset(gui_ld_dts_t *d) {
    if (!d) return;
    ld_dts_discard(d);
    d->previous_end = 0;
    d->encoded = false;
}

bool gui_ld_dts_locked(const gui_ld_dts_t *d) {
    return d && d->locked;
}

bool gui_ld_dts_available(void) {
#if MISRC_HAVE_LD_DTS
    return avcodec_find_decoder(AV_CODEC_ID_DTS) != NULL;
#else
    return false;
#endif
}

static void ld_dts_put_word(uint8_t *out, uint16_t word) {
    out[0] = (uint8_t)word;
    out[1] = (uint8_t)(word >> 8);
}

static bool ld_dts_sync(gui_ld_dts_t *d) {
    /* Compare word values in the little-endian EFM stream byte order. */
    switch (d->sync) {
    case UINT32_C(0x7ffe8001): d->little_endian = true;  d->packed14 = false; return true;
    case UINT32_C(0xfe7f0180): d->little_endian = false; d->packed14 = false; return true;
    case UINT32_C(0x1fffe800): d->little_endian = true;  d->packed14 = true;  return true;
    case UINT32_C(0xff1f00e8): d->little_endian = false; d->packed14 = true;  return true;
    default: return false;
    }
}

static bool ld_dts_read_header(gui_ld_dts_t *d) {
    /* Normalize just enough of the core header to read FSIZE, NBLKS and SFREQ.
     * Leave packet words untouched for libavcodec, which supports all packings. */
    uint8_t header[10];
    unsigned out = 0, bits = 0;
    uint32_t acc = 0;
    for (unsigned i = 0; out < sizeof(header); i += 2) {
        unsigned hi = i + d->little_endian;
        unsigned lo = i + !d->little_endian;
        unsigned word = (unsigned)d->packet[hi] << 8 | d->packet[lo];
        unsigned width = d->packed14 ? 14 : 16;
        acc = (acc << width) | (word & (d->packed14 ? 0x3fffu : 0xffffu));
        bits += width;
        while (bits >= 8 && out < sizeof(header)) {
            bits -= 8;
            header[out++] = (uint8_t)(acc >> bits);
        }
    }

    unsigned blocks = (((header[4] & 1u) << 6) | (header[5] >> 2)) + 1u;
    unsigned bytes = (((header[5] & 3u) << 12) | ((unsigned)header[6] << 4) |
                      (header[7] >> 4)) + 1u;
    unsigned mode = ((header[7] & 15u) << 2) | (header[8] >> 6);
    unsigned rate = (header[8] >> 2) & 15u;
    unsigned samples = blocks * 32u;
    unsigned wire_bytes = d->packed14 ? 2u * ((4u * bytes + 6u) / 7u) : ((bytes + 1u) & ~1u);
    if ((header[4] & 0xfcu) != 0xfcu || blocks % 8u || bytes < 96u || mode >= 10u ||
        rate != 8u || (header[9] & 0x10u) || wire_bytes > samples * 4u)
        return false;

    d->packet_bytes = wire_bytes;
    d->packet_samples = samples;
    return true;
}

#if MISRC_HAVE_LD_DTS
static bool ld_dts_open(gui_ld_dts_t *d) {
    if (d->decoder_tried) return d->codec != NULL;
    d->decoder_tried = true;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_DTS);
    if (!codec) return false;
    d->codec = avcodec_alloc_context3(codec);
    d->frame = av_frame_alloc();
    if (!d->codec || !d->frame ||
        av_opt_set(d->codec->priv_data, "downmix", "stereo", 0) < 0 ||
        av_opt_set_int(d->codec->priv_data, "core_only", 1, 0) < 0 ||
        avcodec_open2(d->codec, codec, NULL) < 0) {
        av_frame_free(&d->frame);
        avcodec_free_context(&d->codec);
        return false;
    }
    return true;
}

static uint64_t ld_dts_ticks(unsigned samples) {
    return ((uint64_t)samples * LD_DTS_RF_RATE + LD_DTS_PCM_RATE / 2) / LD_DTS_PCM_RATE;
}

static void ld_dts_stereo(const AVFrame *frame, float *stereo, unsigned samples) {
    /* Some DTS cores supply their own stereo downmix. Otherwise mix the
     * decoded speaker positions, leaving LFE out and allowing for peaks. */
    static const float weights[][2] = {
        [AV_CHAN_FRONT_LEFT]            = { 1.0f, 0.0f },
        [AV_CHAN_FRONT_RIGHT]           = { 0.0f, 1.0f },
        [AV_CHAN_FRONT_CENTER]          = { 0.7071f, 0.7071f },
        [AV_CHAN_FRONT_LEFT_OF_CENTER]  = { 0.7071f, 0.0f },
        [AV_CHAN_FRONT_RIGHT_OF_CENTER] = { 0.0f, 0.7071f },
        [AV_CHAN_SIDE_LEFT]             = { 0.7071f, 0.0f },
        [AV_CHAN_SIDE_RIGHT]            = { 0.0f, 0.7071f },
        [AV_CHAN_BACK_LEFT]             = { 0.7071f, 0.0f },
        [AV_CHAN_BACK_RIGHT]            = { 0.0f, 0.7071f },
        [AV_CHAN_BACK_CENTER]           = { 0.5f, 0.5f },
        [AV_CHAN_TOP_FRONT_LEFT]         = { 0.7071f, 0.0f },
        [AV_CHAN_TOP_FRONT_RIGHT]        = { 0.0f, 0.7071f },
        [AV_CHAN_TOP_FRONT_CENTER]       = { 0.5f, 0.5f },
        [AV_CHAN_TOP_CENTER]             = { 0.5f, 0.5f },
        [AV_CHAN_TOP_BACK_LEFT]          = { 0.7071f, 0.0f },
        [AV_CHAN_TOP_BACK_RIGHT]         = { 0.0f, 0.7071f },
        [AV_CHAN_TOP_BACK_CENTER]        = { 0.5f, 0.5f },
    };
    float mix[LD_DTS_MIX_CHANNELS][2] = {0};
    unsigned channels = (unsigned)frame->ch_layout.nb_channels;
    if (channels == 1) {
        mix[0][0] = mix[0][1] = 1.0f;
    } else if (channels == 2 && frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        mix[0][0] = mix[1][1] = 1.0f;
    } else {
        for (unsigned c = 0; c < channels; ++c) {
            unsigned speaker = (unsigned)av_channel_layout_channel_from_index(&frame->ch_layout, c);
            if (speaker < sizeof(weights) / sizeof(weights[0])) {
                mix[c][0] = weights[speaker][0];
                mix[c][1] = weights[speaker][1];
            }
        }
    }

    float left = 0.0f, right = 0.0f;
    for (unsigned c = 0; c < channels; ++c) {
        left += mix[c][0];
        right += mix[c][1];
    }
    float peak = left > right ? left : right;
    float gain = peak > 1.0f ? 1.0f / peak : 1.0f;
    memset(stereo, 0, samples * 2 * sizeof(*stereo));
    for (unsigned c = 0; c < channels; ++c) {
        if (mix[c][0] == 0.0f && mix[c][1] == 0.0f) continue;
        if (frame->format == AV_SAMPLE_FMT_FLTP) {
            const float *data = (const float *)frame->extended_data[c];
            for (unsigned i = 0; i < samples; ++i) {
                stereo[2 * i] += data[i] * (mix[c][0] * gain);
                stereo[2 * i + 1] += data[i] * (mix[c][1] * gain);
            }
        } else {
            const int32_t *data = (const int32_t *)frame->extended_data[c];
            float scale = gain / 2147483648.0f;
            for (unsigned i = 0; i < samples; ++i) {
                stereo[2 * i] += data[i] * (mix[c][0] * scale);
                stereo[2 * i + 1] += data[i] * (mix[c][1] * scale);
            }
        }
    }
}

static bool ld_dts_decode(gui_ld_dts_t *d, gui_ld_dts_pcm_t pcm, void *context) {
    if (!ld_dts_open(d)) return false;
    memset(d->packet + d->packet_bytes, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    AVPacket packet = { .data = d->packet, .size = (int)d->packet_bytes };
    if (avcodec_send_packet(d->codec, &packet) < 0 || avcodec_receive_frame(d->codec, d->frame) < 0)
        return false;

    const AVFrame *frame = d->frame;
    unsigned channels = (unsigned)frame->ch_layout.nb_channels;
    unsigned samples = (unsigned)frame->nb_samples;
    if (frame->sample_rate != LD_DTS_PCM_RATE || samples != d->packet_samples ||
        channels < 1 || channels > LD_DTS_MIX_CHANNELS ||
        (frame->format != AV_SAMPLE_FMT_FLTP && frame->format != AV_SAMPLE_FMT_S32P)) {
        av_frame_unref(d->frame);
        return false;
    }

    float stereo[LD_DTS_MAX_SAMPLES * 2];
    ld_dts_stereo(frame, stereo, samples);
    uint64_t end = d->packet_start + ld_dts_ticks(samples - 1u);
    d->lock_until = end + ld_dts_ticks(samples + 2u);
    d->locked = true;
    pcm(context, stereo, samples, end);
    av_frame_unref(d->frame);
    return true;
}
#endif

bool gui_ld_dts_push(gui_ld_dts_t *d, const int16_t pair[2], bool valid,
                     uint64_t end, gui_ld_dts_pcm_t pcm, void *context) {
    if (!d) return false;
    if (!pcm && d->encoded) return true;
#if !MISRC_HAVE_LD_DTS
    (void)context;
    if (d->encoded) return true;
#endif
    if (!valid) {
        ld_dts_discard(d);
        return d->encoded;
    }
    if (d->locked && !d->received && end > d->lock_until) ld_dts_discard(d);

    for (unsigned channel = 0; channel < 2; ++channel) {
        uint16_t word = (uint16_t)pair[channel];
        d->sync = d->sync << 16 | word;
        if (!d->packet_bytes && ld_dts_sync(d)) {
            ld_dts_put_word(d->packet, (uint16_t)(d->sync >> 16));
            ld_dts_put_word(d->packet + 2, word);
            d->packet_start = channel ? end : d->previous_end;
            d->received = 4;
        } else if (d->received) {
            ld_dts_put_word(d->packet + d->received, word);
            d->received += 2;
            if (!d->packet_bytes && d->received == LD_DTS_HEADER_BYTES) {
                if (ld_dts_read_header(d)) {
                    d->encoded = true;
                    if (!pcm) break;
                } else {
                    d->received = 0;
                }
            }
#if MISRC_HAVE_LD_DTS
            if (d->packet_bytes && d->received == d->packet_bytes) {
                if (!ld_dts_decode(d, pcm, context)) {
                    if (d->codec) avcodec_flush_buffers(d->codec);
                    d->locked = false;
                }
                d->received = 0;
                d->packet_bytes = 0;
                d->sync = 0;
            }
#else
            if (d->encoded) break;
#endif
        }
    }
    d->previous_end = end;
    return d->encoded;
}
