/* MISRC GUI LaserDisc AC-3 audio decoder. GPL-3.0-or-later. */
#include "gui_ld_ac3_codec.h"

#include <stdlib.h>

#include "a52.h"

struct gui_ld_ac3_codec {
    a52_state_t *decoder;
};

gui_ld_ac3_codec_t *gui_ld_ac3_codec_create(void) {
    gui_ld_ac3_codec_t *codec = malloc(sizeof(*codec));
    if (!codec) return NULL;
    codec->decoder = a52_init(0);
    if (!codec->decoder) {
        free(codec);
        return NULL;
    }
    return codec;
}

void gui_ld_ac3_codec_destroy(gui_ld_ac3_codec_t *codec) {
    if (!codec) return;
    a52_free(codec->decoder);
    free(codec);
}

void gui_ld_ac3_codec_reset(gui_ld_ac3_codec_t *codec) {
    if (codec) a52_reset(codec->decoder);
}

static bool ld_ac3_crc_valid(const uint8_t *data, size_t size) {
    unsigned crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (unsigned)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc << 1) ^ ((crc & 0x8000u) ? 0x8005u : 0u);
        }
    }
    return (crc & 0xffffu) == 0;
}

bool gui_ld_ac3_codec_decode(gui_ld_ac3_codec_t *codec, const uint8_t *frame,
                             size_t bytes, float *stereo) {
    if (!codec || !frame || !stereo || bytes < 7) return false;

    int flags, sample_rate, bit_rate;
    int frame_size = a52_syncinfo(frame, &flags, &sample_rate, &bit_rate);
    if (frame_size <= 0 || (size_t)frame_size != bytes || sample_rate != 48000) {
        a52_reset(codec->decoder);
        return false;
    }

    /* AC-3 protects the first five eighths and the remainder separately. */
    size_t split = ((bytes >> 2) + (bytes >> 4)) << 1;
    if (!ld_ac3_crc_valid(frame + 2, split - 2) ||
        !ld_ac3_crc_valid(frame + split, bytes - split)) {
        a52_reset(codec->decoder);
        return false;
    }

    flags = A52_STEREO | A52_ADJUST_LEVEL;
    level_t level = 1;
    if (a52_frame(codec->decoder, frame, bytes, &flags, &level, 0) != 0) {
        a52_reset(codec->decoder);
        return false;
    }

    bool mono = (flags & A52_CHANNEL_MASK) == A52_MONO;
    for (size_t block = 0; block < 6; ++block) {
        if (a52_block(codec->decoder) != 0) {
            a52_reset(codec->decoder);
            return false;
        }
        const sample_t *left = a52_samples(codec->decoder);
        const sample_t *right = mono ? left : left + 256;
        float *output = stereo + block * 512;
        for (size_t i = 0; i < 256; ++i) {
            output[2 * i] = left[i];
            output[2 * i + 1] = right[i];
        }
    }
    return true;
}
