/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CXADC_CLOCKGEN_AUDIO_H
#define CXADC_CLOCKGEN_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CXADC_AUDIO_MAX_FRAMES 4096u
/* Native USB L/R/head-switch, packed as four s24le channels for the existing
 * GUI audio pipeline. Channel 4 is always zero; channel 3 is NOT audio.
 * No resampling, normalization or DSP. CoreAudio's native float representation
 * of the signed 24-bit USB samples is converted back exactly.
 * read is single-consumer. Join readers before stop/close. RT callback never
 * allocates, waits or writes to disk. A bounded-queue overflow fails visibly.
 * Rate=0 preserves the device's current 46875/48000 Hz rate. */
typedef struct cxadc_audio cxadc_audio;
typedef struct {
    uint32_t rate_hz, channels, running, error, stop_verified;
    uint64_t captured_frames, delivered_frames, first_host_ns;
    uint64_t timestamp_gaps, queue_overruns;
} cxadc_audio_state;
typedef struct {
    uint32_t abi_version, struct_bytes;
    int32_t (*detect)(void); /* 1=one matching input, 0=none, -1=ambiguous/error */
    int32_t (*open)(uint32_t rate_hz, cxadc_audio **);
    int32_t (*start)(cxadc_audio *);
    int32_t (*read)(cxadc_audio *, void *s24le_four_channels, size_t capacity_frames,
                    size_t *frames, uint64_t *first_host_ns);
    int32_t (*stop)(cxadc_audio *);
    int32_t (*status)(cxadc_audio *, cxadc_audio_state *);
    void (*close)(cxadc_audio *);
    const char *(*error)(void);
} cxadc_audio_api;
const cxadc_audio_api *cxadc_clockgen_audio_get_api_v1(void);
#ifdef __cplusplus
}
#endif
#endif
