/* MISRC GUI LaserDisc AC-3 RF audio preview. GPL-3.0-or-later. */
#include "gui_ld_rf_ac3.h"
#include "gui_ld_ac3_codec.h"
#include "gui_ld_ac3_frames.h"
#include "gui_ld_ac3_rf.h"

#include <stdlib.h>

enum {
    AC3_PCM_FRAMES = 1536,
    AC3_RF_FRAME_TICKS = AC3_PCM_FRAMES * 20000000LL / 48000,
    AC3_LOCK_HOLD_TICKS = 4 * AC3_RF_FRAME_TICKS
};

struct gui_ld_rf_ac3 {
    gui_ld_ac3_rf_t *rf;
    gui_ld_ac3_frames_t *framer;
    gui_ld_ac3_codec_t *codec;
    float stereo[AC3_PCM_FRAMES * 2];
    gui_ld_rf_ac3_pcm_t pcm;
    void *context;
    uint64_t next_sample, symbol_sample, last_decoded_sample, last_frame_end;
    bool have_input, have_frame;
};

gui_ld_rf_ac3_t *gui_ld_rf_ac3_create(void) {
    gui_ld_rf_ac3_t *decoder = calloc(1, sizeof(*decoder));
    if (!decoder) return NULL;
    decoder->rf = gui_ld_ac3_rf_create();
    decoder->framer = gui_ld_ac3_frames_create();
    decoder->codec = gui_ld_ac3_codec_create();
    if (!decoder->rf || !decoder->framer || !decoder->codec) {
        gui_ld_rf_ac3_destroy(decoder);
        return NULL;
    }
    return decoder;
}

void gui_ld_rf_ac3_destroy(gui_ld_rf_ac3_t *decoder) {
    if (!decoder) return;
    gui_ld_ac3_rf_destroy(decoder->rf);
    gui_ld_ac3_frames_destroy(decoder->framer);
    gui_ld_ac3_codec_destroy(decoder->codec);
    free(decoder);
}

void gui_ld_rf_ac3_reset(gui_ld_rf_ac3_t *decoder) {
    if (!decoder) return;
    gui_ld_ac3_rf_reset(decoder->rf);
    gui_ld_ac3_frames_reset(decoder->framer);
    gui_ld_ac3_codec_reset(decoder->codec);
    decoder->have_input = false;
    decoder->have_frame = false;
}

bool gui_ld_rf_ac3_locked(const gui_ld_rf_ac3_t *decoder) {
    return decoder && decoder->have_frame &&
           decoder->next_sample - decoder->last_decoded_sample <= AC3_LOCK_HOLD_TICKS;
}

static void decode_frame(void *context, const uint8_t *ac3, size_t bytes, uint64_t end_sample) {
    gui_ld_rf_ac3_t *decoder = context;
    if (decoder->have_frame &&
        (end_sample < decoder->last_frame_end ||
         end_sample - decoder->last_frame_end > AC3_RF_FRAME_TICKS * 3 / 2))
        gui_ld_ac3_codec_reset(decoder->codec);
    if (!gui_ld_ac3_codec_decode(decoder->codec, ac3, bytes, decoder->stereo)) return;
    decoder->last_frame_end = end_sample;
    decoder->last_decoded_sample = decoder->symbol_sample;
    decoder->have_frame = true;
    decoder->pcm(decoder->context, decoder->stereo, AC3_PCM_FRAMES, end_sample);
}

static void receive_symbol(void *context, uint8_t symbol, uint64_t end_sample) {
    gui_ld_rf_ac3_t *decoder = context;
    decoder->symbol_sample = end_sample;
    gui_ld_ac3_frames_push(decoder->framer, symbol, end_sample, decode_frame, decoder);
}

void gui_ld_rf_ac3_process(gui_ld_rf_ac3_t *decoder, const float *samples,
                           size_t count, uint64_t first_sample,
                           gui_ld_rf_ac3_pcm_t pcm, void *context) {
    if (!decoder || !samples || !count || !pcm) return;
    if (decoder->have_input && decoder->next_sample != first_sample)
        gui_ld_rf_ac3_reset(decoder);
    decoder->have_input = true;
    decoder->next_sample = first_sample + count;
    decoder->pcm = pcm;
    decoder->context = context;
    gui_ld_ac3_rf_process(decoder->rf, samples, count, first_sample, receive_symbol, decoder);
}

void gui_ld_rf_ac3_finish(gui_ld_rf_ac3_t *decoder) {
    if (decoder && decoder->have_input && decoder->pcm)
        gui_ld_ac3_frames_finish(decoder->framer, decode_frame, decoder);
}
