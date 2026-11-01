/* Continuous RF decode worker. The capture producer never waits for the
 * decoder, the renderer, queue space, or an allocation. Queue slots are owned
 * by a single producer and consumer; only the consumer advances read_index. */
#include "gui_rf_preview_worker.h"
#include "gui_rf_preview_policy.h"
#include "../signal/gui_rf_resampler.h"
#include "../../common/threading.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION rf_mutex_t;
static bool rf_mutex_init(rf_mutex_t *m) { InitializeCriticalSection(m); return true; }
static void rf_mutex_destroy(rf_mutex_t *m) { DeleteCriticalSection(m); }
static void rf_mutex_lock(rf_mutex_t *m) { EnterCriticalSection(m); }
static void rf_mutex_unlock(rf_mutex_t *m) { LeaveCriticalSection(m); }
#else
typedef pthread_mutex_t rf_mutex_t;
static bool rf_mutex_init(rf_mutex_t *m) { return pthread_mutex_init(m, NULL) == 0; }
static void rf_mutex_destroy(rf_mutex_t *m) { pthread_mutex_destroy(m); }
static void rf_mutex_lock(rf_mutex_t *m) { pthread_mutex_lock(m); }
static void rf_mutex_unlock(rf_mutex_t *m) { pthread_mutex_unlock(m); }
#endif

#define RF_QUEUE_SLOTS 256u
#define RF_BLOCK_SAMPLES 65536u
#define RF_QUEUE_MAX_US 200000u
#define RF_AUDIO_QUEUE_SLOTS 32u
#define RF_AUDIO_BLOCK_SAMPLES 32768u
#define RF_AUDIO_MAX_US 100000u
/* Rate provisions (see dev/output_stdio_buffer_and_rf_rate_provisions.md):
 * 2x 40 MSPS dual-channel captures use two per-channel workers (each on the
 * 40 MSPS exact path); single-stream captures up to 200 MSPS are accepted. */
#define RF_MIN_RATE 16000000u
#define RF_MAX_RATE 200000000u
#define RF_FRAME_BYTES ((size_t)GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4)

typedef struct {
    gui_rf_block_info_t info;
    size_t count;
    uint64_t enqueue_us;
    uint64_t duration_end_us;
    uint64_t control;
    uint64_t pause_control;
    uint64_t audio_source_generation;
    int16_t samples[RF_BLOCK_SAMPLES];
} rf_input_slot_t;

typedef struct {
    float samples[RF_AUDIO_BLOCK_SAMPLES];
    size_t count;
    uint64_t first_sample, generation, enqueue_us;
} rf_audio_slot_t;

struct gui_rf_preview_worker {
    thrd_t thread, audio_thread;
    atomic_bool audio_ready, audio_enabled, audio_active, audio_budget_waiting, audio_callback_installed;
    atomic_uint_fast64_t audio_generation, applied_audio_generation, audio_source_generation;
    bool producer_audio_seen;
    uint64_t producer_audio_epoch,producer_audio_next;
    uint32_t producer_audio_rate;
    atomic_uint_fast64_t audio_read, audio_write, audio_dropped;
    rf_audio_slot_t *audio_queue;
    uint64_t audio_processing_generation;
    uint64_t audio_last_cpu_us, audio_last_completion_us;
    bool audio_have_cpu_baseline;
    uint64_t video_work_wall_us,video_accounted_cpu_us,audio_work_wall_us,audio_accounted_cpu_us;
    rf_mutex_t frame_mutex;
    void (*monitor_frame_sink)(void *, const uint8_t *, unsigned, unsigned, uint64_t);
    void *monitor_frame_sink_context;
    atomic_bool stop;
    atomic_bool flush_requested;
    atomic_bool producer_gap;
    atomic_flag producer_busy;
    atomic_uint_fast64_t read_index;
    atomic_uint_fast64_t write_index;
    atomic_uint_fast64_t freed_duration_us;
    atomic_uint_fast64_t dropped;
    atomic_uint_fast64_t pause_drops;
    atomic_uint_fast64_t pause_control;
    atomic_uint_fast64_t applied_pause_control;
    atomic_bool budget_waiting;
    atomic_uint_fast64_t control;
    atomic_int requested_profile;
    atomic_bool requested_full_frame;
    bool applied_full_frame;
    uint64_t producer_duration_us;
    rf_input_slot_t *queue;

    gui_rf_decoder_t *decoder;
    gui_beta_hifi_decoder_t *hifi;
    gui_rf_preview_audio_callback_t audio_callback;
    void *audio_context;
    gui_rf_resampler_t *resampler, *wide_resampler;
    float *output, *wide_output;
    size_t output_capacity, wide_output_capacity;
    gui_rf_profile_t profile;
    atomic_uint_fast64_t applied_control;
    uint64_t source_epoch;
    uint64_t expected_source_sample;
    uint32_t source_rate;
    bool stream_valid;
    uint64_t output_sample;      /* Audio and legacy video: public 20 MSPS axis. */
    uint64_t wide_output_sample; /* EDBeta wide-passband video: independent 20 MSPS cursor. */
    bool narrow_stream_valid;   /* ED may omit its audio-only converter while muted. */
    uint64_t narrow_audio_generation;
    uint64_t block_enqueue_us;
    uint64_t block_source_end;
    uint64_t work_since_frame_us;
    uint64_t work_segment_us;
    uint64_t generation;
    uint64_t interval_start_us;
    uint64_t interval_frames;
    double interval_cpu_ms;
    gui_rf_decode_status_t last_status;
    uint64_t last_cpu_us;
    uint64_t last_budget_completion_us;
    bool have_cpu_baseline;

    uint8_t *frame;
    int frame_width;
    int frame_height;
    bool frame_valid;
    uint64_t frame_control;
    gui_rf_preview_stats_t stats;
#ifdef _WIN32
    uint64_t timer_frequency;
    HANDLE input_event, audio_event;
    HANDLE done_event;
#endif
};

static atomic_flag rf_budget_lock = ATOMIC_FLAG_INIT;
static gui_rf_preview_policy_t rf_budget = {
    .cores_millionths=1000000u,
#ifdef _WIN32
    /* GetThreadTimes can report many zero completions followed by 15.625ms
     * already-spent CPU. Preserve at most one such quantum per live counter;
     * the policy bounds the registry and cannot accumulate idle credit beyond
     * that allowance plus the small normal token capacity. */
    .accounting_quantum_cpu_us=15625u,
#endif
};

static void rf_budget_lock_blocking(void) {
    while (atomic_flag_test_and_set_explicit(&rf_budget_lock, memory_order_acquire))
        thrd_sleep_ms(0);
}

/* Global policy lock held. A queued owner is a live worker: its thread removes
 * the request under this same lock before exit, and destroy joins that thread
 * before closing input_event/freeing the owner. No producer participates. */
static void rf_budget_wake_waiters(void) {
#ifdef _WIN32
    for (unsigned int i = 0; i < rf_budget.waiter_count; i++) {
        uintptr_t owner=rf_budget.waiters[i];
        gui_rf_preview_worker_t *waiting = (gui_rf_preview_worker_t *)(owner & ~(uintptr_t)1);
        SetEvent((owner & 1u) ? waiting->audio_event : waiting->input_event);
    }
#endif
}

/* Successful acquisition removes the old FIFO head. A successor can already
 * have consumed its previous notification before that removal, so notify the
 * new head while its owner is still protected by the policy lock. Only one
 * request can become eligible at this step; leave it asleep when all permits
 * or the current reservation credit have been consumed. */
static void rf_budget_wake_head(void) {
#ifdef _WIN32
    if (!rf_budget.waiter_count || rf_budget.active_count >= GUI_RF_POLICY_MAX_ACTIVE) return;
    if (rf_budget.cores_millionths &&
        (rf_budget.active_count * 1000000u >= rf_budget.cores_millionths ||
         rf_budget.credit_cpu_us < (int64_t)GUI_RF_POLICY_RESERVE_CPU_US)) return;
    uintptr_t owner = rf_budget.waiters[0];
    gui_rf_preview_worker_t *waiting = (gui_rf_preview_worker_t *)(owner & ~(uintptr_t)1);
    SetEvent((owner & 1u) ? waiting->audio_event : waiting->input_event);
#endif
}

bool gui_rf_preview_worker_set_budget(double cores) {
    rf_budget_lock_blocking();
    bool accepted = gui_rf_preview_policy_set_budget(&rf_budget, cores);
    if (accepted) rf_budget_wake_waiters();
    atomic_flag_clear_explicit(&rf_budget_lock, memory_order_release);
    return accepted;
}

static bool rf_thread_cpu_us(uint64_t *cpu_us);
static uint64_t rf_now_us(const gui_rf_preview_worker_t *w);

static void rf_register_counter(gui_rf_preview_worker_t *w,bool audio) {
    rf_budget_lock_blocking();
    (void)gui_rf_preview_policy_register_counter(&rf_budget,(uintptr_t)w|(audio ? 1u : 0u));
    atomic_flag_clear_explicit(&rf_budget_lock,memory_order_release);
}
static void rf_unregister_counter(gui_rf_preview_worker_t *w,bool audio) {
    uint64_t now=rf_now_us(w),end_cpu=0;
    uint64_t completed=audio ? w->audio_last_completion_us : w->last_budget_completion_us;
    uint64_t previous=audio ? w->audio_last_cpu_us : w->last_cpu_us;
    bool baseline=audio ? w->audio_have_cpu_baseline : w->have_cpu_baseline;
    /* First pay any newly visible final quantum. As at ordinary acquisition,
     * a genuinely quiet interval must not turn idle polling into fresh debt. */
    uint64_t measured=baseline && now-completed<RF_QUEUE_MAX_US &&
        rf_thread_cpu_us(&end_cpu) && end_cpu>=previous ? end_cpu-previous : 0;
    if(audio) w->audio_accounted_cpu_us+=measured;else w->video_accounted_cpu_us+=measured;
    rf_budget_lock_blocking();
    uintptr_t owner=(uintptr_t)w|(audio ? 1u : 0u);
    gui_rf_preview_policy_cancel(&rf_budget,owner);
    uint64_t work=audio ? w->audio_work_wall_us : w->video_work_wall_us;
    uint64_t accounted=audio ? w->audio_accounted_cpu_us : w->video_accounted_cpu_us;
    /* A live coarse counter uses only measured deltas. At retirement its
     * final sub-quantum work may never be posted; cover at most one quantum
     * and no more than the processing wall not already CPU-accounted. Never
     * report this conservative closing estimate as measured CPU in stats. */
    uint64_t residual=work>accounted ? work-accounted : 0;
    gui_rf_preview_policy_retire_counter(&rf_budget,owner,measured,residual);
    rf_budget_wake_waiters();
    atomic_flag_clear_explicit(&rf_budget_lock,memory_order_release);
}

static bool rf_lower_thread_priority(void) {
#ifdef _WIN32
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != 0;
#elif defined(__APPLE__)
    return pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0) == 0;
#elif defined(__linux__)
    struct sched_param param = {0};
    (void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
    /* Linux nice is per-thread when addressed by TID. Never fall back to the
     * process-wide setpriority(0) used by the common elevation helper. */
#if defined(SYS_gettid)
    pid_t tid = (pid_t)syscall(SYS_gettid);
#elif defined(__NR_gettid)
    pid_t tid = (pid_t)syscall(__NR_gettid);
#else
    pid_t tid = 0;
#endif
    return tid > 0 && setpriority(PRIO_PROCESS, (id_t)tid, 19) == 0;
#else
    int minimum = sched_get_priority_min(SCHED_OTHER);
    struct sched_param param = {0};
    param.sched_priority = minimum < 0 ? 0 : minimum;
    return pthread_setschedparam(pthread_self(), SCHED_OTHER, &param) == 0;
#endif
}

static bool rf_thread_cpu_us(uint64_t *cpu_us) {
#ifdef _WIN32
    FILETIME creation, exit_time, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit_time, &kernel, &user)) return false;
    uint64_t kernel_ticks = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    uint64_t user_ticks = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    *cpu_us = (kernel_ticks + user_ticks) / 10u;
    return true;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return false;
    *cpu_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    return true;
#else
    (void)cpu_us;
    return false;
#endif
}

static uint64_t rf_now_us(const gui_rf_preview_worker_t *w) {
#ifdef _WIN32
    LARGE_INTEGER counter;
    if (w->timer_frequency && QueryPerformanceCounter(&counter)) {
        uint64_t ticks = (uint64_t)counter.QuadPart;
        return (ticks / w->timer_frequency) * 1000000u +
               (ticks % w->timer_frequency) * 1000000u / w->timer_frequency;
    }
#else
    (void)w;
#endif
    return get_time_us();
}

static uint64_t rf_scale_sample_rate(uint64_t sample, uint32_t rate, uint32_t output_rate) {
    return (sample / rate) * output_rate + (sample % rate) * output_rate / rate;
}
static uint64_t rf_scale_sample(uint64_t sample, uint32_t rate) {
    return rf_scale_sample_rate(sample, rate, GUI_RF_DECODE_RATE_HZ);
}

/* frame_mutex held. Sampling a real interval also reports zero after a quiet
 * interval; reset/pause never erases cumulative counters or its baseline. */
static void rf_refresh_interval(gui_rf_preview_worker_t *w, uint64_t now) {
    /* Another consumer/UI snapshot can advance the interval while this
     * thread waits for frame_mutex. Never rewind or underflow that window. */
    if(now<=w->interval_start_us) return;
    uint64_t elapsed = now - w->interval_start_us;
    if (elapsed < 1000000u) return;
    w->stats.fps = (double)(w->stats.total_frames - w->interval_frames) * 1000000.0 / elapsed;
    w->stats.cpu_percent = (w->stats.processing_cpu_ms - w->interval_cpu_ms) * 100000.0 / elapsed;
    w->interval_start_us = now;
    w->interval_frames = w->stats.total_frames;
    w->interval_cpu_ms = w->stats.processing_cpu_ms;
}

static uint64_t rf_counter_delta(uint64_t current, uint64_t previous) {
    return current >= previous ? current - previous : current;
}

/* Only the worker mutates last_status; callbacks run on that same thread. */
static void rf_accumulate_status(gui_rf_preview_worker_t *w,
                                  const gui_rf_decode_status_t *status) {
    rf_mutex_lock(&w->frame_mutex);
#define RF_ACCUMULATE(total, field) \
    w->stats.total += rf_counter_delta((uint64_t)status->field, (uint64_t)w->last_status.field)
    RF_ACCUMULATE(total_frames, frames_decoded);
    RF_ACCUMULATE(total_fields, fields_decoded);
    RF_ACCUMULATE(total_hsyncs, hsync_count);
    RF_ACCUMULATE(total_vsyncs, vsync_count);
    RF_ACCUMULATE(total_vsync_candidates, vsync_candidates);
    RF_ACCUMULATE(total_fields_accepted, fields_accepted);
    RF_ACCUMULATE(total_fields_rejected, fields_rejected);
    RF_ACCUMULATE(total_reject_cadence, reject_cadence);
    RF_ACCUMULATE(total_reject_parity, reject_parity);
    RF_ACCUMULATE(total_reject_line_map, reject_line_map);
    RF_ACCUMULATE(total_color_losses, color_losses);
#undef RF_ACCUMULATE
    w->stats.decode = *status;
    rf_refresh_interval(w, rf_now_us(w));
    rf_mutex_unlock(&w->frame_mutex);
    w->last_status = *status;
}

static void rf_clear_frame(gui_rf_preview_worker_t *w, const char *reason, bool preserve_frame) {
    rf_mutex_lock(&w->frame_mutex);
    if (!preserve_frame) w->frame_valid = false;
    memset(&w->stats.decode, 0, sizeof(w->stats.decode));
    snprintf(w->stats.decode.reason, sizeof(w->stats.decode.reason), "%s", reason);
    w->stats.source_rate_hz = w->source_rate;
    w->stats.resets++;
    w->stats.decode_ms = 0.0;
    w->stats.latency_ms = 0.0;
    /* Use the callback outside frame_mutex to avoid delaying UI readers. */
    void (*sink)(void *, const uint8_t *, unsigned, unsigned, uint64_t) = w->monitor_frame_sink;
    void *sink_context = w->monitor_frame_sink_context;
    rf_mutex_unlock(&w->frame_mutex);
    if (sink) sink(sink_context, rgba, (unsigned)width, (unsigned)height, now);
    w->work_since_frame_us = 0;
}

static void rf_audio_invalidate(gui_rf_preview_worker_t *w) {
    atomic_fetch_add_explicit(&w->audio_generation,1,memory_order_acq_rel);
#ifdef _WIN32
    SetEvent(w->audio_event);
#endif
}

/* Producer/control epochs exclude previously queued RF as well as PCM.
 * Internal DSP resets leave this input identity intact, so an initial file
 * batch is not accidentally discarded by its first stream-boundary reset. */
static void rf_audio_cut_input(gui_rf_preview_worker_t *w) {
    atomic_fetch_add_explicit(&w->audio_source_generation,1,memory_order_acq_rel);
    rf_audio_invalidate(w);
}

static void rf_reset_decoder_preserve(gui_rf_preview_worker_t *w, const char *reason,
                                      bool preserve_frame) {
    gui_rf_decode_status_t status;
    gui_rf_decoder_get_status(w->decoder, &status);
    rf_accumulate_status(w, &status);
    gui_rf_decoder_reset_profile(w->decoder, w->profile);
    rf_audio_invalidate(w);
    memset(&w->last_status, 0, sizeof(w->last_status));
    if (w->resampler) gui_rf_resampler_reset(w->resampler);
    if (w->wide_resampler) gui_rf_resampler_reset(w->wide_resampler);
    w->narrow_stream_valid = false;
    w->stream_valid = false;
    rf_clear_frame(w, reason, preserve_frame);
}

static void rf_reset_decoder(gui_rf_preview_worker_t *w, const char *reason) {
    rf_reset_decoder_preserve(w, reason, false);
}

static bool rf_reserve_output(float **buffer, size_t *available, size_t capacity) {
    if (!capacity || capacity > SIZE_MAX / sizeof(float)) return false;
    if (capacity <= *available) return true;
    float *output = realloc(*buffer, capacity * sizeof(*output));
    if (!output) return false;
    *buffer = output; *available = capacity;
    return true;
}
static const char *rf_rate_error(const gui_rf_preview_worker_t *w) {
    return w->profile == GUI_RF_PROFILE_NTSC_ED_BETA && w->source_rate < GUI_RF_ED_BETA_RATE_HZ
        ? "Unsupported EDBeta RF rate: requires at least 20 MSPS"
        : "Unsupported RF rate or resampler unavailable";
}
static bool rf_configure_rate(gui_rf_preview_worker_t *w, uint32_t rate) {
    gui_rf_resampler_destroy(w->resampler);
    gui_rf_resampler_destroy(w->wide_resampler);
    w->resampler = w->wide_resampler = NULL;
    w->narrow_stream_valid = false;
    w->source_rate = rate;
    bool ed_beta = w->profile == GUI_RF_PROFILE_NTSC_ED_BETA;
    if (rate < RF_MIN_RATE || rate > RF_MAX_RATE ||
        (ed_beta && rate < GUI_RF_ED_BETA_RATE_HZ)) return false;
    if (!rf_reserve_output(&w->output, &w->output_capacity,
            gui_rf_resampler_output_capacity(rate, RF_BLOCK_SAMPLES))) return false;
    w->resampler = gui_rf_resampler_create(rate);
    if (!w->resampler) return false;
    if (ed_beta) {
        if (!rf_reserve_output(&w->wide_output, &w->wide_output_capacity,
                gui_rf_resampler_output_capacity_ed_beta(rate, RF_BLOCK_SAMPLES))) goto fail;
        w->wide_resampler = gui_rf_resampler_create_ed_beta(rate);
        if (!w->wide_resampler) goto fail;
    }
    return true;
fail:
    gui_rf_resampler_destroy(w->resampler);
    gui_rf_resampler_destroy(w->wide_resampler);
    w->resampler = w->wide_resampler = NULL;
    return false;
}

static void rf_frame_callback(void *context, const uint8_t *rgba, int width,
                              int height, uint64_t end_sample,
                              const gui_rf_decode_status_t *status) {
    gui_rf_preview_worker_t *w = context;
    if (!rgba || !status || width <= 0 || width > GUI_RF_FRAME_MAX_WIDTH ||
        height <= 0 || height > GUI_RF_FRAME_MAX_HEIGHT) return;
    rf_accumulate_status(w, status);
    if (atomic_load_explicit(&w->control, memory_order_acquire) != w->applied_control ||
        atomic_load_explicit(&w->pause_control, memory_order_acquire) !=
        atomic_load_explicit(&w->applied_pause_control, memory_order_relaxed) ||
        (atomic_load_explicit(&w->pause_control, memory_order_relaxed) & 1u) ||
        atomic_load_explicit(&w->flush_requested, memory_order_acquire)) return;
    uint64_t now = rf_now_us(w);
    double elapsed_ms = (double)(w->work_since_frame_us + now - w->work_segment_us) / 1000.0;
    uint64_t source_end = rf_scale_sample(w->block_source_end, w->source_rate);
    double sample_age = source_end >= end_sample ? (double)(source_end - end_sample) : 0.0;
    double delay = gui_rf_resampler_delay_samples(w->wide_resampler ? w->wide_resampler : w->resampler);
    /* Frame callbacks retain the public 20 MSPS axis for every profile. */
    if (w->wide_resampler) delay *= (double)GUI_RF_DECODE_RATE_HZ / gui_rf_profile_video_rate_hz(w->profile);
    if (delay > 0.0) sample_age += delay;
    double latency_ms = (double)(now - w->block_enqueue_us) / 1000.0 +
                        sample_age * 1000.0 / GUI_RF_DECODE_RATE_HZ;
    rf_mutex_lock(&w->frame_mutex);
    memcpy(w->frame, rgba, (size_t)width * height * 4);
    w->frame_width = width;
    w->frame_height = height;
    w->frame_control = w->applied_control;
    w->frame_valid = true;
    w->stats.decode = *status;
    w->stats.source_rate_hz = w->source_rate;
    w->stats.generation = ++w->generation;
    w->stats.decode_ms = elapsed_ms;
    w->stats.latency_ms = latency_ms;
    rf_mutex_unlock(&w->frame_mutex);
    w->work_since_frame_us = 0;
    w->work_segment_us = rf_now_us(w);
}

static void rf_release_queued(gui_rf_preview_worker_t *w, bool paused) {
    uint64_t read = atomic_load_explicit(&w->read_index, memory_order_relaxed);
    uint64_t write = atomic_load_explicit(&w->write_index, memory_order_acquire);
    if (write != read) {
        uint64_t freed = w->queue[(write - 1) % RF_QUEUE_SLOTS].duration_end_us;
        atomic_fetch_add_explicit(paused ? &w->pause_drops : &w->dropped,
                                  write - read, memory_order_relaxed);
        atomic_store_explicit(&w->freed_duration_us, freed, memory_order_release);
        atomic_store_explicit(&w->read_index, write, memory_order_release);
    }
#ifdef _WIN32
    SetEvent(w->done_event);
#endif
}

static void rf_discard_queue(gui_rf_preview_worker_t *w) {
    rf_reset_decoder(w, "RF queue overrun; reacquiring");
    rf_release_queued(w, false);
}

static void rf_release_pause_queue(gui_rf_preview_worker_t *w, uint64_t pause_control) {
    uint64_t read = atomic_load_explicit(&w->read_index, memory_order_relaxed);
    uint64_t write = atomic_load_explicit(&w->write_index, memory_order_acquire);
    uint64_t first = read;
    while (read < write) {
        rf_input_slot_t *slot = &w->queue[read % RF_QUEUE_SLOTS];
        /* Resume can race a new submission. Keep those new-epoch payloads. */
        if (!(pause_control & 1u) && slot->pause_control == pause_control) break;
        atomic_store_explicit(&w->freed_duration_us, slot->duration_end_us, memory_order_release);
        read++;
    }
    if (read != first) {
        atomic_fetch_add_explicit(&w->pause_drops, read - first, memory_order_relaxed);
        atomic_store_explicit(&w->read_index, read, memory_order_release);
#ifdef _WIN32
        SetEvent(w->done_event);
#endif
    }
}

static void rf_wait_input(gui_rf_preview_worker_t *w, uint64_t wait_us) {
#ifdef _WIN32
    DWORD milliseconds = (DWORD)((wait_us + 999u) / 1000u);
    if (milliseconds > 10) milliseconds = 10;
    if (!milliseconds) milliseconds = 1;
    WaitForSingleObject(w->input_event, milliseconds);
#else
    (void)w;
    /* Poll only the decoder worker. Producer/control paths never wait here. */
    (void)wait_us;
    thrd_sleep_ms(1);
#endif
}

static bool rf_acquire_budget(gui_rf_preview_worker_t *w, uint64_t control,
                              uint64_t pause_control) {
    uint64_t begin = rf_now_us(w);
    /* A reused worker also spends a little CPU checking an empty/paused input
     * queue. After a genuinely quiet interval, that old CPU must not become
     * fresh processing debt on reconnect. Preserve completion-to-completion
     * accounting for short source gaps and throughout the wait below: Windows
     * can book an earlier active block's CPU between two nearby blocks. Neither
     * a control reset nor a new source epoch alone bypasses this time bound. */
    if (begin - w->last_budget_completion_us >= RF_QUEUE_MAX_US)
        w->have_cpu_baseline = rf_thread_cpu_us(&w->last_cpu_us);
    bool acquired = false;
    bool waited = false;
    for (;;) {
        if (atomic_load_explicit(&w->stop, memory_order_acquire) ||
            atomic_load_explicit(&w->control, memory_order_acquire) != control ||
            atomic_load_explicit(&w->pause_control, memory_order_acquire) != pause_control ||
            atomic_load_explicit(&w->flush_requested, memory_order_acquire)) break;
        uint64_t wait_us = 1000;
        if (!atomic_flag_test_and_set_explicit(&rf_budget_lock, memory_order_acquire)) {
            acquired = gui_rf_preview_policy_acquire(&rf_budget, (uintptr_t)w, rf_now_us(w), &wait_us);
            if (acquired) rf_budget_wake_head();
            atomic_flag_clear_explicit(&rf_budget_lock, memory_order_release);
        }
        if (acquired) break;
        waited = true;
        atomic_store_explicit(&w->budget_waiting, true, memory_order_release);
        rf_wait_input(w, wait_us);
    }
    if (!acquired) {
        rf_budget_lock_blocking();
        gui_rf_preview_policy_cancel(&rf_budget, (uintptr_t)w);
        rf_budget_wake_waiters();
        atomic_flag_clear_explicit(&rf_budget_lock, memory_order_release);
    }
    atomic_store_explicit(&w->budget_waiting, false, memory_order_release);
    if (waited) {
        rf_mutex_lock(&w->frame_mutex);
        w->stats.budget_wait_ms += (double)(rf_now_us(w) - begin) / 1000.0;
        rf_mutex_unlock(&w->frame_mutex);
    }
    return acquired;
}

static void rf_finish_budget(gui_rf_preview_worker_t *w, uint64_t begin_wall) {
    uint64_t wall = rf_now_us(w) - begin_wall;
    uint64_t end_cpu = 0;
    bool read_cpu = rf_thread_cpu_us(&end_cpu);
    bool actual_cpu = w->have_cpu_baseline && read_cpu && end_cpu >= w->last_cpu_us;
    /* Keep the previous completion counter, not a new per-block start value:
     * coarse Windows accounting can otherwise advance between blocks and be
     * lost altogether. This also charges the worker's small dispatch/check
     * overhead, while sleeping/waiting consumes no thread CPU time. */
    uint64_t cpu = actual_cpu ? end_cpu - w->last_cpu_us : wall;
    w->have_cpu_baseline = read_cpu;
    if (read_cpu) w->last_cpu_us = end_cpu;
    w->video_work_wall_us+=wall;w->video_accounted_cpu_us+=cpu;
    rf_budget_lock_blocking();
    gui_rf_preview_policy_finish(&rf_budget,(uintptr_t)w,rf_now_us(w),cpu);
    /* A fast block can finish long before the next source block arrives.
     * Wake FIFO contenders immediately, preserving the shared debt deadline;
     * do not leave an idle permit waiting for a coarse Windows timeout. */
    rf_budget_wake_waiters();
    atomic_flag_clear_explicit(&rf_budget_lock, memory_order_release);
    rf_mutex_lock(&w->frame_mutex);
    w->stats.processing_cpu_ms += (double)cpu / 1000.0;
    w->stats.processing_wall_ms += (double)wall / 1000.0;
    w->stats.cpu_time_is_wall |= !actual_cpu;
    rf_refresh_interval(w, rf_now_us(w));
    rf_mutex_unlock(&w->frame_mutex);
    w->last_budget_completion_us = rf_now_us(w);
}

static bool rf_audio_current(gui_rf_preview_worker_t *w,uint64_t generation) {
    return !atomic_load_explicit(&w->stop,memory_order_acquire) &&
        atomic_load_explicit(&w->audio_enabled,memory_order_acquire) &&
        gui_rf_profile_has_hifi((gui_rf_profile_t)atomic_load_explicit(&w->requested_profile,memory_order_acquire)) &&
        !(atomic_load_explicit(&w->pause_control,memory_order_acquire)&1u) &&
        generation==atomic_load_explicit(&w->audio_generation,memory_order_acquire);
}
static void rf_audio_callback(void *context,const float *stereo,size_t frames,
                              uint64_t end_sample,const gui_beta_hifi_status_t *status) {
    gui_rf_preview_worker_t *w=context;
    if(!rf_audio_current(w,w->audio_processing_generation)) return;
    rf_mutex_lock(&w->frame_mutex);w->stats.hifi=*status;rf_mutex_unlock(&w->frame_mutex);
    if(atomic_load_explicit(&w->audio_callback_installed,memory_order_acquire) &&
       w->audio_callback && rf_audio_current(w,w->audio_processing_generation))
        w->audio_callback(w->audio_context,stereo,frames,end_sample);
}
static void rf_audio_wait(gui_rf_preview_worker_t *w,uint64_t wait_us) {
#ifdef _WIN32
    DWORD ms=(DWORD)((wait_us+999u)/1000u);if(ms>10) ms=10;if(!ms) ms=1;
    WaitForSingleObject(w->audio_event,ms);
#else
    (void)w;(void)wait_us;thrd_sleep_ms(1);
#endif
}
static bool rf_audio_acquire_budget(gui_rf_preview_worker_t *w,uint64_t generation) {
    const uintptr_t owner=(uintptr_t)w|1u;
    uint64_t begin=rf_now_us(w);
    if(begin-w->audio_last_completion_us>=RF_QUEUE_MAX_US)
        w->audio_have_cpu_baseline=rf_thread_cpu_us(&w->audio_last_cpu_us);
    bool acquired=false,waited=false;
    while(rf_audio_current(w,generation)) {
        uint64_t wait=1000;
        if(!atomic_flag_test_and_set_explicit(&rf_budget_lock,memory_order_acquire)) {
            acquired=gui_rf_preview_policy_acquire(&rf_budget,owner,rf_now_us(w),&wait);
            if(acquired) rf_budget_wake_head();
            atomic_flag_clear_explicit(&rf_budget_lock,memory_order_release);
        }
        if(acquired) break;
        waited=true;atomic_store(&w->audio_budget_waiting,true);rf_audio_wait(w,wait);
    }
    if(!acquired) {
        rf_budget_lock_blocking();gui_rf_preview_policy_cancel(&rf_budget,owner);
        rf_budget_wake_waiters();atomic_flag_clear_explicit(&rf_budget_lock,memory_order_release);
    }
    atomic_store(&w->audio_budget_waiting,false);
    if(waited) {
        rf_mutex_lock(&w->frame_mutex);
        w->stats.budget_wait_ms+=(double)(rf_now_us(w)-begin)/1000.0;
        rf_mutex_unlock(&w->frame_mutex);
    }
    return acquired;
}
static void rf_audio_finish_budget(gui_rf_preview_worker_t *w,uint64_t begin) {
    uint64_t now=rf_now_us(w),wall=now-begin,end_cpu=0;
    bool read=rf_thread_cpu_us(&end_cpu);
    bool measured=w->audio_have_cpu_baseline && read && end_cpu>=w->audio_last_cpu_us;
    uint64_t cpu=measured ? end_cpu-w->audio_last_cpu_us : wall;
    w->audio_have_cpu_baseline=read;if(read) w->audio_last_cpu_us=end_cpu;
    w->audio_work_wall_us+=wall;w->audio_accounted_cpu_us+=cpu;
    rf_budget_lock_blocking();
    gui_rf_preview_policy_finish(&rf_budget,(uintptr_t)w|1u,now,cpu);
    rf_budget_wake_waiters();atomic_flag_clear_explicit(&rf_budget_lock,memory_order_release);
    rf_mutex_lock(&w->frame_mutex);
    w->stats.audio_cpu_ms+=(double)cpu/1000.0;w->stats.audio_wall_ms+=(double)wall/1000.0;
    w->stats.processing_cpu_ms+=(double)cpu/1000.0;
    w->stats.processing_wall_ms+=(double)wall/1000.0;
    w->stats.cpu_time_is_wall|=!measured;rf_refresh_interval(w,now);
    rf_mutex_unlock(&w->frame_mutex);w->audio_last_completion_us=now;
}
static int rf_audio_thread(void *context) {
    gui_rf_preview_worker_t *w=context;
    rf_register_counter(w,true);
    (void)rf_lower_thread_priority();
    w->audio_have_cpu_baseline=rf_thread_cpu_us(&w->audio_last_cpu_us);
    w->audio_last_completion_us=rf_now_us(w);
    bool needs_reset=false;
    while(!atomic_load_explicit(&w->stop,memory_order_acquire)) {
        uint64_t generation=atomic_load_explicit(&w->audio_generation,memory_order_acquire);
        if(generation!=atomic_load_explicit(&w->applied_audio_generation,memory_order_relaxed)) {
            atomic_store(&w->audio_active,true);
            /* Flush is cheap and prompt even while muted/paused. Rebuilding
             * DSP state is deferred into the next budgeted active block. */
            needs_reset=true;
            if(atomic_load_explicit(&w->audio_callback_installed,memory_order_acquire) && w->audio_callback)
                w->audio_callback(w->audio_context,NULL,0,0);
            rf_mutex_lock(&w->frame_mutex);memset(&w->stats.hifi,0,sizeof(w->stats.hifi));rf_mutex_unlock(&w->frame_mutex);
            atomic_store_explicit(&w->applied_audio_generation,generation,memory_order_release);
            atomic_store(&w->audio_active,false);
#ifdef _WIN32
            SetEvent(w->done_event);
#endif
        }
        uint64_t read=atomic_load_explicit(&w->audio_read,memory_order_relaxed);
        uint64_t write=atomic_load_explicit(&w->audio_write,memory_order_acquire);
        if(read==write) {rf_audio_wait(w,10000);continue;}
        rf_audio_slot_t *slot=&w->audio_queue[read%RF_AUDIO_QUEUE_SLOTS];
        /* A source/control change may enqueue a current slot after the loop
         * captured its old generation. Refresh before discarding fresh audio. */
        if(slot->generation!=generation &&
           slot->generation==atomic_load_explicit(&w->audio_generation,memory_order_acquire)) continue;
        if(slot->generation!=generation || !rf_audio_current(w,generation)) goto audio_release;
        if(rf_now_us(w)-slot->enqueue_us>RF_AUDIO_MAX_US) {
            atomic_fetch_add(&w->audio_dropped,1);rf_audio_invalidate(w);goto audio_release;
        }
        if(!rf_audio_acquire_budget(w,generation)) continue;
        uint64_t begin=rf_now_us(w);
#ifndef _WIN32
        /* Capture/record startup can renice existing Linux threads. Reassert
         * this consumer's background request as the video worker does. */
        (void)rf_lower_thread_priority();
#endif
        atomic_store_explicit(&w->audio_active,true,memory_order_release);
        if(rf_audio_current(w,generation) && rf_now_us(w)-slot->enqueue_us<=RF_AUDIO_MAX_US) {
            w->audio_processing_generation=generation;
            if(needs_reset) {gui_beta_hifi_reset(w->hifi);needs_reset=false;}
            gui_beta_hifi_process(w->hifi,slot->samples,slot->count,slot->first_sample,rf_audio_callback,w);
        }
        rf_audio_finish_budget(w,begin);
        atomic_store_explicit(&w->audio_active,false,memory_order_release);
audio_release:
        atomic_store_explicit(&w->audio_read,read+1,memory_order_release);
#ifdef _WIN32
        SetEvent(w->done_event);
#endif
    }
    rf_unregister_counter(w,true);
    return 0;
}
static bool rf_audio_start(gui_rf_preview_worker_t *w) {
    if(atomic_load_explicit(&w->audio_ready,memory_order_acquire)) return true;
    w->audio_queue=calloc(RF_AUDIO_QUEUE_SLOTS,sizeof(*w->audio_queue));
    w->hifi=gui_beta_hifi_create();
    if(!w->audio_queue || !w->hifi) goto fail;
    if(thrd_create_with_priority(&w->audio_thread,rf_audio_thread,w,THRD_PRIORITY_NORMAL)!=thrd_success) goto fail;
    atomic_store_explicit(&w->audio_ready,true,memory_order_release);return true;
fail:
    free(w->audio_queue);w->audio_queue=NULL;gui_beta_hifi_destroy(w->hifi);w->hifi=NULL;return false;
}
static void rf_audio_enqueue(gui_rf_preview_worker_t *w,const float *samples,size_t count,uint64_t first,
                             uint64_t generation,uint64_t source_generation,
                             uint64_t control,uint64_t pause_control) {
    if(!atomic_load_explicit(&w->audio_ready,memory_order_acquire) ||
       !atomic_load_explicit(&w->audio_enabled,memory_order_acquire)) return;
    if(!rf_audio_current(w,generation) ||
       source_generation!=atomic_load_explicit(&w->audio_source_generation,memory_order_acquire) ||
       atomic_load_explicit(&w->control,memory_order_acquire)!=control ||
       atomic_load_explicit(&w->pause_control,memory_order_acquire)!=pause_control) return;
    uint64_t write=atomic_load_explicit(&w->audio_write,memory_order_relaxed);
    uint64_t read=atomic_load_explicit(&w->audio_read,memory_order_acquire);
    size_t blocks=(count+RF_AUDIO_BLOCK_SAMPLES-1)/RF_AUDIO_BLOCK_SAMPLES;
    if(blocks>RF_AUDIO_QUEUE_SLOTS-(write-read)) {
        atomic_fetch_add(&w->audio_dropped,blocks);rf_audio_invalidate(w);return;
    }
    uint64_t now=rf_now_us(w);
    while(count) {
        size_t n=count<RF_AUDIO_BLOCK_SAMPLES ? count : RF_AUDIO_BLOCK_SAMPLES;
        rf_audio_slot_t *slot=&w->audio_queue[write%RF_AUDIO_QUEUE_SLOTS];
        slot->count=n;slot->first_sample=first;slot->generation=generation;slot->enqueue_us=now;
        memcpy(slot->samples,samples,n*sizeof(*samples));write++;samples+=n;count-=n;first+=n;
    }
    atomic_store_explicit(&w->audio_write,write,memory_order_release);
#ifdef _WIN32
    SetEvent(w->audio_event);
#endif
}

static int rf_worker_thread(void *context) {
    gui_rf_preview_worker_t *w = context;
    rf_register_counter(w,false);
    rf_mutex_lock(&w->frame_mutex);
    w->stats.priority_lowered = rf_lower_thread_priority();
    rf_mutex_unlock(&w->frame_mutex);
    w->have_cpu_baseline = rf_thread_cpu_us(&w->last_cpu_us);
    w->last_budget_completion_us = rf_now_us(w);
    while (!atomic_load_explicit(&w->stop, memory_order_acquire)) {
        bool full_frame = atomic_load_explicit(&w->requested_full_frame, memory_order_acquire);
        if (full_frame != w->applied_full_frame) {
            gui_rf_decoder_set_frame_mode(w->decoder, full_frame);
            w->applied_full_frame = full_frame;
        }
        uint64_t control = atomic_load_explicit(&w->control, memory_order_acquire);
        if (control != w->applied_control) {
            bool was_ed_beta = w->profile == GUI_RF_PROFILE_NTSC_ED_BETA;
            w->profile = (gui_rf_profile_t)atomic_load_explicit(&w->requested_profile, memory_order_relaxed);
            if (gui_rf_profile_has_hifi(w->profile)) (void)rf_audio_start(w);
            if (w->source_rate && was_ed_beta != (w->profile == GUI_RF_PROFILE_NTSC_ED_BETA))
                (void)rf_configure_rate(w, w->source_rate);
            /* Publish completion only after reset and audio invalidation. */
            rf_reset_decoder(w, w->source_rate && !w->resampler ? rf_rate_error(w) : "RF preview reset; acquiring");
            atomic_store_explicit(&w->applied_control,control,memory_order_release);
        }
        uint64_t pause_control = atomic_load_explicit(&w->pause_control, memory_order_acquire);
        if (pause_control != atomic_load_explicit(&w->applied_pause_control, memory_order_relaxed)) {
            rf_reset_decoder_preserve(w, pause_control & 1u ?
                                      "RF preview paused; last frame frozen" :
                                      "RF preview resumed; reacquiring", true);
            rf_release_pause_queue(w, pause_control);
            atomic_store_explicit(&w->flush_requested, false, memory_order_release);
            atomic_store_explicit(&w->applied_pause_control, pause_control, memory_order_release);
#ifdef _WIN32
            SetEvent(w->done_event);
#endif
            continue;
        }
        if (pause_control & 1u) {
            rf_release_pause_queue(w, pause_control);
            rf_wait_input(w, 10000);
            continue;
        }
        if (atomic_exchange_explicit(&w->flush_requested, false, memory_order_acq_rel)) {
            rf_discard_queue(w);
            continue;
        }
        uint64_t read = atomic_load_explicit(&w->read_index, memory_order_relaxed);
        uint64_t write = atomic_load_explicit(&w->write_index, memory_order_acquire);
        if (read == write) {
#ifdef _WIN32
            /* Sleep(1) can take 15.6 ms in a headless Windows process. An
             * auto-reset event keeps sparse/high-rate batches responsive. */
            WaitForSingleObject(w->input_event, 50);
#else
            thrd_sleep_ms(1);
#endif
            continue;
        }
        rf_input_slot_t *slot = &w->queue[read % RF_QUEUE_SLOTS];
        if (rf_now_us(w) - slot->enqueue_us > RF_QUEUE_MAX_US) {
            rf_discard_queue(w);
            continue;
        }
        if (slot->control == w->applied_control) {
            if (slot->pause_control != pause_control) {
                atomic_fetch_add_explicit(&w->pause_drops, 1, memory_order_relaxed);
                goto release_slot;
            }
            if (!rf_acquire_budget(w, control, pause_control)) continue;
            uint64_t begin_wall = rf_now_us(w);
#ifndef _WIN32
            /* Common capture startup can renice every existing Linux thread.
             * Reassert only this worker's background request before work. */
            bool priority_lowered = rf_lower_thread_priority();
            rf_mutex_lock(&w->frame_mutex);
            w->stats.priority_lowered = priority_lowered;
            rf_mutex_unlock(&w->frame_mutex);
#endif
            /* Recheck after pacing: a pending block must not defeat pause or
             * survive the input-age bound simply because it was waiting. */
            if (atomic_load_explicit(&w->pause_control, memory_order_acquire) != pause_control ||
                atomic_load_explicit(&w->control, memory_order_acquire) != control ||
                atomic_load_explicit(&w->stop, memory_order_acquire) ||
                rf_now_us(w) - slot->enqueue_us > RF_QUEUE_MAX_US) {
                rf_finish_budget(w, begin_wall);
                if (rf_now_us(w) - slot->enqueue_us > RF_QUEUE_MAX_US) rf_discard_queue(w);
                continue;
            }
            bool boundary = !w->stream_valid || slot->info.discontinuity ||
                            slot->info.epoch != w->source_epoch ||
                            slot->info.sample_rate_hz != w->source_rate ||
                            slot->info.start_sample != w->expected_source_sample;
            if (boundary) {
                bool changed_rate = slot->info.sample_rate_hz != w->source_rate;
                if (changed_rate) (void)rf_configure_rate(w, slot->info.sample_rate_hz);
                rf_reset_decoder(w, w->resampler ? "RF discontinuity; acquiring" : rf_rate_error(w));
                w->source_epoch = slot->info.epoch;
                w->output_sample = w->source_rate ? rf_scale_sample(slot->info.start_sample, w->source_rate) : 0;
                w->wide_output_sample = w->source_rate ? rf_scale_sample_rate(slot->info.start_sample,
                    w->source_rate, gui_rf_profile_video_rate_hz(w->profile)) : 0;
                w->stream_valid = true;
            }
            w->expected_source_sample = slot->info.start_sample + slot->count;
            if (w->resampler) {
                w->block_enqueue_us = slot->enqueue_us;
                w->block_source_end = w->expected_source_sample;
                w->work_segment_us = rf_now_us(w);
                size_t output_count = 0;
                /* Never assign a fresh audio epoch to an old block that was
                 * already resampling when mute/reset/seek changed controls. */
                uint64_t audio_generation=atomic_load_explicit(&w->audio_generation,memory_order_acquire);
                size_t wide_count = 0;
                bool narrow_needed = !w->wide_resampler ||
                    atomic_load_explicit(&w->audio_enabled,memory_order_acquire);
                bool resampled = true;
                if (narrow_needed) {
                    if (w->wide_resampler && (!w->narrow_stream_valid ||
                        w->narrow_audio_generation != audio_generation)) {
                        /* This converter supplies only ED audio. Drop any old
                         * buffered tail after mute/control changes and restart
                         * its independent clock at the current source block. */
                        gui_rf_resampler_reset(w->resampler);
                        w->output_sample = rf_scale_sample(slot->info.start_sample,w->source_rate);
                    }
                    resampled = gui_rf_resampler_process(w->resampler, slot->samples, slot->count,
                        w->output, w->output_capacity, &output_count);
                    w->narrow_stream_valid = resampled;
                    w->narrow_audio_generation = audio_generation;
                } else {
                    /* ED video advances normally; resume audio from fresh RF,
                     * rather than pretending its skipped history is contiguous. */
                    w->narrow_stream_valid = false;
                    w->output_sample = rf_scale_sample(w->expected_source_sample,w->source_rate);
                }
                if (resampled && w->wide_resampler)
                    resampled = gui_rf_resampler_process(w->wide_resampler, slot->samples, slot->count,
                        w->wide_output, w->wide_output_capacity, &wide_count);
                if (resampled) {
                    if (output_count && gui_rf_profile_has_hifi(w->profile))
                        rf_audio_enqueue(w,w->output,output_count,w->output_sample,audio_generation,slot->audio_source_generation,control,pause_control);
                    if (w->wide_resampler) {
                        if (wide_count) gui_rf_decoder_process(w->decoder, w->wide_output, wide_count,
                            w->wide_output_sample, rf_frame_callback, w);
                        w->wide_output_sample += wide_count;
                    } else if (output_count) {
                        gui_rf_decoder_process(w->decoder, w->output, output_count,
                            w->output_sample, rf_frame_callback, w);
                    }
                    w->output_sample += output_count;
                    w->work_since_frame_us += rf_now_us(w) - w->work_segment_us;
                    gui_rf_decode_status_t status;
                    gui_rf_decoder_get_status(w->decoder, &status);
                    rf_accumulate_status(w, &status);
                } else {
                    rf_reset_decoder(w, "RF resampling failed; reacquiring");
                }
            }
            rf_finish_budget(w, begin_wall);
        } else {
            atomic_fetch_add_explicit(&w->dropped, 1, memory_order_relaxed);
        }
release_slot:
        atomic_store_explicit(&w->freed_duration_us, slot->duration_end_us, memory_order_release);
        atomic_store_explicit(&w->read_index, read + 1, memory_order_release);
#ifdef _WIN32
        SetEvent(w->done_event);
#endif
    }
    rf_unregister_counter(w,false);
    return 0;
}

gui_rf_preview_worker_t *gui_rf_preview_worker_create(gui_rf_format_t format) {
    if (format != GUI_RF_PAL && format != GUI_RF_NTSC) return NULL;
    return gui_rf_preview_worker_create_profile(format == GUI_RF_NTSC ?
             GUI_RF_PROFILE_NTSC_VHS : GUI_RF_PROFILE_PAL_VHS);
}

gui_rf_preview_worker_t *gui_rf_preview_worker_create_profile(gui_rf_profile_t profile) {
    if ((unsigned)profile >= GUI_RF_PROFILE_COUNT) return NULL;
    gui_rf_preview_worker_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    if (!rf_mutex_init(&w->frame_mutex)) { free(w); return NULL; }
#ifdef _WIN32
    LARGE_INTEGER frequency;
    if (QueryPerformanceFrequency(&frequency)) w->timer_frequency = (uint64_t)frequency.QuadPart;
    w->input_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    w->audio_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    w->done_event = CreateEvent(NULL, FALSE, FALSE, NULL);
#endif
    atomic_init(&w->stop, false);
    atomic_init(&w->audio_ready,false);atomic_init(&w->audio_enabled,true);
    atomic_init(&w->audio_callback_installed,false);
    atomic_init(&w->audio_active,false);atomic_init(&w->audio_budget_waiting,false);
    atomic_init(&w->audio_generation,1);atomic_init(&w->applied_audio_generation,1);
    atomic_init(&w->audio_source_generation,1);
    atomic_init(&w->audio_read,0);atomic_init(&w->audio_write,0);atomic_init(&w->audio_dropped,0);
    atomic_init(&w->flush_requested, false);
    atomic_init(&w->producer_gap, false);
    atomic_flag_clear(&w->producer_busy);
    atomic_init(&w->read_index, 0);
    atomic_init(&w->write_index, 0);
    atomic_init(&w->freed_duration_us, 0);
    atomic_init(&w->dropped, 0);
    atomic_init(&w->pause_drops, 0);
    atomic_init(&w->pause_control, 0);
    atomic_init(&w->applied_pause_control, 0);
    atomic_init(&w->budget_waiting, false);
    atomic_init(&w->control, 1);
    atomic_init(&w->requested_profile, profile);
    atomic_init(&w->requested_full_frame, false);
    w->profile = profile;
    atomic_init(&w->applied_control,1);
    w->interval_start_us = rf_now_us(w);
    w->queue = calloc(RF_QUEUE_SLOTS, sizeof(*w->queue));
    w->frame = malloc(RF_FRAME_BYTES);
    w->decoder = gui_rf_decoder_create_profile(profile);
    if (!w->queue || !w->frame || !w->decoder) goto fail;
    if (gui_rf_profile_has_hifi(profile) && !rf_audio_start(w)) goto fail;
#ifdef _WIN32
    if (!w->input_event || !w->audio_event || !w->done_event) goto fail;
#endif
    snprintf(w->stats.decode.reason, sizeof(w->stats.decode.reason), "Waiting for RF");
    if (thrd_create_with_priority(&w->thread, rf_worker_thread, w, THRD_PRIORITY_NORMAL) != thrd_success) goto fail;
    return w;
fail:
    atomic_store(&w->stop,true);
    if(atomic_load(&w->audio_ready)) {
#ifdef _WIN32
        SetEvent(w->audio_event);
#endif
        thrd_join(w->audio_thread,NULL);
    }
    free(w->audio_queue);
    gui_rf_decoder_destroy(w->decoder);
    gui_beta_hifi_destroy(w->hifi);
    free(w->queue);
    free(w->frame);
    rf_mutex_destroy(&w->frame_mutex);
#ifdef _WIN32
    if (w->input_event) CloseHandle(w->input_event);
    if (w->audio_event) CloseHandle(w->audio_event);
    if (w->done_event) CloseHandle(w->done_event);
#endif
    free(w);
    return NULL;
}

void gui_rf_preview_worker_destroy(gui_rf_preview_worker_t *w) {
    if (!w) return;
    atomic_store_explicit(&w->stop, true, memory_order_release);
#ifdef _WIN32
    SetEvent(w->input_event);
    SetEvent(w->audio_event);
#endif
    thrd_join(w->thread, NULL);
    if(atomic_load(&w->audio_ready)) thrd_join(w->audio_thread,NULL);
    free(w->audio_queue);
    gui_rf_resampler_destroy(w->resampler);
    gui_rf_resampler_destroy(w->wide_resampler);
    gui_rf_decoder_destroy(w->decoder);
    gui_beta_hifi_destroy(w->hifi);
    free(w->output);
    free(w->wide_output);
    free(w->queue);
    free(w->frame);
    rf_mutex_destroy(&w->frame_mutex);
#ifdef _WIN32
    CloseHandle(w->input_event);
    CloseHandle(w->audio_event);
    CloseHandle(w->done_event);
#endif
    free(w);
}

void gui_rf_preview_worker_set_format(gui_rf_preview_worker_t *w, gui_rf_format_t format) {
    if (format != GUI_RF_PAL && format != GUI_RF_NTSC) return;
    gui_rf_preview_worker_set_profile(w, format == GUI_RF_NTSC ?
              GUI_RF_PROFILE_NTSC_VHS : GUI_RF_PROFILE_PAL_VHS);
}

void gui_rf_preview_worker_set_audio_callback(gui_rf_preview_worker_t *w,
                                              gui_rf_preview_audio_callback_t callback,
                                              void *context) {
    if (!w) return;
    /* Install exactly once. The audio thread already exists at creation;
     * publishing this flag supplies the context/pointer happens-before edge. */
    if(atomic_load_explicit(&w->audio_callback_installed,memory_order_acquire)) return;
    w->audio_callback = callback;
    w->audio_context = context;
    atomic_store_explicit(&w->audio_callback_installed,true,memory_order_release);
}

void gui_rf_preview_worker_set_audio_enabled(gui_rf_preview_worker_t *w,bool enabled) {
    if(!w) return;
    if(atomic_exchange_explicit(&w->audio_enabled,enabled,memory_order_acq_rel)!=enabled)
        rf_audio_cut_input(w);
}

void gui_rf_preview_worker_set_profile(gui_rf_preview_worker_t *w, gui_rf_profile_t profile) {
    if (!w || (unsigned)profile >= GUI_RF_PROFILE_COUNT) return;
    if (atomic_exchange_explicit(&w->requested_profile, profile, memory_order_relaxed) != (int)profile) {
        rf_audio_cut_input(w);
        atomic_fetch_add_explicit(&w->control, 1, memory_order_release);
    }
#ifdef _WIN32
    SetEvent(w->input_event);
#endif
}

void gui_rf_preview_worker_set_frame_mode(gui_rf_preview_worker_t *worker,
                                         bool full_frame) {
    if (!worker) return;
    atomic_store_explicit(&worker->requested_full_frame, full_frame, memory_order_release);
}

void gui_rf_preview_worker_reset(gui_rf_preview_worker_t *w) {
    if (!w) return;
    rf_audio_cut_input(w);
    atomic_fetch_add_explicit(&w->control, 1, memory_order_release);
#ifdef _WIN32
    SetEvent(w->input_event);
#endif
}

void gui_rf_preview_worker_set_paused(gui_rf_preview_worker_t *w, bool paused) {
    if (!w) return;
    uint64_t previous = atomic_load_explicit(&w->pause_control, memory_order_relaxed);
    for (;;) {
        if ((bool)(previous & 1u) == paused) return;
        uint64_t next = ((previous & ~(uint64_t)1) + 2u) | (uint64_t)paused;
        if (atomic_compare_exchange_weak_explicit(&w->pause_control, &previous, next,
                                                  memory_order_release, memory_order_relaxed)) break;
    }
    atomic_store_explicit(&w->producer_gap, true, memory_order_release);
    rf_audio_cut_input(w);
#ifdef _WIN32
    SetEvent(w->input_event);
#endif
}

static bool rf_reject_block(gui_rf_preview_worker_t *w, bool flush) {
    atomic_fetch_add_explicit(&w->dropped, 1, memory_order_relaxed);
    atomic_store_explicit(&w->producer_gap, true, memory_order_release);
    if (flush) {
        rf_audio_cut_input(w);
        atomic_store_explicit(&w->flush_requested, true, memory_order_release);
    }
    return false;
}

bool gui_rf_preview_worker_submit(gui_rf_preview_worker_t *w, const int16_t *samples,
                                  size_t count, const gui_rf_block_info_t *info) {
    if (!w) return false;
    uint64_t pause_control = atomic_load_explicit(&w->pause_control, memory_order_acquire);
    if (pause_control & 1u) {
        atomic_fetch_add_explicit(&w->pause_drops, 1, memory_order_relaxed);
        return false;
    }
    if (!samples || !info || !count || count > RF_BLOCK_SAMPLES ||
        info->start_sample > UINT64_MAX - count || atomic_load_explicit(&w->stop, memory_order_relaxed))
        return rf_reject_block(w, false);
    /* This also makes accidental concurrent producers fail immediately. */
    if (atomic_flag_test_and_set_explicit(&w->producer_busy, memory_order_acquire))
        return rf_reject_block(w, false);
    if (atomic_load_explicit(&w->pause_control, memory_order_acquire) != pause_control) {
        atomic_flag_clear_explicit(&w->producer_busy, memory_order_release);
        atomic_fetch_add_explicit(&w->pause_drops, 1, memory_order_relaxed);
        return false;
    }
    uint64_t write = atomic_load_explicit(&w->write_index, memory_order_relaxed);
    uint64_t read = atomic_load_explicit(&w->read_index, memory_order_acquire);
    uint64_t freed = atomic_load_explicit(&w->freed_duration_us, memory_order_acquire);
    uint32_t rate = info->sample_rate_hz;
    if (rate < RF_MIN_RATE) rate = RF_MIN_RATE;
    uint64_t duration = ((uint64_t)count * 1000000u + rate - 1) / rate;
    uint64_t end = w->producer_duration_us + duration;
    if (write - read >= RF_QUEUE_SLOTS || end - freed > RF_QUEUE_MAX_US ||
        atomic_load_explicit(&w->flush_requested, memory_order_acquire)) {
        atomic_flag_clear_explicit(&w->producer_busy, memory_order_release);
        return rf_reject_block(w, true);
    }
    rf_input_slot_t *slot = &w->queue[write % RF_QUEUE_SLOTS];
    slot->info = *info;
    slot->info.discontinuity |= atomic_exchange_explicit(&w->producer_gap, false, memory_order_acq_rel);
    bool audio_boundary=slot->info.discontinuity || (w->producer_audio_seen &&
        (info->epoch!=w->producer_audio_epoch || info->sample_rate_hz!=w->producer_audio_rate ||
         info->start_sample!=w->producer_audio_next));
    if(audio_boundary) rf_audio_cut_input(w);
    slot->audio_source_generation=atomic_load_explicit(&w->audio_source_generation,memory_order_acquire);
    w->producer_audio_seen=true;w->producer_audio_epoch=info->epoch;
    w->producer_audio_rate=info->sample_rate_hz;w->producer_audio_next=info->start_sample+count;
    slot->count = count;
    slot->enqueue_us = rf_now_us(w);
    slot->duration_end_us = end;
    slot->control = atomic_load_explicit(&w->control, memory_order_acquire);
    slot->pause_control = pause_control;
    memcpy(slot->samples, samples, count * sizeof(*samples));
    w->producer_duration_us = end;
    atomic_store_explicit(&w->write_index, write + 1, memory_order_release);
#ifdef _WIN32
    SetEvent(w->input_event);
#endif
    atomic_flag_clear_explicit(&w->producer_busy, memory_order_release);
    return true;
}

void gui_rf_preview_worker_set_monitor_frame_sink(gui_rf_preview_worker_t *w,
    void (*callback)(void *, const uint8_t *, unsigned, unsigned, uint64_t),
    void *context) {
    if (!w) return;
    rf_mutex_lock(&w->frame_mutex);
    w->monitor_frame_sink = callback;
    w->monitor_frame_sink_context = context;
    rf_mutex_unlock(&w->frame_mutex);
}

bool gui_rf_preview_worker_take_frame(gui_rf_preview_worker_t *w, uint8_t *rgba,
                                      size_t capacity, uint64_t *generation,
                                      int *width, int *height, gui_rf_preview_stats_t *stats) {
    if (!w || !rgba || !generation) return false;
    bool copied = false;
    rf_mutex_lock(&w->frame_mutex);
    rf_refresh_interval(w, rf_now_us(w));
    if (w->frame_valid && w->frame_control == atomic_load_explicit(&w->control, memory_order_acquire) &&
        w->stats.generation != *generation && capacity >= (size_t)w->frame_width * w->frame_height * 4 &&
        !atomic_load_explicit(&w->flush_requested, memory_order_acquire)) {
        memcpy(rgba, w->frame, (size_t)w->frame_width * w->frame_height * 4);
        *generation = w->stats.generation;
        if (width) *width = w->frame_width;
        if (height) *height = w->frame_height;
        copied = true;
    }
    if (stats) {
        *stats = w->stats;
        stats->blocks_dropped = atomic_load_explicit(&w->dropped, memory_order_relaxed);
        stats->pause_drops = atomic_load_explicit(&w->pause_drops, memory_order_relaxed);
        stats->paused = (atomic_load_explicit(&w->pause_control, memory_order_acquire) & 1u) != 0;
        stats->budget_waiting = atomic_load_explicit(&w->budget_waiting, memory_order_acquire) ||
            atomic_load_explicit(&w->audio_budget_waiting,memory_order_acquire);
        stats->audio_active=atomic_load_explicit(&w->audio_active,memory_order_acquire);
        stats->audio_enabled=atomic_load_explicit(&w->audio_enabled,memory_order_acquire);
        stats->audio_blocks_dropped=atomic_load_explicit(&w->audio_dropped,memory_order_relaxed);
    }
    rf_mutex_unlock(&w->frame_mutex);
    return copied;
}

void gui_rf_preview_worker_get_stats(gui_rf_preview_worker_t *w, gui_rf_preview_stats_t *stats) {
    if (!w || !stats) return;
    rf_mutex_lock(&w->frame_mutex);
    rf_refresh_interval(w, rf_now_us(w));
    *stats = w->stats;
    stats->blocks_dropped = atomic_load_explicit(&w->dropped, memory_order_relaxed);
    stats->pause_drops = atomic_load_explicit(&w->pause_drops, memory_order_relaxed);
    stats->paused = (atomic_load_explicit(&w->pause_control, memory_order_acquire) & 1u) != 0;
    stats->budget_waiting = atomic_load_explicit(&w->budget_waiting, memory_order_acquire) ||
            atomic_load_explicit(&w->audio_budget_waiting,memory_order_acquire);
        stats->audio_active=atomic_load_explicit(&w->audio_active,memory_order_acquire);
        stats->audio_enabled=atomic_load_explicit(&w->audio_enabled,memory_order_acquire);
        stats->audio_blocks_dropped=atomic_load_explicit(&w->audio_dropped,memory_order_relaxed);
    rf_mutex_unlock(&w->frame_mutex);
}

bool gui_rf_preview_worker_wait_idle(gui_rf_preview_worker_t *w, uint32_t timeout_ms) {
    if (!w) return false;
    uint64_t deadline = rf_now_us(w) + (uint64_t)timeout_ms * 1000u;
    for (;;) {
        uint64_t write = atomic_load_explicit(&w->write_index, memory_order_acquire);
        uint64_t read = atomic_load_explicit(&w->read_index, memory_order_acquire);
        if (read == write &&
            atomic_load_explicit(&w->applied_control,memory_order_acquire)==atomic_load_explicit(&w->control,memory_order_acquire) &&
            (!atomic_load_explicit(&w->audio_ready,memory_order_acquire) ||
             (atomic_load_explicit(&w->audio_read,memory_order_acquire)==atomic_load_explicit(&w->audio_write,memory_order_acquire) &&
              !atomic_load_explicit(&w->audio_active,memory_order_acquire) &&
              atomic_load_explicit(&w->applied_audio_generation,memory_order_acquire)==atomic_load_explicit(&w->audio_generation,memory_order_acquire))) &&
            !atomic_load_explicit(&w->flush_requested, memory_order_acquire) &&
            atomic_load_explicit(&w->applied_pause_control, memory_order_acquire) ==
            atomic_load_explicit(&w->pause_control, memory_order_acquire))
            return true;
        uint64_t now = rf_now_us(w);
        if (atomic_load_explicit(&w->stop, memory_order_acquire) || now >= deadline)
            return false;
#ifdef _WIN32
        uint64_t remaining_ms = (deadline - now + 999u) / 1000u;
        DWORD wait_ms = remaining_ms > 1000u ? 1000u : (DWORD)remaining_ms;
        WaitForSingleObject(w->done_event, wait_ms);
#else
        thrd_sleep_ms(1);
#endif
    }
}
