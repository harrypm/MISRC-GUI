/* Production monitor queue, device callback and lifecycle without hardware or SOXR. */
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../common/threading.h"
static uint64_t monitor_test_now_us;
static uint64_t monitor_test_clock_us(void) { return monitor_test_now_us; }
#define GUI_AUDIO_CLOCK_US monitor_test_clock_us
static bool monitor_test_flag_take(volatile atomic_flag *flag);
static void monitor_test_flag_clear(volatile atomic_flag *flag);
/* Faults run at the real queue-lock boundary, without production test hooks. */
#undef atomic_flag_test_and_set
#undef atomic_flag_clear
#define atomic_flag_test_and_set(flag) monitor_test_flag_take(flag)
#define atomic_flag_clear(flag) monitor_test_flag_clear(flag)
#ifdef LIBSOXR_ENABLED
#undef LIBSOXR_ENABLED
#endif
#define LIBSOXR_ENABLED 0
#include "../misrc_gui/output/gui_audio.c"
#undef GUI_AUDIO_CLOCK_US

static int stream_loads, stream_unloads, stream_updates, callback_installs;
static bool stream_playing, stream_live;
static bool fail_audio_device, fail_audio_stream;
static int buffer_frames, processed_halves;
static AudioCallback stream_callback, mixed_processor;
static int16_t last_pcm[5120 * 2];
static gui_app_t app;
static int source_a, source_b;
static int before_pop_fault, after_pop_fault;
static unsigned before_pop_faults, after_pop_faults;
static enum { LD_QUEUE_NONE, LD_QUEUE_RELEASE, LD_QUEUE_SEEK,
              LD_QUEUE_TRACK, LD_QUEUE_STOP } ld_queue_fault;
static unsigned ld_queue_attempts;
static bool ld_queue_generation_current;
static const float fresh_pcm[] = {0.25f, -0.5f, 0.5f, -0.25f};
atomic_int do_exit;

static bool monitor_test_flag_take(volatile atomic_flag *flag)
{
    if (flag == &s_hifi_q_guard && before_pop_fault) {
        int fault = before_pop_fault;
        before_pop_fault = 0;
        ++before_pop_faults;
        if (fault == 1) gui_audio_set_hifi_source(&source_b);
        else gui_audio_reset_hifi(&source_a);
        gui_audio_push_hifi(fault == 1 ? &source_b : &source_a, fresh_pcm, 2);
    }
    bool busy = atomic_flag_test_and_set_explicit(flag, memory_order_seq_cst);
    if (flag == &s_hifi_q_guard && ld_queue_fault) {
        ++ld_queue_attempts;
        if (ld_queue_attempts == 1) {
            assert(busy);
            if (ld_queue_fault == LD_QUEUE_RELEASE)
                atomic_flag_clear_explicit(flag, memory_order_seq_cst);
            else if (ld_queue_fault == LD_QUEUE_SEEK)
                gui_audio_reset_hifi(&source_a);
            else if (ld_queue_fault == LD_QUEUE_TRACK)
                ld_queue_generation_current = false;
            else
                atomic_store(&s_monitor_session_active, false);
        } else {
            assert(ld_queue_fault == LD_QUEUE_RELEASE && !busy);
        }
    }
    return busy;
}
static void monitor_test_flag_clear(volatile atomic_flag *flag)
{
    atomic_flag_clear_explicit(flag, memory_order_seq_cst);
    if (flag == &s_hifi_q_guard && after_pop_fault) {
        int fault = after_pop_fault;
        after_pop_fault = 0;
        ++after_pop_faults;
        /* Simulate another thread changing a gate after PCM has been copied. */
        if (fault == 1) atomic_store(&s_playback_enabled, false);
        else atomic_store(&s_monitor_session_active, false);
    }
}

static const char *approved_wav_path;
void gui_app_set_status(gui_app_t *a, const char *s) { (void)a; (void)s; assert(false); }
FILE *file_open_utf8(const char *p, const char *m)
{
    assert(approved_wav_path && strcmp(p, approved_wav_path) == 0 && strcmp(m, "wbx") == 0);
    approved_wav_path = NULL;
    return tmpfile();
}

#undef fwrite
#undef fclose
#undef ferror
size_t mls_proxy_write(const void *data, size_t size, size_t count, FILE *file)
{ return fwrite(data, size, count, file); }
int mls_proxy_seek(FILE *file, int64_t offset, int whence)
{ return fseek(file, (long)offset, whence); }
int mls_proxy_close(FILE *file) { return fclose(file); }
int mls_proxy_error(FILE *file) { return ferror(file); }

static void test_wav_uses_approved_path(void)
{
    char paths[GUI_AUDIO_RECORD_OUTPUT_COUNT][GUI_AUDIO_RECORD_PATH_MAX] = {"approved/quad.wav"};
    audio_ctx_t writer = {.app = &app};
    atomic_store(&s_audio_thread_active, true);
    gui_audio_begin_recording(&app, paths, 0);
    assert(atomic_load(&s_audio_record_state) == AUDIO_RECORD_ACTIVE);
    strcpy(paths[0], "different/other.wav");
    approved_wav_path = "approved/quad.wav";
    audio_open_record_files(&writer);
    assert(writer.f_4ch && !approved_wav_path);
    audio_close_record_files(&writer);
    assert(!atomic_load(&s_audio_record_failed));
    atomic_store(&s_audio_record_state, AUDIO_RECORD_IDLE);
    atomic_store(&s_audio_thread_active, false);
}
/* gui_audio.c taps the capture stream for the M-Server monitor display only
 * while the unified server runs; idle in this harness. */
bool gui_monitor_server_active(void) { return false; }
int bufmgr_ensure_init(buffer_manager_t *m, buffer_id_t id) { (void)m; (void)id; assert(false); return -1; }
void *bufmgr_read_begin(buffer_manager_t *m, buffer_id_t id, size_t n, int ms)
{ (void)m; (void)id; (void)n; (void)ms; assert(false); return NULL; }
void bufmgr_read_end(buffer_manager_t *m, buffer_id_t id, size_t n)
{ (void)m; (void)id; (void)n; assert(false); }
size_t bufmgr_fill_level(buffer_manager_t *m, buffer_id_t id)
{ (void)m; (void)id; assert(false); return 0; }
void extract_audio_2ch_C(uint16_t *in, size_t n, uint16_t *a, uint16_t *b)
{ (void)in; (void)n; (void)a; (void)b; assert(false); }
void extract_audio_1ch_C(uint8_t *in, size_t n, uint8_t *a, uint8_t *b, uint8_t *c, uint8_t *d)
{ (void)in; (void)n; (void)a; (void)b; (void)c; (void)d; assert(false); }
void gui_settings_save(const gui_settings_t *settings) { (void)settings; }
void InitAudioDevice(void) {}
bool IsAudioDeviceReady(void) { return !fail_audio_device; }
void SetAudioStreamBufferSizeDefault(int frames)
{
    assert(frames == (gui_audio_get_hifi_source() ? 1024 : 4096));
    buffer_frames = frames;
}
AudioStream LoadAudioStream(unsigned int rate, unsigned int bits, unsigned int channels)
{
    assert(!stream_live && rate == 48000 && bits == 16 && channels == 2);
    if (fail_audio_stream) return (AudioStream){0};
    ++stream_loads;
    stream_live = true;
    stream_callback = NULL;
    processed_halves = 1;
    return (AudioStream){.buffer = (void *)1, .sampleRate = rate,
                         .sampleSize = bits, .channels = channels};
}
bool IsAudioStreamValid(AudioStream stream) { return stream.buffer != NULL; }
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback)
{
    assert(stream_live && stream.buffer && callback && !stream_playing);
    stream_callback = callback;
    ++callback_installs;
}
void AttachAudioMixedProcessor(AudioCallback process)
{
    assert(stream_live && stream_callback && !stream_playing && !mixed_processor);
    mixed_processor = process;
}
void DetachAudioMixedProcessor(AudioCallback process)
{
    assert(stream_live && !stream_playing && mixed_processor == process);
    mixed_processor = NULL;
}
void PlayAudioStream(AudioStream stream) { assert(stream_live && stream.buffer); stream_playing = true; }
void StopAudioStream(AudioStream stream) { assert(stream_live && stream.buffer); stream_playing = false; }
void UnloadAudioStream(AudioStream stream)
{
    assert(stream_live && stream.buffer && !stream_playing && !mixed_processor);
    stream_live = false;
    stream_callback = NULL;
    ++stream_unloads;
}
bool IsAudioStreamPlaying(AudioStream stream) { assert(stream_live && stream.buffer); return stream_playing; }
bool IsAudioStreamProcessed(AudioStream stream)
{
    assert(stream_live && stream.buffer && !stream_callback);
    return processed_halves > 0;
}
void UpdateAudioStream(AudioStream stream, const void *pcm, int frames)
{
    /* Only legacy audio may use GUI-driven whole-half writes. */
    assert(stream_live && stream.buffer && !stream_callback);
    assert(frames == 4096 && frames == buffer_frames && processed_halves > 0);
    --processed_halves;
    memcpy(last_pcm, pcm, (size_t)frames * 2 * sizeof(last_pcm[0]));
    ++stream_updates;
}

static void request_pcm_slice(unsigned frames)
{
    assert(stream_live && stream_playing && stream_callback && frames <= sizeof(last_pcm) / (2 * sizeof(*last_pcm)));
    stream_callback(last_pcm, frames);
}
static void finish_pcm_period(unsigned frames)
{
    if (mixed_processor) mixed_processor(NULL, frames);
}
static void request_pcm(unsigned frames)
{
    /* This is raylib's stream input format: 48 kHz, signed 16-bit, stereo.
     * The mixer requests frames; it does not promise a 1024-frame half. */
    for (size_t i = 0; i < sizeof(last_pcm)/sizeof(*last_pcm); ++i) last_pcm[i] = 12345;
    request_pcm_slice(frames);
    for (size_t i = (size_t)frames * 2; i < sizeof(last_pcm)/sizeof(*last_pcm); ++i)
        assert(last_pcm[i] == 12345); /* No write beyond the requested frames. */
    finish_pcm_period(frames);
}
static void expect_silence(unsigned frames)
{
    request_pcm(frames);
    for (size_t i = 0; i < (size_t)frames * 2; ++i) assert(last_pcm[i] == 0);
}
static void expect_fresh(unsigned frames)
{
    request_pcm(frames);
    assert(last_pcm[0] == 8192 && last_pcm[1] == -16384);
    assert(last_pcm[2] == 16384 && last_pcm[3] == -8192);
    for (size_t i = 4; i < (size_t)frames * 2; ++i) assert(last_pcm[i] == 0);
}
static size_t pop_hifi(int16_t *out, size_t count)
{
    return hifi_q_pop_samples(out, count, atomic_load(&s_monitor_epoch), count / 2, false, NULL);
}
static atomic_bool stress_done;
static int stress_push(void *unused)
{
    (void)unused;
    const float pcm[] = {0.25f, -0.25f, 0.5f, -0.5f};
    for (int i = 0; i < 40000; ++i) gui_audio_push_hifi(&source_a, pcm, 2);
    atomic_store(&stress_done, true);
    return 0;
}

static void test_callback_frames(void)
{
    const unsigned requests[] = {480, 1920, 2560, 512, 4096};
    float pcm[HIFI_Q_FRAMES * 2];
    for (size_t i = 0; i < HIFI_Q_FRAMES; ++i) { pcm[i * 2] = .25f; pcm[i * 2 + 1] = -.5f; }
    int loads = stream_loads, updates = stream_updates;
    for (unsigned repeat = 0; repeat < 4; ++repeat) for (size_t n = 0; n < sizeof(requests)/sizeof(*requests); ++n) {
        unsigned frames = requests[n];
        gui_audio_push_hifi(&source_a, pcm, frames);
        request_pcm(frames); /* No GUI pump between producer/device callbacks. */
        for (unsigned i = 0; i < frames; ++i)
            assert(last_pcm[i * 2] == 8192 && last_pcm[i * 2 + 1] == -16384);
        assert(s_hifi_q_count == 0);
    }
    processed_halves = 2;
    gui_audio_update_playback(&app);
    assert(stream_loads == loads && stream_updates == updates);
    assert(processed_halves == 2); /* Hi-Fi never polls/updates the stream halves. */
    stream_callback(NULL, 1920);
    request_pcm(0);
}

static void test_callback_control_boundaries(void)
{
    for (int fault = 1; fault <= 2; ++fault) {
        gui_audio_set_hifi_source(&source_a);
        gui_audio_update_playback(&app);
        gui_audio_push_hifi(&source_a, fresh_pcm, 2);
        int loads = stream_loads;
        /* Precheck passes for the old stream. Before taking the queue lock,
         * a source change or gap publishes a fresh epoch and fresh PCM. */
        before_pop_fault = fault;
        expect_silence(1920);
        assert(!before_pop_fault && s_hifi_q_count == 2);
        assert(stream_loads == loads); /* Old callback is still installed. */
        gui_audio_update_playback(&app);
        assert(stream_loads == loads + 1);
        expect_fresh(2560); /* The stale callback must not swallow fresh PCM. */
    }
    assert(before_pop_faults == 2);
    for (int fault = 1; fault <= 2; ++fault) {
        gui_audio_set_hifi_source(&source_a);
        gui_audio_update_playback(&app);
        gui_audio_push_hifi(&source_a, fresh_pcm, 2);
        after_pop_fault = fault;
        expect_silence(1920); /* A concurrent mute/session end discards copied PCM. */
        assert(!after_pop_fault);
        if (fault == 1) app.settings.audio_monitor_playback = false;
        else app.is_capturing = false;
        gui_audio_update_playback(&app);
        assert(!s_play_stream_inited && !stream_live);
        app.is_capturing = true;
        gui_audio_set_playback_enabled(&app, true);
        gui_audio_update_playback(&app);
        expect_silence(1920);
    }
    assert(after_pop_faults == 2);
}

static void test_playback_sidecar_integration(void)
{
    app.is_capturing = true;
    app.settings.audio_monitor_playback = true;
    gui_audio_set_hifi_source(NULL);
    gui_audio_update_playback(&app);
    const int16_t old_pcm[] = {1234, -2345};
    const int16_t new_pcm[] = {3456, -4567};
    gui_audio_push_playback(old_pcm, 1);
    processed_halves = 1;
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 1234 && last_pcm[1] == -2345 && buffer_frames == 4096);
    gui_audio_push_playback(old_pcm, 1);
    assert(!atomic_flag_test_and_set(&s_play_q_guard));
    gui_audio_reset_playback_queue();
    atomic_flag_clear(&s_play_q_guard);
    gui_audio_push_playback(new_pcm, 1);
    int loads = stream_loads, unloads = stream_unloads;
    gui_audio_update_playback(&app);
    assert(stream_loads == loads + 1 && stream_unloads == unloads + 1);
    assert(last_pcm[0] == 3456 && last_pcm[1] == -4567);
    for (size_t i = 2; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);
    gui_audio_push_playback(old_pcm, 1);
    gui_audio_reset_playback_queue();
    int16_t output[8];
    assert(play_q_pop_samples(output, 8) == 0);
    gui_audio_push_playback(old_pcm, SIZE_MAX);
    assert(play_q_pop_samples(output, 8) == 0);
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    size_t epoch = atomic_load(&s_monitor_epoch);
    size_t generation = atomic_load(&s_play_q_generation);
    loads = stream_loads; unloads = stream_unloads;
    gui_audio_push_playback(old_pcm, 1);
    gui_audio_reset_playback_queue();
    assert(atomic_load(&s_monitor_epoch) == epoch && atomic_load(&s_play_q_generation) == generation);
    assert(play_q_pop_samples(output, 8) == 0);
    atomic_fetch_add(&s_play_q_generation, 1); /* Delayed legacy seek must not flush Hi-Fi. */
    gui_audio_update_playback(&app);
    assert(stream_loads == loads && stream_unloads == unloads);
    expect_fresh(1920);
    assert(atomic_load(&s_monitor_epoch) == epoch);
    gui_audio_set_hifi_source(NULL);
    gui_audio_push_playback(new_pcm, 1);
    /* The old, still-live Hi-Fi stream must not consume new legacy input. */
    expect_silence(1920);
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 3456 && last_pcm[1] == -4567 && buffer_frames == 4096);
    assert(!stream_callback);
    gui_audio_stop(&app);
    assert(stream_loads == stream_unloads);
}

static float ac3_test_sample(size_t frame)
{
    return (float)(1 + frame % 251) / 512.0f;
}

static void test_ac3_bursts(void)
{
    /* Model the LD 40 ms delivery cadence against device callbacks which
     * request a different number of frames per call. */
    float pcm[3072 * 2];
    const unsigned requests[] = {2048, 4096};
    for (unsigned i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        unsigned request = requests[i];
        gui_audio_set_hifi_source_mode(&source_a, true);
        gui_audio_reset_hifi(&source_a);
        gui_audio_update_playback(&app);
        unsigned block = 0, next_callback = request / 3;
        size_t sent = 0, heard = 0;
        while (next_callback < 96000) {
            if (block * 1920 <= next_callback) {
                size_t frames = block % 4 == 3 ? 3072 : 1536;
                for (size_t j = 0; j < frames; ++j) {
                    pcm[j * 2] = ac3_test_sample(sent + j);
                    pcm[j * 2 + 1] = -pcm[j * 2];
                }
                gui_audio_push_hifi(&source_a, pcm, frames);
                sent += frames;
                ++block;
                continue;
            }
            request_pcm(request);
            if (heard || last_pcm[0]) {
                for (unsigned j = 0; j < request; ++j) {
                    float sample = ac3_test_sample(heard + j);
                    assert(last_pcm[j * 2] == hifi_float_to_i16(sample));
                    assert(last_pcm[j * 2 + 1] == hifi_float_to_i16(-sample));
                }
                heard += request;
            }
            next_callback += request;
        }
        assert(heard >= 80000);
    }
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    expect_fresh(1920);
}

static uint64_t ld_test_rf_position(uint64_t frames)
{
    /* Round up so converting this exact PCM-frame boundary back to 48 kHz
     * also works at positions between two RF samples. */
    return (frames * LD_RF_RATE + LD_PCM_RATE - 1) / LD_PCM_RATE;
}

static bool ld_test_stale_generation(const void *source)
{
    assert(source == &source_a);
    return false;
}

static void test_ld_timed_monitor(void)
{
    const unsigned request = 480;
    const uint64_t run_frames = 180u * LD_PCM_RATE;
    const uint64_t seek_frames[] = {0, 610u * LD_PCM_RATE, 2u * LD_PCM_RATE};
    const size_t packet_frames[] = {960, 960, 1536};
    float pcm[3072];
    uint64_t position;

    for (unsigned mode = 0; mode < 3; ++mode) {
        const uint64_t first = seek_frames[mode];
        const size_t packet = packet_frames[mode];
        const float tone = (float)(mode + 1) * 0.25f;
        for (size_t i = 0; i < packet; ++i) {
            pcm[i * 2] = tone;
            pcm[i * 2 + 1] = -tone;
        }
        /* All tracks share a stable token, including two non-burst tracks
         * in a row. The explicit reset must discard their old queued PCM. */
        gui_audio_set_rf_source(&source_a, mode == 2, GUI_AUDIO_RF_LD_TIMED);
        gui_audio_reset_hifi(&source_a);
        assert(!gui_audio_get_ld_position(&source_a, &position));
        if (mode) expect_silence(request);
        gui_audio_update_playback(&app);
        gui_audio_push_rf(&source_a, pcm, packet, ld_test_rf_position(first + packet));
        expect_silence(request);
        assert(!gui_audio_get_ld_position(&source_a, &position));

        uint64_t sent = packet;
        for (uint64_t heard = 0; heard < run_frames; heard += request) {
            while (sent < heard + LD_PREFILL_FRAMES) {
                sent += packet;
                gui_audio_push_rf(&source_a, pcm, packet, ld_test_rf_position(first + sent));
            }
            request_pcm(request);
            assert(last_pcm[0] == hifi_float_to_i16(tone));
            assert(last_pcm[1] == hifi_float_to_i16(-tone));
            assert(last_pcm[request * 2 - 1] == hifi_float_to_i16(-tone));
            assert(gui_audio_get_ld_position(&source_a, &position));
            assert(position == ld_test_rf_position(first + heard));
            monitor_test_now_us += 5000;
            assert(gui_audio_get_ld_position(&source_a, &position));
            assert(position == ld_test_rf_position(first + heard + request / 2));
            monitor_test_now_us += 5000;
        }
    }

    uint64_t previous = ld_test_rf_position(seek_frames[2] + run_frames);
    monitor_test_now_us += 50000;
    assert(gui_audio_get_ld_position(&source_a, &position) && position == previous);
    assert(!gui_audio_get_ld_position(&source_b, &position));
    monitor_test_now_us += LD_CLOCK_STALE_US;
    assert(!gui_audio_get_ld_position(&source_a, &position));

    /* A forward hole must discard queued pre-gap PCM and acquire reserve at
     * the new position. Late packets entirely behind it must be rejected. */
    const uint64_t next = 250u * LD_PCM_RATE;
    for (size_t i = 0; i < 1536; ++i) {
        pcm[i * 2] = 0.25f;
        pcm[i * 2 + 1] = -0.25f;
    }
    gui_audio_push_rf(&source_a, pcm, 1536, ld_test_rf_position(next + 1536));
    expect_silence(request);
    assert(!gui_audio_get_ld_position(&source_a, &position));
    gui_audio_push_rf(&source_a, pcm, 1536, ld_test_rf_position(next));
    for (unsigned i = 2; i <= 4; ++i)
        gui_audio_push_rf(&source_a, pcm, 1536, ld_test_rf_position(next + 1536u * i));
    /* Raylib provides the exact boundary after back-to-back stream requests. */
    request_pcm_slice(request);
    assert(last_pcm[0] == 8192 && last_pcm[request * 2 - 1] == -8192);
    request_pcm_slice(request);
    finish_pcm_period(2u * request);
    assert(gui_audio_get_ld_position(&source_a, &position));
    assert(position == ld_test_rf_position(next));
    monitor_test_now_us += 500;
    assert(gui_audio_get_ld_position(&source_a, &position));
    assert(position == ld_test_rf_position(next + 24));
    monitor_test_now_us += 14500;
    assert(gui_audio_get_ld_position(&source_a, &position));
    assert(position == ld_test_rf_position(next + 720));

    /* A queue contention or invalid packet can leave a short timestamp
     * hole. Preserve surrounding PCM and its timeline through the hole. */
    gui_audio_reset_hifi(&source_a);
    gui_audio_update_playback(&app);
    gui_audio_push_rf_checked(&source_a, pcm, request, ld_test_rf_position(next + request),
                              ld_test_stale_generation);
    gui_audio_push_rf(&source_a, pcm, request, ld_test_rf_position(next + request));
    assert(s_hifi_q_count == request);
    for (unsigned i = 0; i < 4; ++i)
        gui_audio_push_rf(&source_a, pcm, 1536, ld_test_rf_position(next + 2 * request + 1536u * (i + 1)));
    request_pcm(request);
    assert(last_pcm[0] == 8192);
    monitor_test_now_us += 10000;
    expect_silence(request);
    assert(gui_audio_get_ld_position(&source_a, &position));
    assert(position == ld_test_rf_position(next + request));
    monitor_test_now_us += 10000;
    request_pcm(request);
    assert(last_pcm[0] == 8192);
    assert(gui_audio_get_ld_position(&source_a, &position));
    assert(position == ld_test_rf_position(next + 2 * request));

    gui_audio_set_hifi_source(&source_a);
    assert(!gui_audio_get_ld_position(&source_a, &position));
    gui_audio_update_playback(&app);
    gui_audio_push_rf(&source_a, fresh_pcm, 2, 0);
    expect_fresh(1920);
}

static void ld_clock_push_tone(uint64_t first, unsigned frames)
{
    float pcm[1536 * 2];
    for (size_t i = 0; i < sizeof(pcm) / sizeof(*pcm); ++i)
        pcm[i] = i & 1u ? -.25f : .25f;
    while (frames) {
        unsigned take = frames < 1536u ? frames : 1536u;
        first += take;
        gui_audio_push_rf(&source_a, pcm, take, ld_test_rf_position(first));
        frames -= take;
    }
}

static void expect_ld_clock_frame_at(uint64_t start_us, unsigned offset, uint64_t expected)
{
    monitor_test_now_us = start_us + ((uint64_t)offset * 1000000u + LD_PCM_RATE - 1u) / LD_PCM_RATE;
    uint64_t position;
    assert(gui_audio_get_ld_position(&source_a, &position));
    assert(position == ld_scale_position(expected, LD_PCM_RATE, LD_RF_RATE));
}

static void test_ld_mixed_period_clock_gaps(void)
{
    const struct {
        unsigned slices[8], count, fault_index, native_frames;
    } cases[] = {
        {{512, 512, 512, 512, 512, 512, 512, 512}, 8, 6, 4096},
        {{128, 128, 128, 96}, 4, 1, 1920},
    };
    for (unsigned track = 0; track < 3; ++track) {
        for (size_t run = 0; run < sizeof(cases) / sizeof(*cases); ++run) {
            const unsigned fault = cases[run].fault_index;
            const unsigned fault_frames = cases[run].slices[fault];
            unsigned fault_offset = 0, period = 0;
            for (unsigned i = 0; i < cases[run].count; ++i) {
                period += cases[run].slices[i];
                if (i < fault) fault_offset += cases[run].slices[i];
            }
            for (unsigned partial = 0; partial <= 1; ++partial) {
                const uint64_t first = 3u * LD_PCM_RATE;
                const unsigned played_in_fault = partial ? fault_frames / 2 : 0;
                gui_audio_set_rf_source(&source_a, track == 2, GUI_AUDIO_RF_LD_TIMED);
                gui_audio_reset_hifi(&source_a);
                gui_audio_update_playback(&app);
                ld_clock_push_tone(first, partial ? fault_offset + played_in_fault : 8192u);
                if (partial) {
                    // Model a primed stream that has reached a genuine low queue.
                    s_hifi_q_primed = true;
                    s_ld_rate_level_valid = true;
                    s_ld_rate_queue_level = LD_PREFILL_FRAMES;
                }
                const uint64_t start_us = monitor_test_now_us + 100000u;
                monitor_test_now_us = start_us;
                for (unsigned i = 0; i < cases[run].count; ++i) {
                    unsigned frames = cases[run].slices[i];
                    const size_t before = s_hifi_q_count;
                    if (i == fault && !partial)
                        assert(!atomic_flag_test_and_set_explicit(&s_hifi_q_guard, memory_order_seq_cst));
                    request_pcm_slice(frames);
                    if (i != fault) {
                        assert(last_pcm[0] == 8192 && last_pcm[frames * 2 - 1] == -8192);
                    } else {
                        for (unsigned j = 0; j < frames; ++j) {
                            assert(last_pcm[2 * j] == (j < played_in_fault ? 8192 : 0));
                            assert(last_pcm[2 * j + 1] == (j < played_in_fault ? -8192 : 0));
                        }
                        if (partial) {
                            assert(before == played_in_fault && !s_hifi_q_count && !s_hifi_q_primed);
                            ld_clock_push_tone(first + fault_offset + played_in_fault, 8192u);
                        } else {
                            assert(s_hifi_q_count == before && s_hifi_q_primed);
                            atomic_flag_clear_explicit(&s_hifi_q_guard, memory_order_seq_cst);
                        }
                    }
                }
                uint64_t position;
                assert(!gui_audio_get_ld_position(&source_a, &position)); // Period not submitted yet.
                finish_pcm_period(cases[run].native_frames);
                assert(s_ld_mixed_clock.period == period);
                assert(atomic_load(&s_ld_clock_device_frames) == period &&
                       atomic_load(&s_ld_clock_span_count) == 2);
                const unsigned gap_start = fault_offset + played_in_fault;
                const unsigned gap_end = fault_offset + fault_frames;
                expect_ld_clock_frame_at(start_us, 0, first);
                expect_ld_clock_frame_at(start_us, gap_start - 1, first + gap_start - 1);
                expect_ld_clock_frame_at(start_us, gap_start, first + gap_start);
                expect_ld_clock_frame_at(start_us, gap_end - 1, first + gap_start);
                expect_ld_clock_frame_at(start_us, gap_end + 2, first + gap_start + 2);
                expect_ld_clock_frame_at(start_us, period, first + period - (gap_end - gap_start));
            }
        }
    }
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
}

static void test_ld_mixed_period_generation(void)
{
    for (unsigned track = 0; track < 3; ++track) {
        for (unsigned change_track = 0; change_track <= 1; ++change_track) {
            const bool next_ac3 = (track + change_track) % 3 == 2;
            gui_audio_set_rf_source(&source_a, track == 2, GUI_AUDIO_RF_LD_TIMED);
            gui_audio_reset_hifi(&source_a);
            gui_audio_update_playback(&app);
            ld_clock_push_tone(3u * LD_PCM_RATE, 8192u);
            request_pcm(480);
            uint64_t position;
            assert(gui_audio_get_ld_position(&source_a, &position));

            // The previous track/seek has mixed one slice but not submitted it.
            monitor_test_now_us += 10000u;
            request_pcm_slice(128);
            gui_audio_set_rf_source(&source_a, next_ac3, GUI_AUDIO_RF_LD_TIMED);
            gui_audio_reset_hifi(&source_a);
            finish_pcm_period(512);
            assert(!gui_audio_get_ld_position(&source_a, &position));

            gui_audio_update_playback(&app);
            const uint64_t fresh = 17u * LD_PCM_RATE;
            ld_clock_push_tone(fresh, 8192u);
            request_pcm(480);
            assert(gui_audio_get_ld_position(&source_a, &position) &&
                   position == ld_test_rf_position(fresh));
            assert(s_ld_mixed_clock.epoch == atomic_load(&s_monitor_epoch) &&
                   s_ld_mixed_clock.ac3 == next_ac3);
        }
    }
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
}

static void test_ld_clock_skew(void)
{
    enum { TEST_AC3, TEST_ANALOG, TEST_PCM };
    const struct {
        int ppm;
        unsigned request, jitter_us, phase_us, slice, track;
    } cases[] = {
        {-100, 4096, 0, 0, 0, TEST_AC3},
        {0, 4096, 0, 0, 0, TEST_AC3},
        {0, 4096, 0, 35039, 0, TEST_AC3},
        {0, 3600, 0, 46000, 0, TEST_AC3},
        {0, 5120, 0, 54000, 0, TEST_AC3},
        {0, 5120, 15000, 0, 0, TEST_AC3},
        {100, 4096, 0, 0, 0, TEST_AC3},
        {100, 3840, 0, 0, 0, TEST_AC3},
        {100, 4096, 10000, 0, 0, TEST_AC3},
        {100, 5120, 15000, 0, 0, TEST_AC3},
        {-100, 5120, 15000, 54000, 0, TEST_AC3},
        // The 192 kHz device makes 480 logical frames in 128-frame slices.
        {0, 480, 15000, 0, 128, TEST_AC3},
        {100, 480, 15000, 0, 128, TEST_AC3},
        {-100, 480, 15000, 3000, 128, TEST_AC3},
        // A 48 kHz mixer splits larger periods into 512-frame reads.
        {0, 960, 15000, 20000, 512, TEST_AC3},
        {0, 4096, 15000, 0, 512, TEST_AC3},
        {100, 4096, 15000, 0, 512, TEST_AC3},
        {-100, 4096, 15000, 54000, 512, TEST_AC3},
        {100, 480, 15000, 0, 128, TEST_ANALOG},
        {-100, 480, 15000, 3000, 128, TEST_ANALOG},
        {100, 4096, 15000, 0, 512, TEST_ANALOG},
        {-100, 4096, 15000, 3000, 512, TEST_ANALOG},
        {100, 480, 15000, 0, 128, TEST_PCM},
        {-100, 480, 15000, 3000, 128, TEST_PCM},
        {100, 4096, 15000, 0, 512, TEST_PCM},
        {-100, 4096, 15000, 3000, 512, TEST_PCM},
    };
    float pcm[4 * HIFI_AC3_FRAME_SAMPLES];
    for (size_t i = 0; i < sizeof(pcm) / sizeof(*pcm); ++i)
        pcm[i] = i & 1u ? -.25f : .25f;

    for (size_t run = 0; run < sizeof(cases) / sizeof(*cases); ++run) {
        int ppm = cases[run].ppm;
        unsigned request = cases[run].request;
        unsigned slice = cases[run].slice ? cases[run].slice : request;
        const bool ac3 = cases[run].track == TEST_AC3;
        const unsigned packet_frames = cases[run].track == TEST_ANALOG ? 480u : 960u;
        const unsigned packet_us = ac3 ? 40000u : packet_frames * 1000000u / LD_PCM_RATE;
        uint64_t duration_us = (ppm ? UINT64_C(3300) : 30u) * 1000000u;
        uint64_t start_us = monitor_test_now_us + 10000u;
        uint64_t sent = 0, burst = 0;
        const bool grouped_drift = ppm && slice < request;
        const uint64_t gap_interval_us = UINT64_C(300) * 1000000u;
        uint64_t next_gap_us = gap_interval_us;
        unsigned gaps = 0;
        int64_t corrected = 0;
        bool primed = false;

        gui_audio_set_rf_source(&source_a, ac3, GUI_AUDIO_RF_LD_TIMED);
        gui_audio_reset_hifi(&source_a);
        gui_audio_update_playback(&app);
        for (uint64_t output = 0;; output += request) {
            // Advance the producer at wall time and the device at its own rate.
            uint64_t elapsed = cases[run].phase_us + (uint64_t)((double)output * 1e12 /
                (LD_PCM_RATE * (1000000.0 + ppm)));
            if (grouped_drift && cases[run].jitter_us) {
                // Add bounded device wake jitter independently of delayed RF bursts.
                static const int device_jitter_us[] = {0, 650, -850, 1000, -250, 350, -900};
                size_t cycle = (output / request) % (sizeof(device_jitter_us) / sizeof(*device_jitter_us));
                elapsed = (uint64_t)((int64_t)elapsed + device_jitter_us[cycle]);
            }
            if (elapsed >= duration_us) break;
            monitor_test_now_us = start_us + elapsed;
            while (burst * packet_us + (burst % 16 == 3 ? cases[run].jitter_us : 0) <= elapsed) {
                unsigned frames = ac3 ? HIFI_AC3_FRAME_SAMPLES * (burst % 4 == 3 ? 2u : 1u)
                                      : packet_frames;
                uint64_t before = s_ld_q_frame;
                gui_audio_push_rf(&source_a, pcm, frames, ld_test_rf_position(sent + frames));
                assert(!sent || s_ld_q_frame == before); // No overflow drops.
                sent += frames;
                ++burst;
            }
            uint64_t first = s_ld_q_frame, gap_frame = 0;
            const unsigned gap_offset = (request / slice / 2) * slice;
            const bool inject_gap = grouped_drift && elapsed >= next_gap_us;
            unsigned gap_frames = 0;
            for (unsigned read = 0; read < request;) {
                unsigned count = request - read < slice ? request - read : slice;
                if (inject_gap && read == gap_offset) {
                    const size_t queued = s_hifi_q_count;
                    assert(primed && s_hifi_q_primed && queued >= count);
                    assert(!atomic_flag_test_and_set_explicit(&s_hifi_q_guard, memory_order_seq_cst));
                    request_pcm_slice(count);
                    atomic_flag_clear_explicit(&s_hifi_q_guard, memory_order_seq_cst);
                    for (unsigned i = 0; i < count * 2; ++i) assert(last_pcm[i] == 0);
                    assert(s_hifi_q_count == queued && s_hifi_q_primed);
                    gap_frame = s_ld_q_frame;
                    gap_frames = count;
                    read += count;
                    continue;
                }
                request_pcm_slice(count);
                read += count;
                if (!primed && !s_hifi_q_primed) {
                    assert(elapsed < 250000 && last_pcm[0] == 0);
                    continue;
                }
                primed = true;
                assert(s_hifi_q_primed); // Never underrun or restart after prefill.
                assert(last_pcm[0] == 8192 && last_pcm[count * 2 - 1] == -8192);
            }
            // The mixed processor's native-frame count is not the stream total.
            finish_pcm_period(slice == 128 ? 4u * request : request);
            assert(s_ld_mixed_clock.period == request && s_ld_mixed_clock.ac3 == ac3);
            if (!ac3) assert(!s_ld_mixed_clock.frames && !s_ld_rate_ac3_protect);
            if (!ppm) assert(!s_ld_mixed_clock.fast);
            if (!primed) continue;
            size_t consumed = (size_t)(s_ld_q_frame - first);
            unsigned played = request - gap_frames;
            size_t limit = (size_t)(played * LD_RATE_MAX_ADJUST) + 1;
            assert(consumed + limit >= played && consumed <= played + limit);
            if (!ppm) assert(consumed == request && s_ld_rate_fraction == 0);
            corrected += (int64_t)consumed - played;
            if (gap_frames) {
                ++gaps;
                next_gap_us += gap_interval_us;
                const uint64_t period_start = start_us + elapsed;
                expect_ld_clock_frame_at(period_start, 0, first);
                expect_ld_clock_frame_at(period_start, gap_offset, gap_frame);
                expect_ld_clock_frame_at(period_start, gap_offset + gap_frames - 1, gap_frame);
                expect_ld_clock_frame_at(period_start, gap_offset + gap_frames, gap_frame);
                expect_ld_clock_frame_at(period_start, request, first + consumed);
                continue;
            }
            uint64_t position;
            assert(gui_audio_get_ld_position(&source_a, &position));
            assert(position == ld_scale_position(first, LD_PCM_RATE, LD_RF_RATE));
            monitor_test_now_us += 50000;
            assert(gui_audio_get_ld_position(&source_a, &position));
            unsigned advance = request < 2400u ? request : 2400u;
            uint64_t middle = first + advance * consumed / request;
            assert(position == ld_scale_position(middle, LD_PCM_RATE, LD_RF_RATE));
        }
        assert(primed && (ppm < 0 ? corrected > 0 :
                          ppm > 0 ? corrected < 0 : corrected == 0));
        assert(gaps == (grouped_drift ? 10u : 0u));

        // EOF releases all remaining source frames at the nominal rate.
        assert(!gui_audio_finish_rf_source(&source_a));
        while (s_hifi_q_count) {
            size_t take = s_hifi_q_count < request ? s_hifi_q_count : request;
            uint64_t first = s_ld_q_frame;
            request_pcm(request);
            assert(s_ld_q_frame == first + take && s_ld_rate_fraction == 0);
        }
        monitor_test_now_us += LD_EOF_DEVICE_GRACE_US +
                               ld_scale_position(request, LD_PCM_RATE, 1000000u);
        assert(gui_audio_finish_rf_source(&source_a));
    }
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
}

static void test_ld_clock_counts_silent_requests(void)
{
    const unsigned frames = 5120;
    gui_audio_set_rf_source(&source_a, true, GUI_AUDIO_RF_LD_TIMED);
    gui_audio_reset_hifi(&source_a);
    gui_audio_update_playback(&app);
    assert(!atomic_flag_test_and_set_explicit(&s_hifi_q_guard, memory_order_seq_cst));
    expect_silence(frames);
    atomic_flag_clear_explicit(&s_hifi_q_guard, memory_order_seq_cst);
    assert(s_ld_mixed_clock.frames == frames && s_ld_mixed_clock.period == frames);

    monitor_test_now_us += ld_scale_position(frames, LD_PCM_RATE, 1000000u);
    expect_silence(frames);
    assert(s_ld_mixed_clock.frames == 2u * frames && !s_ld_mixed_clock.fast);

    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
}

static bool ld_test_queue_generation_current(const void *source)
{
    assert(source == &source_a);
    return ld_queue_generation_current;
}

static void test_ld_queue_contention_before_eof(void)
{
    for (int timed = 0; timed <= 1; ++timed) {
        for (int fault = LD_QUEUE_RELEASE; fault <= LD_QUEUE_STOP; ++fault) {
            gui_audio_set_rf_source(&source_a, false, timed ? GUI_AUDIO_RF_LD_TIMED : GUI_AUDIO_RF_LD);
            gui_audio_reset_hifi(&source_a);
            gui_audio_update_playback(&app);
            expect_silence(480); // Apply the new queue epoch and leave it empty.
            assert(!atomic_flag_test_and_set_explicit(&s_hifi_q_guard, memory_order_seq_cst));
            gui_audio_push_rf(&source_a, fresh_pcm, 2, ld_test_rf_position(2));
            assert(!s_hifi_q_count); // The ordinary realtime producer drops on contention.
            ld_queue_attempts = 0;
            ld_queue_generation_current = true;
            ld_queue_fault = fault;
            assert(!s_hifi_q_end_timeout_us);
            gui_audio_push_ld(&source_a, fresh_pcm, 2, ld_test_rf_position(2),
                              ld_test_queue_generation_current);
            ld_queue_fault = LD_QUEUE_NONE;
            if (fault == LD_QUEUE_RELEASE) {
                assert(ld_queue_attempts == 2 && s_hifi_q_count == 2);
                assert(!gui_audio_finish_rf_source(&source_a));
                expect_fresh(480);
            } else {
                assert(ld_queue_attempts == 1 && !s_hifi_q_count);
                atomic_flag_clear_explicit(&s_hifi_q_guard, memory_order_seq_cst);
            }
        }
    }
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
}

static void test_ld_natural_eof(void)
{
    const unsigned request = 480;
    const unsigned tail = 100;
    float pcm[200];
    for (unsigned i = 0; i < tail * 2; ++i) pcm[i] = 0.25f;
    for (unsigned track = 0; track < 3; ++track) {
        gui_audio_set_rf_source(&source_a, track == 2, GUI_AUDIO_RF_LD_TIMED);
        gui_audio_reset_hifi(&source_a);
        gui_audio_update_playback(&app);
        gui_audio_push_rf(&source_a, pcm, tail, ld_test_rf_position(48000u + tail));
        expect_silence(request); // A short tail normally cannot prime the queue.
        assert(!gui_audio_finish_rf_source(&source_a));
        request_pcm(request);
        for (unsigned i = 0; i < request * 2; ++i)
            assert(last_pcm[i] == (i < tail * 2 ? 8192 : 0));
        monitor_test_now_us += LD_EOF_DEVICE_GRACE_US;
        assert(!gui_audio_finish_rf_source(&source_a)); // Mixer still owns its last buffer.
        monitor_test_now_us += 10000;
        assert(gui_audio_finish_rf_source(&source_a));
        expect_silence(request);
        assert(s_hifi_q_count == 0);
    }
    gui_audio_reset_hifi(&source_a);
    gui_audio_update_playback(&app);
    gui_audio_push_rf(&source_a, pcm, tail, ld_test_rf_position(tail));
    expect_silence(request); // Fresh seek/session restores normal prefill.
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
}

static void test_ld_mixed_period_eof(void)
{
    enum { PERIOD = 4096, SLICE = 512 };
    float pcm[2 * PERIOD * 2];
    for (size_t i = 0; i < sizeof(pcm) / sizeof(*pcm); ++i) pcm[i] = .25f;
    const uint64_t period_us = ld_scale_position(PERIOD, LD_PCM_RATE, 1000000u);
    const uint64_t slice_us = ld_scale_position(SLICE, LD_PCM_RATE, 1000000u);
    for (unsigned track = 0; track < 3; ++track) {
        for (unsigned eof_phase = 0; eof_phase < 3; ++eof_phase) {
            gui_audio_set_rf_source(&source_a, track == 2, GUI_AUDIO_RF_LD_TIMED);
            gui_audio_reset_hifi(&source_a);
            gui_audio_update_playback(&app);
            gui_audio_push_ld(&source_a, pcm, PERIOD * 2, ld_test_rf_position(PERIOD * 2), NULL);
            request_pcm(PERIOD);
            assert(s_hifi_q_count == PERIOD);
            monitor_test_now_us += period_us;
            if (eof_phase == 0) assert(!gui_audio_finish_rf_source(&source_a));
            for (unsigned i = 0; i < PERIOD / SLICE; ++i) {
                request_pcm_slice(SLICE);
                assert(last_pcm[0] == 8192 && last_pcm[2 * SLICE - 1] == 8192);
            }
            assert(!s_hifi_q_count && atomic_load(&s_ld_mixed_pcm_inflight_epoch) ==
                   atomic_load(&s_monitor_epoch));
            if (eof_phase == 1) {
                assert(!gui_audio_finish_rf_source(&source_a));
                monitor_test_now_us += LD_EOF_DEVICE_GRACE_US + slice_us;
                assert(!gui_audio_finish_rf_source(&source_a));
            }
            const uint64_t mixed_at = monitor_test_now_us;
            finish_pcm_period(PERIOD);
            if (eof_phase == 2) assert(!gui_audio_finish_rf_source(&source_a));
            monitor_test_now_us = mixed_at + LD_EOF_DEVICE_GRACE_US + slice_us;
            assert(!gui_audio_finish_rf_source(&source_a));
            monitor_test_now_us = mixed_at + period_us;
            expect_silence(PERIOD); // A silent next period must not move the deadline.
            monitor_test_now_us = mixed_at + period_us + LD_EOF_DEVICE_GRACE_US - 1;
            assert(!gui_audio_finish_rf_source(&source_a));
            ++monitor_test_now_us;
            assert(gui_audio_finish_rf_source(&source_a));
        }
    }
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
}

static void test_ld_untimed_mixed_eof(void)
{
    enum { PERIOD = 4096, SLICE = 512 };
    static float pcm[PERIOD * 2];
    for (size_t i = 0; i < PERIOD * 2; ++i) pcm[i] = i % 2 ? -.25f : .25f;
    const uint64_t period_us = ld_scale_position(PERIOD, LD_PCM_RATE, 1000000u);
    const uint64_t slice_us = ld_scale_position(SLICE, LD_PCM_RATE, 1000000u);
    for (unsigned track = 0; track < 3; ++track) {
        for (unsigned eof_phase = 0; eof_phase < 3; ++eof_phase) {
            gui_audio_set_rf_source(&source_a, track == 2, GUI_AUDIO_RF_LD);
            gui_audio_reset_hifi(&source_a);
            gui_audio_update_playback(&app);
            assert(mixed_processor);
            gui_audio_push_ld(&source_a, pcm, PERIOD, ld_test_rf_position(PERIOD), NULL);
            const uint64_t start = monitor_test_now_us;
            if (eof_phase == 0) assert(!gui_audio_finish_rf_source(&source_a));
            for (unsigned i = 0; i < PERIOD / SLICE; ++i) {
                request_pcm_slice(SLICE);
                assert(last_pcm[0] == 8192 && last_pcm[2 * SLICE - 1] == -8192);
            }
            assert(!s_hifi_q_count && atomic_load(&s_ld_mixed_pcm_inflight_epoch) ==
                   atomic_load(&s_monitor_epoch));
            if (eof_phase == 1) assert(!gui_audio_finish_rf_source(&source_a));
            finish_pcm_period(PERIOD);
            if (eof_phase == 2) assert(!gui_audio_finish_rf_source(&source_a));
            uint64_t position;
            assert(!gui_audio_get_ld_position(&source_a, &position));
            assert(!s_ld_mixed_clock.ac3 && !s_ld_rate_level_valid);
            monitor_test_now_us = start + LD_EOF_DEVICE_GRACE_US + slice_us;
            assert(gui_audio_ld_tail_pending() && !gui_audio_finish_rf_source(&source_a));
            monitor_test_now_us = start + LD_EOF_DEVICE_GRACE_US + period_us - 1;
            assert(gui_audio_ld_tail_pending() && !gui_audio_finish_rf_source(&source_a));
            ++monitor_test_now_us;
            assert(!gui_audio_ld_tail_pending() && gui_audio_finish_rf_source(&source_a));
        }
    }
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    assert(!mixed_processor);
}

static void test_ld_late_eof_callback(void)
{
    enum { PERIOD = 4096, SLICE = 512 };
    float pcm[2 * PERIOD];
    for (size_t i = 0; i < sizeof(pcm) / sizeof(*pcm); ++i) pcm[i] = .25f;
    const uint64_t period_us = ld_scale_position(PERIOD, LD_PCM_RATE, 1000000u);
    enum { MIX_LATE, MIX_STOPS, MIX_RESUMES_LATE, MIX_CASE_COUNT };
    for (unsigned track = 0; track < 3; ++track) {
        for (unsigned scenario = 0; scenario < MIX_CASE_COUNT; ++scenario) {
            gui_audio_set_rf_source(&source_a, track == 2, GUI_AUDIO_RF_LD_TIMED);
            gui_audio_reset_hifi(&source_a);
            gui_audio_update_playback(&app);
            gui_audio_push_ld(&source_a, pcm, PERIOD, ld_test_rf_position(PERIOD), NULL);
            const uint64_t start = monitor_test_now_us;
            assert(!gui_audio_finish_rf_source(&source_a));
            const uint64_t late = start + LD_EOF_DEVICE_TIMEOUT_US - 40000;
            monitor_test_now_us = late;
            for (unsigned i = 0; i < (scenario == MIX_STOPS ? 1u : PERIOD / SLICE); ++i) {
                request_pcm_slice(SLICE);
                assert(last_pcm[0] == 8192 && last_pcm[2 * SLICE - 1] == 8192);
            }
            if (scenario == MIX_LATE) finish_pcm_period(PERIOD);
            monitor_test_now_us = start + LD_EOF_DEVICE_TIMEOUT_US;
            assert(!gui_audio_finish_rf_source(&source_a));
            uint64_t end = late + period_us + LD_EOF_DEVICE_GRACE_US;
            if (scenario == MIX_STOPS) {
                end = late + LD_EOF_DEVICE_TIMEOUT_US;
            } else if (scenario == MIX_RESUMES_LATE) {
                // The resumed mixer completes just before its refreshed
                // watchdog. Its finite output horizon extends past that bound.
                monitor_test_now_us = late + LD_EOF_DEVICE_TIMEOUT_US - 10000;
                finish_pcm_period(PERIOD);
                end = monitor_test_now_us + period_us + LD_EOF_DEVICE_GRACE_US;
                monitor_test_now_us = late + LD_EOF_DEVICE_TIMEOUT_US;
                assert(gui_audio_ld_tail_pending());
                assert(!gui_audio_finish_rf_source(&source_a));
            }
            monitor_test_now_us = end - 1;
            assert(!gui_audio_finish_rf_source(&source_a));
            ++monitor_test_now_us;
            assert(!gui_audio_ld_tail_pending());
            assert(gui_audio_finish_rf_source(&source_a));
        }
    }
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
}

static void test_ld_overlapping_eof_periods(void)
{
    enum { PERIOD = 4096, SLICE = 512 };
    static float pcm[3 * PERIOD * 2];
    for (size_t i = 0; i < sizeof(pcm) / sizeof(*pcm); ++i) pcm[i] = .25f;
    const uint64_t duration = ld_scale_position(PERIOD, LD_PCM_RATE, 1000000u);
    enum { OVERLAP, QUEUED, SHORTER, CASE_COUNT };
    for (unsigned track = 0; track < 3; ++track) {
        for (unsigned scenario = 0; scenario < CASE_COUNT; ++scenario) {
            gui_audio_set_rf_source(&source_a, track == 2, GUI_AUDIO_RF_LD_TIMED);
            gui_audio_reset_hifi(&source_a);
            gui_audio_update_playback(&app);
            gui_audio_push_ld(&source_a, pcm, 3 * PERIOD, ld_test_rf_position(3 * PERIOD), NULL);
            const uint64_t start = monitor_test_now_us;
            assert(!gui_audio_finish_rf_source(&source_a));
            monitor_test_now_us = start + 990000;
            for (unsigned i = 0; i < PERIOD / SLICE; ++i) request_pcm_slice(SLICE);
            finish_pcm_period(PERIOD);
            monitor_test_now_us = start + 1985000;
            for (unsigned i = 0; i < PERIOD / SLICE; ++i) request_pcm_slice(SLICE);
            finish_pcm_period(PERIOD);
            assert(s_hifi_q_count == PERIOD);
            const uint64_t prior_end = monitor_test_now_us + duration + LD_EOF_DEVICE_GRACE_US;
            assert(atomic_load(&s_ld_mixed_pcm_until_us) == prior_end);

            // The producer watchdog has expired. One case is in a new period
            // while the prior horizon still owns output. The other is still
            // waiting for a late device request with PCM left in the queue.
            monitor_test_now_us = scenario == QUEUED ? prior_end + 5000 :
                start + (scenario == SHORTER ? 2010000 : 2070000);
            assert(gui_audio_ld_tail_pending());
            request_pcm_slice(SLICE);
            assert(s_hifi_q_count == PERIOD - SLICE);
            if (scenario == SHORTER) finish_pcm_period(SLICE);
            assert(atomic_load(&s_ld_mixed_pcm_until_us) == prior_end);
            assert(gui_audio_ld_tail_pending() && !gui_audio_finish_rf_source(&source_a));
            monitor_test_now_us = prior_end + 10000;
            assert(gui_audio_ld_tail_pending() && !gui_audio_finish_rf_source(&source_a));
            for (unsigned i = 1; i < PERIOD / SLICE; ++i) request_pcm_slice(SLICE);
            const unsigned final_period = scenario == SHORTER ? PERIOD - SLICE : PERIOD;
            finish_pcm_period(final_period);
            const uint64_t final_end = monitor_test_now_us + LD_EOF_DEVICE_GRACE_US +
                ld_scale_position(final_period, LD_PCM_RATE, 1000000u);
            monitor_test_now_us = final_end - 1;
            assert(gui_audio_ld_tail_pending() && !gui_audio_finish_rf_source(&source_a));
            ++monitor_test_now_us;
            assert(!gui_audio_ld_tail_pending() && gui_audio_finish_rf_source(&source_a));
        }
    }
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
}

static void test_ld_unavailable_output_eof(void)
{
    const unsigned request = 480;
    app.is_capturing = true;
    app.settings.audio_monitor_playback = true;
    gui_audio_set_rf_source(&source_a, false, GUI_AUDIO_RF_LD_TIMED);

    fail_audio_device = true;
    gui_audio_update_playback(&app);
    assert(!stream_live && gui_audio_rf_output_unavailable());
    gui_audio_push_rf(&source_a, fresh_pcm, 2, ld_test_rf_position(2));
    assert(gui_audio_finish_rf_source(&source_a) && !s_hifi_q_end_timeout_us);

    fail_audio_device = false;
    fail_audio_stream = true;
    gui_audio_reset_hifi(&source_a);
    assert(!gui_audio_rf_output_unavailable()); // New epoch is pending its pump.
    gui_audio_update_playback(&app);
    assert(!stream_live && gui_audio_rf_output_unavailable());
    gui_audio_push_rf(&source_a, fresh_pcm, 2, ld_test_rf_position(2));
    assert(gui_audio_finish_rf_source(&source_a) && !s_hifi_q_end_timeout_us);

    fail_audio_stream = false;
    gui_audio_update_playback(&app);
    assert(stream_live && stream_callback && !gui_audio_rf_output_unavailable());
    expect_silence(request); // Reopening does not bypass normal prefill.
    assert(!gui_audio_finish_rf_source(&source_a));
    expect_fresh(request);
    monitor_test_now_us += LD_EOF_DEVICE_GRACE_US + 10000;
    assert(gui_audio_finish_rf_source(&source_a));

    // A stream that opened successfully but stopped calling back still
    // gets the bounded device-loss grace, with no indefinite loop hold.
    gui_audio_reset_hifi(&source_a);
    gui_audio_update_playback(&app);
    gui_audio_push_rf(&source_a, fresh_pcm, 2, ld_test_rf_position(2));
    assert(!gui_audio_finish_rf_source(&source_a));
    monitor_test_now_us += LD_EOF_DEVICE_TIMEOUT_US - 1;
    assert(!gui_audio_finish_rf_source(&source_a));
    ++monitor_test_now_us;
    assert(gui_audio_finish_rf_source(&source_a));

    gui_audio_set_playback_enabled(&app, false);
    gui_audio_stop(&app);
    gui_audio_set_hifi_source(NULL);
}

int main(void)
{
    test_ld_unavailable_output_eof();
    app.is_capturing = true;
    assert(!atomic_load(&s_playback_enabled));
    app.settings.audio_monitor_playback = true;
    gui_audio_update_playback(&app); /* Persisted setting without audio writer start. */
    assert(atomic_load(&s_playback_enabled) && !s_audio_running && !stream_callback);
    const int16_t restored_pcm[] = {321, -654};
    gui_audio_push_demod(restored_pcm, 1);
    processed_halves = 1;
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 321 && last_pcm[1] == -654 && buffer_frames == 4096);
    gui_audio_stop(&app);
    assert(stream_loads == stream_unloads);
    stream_loads = stream_unloads = stream_updates = callback_installs = 0;
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    assert(atomic_load(&s_monitor_session_active) && callback_installs == 1);
    expect_silence(1920);
    const float pcm[] = {-1.0f, 1.0f, NAN, INFINITY, -0.25f, 0.5f};
    const int16_t legacy[] = {123, 456};
    gui_audio_push_hifi(&source_b, pcm, 3);
    gui_audio_push_demod(legacy, 1);
    gui_audio_push_hifi(&source_a, pcm, 3);
    request_pcm(1920);
    assert(stream_loads == 1 && stream_updates == 0);
    assert(last_pcm[0] == INT16_MIN && last_pcm[1] == INT16_MAX);
    assert(last_pcm[2] == 0 && last_pcm[3] == 0);
    assert(last_pcm[4] == -8192 && last_pcm[5] == 16384);
    for (size_t i = 6; i < 1920 * 2; ++i) assert(last_pcm[i] == 0);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    gui_audio_reset_hifi(&source_b); /* Wrong owner cannot invalidate selected PCM. */
    expect_fresh(2560);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_reset_hifi(&source_a);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    int loads = stream_loads;
    expect_silence(1920);
    assert(s_hifi_q_count == 2);
    gui_audio_update_playback(&app);
    assert(stream_loads == loads + 1);
    expect_fresh(1920);

    int16_t out[HIFI_Q_FRAMES * 2];
    const size_t big_frames = HIFI_Q_FRAMES + 97;
    float *big = malloc(big_frames * 2 * sizeof(*big));
    assert(big);
    for (size_t i = 0; i < big_frames; ++i) {
        big[i * 2] = i < 97 ? -0.9f : 0.25f;
        big[i * 2 + 1] = i < 97 ? 0.9f : -0.5f;
    }
    gui_audio_push_hifi(&source_a, big, big_frames);
    assert(pop_hifi(out, sizeof(out)/sizeof(*out)) == HIFI_Q_FRAMES * 2);
    for (size_t i = 0; i < HIFI_Q_FRAMES; ++i) assert(out[i * 2] == 8192 && out[i * 2 + 1] == -16384);
    gui_audio_push_hifi(&source_a, big + 97 * 2, HIFI_Q_FRAMES);
    for (size_t i = 0; i < 128; ++i) { big[i * 2] = 0.75f; big[i * 2 + 1] = -0.75f; }
    gui_audio_push_hifi(&source_a, big, 128);
    assert(pop_hifi(out, sizeof(out)/sizeof(*out)) == HIFI_Q_FRAMES * 2);
    for (size_t i = 0; i < HIFI_Q_FRAMES; ++i) {
        assert(out[i * 2] == (i < HIFI_Q_FRAMES - 128 ? 8192 : 24575));
        assert(out[i * 2 + 1] == (i < HIFI_Q_FRAMES - 128 ? -16384 : -24575));
    }
    free(big);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    assert(!atomic_flag_test_and_set(&s_hifi_q_guard));
    gui_audio_push_hifi(&source_a, pcm, 3); /* Contention is bounded and does not wait. */
    expect_silence(1920);
    atomic_flag_clear(&s_hifi_q_guard);
    expect_fresh(1920); /* Consumer contention must not consume queued data. */
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_set_hifi_source(&source_b);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_push_hifi(&source_b, fresh_pcm, 2);
    loads = stream_loads;
    expect_silence(2560);
    assert(s_hifi_q_count == 2);
    gui_audio_update_playback(&app);
    assert(stream_loads == loads + 1);
    expect_fresh(1920);
    gui_audio_set_hifi_source(NULL);
    gui_audio_push_demod(legacy, 1);
    expect_silence(1920);
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 123 && last_pcm[1] == 456 && buffer_frames == 4096);
    assert(stream_updates == 1 && !stream_callback);
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_set_playback_enabled(&app, false);
    assert(!stream_live);
    gui_audio_push_hifi(&source_a, pcm, 3);
    int updates = stream_updates;
    gui_audio_update_playback(&app);
    assert(stream_updates == updates);
    loads = stream_loads;
    gui_audio_set_playback_enabled(&app, true);
    gui_audio_update_playback(&app);
    assert(stream_loads == loads + 1);
    expect_silence(1920);
    loads = stream_loads;
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    assert(stream_loads == loads);
    test_callback_frames();
    test_callback_control_boundaries();
    test_ac3_bursts();
    test_ld_timed_monitor();
    test_ld_mixed_period_clock_gaps();
    test_ld_mixed_period_generation();
    test_ld_clock_skew();
    test_ld_clock_counts_silent_requests();
    test_ld_queue_contention_before_eof();
    test_ld_natural_eof();
    test_ld_mixed_period_eof();
    test_ld_untimed_mixed_eof();
    test_ld_late_eof_callback();
    test_ld_overlapping_eof_periods();

    gui_audio_push_hifi(&source_a, pcm, 3);
    app.is_capturing = false;
    int unloads = stream_unloads;
    gui_audio_update_playback(&app);
    assert(!atomic_load(&s_monitor_session_active) && !s_play_stream_inited && !stream_live);
    assert(stream_unloads == unloads + 1);
    gui_audio_push_hifi(&source_a, pcm, 3);
    thrd_t late_producer;
    assert(thrd_create(&late_producer, stress_push, NULL) == thrd_success);
    thrd_join(late_producer, NULL);
    assert(pop_hifi(out, sizeof(out)/sizeof(*out)) == 0);
    atomic_store(&stress_done, false);
    app.is_capturing = true;
    gui_audio_push_hifi(&source_a, pcm, 3); /* Reconnect before first pump is not a session. */
    gui_audio_update_playback(&app);
    assert(atomic_load(&s_monitor_session_active));
    expect_silence(1920);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    expect_fresh(2560);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_stop(&app);
    assert(!atomic_load(&s_monitor_session_active));
    gui_audio_push_hifi(&source_a, pcm, 3);
    assert(pop_hifi(out, sizeof(out)/sizeof(*out)) == 0);
    gui_audio_update_playback(&app);
    expect_silence(1920);
    thrd_t producer;
    assert(thrd_create(&producer, stress_push, NULL) == thrd_success);
    while (!atomic_load(&stress_done)) {
        gui_audio_reset_hifi(&source_a);
        gui_audio_set_hifi_source(&source_b);
        gui_audio_set_hifi_source(&source_a);
        pop_hifi(out, sizeof(out)/sizeof(*out));
    }
    thrd_join(producer, NULL);
    assert(s_hifi_q_count <= HIFI_Q_FRAMES);
    gui_audio_stop(&app);
    assert(!s_play_stream_inited && stream_loads == stream_unloads);
    test_playback_sidecar_integration();
    test_wav_uses_approved_path();
    puts("gui_audio_hifi_monitor: callback frame counts, queue bounds, control races, mute/session restart and legacy sidecar passed");
    return 0;
}
