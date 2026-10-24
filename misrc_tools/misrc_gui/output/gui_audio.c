#include "gui_audio.h"

#include "../net/local_save_proxy.h"

#include "raylib.h"

#include "../../common/extract.h"
#include "../../common/wave.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"
#include "../../common/buffer_manager.h"
#include "../../common/file_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if LIBSOXR_ENABLED
#include <soxr.h>
#endif
/* Local Save proxy API redirects only recording files, never OS-level resources. */
#define fwrite mls_proxy_write
#define fseek mls_proxy_seek
#define fclose mls_proxy_close
#define fflush mls_proxy_flush
#define ferror mls_proxy_error
#define clearerr mls_proxy_clearerr
#define file_open_utf8 mls_proxy_open


// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

#define BUFFER_AUDIO_READ_SIZE  (65536 * 3)
#define AUDIO_DEFAULT_SAMPLE_RATE_HZ 78125U
#define AUDIO_CXADC_DEFAULT_SAMPLE_RATE_HZ 46875U

typedef struct {
    buffer_manager_t *bufmgr;
    gui_app_t *app;

    FILE *f_4ch;
    FILE *f_2ch[2];
    FILE *f_1ch[4];

    uint64_t total_bytes;

    bool convert_1ch;
    bool convert_2ch;

    uint8_t *buffer_1ch[4];
    uint8_t *buffer_2ch[2];
} audio_ctx_t;

static thrd_t s_audio_thread;
static bool s_audio_running = false;
static atomic_bool s_audio_stop = false;
static audio_ctx_t s_audio_ctx;

// Playback monitoring (audio thread produces into queue; main thread consumes into AudioStream)
static atomic_bool s_playback_enabled = false;
// Only the main-thread pump opens a capture monitoring session. Audio Mon
// controls mute separately; a stopped capture must reject late RF callbacks.
static atomic_bool s_monitor_session_active = false;
static AudioStream s_play_stream;
static bool s_play_stream_inited = false;
static int s_play_stream_chunk_frames;
static size_t s_play_stream_epoch;
static size_t s_play_stream_generation;
static bool s_audio_device_inited = false;

// Resampler for source audio rate -> 48kHz conversion
#if LIBSOXR_ENABLED
static soxr_t s_resampler = NULL;
static uint32_t s_resampler_input_rate_hz = 0;
static size_t s_resampler_epoch;
// While capture runs, only audio_thread_main may create, clear, replace or
// process this object. gui_audio_stop destroys it only after joining that owner.
#endif

// Both inputs share one device stream, but never one producer queue. Queue
// guards use a single try-lock: monitoring can drop a block, never stall RF or
// capture. A generation invalidates buffered PCM on source/mute/discontinuity.
#define PLAY_Q_FRAMES 96000
#define HIFI_Q_FRAMES 4096 // At most 85 ms at 48 kHz.
static int16_t s_play_q[PLAY_Q_FRAMES * 2];
static size_t s_play_q_head, s_play_q_count, s_play_q_epoch;
// A sidecar seek invalidates only the legacy input. Keeping this separate
// from the source epoch prevents a late seek from flushing selected HiFi.
static atomic_size_t s_play_q_generation = 1;
static size_t s_play_q_applied_generation;
static atomic_flag s_play_q_guard = ATOMIC_FLAG_INIT;
static int16_t s_hifi_q[HIFI_Q_FRAMES * 2];
static size_t s_hifi_q_head, s_hifi_q_count, s_hifi_q_epoch;
static atomic_flag s_hifi_q_guard = ATOMIC_FLAG_INIT;
static atomic_uintptr_t s_hifi_source = 0;
static atomic_size_t s_monitor_epoch = 1;
static atomic_bool s_stream_reset_requested = false;

static inline int32_t signext24(uint32_t v)
{
    v &= 0x00FFFFFFu;
    if (v & 0x00800000u) v |= 0xFF000000u;
    return (int32_t)v;
}

static void monitor_invalidate(void)
{
    atomic_fetch_add(&s_monitor_epoch, 1);
    atomic_store(&s_stream_reset_requested, true);
}

static bool monitor_legacy_selected(void)
{
    return atomic_load(&s_hifi_source) == 0;
}

static void play_q_push_samples_for_epoch(const int16_t *samples, size_t count,
                                          size_t epoch)
{
    const size_t generation = atomic_load(&s_play_q_generation);
    if (!samples || !count || !atomic_load(&s_playback_enabled) ||
        !atomic_load(&s_monitor_session_active) || !monitor_legacy_selected()) return;
    if (atomic_flag_test_and_set(&s_play_q_guard)) return;
    if (epoch != atomic_load(&s_monitor_epoch) ||
        generation != atomic_load(&s_play_q_generation) ||
        !atomic_load(&s_monitor_session_active)) {
        atomic_flag_clear(&s_play_q_guard);
        return;
    }
    if (s_play_q_epoch != epoch || s_play_q_applied_generation != generation) {
        s_play_q_head = s_play_q_count = 0;
        s_play_q_epoch = epoch;
        s_play_q_applied_generation = generation;
    }
    if (!atomic_load(&s_playback_enabled) || !atomic_load(&s_monitor_session_active) ||
        !monitor_legacy_selected()) {
        atomic_flag_clear(&s_play_q_guard);
        return;
    }
    const size_t cap = PLAY_Q_FRAMES * 2;
    count -= count % 2;
    if (count > cap) { samples += count - cap; count = cap; }
    if (count > cap - s_play_q_count) {
        size_t drop = count - (cap - s_play_q_count);
        s_play_q_head = (s_play_q_head + drop) % cap;
        s_play_q_count -= drop;
    }
    size_t tail = (s_play_q_head + s_play_q_count) % cap;
    size_t first = count < cap - tail ? count : cap - tail;
    memcpy(s_play_q + tail, samples, first * sizeof(*samples));
    memcpy(s_play_q, samples + first, (count - first) * sizeof(*samples));
    s_play_q_count += count;
    atomic_flag_clear(&s_play_q_guard);
}

void gui_audio_push_demod(const int16_t *stereo_48k, size_t frames)
{
    if (frames > SIZE_MAX / 2) return;
    play_q_push_samples_for_epoch(stereo_48k, frames * 2,
                                  atomic_load(&s_monitor_epoch));
}

// Sidecar PCM shares the ordinary monitor input and its nonblocking rules.
void gui_audio_push_playback(const int16_t *stereo_48k, size_t frames)
{
    gui_audio_push_demod(stereo_48k, frames);
}

void gui_audio_reset_playback_queue(void)
{
    if (monitor_legacy_selected()) atomic_fetch_add(&s_play_q_generation, 1);
}

static size_t play_q_pop_samples(int16_t *out, size_t max_count)
{
    if (!out || !max_count || atomic_flag_test_and_set(&s_play_q_guard)) return 0;
    const size_t epoch = atomic_load(&s_monitor_epoch);
    const size_t generation = atomic_load(&s_play_q_generation);
    if (s_play_q_epoch != epoch || s_play_q_applied_generation != generation) {
        s_play_q_head = s_play_q_count = 0;
        s_play_q_epoch = epoch;
        s_play_q_applied_generation = generation;
    }
    size_t n = s_play_q_count < max_count ? s_play_q_count : max_count;
    n -= n % 2;
    const size_t cap = PLAY_Q_FRAMES * 2;
    size_t first = n < cap - s_play_q_head ? n : cap - s_play_q_head;
    memcpy(out, s_play_q + s_play_q_head, first * sizeof(*out));
    memcpy(out + first, s_play_q, (n - first) * sizeof(*out));
    s_play_q_head = (s_play_q_head + n) % cap;
    s_play_q_count -= n;
    atomic_flag_clear(&s_play_q_guard);
    return epoch == atomic_load(&s_monitor_epoch) &&
           generation == atomic_load(&s_play_q_generation) ? n : 0;
}

void gui_audio_set_hifi_source(const void *source)
{
    uintptr_t next = (uintptr_t)source;
    if (atomic_exchange(&s_hifi_source, next) != next) monitor_invalidate();
}

const void *gui_audio_get_hifi_source(void)
{
    return (const void *)atomic_load(&s_hifi_source);
}

void gui_audio_reset_hifi(const void *source)
{
    if (source && atomic_load(&s_hifi_source) == (uintptr_t)source) {
        monitor_invalidate();
    }
}

static int16_t hifi_float_to_i16(float sample)
{
    // Test bits rather than isfinite: GUI builds use -ffast-math, which can
    // assume all floating-point inputs are finite and remove isfinite tests.
    uint32_t bits;
    memcpy(&bits, &sample, sizeof(bits));
    if ((bits & 0x7f800000u) == 0x7f800000u) return 0;
    if (sample >= 1.0f) return INT16_MAX;
    if (sample <= -1.0f) return INT16_MIN;
    return (int16_t)lrintf(sample * 32767.0f);
}

void gui_audio_push_hifi(const void *source, const float *stereo_48k, size_t frames)
{
    const size_t epoch = atomic_load(&s_monitor_epoch);
    if (!source || !stereo_48k || !frames || frames > SIZE_MAX / 2 ||
        !atomic_load(&s_playback_enabled) || !atomic_load(&s_monitor_session_active) ||
        atomic_load(&s_hifi_source) != (uintptr_t)source) return;
    if (atomic_flag_test_and_set(&s_hifi_q_guard)) return;
    if (epoch != atomic_load(&s_monitor_epoch) ||
        !atomic_load(&s_monitor_session_active)) {
        atomic_flag_clear(&s_hifi_q_guard);
        return;
    }
    if (s_hifi_q_epoch != epoch) {
        s_hifi_q_head = s_hifi_q_count = 0;
        s_hifi_q_epoch = epoch;
    }
    if (!atomic_load(&s_playback_enabled) || !atomic_load(&s_monitor_session_active) ||
        atomic_load(&s_hifi_source) != (uintptr_t)source) {
        atomic_flag_clear(&s_hifi_q_guard);
        return;
    }
    if (frames > HIFI_Q_FRAMES) {
        stereo_48k += (frames - HIFI_Q_FRAMES) * 2;
        frames = HIFI_Q_FRAMES;
    }
    if (frames > HIFI_Q_FRAMES - s_hifi_q_count) {
        size_t drop = frames - (HIFI_Q_FRAMES - s_hifi_q_count);
        s_hifi_q_head = (s_hifi_q_head + drop) % HIFI_Q_FRAMES;
        s_hifi_q_count -= drop;
    }
    size_t tail = (s_hifi_q_head + s_hifi_q_count) % HIFI_Q_FRAMES;
    for (size_t i = 0; i < frames; ++i) {
        size_t dst = ((tail + i) % HIFI_Q_FRAMES) * 2;
        s_hifi_q[dst] = hifi_float_to_i16(stereo_48k[i * 2]);
        s_hifi_q[dst + 1] = hifi_float_to_i16(stereo_48k[i * 2 + 1]);
    }
    s_hifi_q_count += frames;
    atomic_flag_clear(&s_hifi_q_guard);
}

static size_t hifi_q_pop_samples(int16_t *out, size_t max_count)
{
    if (!out || !max_count || atomic_flag_test_and_set(&s_hifi_q_guard)) return 0;
    const size_t epoch = atomic_load(&s_monitor_epoch);
    if (s_hifi_q_epoch != epoch) {
        s_hifi_q_head = s_hifi_q_count = 0;
        s_hifi_q_epoch = epoch;
    }
    size_t frames = s_hifi_q_count < max_count / 2 ? s_hifi_q_count : max_count / 2;
    for (size_t i = 0; i < frames; ++i) {
        size_t src = ((s_hifi_q_head + i) % HIFI_Q_FRAMES) * 2;
        out[i * 2] = s_hifi_q[src];
        out[i * 2 + 1] = s_hifi_q[src + 1];
    }
    s_hifi_q_head = (s_hifi_q_head + frames) % HIFI_Q_FRAMES;
    s_hifi_q_count -= frames;
    atomic_flag_clear(&s_hifi_q_guard);
    return epoch == atomic_load(&s_monitor_epoch) ? frames * 2 : 0;
}

static inline int16_t pcm24_to_i16(int32_t s24) {
    // s24 is signed 24-bit in int32; convert to 16-bit by dropping LSBs
    return (int16_t)(s24 >> 8);
}

static inline int32_t audio_read_s24le_channel(const uint8_t *frame, int channel_index)
{
    if (!frame || channel_index < 0 || channel_index > 3) return 0;
    size_t base = (size_t)channel_index * 3;
    uint32_t raw = (uint32_t)frame[base] |
                   ((uint32_t)frame[base + 1] << 8) |
                   ((uint32_t)frame[base + 2] << 16);
    return signext24(raw);
}

static bool gui_audio_selected_device_is_cxadc(const gui_app_t *app)
{
    if (!app) return false;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return false;
    // MISRC Clockgen is a clockgen-family device for audio purposes: it uses
    // the same 3-channel clockgen audio layout (CH1/2 + headswitch CH3) and the
    // same CH3 dual-mono monitor path as CXADC Clockgen.
    device_type_t t = app->devices[app->selected_device].type;
    return t == DEVICE_TYPE_CXADC || t == DEVICE_TYPE_MISRC_CLOCKGEN;
}

static uint32_t gui_audio_capture_rate_hz(const gui_app_t *app)
{
    if (!gui_audio_selected_device_is_cxadc(app)) {
        return AUDIO_DEFAULT_SAMPLE_RATE_HZ;
    }

    uint32_t rate = atomic_load(&app->audio_sample_rate);
    if (rate < 8000 || rate > 384000) {
        rate = AUDIO_CXADC_DEFAULT_SAMPLE_RATE_HZ;
    }
    return rate;
}

static void audio_update_peaks(gui_app_t *app, const uint8_t *buf, size_t len)
{
    if (!app || !buf) return;
    if (len < 12) return;

    uint32_t peak[4] = {0, 0, 0, 0};

    // The clockgen device streams 3 channels (CH1/CH2 + headswitch CH3).
    // CH4 is a mirror of CH3 written by the capture thread, but on MISRC
    // Clockgen the 3-channel device data may be mangled by Windows shared-
    // mode resampling, so CH3/CH4 contain invalid data. Only update peaks
    // for CH1/CH2 on MISRC Clockgen; leave CH3/CH4 at 0 (no valid signal).
    const bool is_misrc_clockgen =
        app->selected_device >= 0 && app->selected_device < app->device_count &&
        app->devices[app->selected_device].type == DEVICE_TYPE_MISRC_CLOCKGEN;
    const int peak_channels = is_misrc_clockgen ? 2 : 4;

    // 4ch 24-bit, little-endian, interleaved per sample frame:
    // ch1: b0 b1 b2, ch2: b3 b4 b5, ch3: b6 b7 b8, ch4: b9 b10 b11
    for (size_t i = 0; i + 12 <= len; i += 12) {
        for (int ch = 0; ch < peak_channels; ch++) {
            size_t o = i + (size_t)ch * 3;
            uint32_t raw = (uint32_t)buf[o] | ((uint32_t)buf[o + 1] << 8) | ((uint32_t)buf[o + 2] << 16);
            int32_t s = signext24(raw);
            uint32_t a = (uint32_t)(s < 0 ? -s : s);
            if (a > peak[ch]) peak[ch] = a;
        }
    }

    for (int ch = 0; ch < 4; ch++) {
        atomic_store(&app->audio_peak[ch], peak[ch]);
    }
}

/* --- F1: record-alignment helpers ---------------------------------------
 *
 * The clockgen capture path writes BUF_CAPTURE_AUDIO in 576 B blocks
 * (48 frames x 12 B = ~1 ms @ 46,875 Hz, matching the ALSA readi period).
 * Reading/discarding at that same granularity reaches the ring's current
 * write position ("now") with at most one producer block of residual.
 */
#define AUDIO_SYNC_CHUNK_BYTES 576

// Write one already-read audio block to the enabled output files using the
// same conversion path as the main loop (4ch direct + 1ch/2ch conversions).
static void audio_write_recorded_block(audio_ctx_t *a, const uint8_t *buf, size_t n)
{
    if (!a || !buf || n == 0) return;
    if (a->f_4ch) fwrite(buf, 1, n, a->f_4ch);
    if (a->convert_1ch) {
        extract_audio_1ch_C((uint8_t *)buf, n,
                            a->buffer_1ch[0], a->buffer_1ch[1],
                            a->buffer_1ch[2], a->buffer_1ch[3]);
        for (int i = 0; i < 4; i++) {
            if (a->f_1ch[i]) fwrite(a->buffer_1ch[i], 1, n / 4, a->f_1ch[i]);
        }
    }
    if (a->convert_2ch) {
        extract_audio_2ch_C((uint16_t *)buf, n,
                            (uint16_t *)a->buffer_2ch[0],
                            (uint16_t *)a->buffer_2ch[1]);
        for (int i = 0; i < 2; i++) {
            if (a->f_2ch[i]) fwrite(a->buffer_2ch[i], 1, n / 2, a->f_2ch[i]);
        }
    }
    a->total_bytes += n;
}

// Discard all currently-buffered capture audio (up to the ring's write
// position = "now") so a recording session starts at the transition instant
// instead of writing up to one full read block (~350 ms) of pre-record audio.
static uint64_t audio_discard_ring_to_now(audio_ctx_t *a)
{
    uint64_t dropped = 0;
    if (!a || !a->bufmgr) return 0;
    for (;;) {
        void *d = bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO,
                                    AUDIO_SYNC_CHUNK_BYTES, 0);
        if (!d) break;
        bufmgr_read_end(a->bufmgr, BUF_CAPTURE_AUDIO, AUDIO_SYNC_CHUNK_BYTES);
        dropped += AUDIO_SYNC_CHUNK_BYTES;
    }
    return dropped;
}

// Flush all currently-buffered capture audio (up to "now") to the open
// output files so a recording session ends at the transition instant instead
// of losing up to one full read block (~350 ms) of tail audio.
static uint64_t audio_flush_ring_to_files(audio_ctx_t *a)
{
    uint64_t flushed = 0;
    if (!a || !a->bufmgr) return 0;
    for (;;) {
        void *d = bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO,
                                    AUDIO_SYNC_CHUNK_BYTES, 0);
        if (!d) break;
        audio_update_peaks(a->app, (const uint8_t *)d, AUDIO_SYNC_CHUNK_BYTES);
        audio_write_recorded_block(a, (const uint8_t *)d, AUDIO_SYNC_CHUNK_BYTES);
        bufmgr_read_end(a->bufmgr, BUF_CAPTURE_AUDIO, AUDIO_SYNC_CHUNK_BYTES);
        flushed += AUDIO_SYNC_CHUNK_BYTES;
    }
    return flushed;
}

#if LIBSOXR_ENABLED
static void audio_monitor_captured_pcm(gui_app_t *app, const uint8_t *buf, size_t len)
{
    const size_t epoch = atomic_load(&s_monitor_epoch);
    if (!app || !buf || !atomic_load(&s_playback_enabled) ||
        !atomic_load(&s_monitor_session_active) || !monitor_legacy_selected()) return;
    const uint32_t monitor_input_rate_hz = gui_audio_capture_rate_hz(app);
    if (!s_resampler || s_resampler_input_rate_hz != monitor_input_rate_hz) {
        if (s_resampler) soxr_delete(s_resampler);
        s_resampler = NULL;
        s_resampler_input_rate_hz = 0;
        soxr_error_t err = NULL;
        soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
        soxr_quality_spec_t qual = soxr_quality_spec(SOXR_HQ, 0);
        s_resampler = soxr_create((double)monitor_input_rate_hz, 48000.0, 2,
                                  &err, &io_spec, &qual, NULL);
        if (err || !s_resampler) {
            fprintf(stderr, "[AUDIO] Failed to create resampler: %s\n",
                    err ? soxr_strerror(err) : "allocation failed");
            if (s_resampler) soxr_delete(s_resampler);
            s_resampler = NULL;
            return;
        }
        s_resampler_input_rate_hz = monitor_input_rate_hz;
        s_resampler_epoch = epoch;
        fprintf(stderr, "[AUDIO] Created resampler %uHz -> 48kHz\n",
                monitor_input_rate_hz);
    } else if (s_resampler_epoch != epoch) {
        // A mute, source or capture transition is a fresh signal even when
        // the sample rate stayed constant. Never replay filter history.
        soxr_clear(s_resampler);
        s_resampler_epoch = epoch;
    }
    const uint8_t *b = buf;
    const size_t frames_in = len / 12; // 4ch * 3 bytes per frame

    // Use static buffers to avoid alloca issues
    static int16_t tmp_in[65536];
    static int16_t tmp_48khz[65536];

    if (frames_in * 2 > 65536) {
        fprintf(stderr, "[AUDIO] Buffer too large for monitoring: %zu frames\n", frames_in);
        return;
    }

    // Downmix 4ch 24-bit to stereo 16-bit at source sample rate.
    // Select CH1/2 or CH3/4 based on user setting.
    // For CXADC sources, CH3 is headswitch and CH4 may be silence/
    // mirror depending on backend mapping; use CH3 dual-mono in the
    // CH3/4 monitor mode so the control always produces valid output.
    const bool use_ch34 = app->settings.audio_monitor_ch34;
    const bool cxadc_mode = gui_audio_selected_device_is_cxadc(app);
    // MISRC Clockgen: CH3/CH4 contain invalid data (Windows shared-
    // mode resampling corrupts the headswitch). Force CH1/2 monitoring
    // regardless of the user's CH3/4 toggle so monitoring produces valid
    // audio from the baseband channels only.
    const bool misrc_clockgen =
        app->selected_device >= 0 &&
        app->selected_device < app->device_count &&
        app->devices[app->selected_device].type == DEVICE_TYPE_MISRC_CLOCKGEN;
    const bool effective_use_ch34 = use_ch34 && cxadc_mode && !misrc_clockgen;

    for (size_t i = 0; i < frames_in; i++) {
        const uint8_t *frame = b + (i * 12);
        int16_t s16 = 0;
        if (effective_use_ch34 && cxadc_mode) {
            int32_t hs = audio_read_s24le_channel(frame, 2); // CH3 headswitch
            s16 = pcm24_to_i16(hs);
        } else {
            int ch_a = use_ch34 ? 2 : 0;
            int ch_b = use_ch34 ? 3 : 1;
            int32_t s1 = audio_read_s24le_channel(frame, ch_a);
            int32_t s2 = audio_read_s24le_channel(frame, ch_b);
            int32_t mix = (s1 / 2) + (s2 / 2);
            s16 = pcm24_to_i16(mix);
        }
        tmp_in[i * 2 + 0] = s16;
        tmp_in[i * 2 + 1] = s16;
    }

    // Resample source-rate -> 48kHz.
    size_t frames_48khz_max = (size_t)((double)frames_in * 48000.0 / (double)monitor_input_rate_hz) + 64;
    if (frames_48khz_max * 2 > 65536) {
        fprintf(stderr, "[AUDIO] Output buffer too large: %zu frames\n", frames_48khz_max);
        return;
    }

    size_t idone = 0, odone = 0;
    soxr_error_t err = soxr_process(s_resampler,
        tmp_in, frames_in, &idone,
        tmp_48khz, frames_48khz_max, &odone);

    if (!err && odone > 0) {
        play_q_push_samples_for_epoch(tmp_48khz, odone * 2, epoch);
    } else if (err) {
        static int err_count = 0;
        if (err_count < 3) {
            fprintf(stderr, "[AUDIO] Resampler error: %s\n", soxr_strerror(err));
            err_count++;
        }
    }
}
#endif

static int audio_thread_main(void *ctx)
{
    audio_ctx_t *a = (audio_ctx_t *)ctx;
    size_t len = BUFFER_AUDIO_READ_SIZE;
    void *buf;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);
    
    fprintf(stderr, "[AUDIO] Audio thread started\n");

    wave_header_t h;
    memset(&h, 0, sizeof(h));

    a->total_bytes = 0;

    // Write placeholder headers
    if (a->f_4ch && a->f_4ch != stdout) fwrite(&h, 1, sizeof(h), a->f_4ch);
    for (int i = 0; i < 2; i++) {
        if (a->f_2ch[i] && a->f_2ch[i] != stdout) fwrite(&h, 1, sizeof(h), a->f_2ch[i]);
    }
    for (int i = 0; i < 4; i++) {
        if (a->f_1ch[i] && a->f_1ch[i] != stdout) fwrite(&h, 1, sizeof(h), a->f_1ch[i]);
    }

    // Allocate conversion buffers if needed
    if (a->convert_1ch) {
        a->buffer_1ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
        if (!a->buffer_1ch[0]) return -1;
        for (int i = 1; i < 4; i++) a->buffer_1ch[i] = a->buffer_1ch[0] + (BUFFER_AUDIO_READ_SIZE / 4) * i;
    }
    if (a->convert_2ch) {
        a->buffer_2ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
        if (!a->buffer_2ch[0]) return -1;
        a->buffer_2ch[1] = a->buffer_2ch[0] + (BUFFER_AUDIO_READ_SIZE / 2);
    }

    size_t iter_count = 0;
    // gui_audio_start may pre-open output files when a record is already
    // active at thread start; begin in the recording state so the post-read
    // open-files transition below does not double-open them.
    bool was_recording = (a->f_4ch != NULL) || (a->f_2ch[0] != NULL) ||
                         (a->f_2ch[1] != NULL) || (a->f_1ch[0] != NULL) ||
                         (a->f_1ch[1] != NULL) || (a->f_1ch[2] != NULL) ||
                         (a->f_1ch[3] != NULL);
    bool rec_start_aligned = false;
    while (1) {
        /*
         * F1 record-alignment transitions, handled at the TOP of every
         * loop iteration. Each iteration is bounded by the 2 ms read
         * timeout, so the record button is observed within ~2-4 ms.
         * Previously the transitions were only observed after a full
         * BUFFER_AUDIO_READ_SIZE (196,608 B ~= 350 ms of clockgen audio)
         * block read succeeded, which wrote up to one full block of
         * pre-record audio at record START (the WAV led the RF files by
         * 0-350 ms). Now: START discards everything buffered before the
         * transition (the ring's write position = "now", at the capture
         * thread's 576 B write granularity = the ~1 ms USB delivery
         * quantum) so the WAV files begin at the record instant. At record
         * STOP the record-stop entry stops + restarts this thread within
         * ~us of clearing is_recording, so the exit-drain path normally
         * ends the files (draining the ring to "now", ~stop + 2-10 ms);
         * the STOP flush here is the safety net for any path where the
         * thread survives a record stop. The post-read open/close blocks
         * below still own the file lifecycle.
         */
        {
            bool rec_now = a->app && a->app->is_recording;
            if (rec_now && !rec_start_aligned) {
                rec_start_aligned = true;
                uint64_t dropped = audio_discard_ring_to_now(a);
                uint32_t rate_hz = gui_audio_capture_rate_hz(a->app);
                if (rate_hz == 0) rate_hz = AUDIO_CXADC_DEFAULT_SAMPLE_RATE_HZ;
                double dropped_ms = (double)dropped / 12.0 / (double)rate_hz * 1000.0;
                fprintf(stderr,
                        "[AUDIO] Record start aligned: discarded %llu B (~%.1f ms) of pre-record audio\n",
                        (unsigned long long)dropped, dropped_ms);
            } else if (!rec_now && rec_start_aligned) {
                rec_start_aligned = false;
                uint64_t flushed = audio_flush_ring_to_files(a);
                uint32_t rate_hz = gui_audio_capture_rate_hz(a->app);
                if (rate_hz == 0) rate_hz = AUDIO_CXADC_DEFAULT_SAMPLE_RATE_HZ;
                double flushed_ms = (double)flushed / 12.0 / (double)rate_hz * 1000.0;
                fprintf(stderr,
                        "[AUDIO] Record stop aligned: flushed %llu B (~%.1f ms) of tail audio\n",
                        (unsigned long long)flushed, flushed_ms);
            }
        }
        // Read from buffer manager with timeout. The 2 ms timeout keeps the
        // loop spinning at ~500 Hz so the record-state transition above is
        // observed within ~2-4 ms of the record button (the old 10 ms timeout
        // bounded it at ~10-20 ms).
        buf = bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, len, 2);
        if (!buf) {
            if (iter_count < 5) {
                fprintf(stderr, "[AUDIO] No data in buffer (iter %zu), fill_level=%zu\n", 
                        iter_count, bufmgr_fill_level(a->bufmgr, BUF_CAPTURE_AUDIO));
            }
            iter_count++;
            if (atomic_load(&do_exit) || atomic_load(&s_audio_stop) || !a->app || !a->app->is_capturing) {
                // Drain any remaining data before exiting
                size_t remaining = bufmgr_fill_level(a->bufmgr, BUF_CAPTURE_AUDIO);
                while (remaining > 0) {
                    size_t drain_len = (remaining < len) ? remaining : len;
                    buf = bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, drain_len, 0);
                    if (!buf) break;

                    audio_update_peaks(a->app, (const uint8_t *)buf, drain_len);
                    if (a->f_4ch) fwrite(buf, 1, drain_len, a->f_4ch);
                    if (a->convert_1ch) extract_audio_1ch_C((uint8_t *)buf, drain_len, a->buffer_1ch[0], a->buffer_1ch[1], a->buffer_1ch[2], a->buffer_1ch[3]);
                    if (a->convert_2ch) extract_audio_2ch_C((uint16_t *)buf, drain_len, (uint16_t *)a->buffer_2ch[0], (uint16_t *)a->buffer_2ch[1]);

                    bufmgr_read_end(a->bufmgr, BUF_CAPTURE_AUDIO, drain_len);

                    for (int i = 0; i < 2; i++) if (a->f_2ch[i]) fwrite(a->buffer_2ch[i], 1, drain_len / 2, a->f_2ch[i]);
                    for (int i = 0; i < 4; i++) if (a->f_1ch[i]) fwrite(a->buffer_1ch[i], 1, drain_len / 4, a->f_1ch[i]);

                    a->total_bytes += drain_len;
                    remaining = bufmgr_fill_level(a->bufmgr, BUF_CAPTURE_AUDIO);
                }
                break;
            }
            continue;
        }

        // Update monitoring peaks
        audio_update_peaks(a->app, (const uint8_t *)buf, len);
        
        if (iter_count < 5) {
            fprintf(stderr, "[AUDIO] Got data: %zu bytes, peaks will be updated\n", len);
        }
        iter_count++;

#if LIBSOXR_ENABLED
        audio_monitor_captured_pcm(a->app, (const uint8_t *)buf, len);
#endif

        // Check if recording state changed
        bool is_recording = a->app && a->app->is_recording;
        
        // RECORDING JUST STARTED - open files dynamically
        if (!was_recording && is_recording) {
            fprintf(stderr, "[AUDIO] Recording started, opening files\n");
            
            char path[FILE_PATH_MAX + MAX_FILENAME_LEN];
            wave_header_t hdr;
            memset(&hdr, 0, sizeof(hdr));
            
            if (a->app->settings.enable_audio_4ch) {
                snprintf(path, sizeof(path), "%s/%s", a->app->settings.output_path, a->app->settings.audio_4ch_filename);
                a->f_4ch = file_open_utf8(path, "wb");
                if (a->f_4ch && a->f_4ch != stdout) fwrite(&hdr, 1, sizeof(hdr), a->f_4ch);
            }
            
            if (a->app->settings.enable_audio_2ch_12) {
                snprintf(path, sizeof(path), "%s/%s", a->app->settings.output_path, a->app->settings.audio_2ch_12_filename);
                a->f_2ch[0] = file_open_utf8(path, "wb");
                if (a->f_2ch[0] && a->f_2ch[0] != stdout) fwrite(&hdr, 1, sizeof(hdr), a->f_2ch[0]);
                a->convert_2ch = true;
            }
            
            if (a->app->settings.enable_audio_2ch_34) {
                snprintf(path, sizeof(path), "%s/%s", a->app->settings.output_path, a->app->settings.audio_2ch_34_filename);
                a->f_2ch[1] = file_open_utf8(path, "wb");
                if (a->f_2ch[1] && a->f_2ch[1] != stdout) fwrite(&hdr, 1, sizeof(hdr), a->f_2ch[1]);
                a->convert_2ch = true;
            }
            
            for (int i = 0; i < 4; i++) {
                if (a->app->settings.enable_audio_1ch[i]) {
                    snprintf(path, sizeof(path), "%s/%s", a->app->settings.output_path, a->app->settings.audio_1ch_filenames[i]);
                    a->f_1ch[i] = file_open_utf8(path, "wb");
                    if (a->f_1ch[i] && a->f_1ch[i] != stdout) fwrite(&hdr, 1, sizeof(hdr), a->f_1ch[i]);
                    a->convert_1ch = true;
                }
            }
            
a->total_bytes = 0;
            
            // Allocate conversion buffers if needed and not already allocated
            if (a->convert_1ch && !a->buffer_1ch[0]) {
                a->buffer_1ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
                if (!a->buffer_1ch[0]) {
                    fprintf(stderr, "[AUDIO] Failed to allocate 1ch buffer\n");
                    // Close 1ch files and reset state
                    for (int i = 0; i < 4; i++) {
                        if (a->f_1ch[i]) { fclose(a->f_1ch[i]); a->f_1ch[i] = NULL; }
                    }
                    a->convert_1ch = false;
                    goto skip_file_ops;
                }
                for (int i = 1; i < 4; i++) a->buffer_1ch[i] = a->buffer_1ch[0] + (BUFFER_AUDIO_READ_SIZE / 4) * i;
            }
            if (a->convert_2ch && !a->buffer_2ch[0]) {
                a->buffer_2ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
                if (!a->buffer_2ch[0]) {
                    fprintf(stderr, "[AUDIO] Failed to allocate 2ch buffer\n");
                    // Close 2ch files and reset state
                    if (a->f_2ch[0]) { fclose(a->f_2ch[0]); a->f_2ch[0] = NULL; }
                    if (a->f_2ch[1]) { fclose(a->f_2ch[1]); a->f_2ch[1] = NULL; }
                    a->convert_2ch = false;
                    goto skip_file_ops;
                }
                a->buffer_2ch[1] = a->buffer_2ch[0] + (BUFFER_AUDIO_READ_SIZE / 2);
            }
        }
        
skip_file_ops:
        ;
        
        // RECORDING JUST STOPPED - close files and write headers
        if (was_recording && !is_recording) {
            wave_header_t h;
            uint32_t capture_rate_hz = gui_audio_capture_rate_hz(a->app);
            if (a->f_4ch && a->f_4ch != stdout) {
                fseek(a->f_4ch, 0, SEEK_SET);
                create_wave_header(&h, a->total_bytes / 12, capture_rate_hz, 4, 24);
                fwrite(&h, 1, sizeof(h), a->f_4ch);
                fclose(a->f_4ch);
                a->f_4ch = NULL;
            }
            for (int i = 0; i < 2; i++) {
                if (a->f_2ch[i] && a->f_2ch[i] != stdout) {
                    fseek(a->f_2ch[i], 0, SEEK_SET);
                    create_wave_header(&h, a->total_bytes / 12, capture_rate_hz, 2, 24);
                    fwrite(&h, 1, sizeof(h), a->f_2ch[i]);
                    fclose(a->f_2ch[i]);
                    a->f_2ch[i] = NULL;
                }
            }
            for (int i = 0; i < 4; i++) {
                if (a->f_1ch[i] && a->f_1ch[i] != stdout) {
                    fseek(a->f_1ch[i], 0, SEEK_SET);
                    create_wave_header(&h, a->total_bytes / 12, capture_rate_hz, 1, 24);
                    fwrite(&h, 1, sizeof(h), a->f_1ch[i]);
                    fclose(a->f_1ch[i]);
                    a->f_1ch[i] = NULL;
                }
            }
            fprintf(stderr, "[AUDIO] Recording stopped, files closed\n");
            // DO NOT reset a->total_bytes here
        }
        
        was_recording = is_recording;

        // Write files and extract only if recording active
        if (is_recording) {
            if (a->f_4ch) fwrite(buf, 1, len, a->f_4ch);
            if (a->convert_1ch) extract_audio_1ch_C((uint8_t *)buf, len, a->buffer_1ch[0], a->buffer_1ch[1], a->buffer_1ch[2], a->buffer_1ch[3]);
            if (a->convert_2ch) extract_audio_2ch_C((uint16_t *)buf, len, (uint16_t *)a->buffer_2ch[0], (uint16_t *)a->buffer_2ch[1]);
        }
        
        bufmgr_read_end(a->bufmgr, BUF_CAPTURE_AUDIO, len);

        if (is_recording) {
            for (int i = 0; i < 2; i++) if (a->f_2ch[i]) fwrite(a->buffer_2ch[i], 1, len / 2, a->f_2ch[i]);
            for (int i = 0; i < 4; i++) if (a->f_1ch[i]) fwrite(a->buffer_1ch[i], 1, len / 4, a->f_1ch[i]);
            a->total_bytes += len;
        }
    }

    // Cleanup at thread exit
    uint32_t capture_rate_hz = gui_audio_capture_rate_hz(a->app);
    if (a->f_4ch && a->f_4ch != stdout) {
        fseek(a->f_4ch, 0, SEEK_SET);
        create_wave_header(&h, a->total_bytes / 12, capture_rate_hz, 4, 24);
        fwrite(&h, 1, sizeof(h), a->f_4ch);
        fclose(a->f_4ch);
    }
    for (int i = 0; i < 2; i++) {
        if (a->f_2ch[i] && a->f_2ch[i] != stdout) {
            fseek(a->f_2ch[i], 0, SEEK_SET);
            create_wave_header(&h, a->total_bytes / 12, capture_rate_hz, 2, 24);
            fwrite(&h, 1, sizeof(h), a->f_2ch[i]);
            fclose(a->f_2ch[i]);
        }
    }
    for (int i = 0; i < 4; i++) {
        if (a->f_1ch[i] && a->f_1ch[i] != stdout) {
            fseek(a->f_1ch[i], 0, SEEK_SET);
            create_wave_header(&h, a->total_bytes / 12, capture_rate_hz, 1, 24);
            fwrite(&h, 1, sizeof(h), a->f_1ch[i]);
            fclose(a->f_1ch[i]);
        }
    }

    if (a->convert_1ch && a->buffer_1ch[0]) aligned_free(a->buffer_1ch[0]);
    if (a->convert_2ch && a->buffer_2ch[0]) aligned_free(a->buffer_2ch[0]);

    return 0;
}

bool gui_audio_is_running(void)
{
    return s_audio_running;
}

int gui_audio_start(gui_app_t *app, buffer_manager_t *bufmgr)
{
    if (!app || !bufmgr) return -1;
    if (s_audio_running) return 0;

    atomic_store(&s_playback_enabled, app->settings.audio_monitor_playback);

    // Ensure audio buffer is initialized
    if (bufmgr_ensure_init(bufmgr, BUF_CAPTURE_AUDIO) < 0) {
        gui_app_set_status(app, "Failed to initialize audio buffer");
        return -1;
    }

    // Decide if audio outputs should be written to files.
    // Audio capture is always-on during capture for monitoring/draining.
    const bool write_files = app->is_recording;

    bool want_4ch = write_files && app->settings.enable_audio_4ch;
    bool want_2ch_12 = write_files && app->settings.enable_audio_2ch_12;
    bool want_2ch_34 = write_files && app->settings.enable_audio_2ch_34;

    memset(&s_audio_ctx, 0, sizeof(s_audio_ctx));
    s_audio_ctx.bufmgr = bufmgr;
    s_audio_ctx.app = app;

    // Open files under output_path
    char path[FILE_PATH_MAX + MAX_FILENAME_LEN];

    if (want_4ch) {
        snprintf(path, sizeof(path), "%s/%s", app->settings.output_path, app->settings.audio_4ch_filename);
        s_audio_ctx.f_4ch = file_open_utf8(path, "wb");
        if (!s_audio_ctx.f_4ch) {
            gui_app_set_status(app, "Failed to open audio_4ch output");
            return -1;
        }
    }

    if (want_2ch_12) {
        snprintf(path, sizeof(path), "%s/%s", app->settings.output_path, app->settings.audio_2ch_12_filename);
        s_audio_ctx.f_2ch[0] = file_open_utf8(path, "wb");
        if (!s_audio_ctx.f_2ch[0]) {
            gui_app_set_status(app, "Failed to open audio_2ch_12 output");
            return -1;
        }
        s_audio_ctx.convert_2ch = true;
    }

    if (want_2ch_34) {
        snprintf(path, sizeof(path), "%s/%s", app->settings.output_path, app->settings.audio_2ch_34_filename);
        s_audio_ctx.f_2ch[1] = file_open_utf8(path, "wb");
        if (!s_audio_ctx.f_2ch[1]) {
            gui_app_set_status(app, "Failed to open audio_2ch_34 output");
            return -1;
        }
        s_audio_ctx.convert_2ch = true;
    }

    for (int i = 0; i < 4; i++) {
        if (write_files && app->settings.enable_audio_1ch[i]) {
            snprintf(path, sizeof(path), "%s/%s", app->settings.output_path, app->settings.audio_1ch_filenames[i]);
            s_audio_ctx.f_1ch[i] = file_open_utf8(path, "wb");
            if (!s_audio_ctx.f_1ch[i]) {
                gui_app_set_status(app, "Failed to open audio_1ch output");
                return -1;
            }
            s_audio_ctx.convert_1ch = true;
        }
    }

    // Reset peaks
    for (int i = 0; i < 4; i++) atomic_store(&app->audio_peak[i], 0);

    atomic_store(&s_audio_stop, false);

    if (thrd_create_with_priority(&s_audio_thread,
                                  audio_thread_main,
                                  &s_audio_ctx,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        gui_app_set_status(app, "Failed to start audio thread");
        return -1;
    }

    s_audio_running = true;
    return 0;
}

void gui_audio_stop(gui_app_t *app)
{
    (void)app;
    atomic_store(&s_monitor_session_active, false);
    monitor_invalidate();
    if (s_audio_running) {
        atomic_store(&s_audio_stop, true);
        thrd_join(s_audio_thread, NULL);
        s_audio_running = false;
    }
    
    // Cleanup audio stream and resampler - no goat. Honestly.
    if (s_play_stream_inited) {
        StopAudioStream(s_play_stream);
        UnloadAudioStream(s_play_stream);
        s_play_stream_inited = false;
    }
#if LIBSOXR_ENABLED
    if (s_resampler) {
        soxr_delete(s_resampler);
        s_resampler = NULL;
        s_resampler_input_rate_hz = 0;
        s_resampler_epoch = 0;
    }
#endif
}

void gui_audio_set_playback_enabled(gui_app_t *app, bool enabled)
{
    if (!app) return;
    
    // Stop and cleanup existing stream when disabling
    if (!enabled && s_play_stream_inited) {
        StopAudioStream(s_play_stream);
        UnloadAudioStream(s_play_stream);
        s_play_stream_inited = false;
    }
    
    app->settings.audio_monitor_playback = enabled;
    atomic_store(&s_playback_enabled, enabled);
    gui_settings_save(&app->settings);

    // The capture producer observes this generation and clears its own
    // resampler before the next process call; GUI controls never mutate it.
    monitor_invalidate();
}

void gui_audio_update_playback(gui_app_t *app)
{
    if (!app) return;
    // File playback/DDD can run without gui_audio_start. Honor a persisted
    // Audio Mon setting there too, and propagate any settings-only change.
    const bool enabled = app->settings.audio_monitor_playback;
    if (atomic_exchange(&s_playback_enabled, enabled) != enabled) monitor_invalidate();
    if (!app->is_capturing) {
        // DDD/file-playback stops need not run gui_audio_stop(). Observe the
        // capture boundary here as well, before any early return for mute.
        if (atomic_exchange(&s_monitor_session_active, false)) monitor_invalidate();
        if (s_play_stream_inited) {
            StopAudioStream(s_play_stream);
            UnloadAudioStream(s_play_stream);
            s_play_stream_inited = false;
        }
        return;
    }
    if (!atomic_load(&s_monitor_session_active)) {
        // Invalidate both queues and destroy old device buffers before letting
        // callbacks populate the new session, including a same-token restart.
        monitor_invalidate();
        if (s_play_stream_inited) {
            StopAudioStream(s_play_stream);
            UnloadAudioStream(s_play_stream);
            s_play_stream_inited = false;
        }
        atomic_store(&s_monitor_session_active, true);
    }
    if (!enabled) {
        if (s_play_stream_inited) {
            StopAudioStream(s_play_stream);
            UnloadAudioStream(s_play_stream);
            s_play_stream_inited = false;
        }
        return;
    }
    // All raylib device/stream calls stay on the GUI thread. An RF worker
    // requests a flush atomically; it never operates the audio device itself.
    const size_t epoch = atomic_load(&s_monitor_epoch);
    const bool legacy_input = monitor_legacy_selected();
    const size_t legacy_generation = atomic_load(&s_play_q_generation);
    const int frames_per_chunk = legacy_input ? 4096 : 1024;
    const bool reset_requested = atomic_exchange(&s_stream_reset_requested, false);
    if (s_play_stream_inited && (reset_requested ||
        s_play_stream_epoch != epoch || s_play_stream_chunk_frames != frames_per_chunk ||
        (legacy_input && s_play_stream_generation != legacy_generation))) {
        StopAudioStream(s_play_stream);
        UnloadAudioStream(s_play_stream);
        s_play_stream_inited = false;
    }

    if (!s_audio_device_inited) {
        InitAudioDevice();
        s_audio_device_inited = IsAudioDeviceReady();
        if (!s_audio_device_inited) return;
    }

    if (!s_play_stream_inited) {
        // Set buffer size BEFORE creating stream
        SetAudioStreamBufferSizeDefault(frames_per_chunk);
        
        // Stream at 48kHz (resampled audio)
        s_play_stream = LoadAudioStream(48000, 16, 2);
        if (!IsAudioStreamValid(s_play_stream)) return;
        PlayAudioStream(s_play_stream);
        s_play_stream_inited = true;
        s_play_stream_chunk_frames = frames_per_chunk;
        s_play_stream_epoch = epoch;
        s_play_stream_generation = legacy_generation;
        fprintf(stderr, "[AUDIO] Initialized playback stream at 48kHz (buffer: %d frames)\n",
                frames_per_chunk);
    }

    if (!IsAudioStreamPlaying(s_play_stream)) {
        PlayAudioStream(s_play_stream);
    }

    // A 1024-frame HiFi half-buffer holds 21 ms. Refill both halves if a
    // slower GUI frame consumed both, without unbounded work on the UI thread.
    for (unsigned int fed = 0; fed < 2 && IsAudioStreamProcessed(s_play_stream); ++fed) {
        if (s_play_stream_epoch != atomic_load(&s_monitor_epoch) ||
            (legacy_input && legacy_generation != atomic_load(&s_play_q_generation))) return;
        int16_t chunk[4096 * 2]; // Maximum ordinary-monitor stereo chunk.
        size_t samples_wanted = frames_per_chunk * 2;
        size_t got = legacy_input
            ? play_q_pop_samples(chunk, samples_wanted)
            : hifi_q_pop_samples(chunk, samples_wanted);
        if (s_play_stream_epoch != atomic_load(&s_monitor_epoch) ||
            (legacy_input && legacy_generation != atomic_load(&s_play_q_generation))) return;
        
        if (got >= samples_wanted) {
            UpdateAudioStream(s_play_stream, chunk, frames_per_chunk);
        } else if (got > 0) {
            // Partial data - pad with silence to avoid glitches
            memset(chunk + got, 0, (samples_wanted - got) * sizeof(int16_t));
            UpdateAudioStream(s_play_stream, chunk, frames_per_chunk);
        } else {
            // Complete underrun: feed silence
            memset(chunk, 0, sizeof(chunk));
            UpdateAudioStream(s_play_stream, chunk, frames_per_chunk);
        }
    }
}
