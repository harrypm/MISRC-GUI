#include "gui_audio.h"
#include "../net/monitor_local_save_proxy.h"

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

/* Only M-Server Local Save wraps files. Genuine stdio remains unchanged. */
#define fwrite mls_proxy_write
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
    uint32_t record_rate_hz;

    bool convert_1ch;
    bool convert_2ch;

    uint8_t *buffer_1ch[4];
    uint8_t *buffer_2ch[2];
} audio_ctx_t;

static thrd_t s_audio_thread;
static bool s_audio_running = false;
static atomic_bool s_audio_stop = false;
static atomic_bool s_audio_thread_active;
enum { AUDIO_RECORD_IDLE, AUDIO_RECORD_ACTIVE, AUDIO_RECORD_STOPPING };
static atomic_int s_audio_record_state;
static atomic_flag s_audio_record_control = ATOMIC_FLAG_INIT;
static size_t s_audio_record_start_tail;
static size_t s_audio_record_stop_tail;
static uint_fast32_t s_audio_record_start_drops;
static char s_audio_record_paths[GUI_AUDIO_RECORD_OUTPUT_COUNT][GUI_AUDIO_RECORD_PATH_MAX];
static uint32_t s_audio_record_rate_hz;
static unsigned s_audio_record_overwrite_mask;
static atomic_bool s_audio_record_failed;
static audio_ctx_t s_audio_ctx;

// Playback monitoring (audio thread produces into queue; main thread consumes into AudioStream)
static atomic_bool s_playback_enabled = false;
// Only the main-thread pump opens a capture monitoring session. Audio Mon
// controls mute separately; a stopped capture must reject late RF callbacks.
static atomic_bool s_monitor_session_active = false;
static AudioStream s_play_stream;
static bool s_play_stream_inited = false;
static int s_play_stream_chunk_frames;
static atomic_size_t s_play_stream_epoch;
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

static size_t hifi_q_pop_samples(int16_t *out, size_t max_count, size_t epoch)
{
    if (!out || !max_count || atomic_flag_test_and_set(&s_hifi_q_guard)) return 0;
    // A source/gap may change between the callback's precheck and this lock.
    // Never consume fresh PCM on behalf of a stale device callback.
    if (epoch != atomic_load(&s_monitor_epoch)) {
        atomic_flag_clear(&s_hifi_q_guard);
        return 0;
    }
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

// The device requests its actual frame count. A fixed UpdateAudioStream block
// can be smaller than raylib's device-clamped half-buffer, which inserts silence;
// refilling from the GUI also underruns when a frame takes longer than a half.
static void hifi_stream_callback(void *buffer, unsigned int frames)
{
    if (!buffer || !frames) return;
    int16_t *out = buffer;
    const size_t wanted = (size_t)frames * 2;
    const size_t epoch = atomic_load(&s_monitor_epoch);
    size_t got = 0;
    if (epoch == atomic_load(&s_play_stream_epoch) &&
        atomic_load(&s_playback_enabled) && atomic_load(&s_monitor_session_active) &&
        !monitor_legacy_selected()) {
        got = hifi_q_pop_samples(out, wanted, epoch);
    }
    // Source/mute/stop changes invalidate a concurrent pop too. The callback
    // never allocates, waits for PCM or calls device/stream lifecycle functions.
    if (epoch != atomic_load(&s_monitor_epoch) || epoch != atomic_load(&s_play_stream_epoch) ||
        !atomic_load(&s_playback_enabled) || !atomic_load(&s_monitor_session_active) ||
        monitor_legacy_selected()) got = 0;
    if (got < wanted) memset(out + got, 0, (wanted - got) * sizeof(*out));
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

// Serialize publication of the stop cursor with the worker's commitment
// to a full-block write. No file I/O or ringbuffer waits hold this lock.
static void audio_record_lock(void)
{
    while (atomic_flag_test_and_set(&s_audio_record_control)) thrd_sleep_ms(1);
}

static void audio_record_unlock(void)
{
    atomic_flag_clear(&s_audio_record_control);
}

// A read acquired while idle may finish after Start, but only if that
// entire block was published before the start cursor. Ignore cursors the
// monitor has already crossed. The ring wraps at twice its capacity.
static size_t audio_record_bytes_to_cursor(const buffer_manager_t *buffers, size_t tail)
{
    const ringbuffer_t *ring = &buffers->buffers[BUF_CAPTURE_AUDIO];
    size_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t bytes = tail >= head ? tail - head : ring->buffer_size * 2 - (head - tail);
    return bytes <= ring->buffer_size ? bytes - bytes % 12 : 0;
}

static uint64_t audio_discard_ring_to_start(audio_ctx_t *a)
{
    size_t remaining = audio_record_bytes_to_cursor(a->bufmgr, s_audio_record_start_tail);
    uint64_t dropped = 0;
    while (remaining > 0) {
        size_t take = remaining < BUFFER_AUDIO_READ_SIZE ? remaining : BUFFER_AUDIO_READ_SIZE;
        if (!bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, take, 0)) {
            atomic_store(&s_audio_record_failed, true);
            break;
        }
        bufmgr_read_end(a->bufmgr, BUF_CAPTURE_AUDIO, take);
        dropped += take;
        remaining -= take;
    }
    return dropped;
}

static size_t audio_record_limit_block(const buffer_manager_t *buffers, size_t bytes)
{
    audio_record_lock();
    int state = atomic_load(&s_audio_record_state);
    if (state == AUDIO_RECORD_STOPPING) {
        size_t pending = audio_record_bytes_to_cursor(buffers, s_audio_record_stop_tail);
        if (bytes > pending) bytes = pending;
    } else if (state != AUDIO_RECORD_ACTIVE) {
        bytes = 0;
    }
    audio_record_unlock();
    return bytes;
}

// Drain only complete PCM frames through the producer position captured at
// Stop. When the capture itself ends without a record stop, drain what is left.
static uint64_t audio_flush_ring_to_files(audio_ctx_t *a)
{
    uint64_t flushed = 0;
    if (!a || !a->bufmgr) return 0;
    const size_t frame_bytes = 12;
    audio_record_lock();
    size_t remaining = atomic_load(&s_audio_record_state) == AUDIO_RECORD_STOPPING
        ? audio_record_bytes_to_cursor(a->bufmgr, s_audio_record_stop_tail)
        : bufmgr_fill_level(a->bufmgr, BUF_CAPTURE_AUDIO);
    audio_record_unlock();
    remaining -= remaining % frame_bytes;
    while (remaining > 0) {
        size_t take = remaining < BUFFER_AUDIO_READ_SIZE ? remaining : BUFFER_AUDIO_READ_SIZE;
        void *data = bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, take, 0);
        if (!data) {
            atomic_store(&s_audio_record_failed, true);
            break;
        }
        audio_update_peaks(a->app, (const uint8_t *)data, take);
        audio_write_recorded_block(a, (const uint8_t *)data, take);
        bufmgr_read_end(a->bufmgr, BUF_CAPTURE_AUDIO, take);
        flushed += take;
        remaining -= take;
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

static bool audio_close_record_file(FILE **file, uint64_t frames, uint32_t rate_hz, int channels)
{
    if (!*file || *file == stdout) return true;
    wave_header_t header;
    create_wave_header(&header, frames, rate_hz, channels, 24);
    bool ok = !ferror(*file);
    if (mls_proxy_seek(*file, 0, SEEK_SET) != 0 ||
        fwrite(&header, 1, sizeof(header), *file) != sizeof(header)) ok = false;
    if (fclose(*file) != 0) ok = false;
    *file = NULL;
    return ok;
}

static void audio_close_record_files(audio_ctx_t *a)
{
    uint32_t rate_hz = a->record_rate_hz;
    uint64_t frames = a->total_bytes / 12;
    bool ok = audio_close_record_file(&a->f_4ch, frames, rate_hz, 4);
    for (int i = 0; i < 2; ++i)
        ok = audio_close_record_file(&a->f_2ch[i], frames, rate_hz, 2) && ok;
    for (int i = 0; i < 4; ++i)
        ok = audio_close_record_file(&a->f_1ch[i], frames, rate_hz, 1) && ok;
    if (!ok) {
        atomic_store(&s_audio_record_failed, true);
        fprintf(stderr, "[AUDIO] Failed to finalize a WAV output\n");
    }
}

static void audio_open_record_files(audio_ctx_t *a)
{
    FILE **files[GUI_AUDIO_RECORD_OUTPUT_COUNT] = {
        &a->f_4ch, &a->f_2ch[0], &a->f_2ch[1],
        &a->f_1ch[0], &a->f_1ch[1], &a->f_1ch[2], &a->f_1ch[3]
    };
    wave_header_t placeholder = {0};
    a->record_rate_hz = s_audio_record_rate_hz;
    a->total_bytes = 0;
    a->convert_1ch = a->convert_2ch = false;
    for (size_t i = 0; i < GUI_AUDIO_RECORD_OUTPUT_COUNT; ++i) {
        if (!s_audio_record_paths[i][0]) continue;
        const char *mode = (s_audio_record_overwrite_mask & (1u << i)) ? "wb" : "wbx";
        *files[i] = file_open_utf8(s_audio_record_paths[i], mode);
        if (!*files[i] || (*files[i] != stdout &&
            fwrite(&placeholder, 1, sizeof(placeholder), *files[i]) != sizeof(placeholder)))
            goto failed;
    }

    if (a->f_1ch[0] || a->f_1ch[1] || a->f_1ch[2] || a->f_1ch[3]) {
        if (!a->buffer_1ch[0]) a->buffer_1ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
        if (!a->buffer_1ch[0]) goto failed;
        for (int i = 1; i < 4; ++i)
            a->buffer_1ch[i] = a->buffer_1ch[0] + (BUFFER_AUDIO_READ_SIZE / 4) * i;
        a->convert_1ch = true;
    }
    if (a->f_2ch[0] || a->f_2ch[1]) {
        if (!a->buffer_2ch[0]) a->buffer_2ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
        if (!a->buffer_2ch[0]) goto failed;
        a->buffer_2ch[1] = a->buffer_2ch[0] + BUFFER_AUDIO_READ_SIZE / 2;
        a->convert_2ch = true;
    }
    return;

failed:
    atomic_store(&s_audio_record_failed, true);
    fprintf(stderr, "[AUDIO] Failed to open WAV output or allocate its buffer\n");
    audio_close_record_files(a);
    a->convert_1ch = a->convert_2ch = false;
}

static int audio_thread_main(void *ctx)
{
    audio_ctx_t *a = (audio_ctx_t *)ctx;
    size_t len = BUFFER_AUDIO_READ_SIZE;
    void *buf;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    fprintf(stderr, "[AUDIO] Audio thread started\n");
    size_t iter_count = 0;
    bool rec_start_aligned = false;
    while (1) {
        // Apply record transitions before reading, including when no more
        // audio arrives. Stop leaves the monitor running while this thread
        // writes the remaining WAV samples and headers.
        int record_state = atomic_load(&s_audio_record_state);
        if (record_state != AUDIO_RECORD_IDLE && !rec_start_aligned) {
            uint64_t dropped = audio_discard_ring_to_start(a);
            fprintf(stderr, "[AUDIO] Record start aligned: discarded %llu B (~%.1f ms)\n",
                    (unsigned long long)dropped,
                    (double)dropped / 12.0 / s_audio_record_rate_hz * 1000.0);
            audio_open_record_files(a);
            rec_start_aligned = true;
        }
        if (rec_start_aligned && atomic_load(&s_audio_record_state) == AUDIO_RECORD_STOPPING) {
            uint64_t flushed = audio_flush_ring_to_files(a);
            fprintf(stderr, "[AUDIO] Record stop aligned: flushed %llu B (~%.1f ms) of tail audio\n",
                    (unsigned long long)flushed,
                    (double)flushed / 12.0 / a->record_rate_hz * 1000.0);
            audio_close_record_files(a);
            a->convert_1ch = a->convert_2ch = false;
            rec_start_aligned = false;
            atomic_store(&s_audio_record_state, AUDIO_RECORD_IDLE);
        }
        // Read from buffer manager with timeout. The 2 ms timeout keeps the
        // loop spinning at ~500 Hz so the record-state transition above is
        // observed within ~2-4 ms of the record button (the old 10 ms timeout
        // bounded it at ~10-20 ms).
        buf = bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, len, 2);
        // A Start published during the read can split this block. Leave it
        // in the ring so the next iteration can align to the exact start.
        if (!rec_start_aligned) {
            audio_record_lock();
            bool start_pending = atomic_load(&s_audio_record_state) != AUDIO_RECORD_IDLE;
            audio_record_unlock();
            if (start_pending) continue;
        }
        if (!buf) {
            if (iter_count < 5) {
                fprintf(stderr, "[AUDIO] No data in buffer (iter %zu), fill_level=%zu\n", 
                        iter_count, bufmgr_fill_level(a->bufmgr, BUF_CAPTURE_AUDIO));
            }
            iter_count++;
            if (atomic_load(&do_exit) || atomic_load(&s_audio_stop) || !a->app || !a->app->is_capturing) {
                (void)audio_flush_ring_to_files(a);
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

        // A stop may have landed during the blocking ring read or monitor
        // processing. Only the prefix already published at Stop goes to WAV.
        size_t record_bytes = rec_start_aligned ? audio_record_limit_block(a->bufmgr, len) : 0;
        if (record_bytes) {
            if (a->f_4ch) fwrite(buf, 1, record_bytes, a->f_4ch);
            if (a->convert_1ch) extract_audio_1ch_C((uint8_t *)buf, record_bytes,
                a->buffer_1ch[0], a->buffer_1ch[1], a->buffer_1ch[2], a->buffer_1ch[3]);
            if (a->convert_2ch) extract_audio_2ch_C((uint16_t *)buf, record_bytes,
                (uint16_t *)a->buffer_2ch[0], (uint16_t *)a->buffer_2ch[1]);
        }

        bufmgr_read_end(a->bufmgr, BUF_CAPTURE_AUDIO, len);

        if (record_bytes) {
            for (int i = 0; i < 2; i++)
                if (a->f_2ch[i]) fwrite(a->buffer_2ch[i], 1, record_bytes / 2, a->f_2ch[i]);
            for (int i = 0; i < 4; i++)
                if (a->f_1ch[i]) fwrite(a->buffer_1ch[i], 1, record_bytes / 4, a->f_1ch[i]);
            a->total_bytes += record_bytes;
        }
    }

    audio_close_record_files(a);
    if (a->buffer_1ch[0]) aligned_free(a->buffer_1ch[0]);
    if (a->buffer_2ch[0]) aligned_free(a->buffer_2ch[0]);
    audio_record_lock();
    if (atomic_load(&s_audio_record_state) == AUDIO_RECORD_ACTIVE)
        atomic_store(&s_audio_record_failed, true);
    atomic_store(&s_audio_record_state, AUDIO_RECORD_IDLE);
    atomic_store(&s_audio_thread_active, false);
    audio_record_unlock();

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

    memset(&s_audio_ctx, 0, sizeof(s_audio_ctx));
    s_audio_ctx.bufmgr = bufmgr;
    s_audio_ctx.app = app;

    // Reset peaks
    for (int i = 0; i < 4; i++) atomic_store(&app->audio_peak[i], 0);

    atomic_store(&s_audio_stop, false);
    atomic_store(&s_audio_thread_active, true);

    if (thrd_create_with_priority(&s_audio_thread,
                                  audio_thread_main,
                                  &s_audio_ctx,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        atomic_store(&s_audio_thread_active, false);
        gui_app_set_status(app, "Failed to start audio thread");
        return -1;
    }

    s_audio_running = true;
    return 0;
}


void gui_audio_begin_recording(const gui_app_t *app,
    const char paths[GUI_AUDIO_RECORD_OUTPUT_COUNT][GUI_AUDIO_RECORD_PATH_MAX],
    unsigned overwrite_mask)
{
    audio_record_lock();
    memcpy(s_audio_record_paths, paths, sizeof(s_audio_record_paths));
    s_audio_record_overwrite_mask = overwrite_mask;
    s_audio_record_start_drops = atomic_load(&app->buffers.stats[BUF_CAPTURE_AUDIO].write_drops);
    s_audio_record_rate_hz = gui_audio_capture_rate_hz(app);
    if (!s_audio_record_rate_hz) s_audio_record_rate_hz = AUDIO_CXADC_DEFAULT_SAMPLE_RATE_HZ;
    bool unavailable = !atomic_load(&s_audio_thread_active);
    bool has_output = false;
    for (size_t i = 0; i < GUI_AUDIO_RECORD_OUTPUT_COUNT; ++i)
        has_output |= paths[i][0] != '\0';
    atomic_store(&s_audio_record_failed, unavailable && has_output);
    if (has_output && !unavailable) {
        s_audio_record_start_tail = atomic_load_explicit(
            &app->buffers.buffers[BUF_CAPTURE_AUDIO].tail, memory_order_acquire);
        atomic_store(&s_audio_record_state, AUDIO_RECORD_ACTIVE);
    }
    audio_record_unlock();
}

void gui_audio_request_record_stop(const gui_app_t *app)
{
    bool overflowed = false;
    audio_record_lock();
    if (atomic_load(&s_audio_record_state) == AUDIO_RECORD_ACTIVE) {
        const buffer_manager_t *buffers = &app->buffers;
        s_audio_record_stop_tail = atomic_load_explicit(
            &buffers->buffers[BUF_CAPTURE_AUDIO].tail, memory_order_acquire);
        overflowed = atomic_load(&buffers->stats[BUF_CAPTURE_AUDIO].write_drops) != s_audio_record_start_drops;
        if (overflowed) atomic_store(&s_audio_record_failed, true);
        atomic_store(&s_audio_record_state, AUDIO_RECORD_STOPPING);
    }
    audio_record_unlock();
    if (overflowed) fprintf(stderr, "[AUDIO] Capture buffer overflowed during WAV recording\n");
}

bool gui_audio_wait_recording_idle(void)
{
    while (atomic_load(&s_audio_thread_active) &&
           atomic_load(&s_audio_record_state) == AUDIO_RECORD_STOPPING)
        thrd_sleep_ms(1);
    return !atomic_load(&s_audio_record_failed);
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
        // Publish the epoch before the device can invoke its callback. The old
        // stream must not consume new-source PCM before the main-thread flush.
        atomic_store(&s_play_stream_epoch, epoch);
        if (!legacy_input) SetAudioStreamCallback(s_play_stream, hifi_stream_callback);
        PlayAudioStream(s_play_stream);
        s_play_stream_inited = true;
        s_play_stream_chunk_frames = frames_per_chunk;
        s_play_stream_generation = legacy_generation;
        fprintf(stderr, "[AUDIO] Initialized playback stream at 48kHz (%s, requested buffer: %d frames)\n",
                legacy_input ? "queued" : "callback", frames_per_chunk);
    }

    if (!IsAudioStreamPlaying(s_play_stream)) {
        PlayAudioStream(s_play_stream);
    }

    // HiFi PCM is consumed by the audio device callback, independently of GUI
    // frame timing and the actual device buffer size. Keep the ordinary monitor
    // on its existing queued path.
    if (!legacy_input) return;
    // Refill both ordinary-monitor halves after a delayed GUI frame.
    for (unsigned int fed = 0; fed < 2 && IsAudioStreamProcessed(s_play_stream); ++fed) {
        if (s_play_stream_epoch != atomic_load(&s_monitor_epoch) ||
            (legacy_input && legacy_generation != atomic_load(&s_play_q_generation))) return;
        int16_t chunk[4096 * 2]; // Maximum ordinary-monitor stereo chunk.
        size_t samples_wanted = frames_per_chunk * 2;
        size_t got = play_q_pop_samples(chunk, samples_wanted);
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
