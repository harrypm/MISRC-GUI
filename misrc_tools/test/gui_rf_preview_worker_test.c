/* Worker lifecycle/queue tests use deterministic DSP substitutes. Actual RF
 * recovery and filter fidelity are tested by the RF decoder/resampler suites. */
#include "../misrc_gui/processing/gui_rf_preview_worker.h"
#include "../misrc_gui/signal/gui_rf_resampler.h"
#include "../common/threading.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool hold_process;
static atomic_bool process_entered;
static atomic_uint process_calls;
static atomic_uint reset_calls;
static atomic_uint create_calls;
static atomic_uint narrow_resample_calls,wide_resample_calls,narrow_reset_calls;
static atomic_uint busy_ms;
static atomic_uint active_processes;
static atomic_uint max_active_processes;
static atomic_uint_fast64_t latest_first_sample,latest_video_count;
static atomic_int latest_video_marker;
static atomic_bool delay_wide_once,delay_audio_once;
static atomic_uint_fast64_t counter_seed;
static atomic_bool hold_audio,audio_entered,hold_resample,resample_entered;
static atomic_bool hold_audio_flush,audio_flush_entered;
static atomic_uint audio_calls,audio_callbacks,audio_resets;
static atomic_uint_fast64_t audio_first,audio_end,audio_count;
static atomic_int audio_marker,expected_audio_marker;
static atomic_uint wrong_audio_callbacks;
struct gui_beta_hifi_decoder { int placeholder; };
gui_beta_hifi_decoder_t *gui_beta_hifi_create(void) {return calloc(1,sizeof(gui_beta_hifi_decoder_t));}
void gui_beta_hifi_destroy(gui_beta_hifi_decoder_t *d) {free(d);}
void gui_beta_hifi_reset(gui_beta_hifi_decoder_t *d) {(void)d;atomic_fetch_add(&audio_resets,1);}
void gui_beta_hifi_process(gui_beta_hifi_decoder_t *d,const float *samples,size_t count,uint64_t first,
                          gui_beta_hifi_pcm_callback_t cb,void *ctx) {
    (void)d;atomic_store(&audio_entered,true);
    while(atomic_load(&hold_audio)) thrd_sleep_ms(1);
    atomic_store(&audio_first,first);atomic_store(&audio_count,count);atomic_store(&audio_marker,(int)samples[0]);
    float pcm[2]={samples[0],-samples[0]};gui_beta_hifi_status_t status={.locked=true};
    cb(ctx,pcm,1,first+count,&status);atomic_fetch_add(&audio_calls,1);
}
void gui_beta_hifi_get_status(const gui_beta_hifi_decoder_t *d,gui_beta_hifi_status_t *s) {(void)d;memset(s,0,sizeof(*s));}
static void audio_test_callback(void *ctx,const float *pcm,size_t frames,uint64_t end) {
    (void)ctx;
    if(!pcm || !frames) {
        atomic_store(&audio_flush_entered,true);
        while(atomic_load(&hold_audio_flush)) thrd_sleep_ms(1);
        return;
    }
    if(pcm && frames) {
        int expected=atomic_load(&expected_audio_marker);
        if(expected && (int)pcm[0]!=expected) atomic_fetch_add(&wrong_audio_callbacks,1);
        atomic_store(&audio_end,end);atomic_fetch_add(&audio_callbacks,1);
    }
}
static int failures;

struct gui_rf_decoder { gui_rf_format_t format; gui_rf_profile_t profile; gui_rf_decode_status_t status; };
bool gui_rf_profile_has_hifi(gui_rf_profile_t profile) {
    return profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI || profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI ||
        profile==GUI_RF_PROFILE_NTSC_ED_BETA;
}
uint32_t gui_rf_profile_video_rate_hz(gui_rf_profile_t profile) {
    (void)profile;return 20000000u;
}
struct gui_rf_resampler { uint32_t rate,output_rate;uint64_t phase;size_t pending;float pending_marker;bool first,ed_beta; };

gui_rf_decoder_t *gui_rf_decoder_create(gui_rf_format_t format) {
    gui_rf_decoder_t *d = calloc(1, sizeof(*d));
    if (d) d->format = format;
    return d;
}
void gui_rf_decoder_destroy(gui_rf_decoder_t *d) { free(d); }
void gui_rf_decoder_set_frame_mode(gui_rf_decoder_t *d, bool full) { (void)d; (void)full; }
gui_rf_decoder_t *gui_rf_decoder_create_profile(gui_rf_profile_t profile) {
    gui_rf_decoder_t *d=gui_rf_decoder_create(profile==GUI_RF_PROFILE_PAL_VHS ||
        profile==GUI_RF_PROFILE_PAL_SVHS ? GUI_RF_PAL : GUI_RF_NTSC);
    if(d)d->profile=profile;
    return d;
}
void gui_rf_decoder_reset_profile(gui_rf_decoder_t *d, gui_rf_profile_t profile) {
    gui_rf_decoder_reset(d, profile==GUI_RF_PROFILE_PAL_VHS ||
        profile==GUI_RF_PROFILE_PAL_SVHS ? GUI_RF_PAL : GUI_RF_NTSC);
    d->profile=profile;
}
void gui_rf_decoder_reset(gui_rf_decoder_t *d, gui_rf_format_t format) {
    d->format = format;
    memset(&d->status, 0, sizeof(d->status));
    atomic_fetch_add(&reset_calls, 1);
}
void gui_rf_decoder_get_status(const gui_rf_decoder_t *d, gui_rf_decode_status_t *s) { *s = d->status; }
void gui_rf_decoder_process(gui_rf_decoder_t *d, const float *samples, size_t count,
                            uint64_t first, gui_rf_frame_callback_t callback, void *context) {
    atomic_store(&process_entered, true);
    unsigned int active = atomic_fetch_add(&active_processes, 1) + 1;
    unsigned int maximum = atomic_load(&max_active_processes);
    while (active > maximum && !atomic_compare_exchange_weak(&max_active_processes, &maximum, active)) {}
    while (atomic_load(&hold_process)) thrd_sleep_ms(1);
    uint64_t busy_until = get_time_us() + (uint64_t)atomic_load(&busy_ms) * 1000u;
    volatile uint64_t arithmetic = 1;
    while (get_time_us() < busy_until) arithmetic = arithmetic * 1664525u + 1013904223u;
    (void)arithmetic;
    atomic_store(&latest_first_sample, first);
    atomic_store(&latest_video_count,count);atomic_store(&latest_video_marker,(int)samples[0]);
    d->status.h_locked = d->status.v_locked = d->status.color_locked = true;
    uint64_t seed = atomic_exchange(&counter_seed, 0);
    if (seed) {
        d->status.hsync_count = seed - 625;
        d->status.vsync_count = d->status.fields_decoded = seed - 2;
    }
    d->status.frames_decoded++;
    d->status.fields_decoded += 2;
    d->status.hsync_count += 625;
    d->status.vsync_count += 2;
    d->status.vsync_candidates += 3;
    d->status.fields_accepted += 2;
    d->status.fields_rejected++;
    d->status.reject_parity++;
    d->status.color_losses++;
    snprintf(d->status.reason, sizeof(d->status.reason), "Test frame");
    uint8_t rgba[8 * 6 * 4];
    int height = d->format == GUI_RF_PAL ? 6 : 4;
    /* Different values on alternating rows catch accidental bob/copy tearing. */
    for (int y = 0; y < height; y++) {
        memset(rgba + y * 8 * 4, (uint8_t)((int)samples[0] + (y & 1)), 8 * 4);
    }
    uint64_t public_end=first+count;
    callback(context, rgba, 8, height, public_end, &d->status);
    atomic_fetch_add(&process_calls, 1);
    atomic_fetch_sub(&active_processes, 1);
}

gui_rf_resampler_t *gui_rf_resampler_create_rate(uint32_t rate,uint32_t output_rate) {
    atomic_fetch_add(&create_calls, 1);
    if (rate == 28636360u) return NULL; /* No soxr at this unsupported fixture ratio. */
    gui_rf_resampler_t *r=calloc(1,sizeof(*r));
    if(r){r->rate=rate;r->output_rate=output_rate;r->first=true;}
    return r;
}
gui_rf_resampler_t *gui_rf_resampler_create(uint32_t rate) {
    return gui_rf_resampler_create_rate(rate,GUI_RF_DECODE_RATE_HZ);
}
gui_rf_resampler_t *gui_rf_resampler_create_ed_beta(uint32_t rate) {
    gui_rf_resampler_t *r=gui_rf_resampler_create_rate(rate,GUI_RF_DECODE_RATE_HZ);
    if(r)r->ed_beta=true;
    return r;
}
void gui_rf_resampler_destroy(gui_rf_resampler_t *r) { free(r); }
void gui_rf_resampler_reset(gui_rf_resampler_t *r) {
    if(!r->ed_beta) atomic_fetch_add(&narrow_reset_calls,1);
    r->phase=0;r->pending=0;r->first=true;
}
size_t gui_rf_resampler_output_capacity_rate(uint32_t rate,uint32_t output_rate,size_t count) {
    return ((uint64_t)count*output_rate+rate-1)/rate+64;
}
size_t gui_rf_resampler_output_capacity(uint32_t rate,size_t count) {
    return gui_rf_resampler_output_capacity_rate(rate,GUI_RF_DECODE_RATE_HZ,count);
}
size_t gui_rf_resampler_output_capacity_ed_beta(uint32_t rate,size_t count) {
    return gui_rf_resampler_output_capacity_rate(rate,GUI_RF_DECODE_RATE_HZ,count);
}
bool gui_rf_resampler_process(gui_rf_resampler_t *r,const int16_t *input,size_t count,
                              float *output,size_t capacity,size_t *output_count) {
    atomic_store(&resample_entered,true);
    if(!r->ed_beta) atomic_fetch_add(&narrow_resample_calls,1);
    else atomic_fetch_add(&wide_resample_calls,1);
    while(atomic_load(&hold_resample))thrd_sleep_ms(1);
    uint64_t scaled=(uint64_t)count*r->output_rate+r->phase;
    size_t n=(size_t)(scaled/r->rate);r->phase=scaled%r->rate;
    bool withhold=r->rate==54000000u && r->first &&
        (r->ed_beta?atomic_load(&delay_wide_once):atomic_load(&delay_audio_once));
    r->first=false;
    float marker=(float)input[0]+(r->ed_beta?1000.0f:0.0f);
    if(withhold){r->pending=n;r->pending_marker=marker;*output_count=0;return true;}
    size_t pending=r->pending;n+=pending;r->pending=0;
    if(n>capacity)return false;
    for(size_t i=0;i<n;i++)output[i]=i<pending?r->pending_marker:marker;
    *output_count=n;return true;
}
/* Deliberately positive ED-only fixture delay: the real soxr converter is
 * aligned at zero, but this stub keeps worker delay accounting observable. */
#define TEST_ED_DELAY_SAMPLES 3.25
double gui_rf_resampler_delay_samples(const gui_rf_resampler_t *r) {
    return r->ed_beta?TEST_ED_DELAY_SAMPLES:r->rate==r->output_rate*2u?7.5:r->rate==r->output_rate*4u?11.25:0.0;
}

static void check(bool condition, const char *description) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", description); failures++; }
}

static bool wait_calls(unsigned int target) {
    uint64_t until = get_time_ms() + 3000;
    while (get_time_ms() < until) {
        if (atomic_load(&process_calls) >= target) return true;
        thrd_sleep_ms(1);
    }
    return false;
}

static bool wait_frame(gui_rf_preview_worker_t *w, uint8_t *frame, uint64_t *generation,
                       int *width, int *height, gui_rf_preview_stats_t *stats) {
    uint64_t until = get_time_ms() + 3000;
    while (get_time_ms() < until) {
        if (gui_rf_preview_worker_take_frame(w, frame, GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4,
                                             generation, width, height, stats)) return true;
        thrd_sleep_ms(1);
    }
    return false;
}

static bool submit_marker(gui_rf_preview_worker_t *w, uint32_t rate, uint64_t offset,
                          uint64_t epoch, bool gap, int16_t marker) {
    int16_t samples[32] = {0};
    samples[0] = marker;
    gui_rf_block_info_t info = { rate, offset, epoch, gap };
    return gui_rf_preview_worker_submit(w, samples, 32, &info);
}

static void test_frames_and_commands(uint8_t *frame) {
    gui_rf_preview_worker_t *w = gui_rf_preview_worker_create(GUI_RF_PAL);
    check(w != NULL, "worker creation");
    if (!w) return;
    uint64_t generation = 0;
    int width = 0, height = 0;
    gui_rf_preview_stats_t stats;
    check(submit_marker(w, 40000000u, 0, 1, false, 17), "initial submit");
    check(wait_frame(w, frame, &generation, &width, &height, &stats), "first complete frame");
    check(width == 8 && height == 6 && frame[0] == 17 && frame[32] == 18,
          "copy retains distinct original field rows");
    check(stats.source_rate_hz == 40000000u && stats.decode.color_locked,
          "source metadata/status publication");
    check(stats.latency_ms >= 7.5 * 1000.0 / GUI_RF_DECODE_RATE_HZ,
          "latency includes uncompensated resampler group delay");
    check(!gui_rf_preview_worker_take_frame(w, frame, 4, &generation, NULL, NULL, NULL),
          "same generation is never delivered twice");

    unsigned int target = atomic_load(&process_calls) + 2;
    check(submit_marker(w, 40000000u, 32, 1, false, 19), "second submit");
    check(submit_marker(w, 40000000u, 64, 1, false, 21), "third submit");
    check(wait_calls(target), "newer frames processed");
    uint64_t unchanged_generation = generation;
    check(!gui_rf_preview_worker_take_frame(w, frame, 4, &generation, NULL, NULL, NULL) &&
          generation == unchanged_generation,
          "insufficient destination cannot consume a new generation");
    check(wait_frame(w, frame, &generation, &width, &height, &stats) && frame[0] == 21,
          "take returns newest complete frame");

    unsigned int before_reset = atomic_load(&reset_calls);
    check(submit_marker(w, 40000000u, 100000, 1, false, 23), "unannounced source gap submit");
    check(wait_frame(w, frame, &generation, &width, &height, &stats), "gap reacquisition frame");
    check(atomic_load(&reset_calls) > before_reset && atomic_load(&latest_first_sample) == 50000,
          "offset gap resets decoder and starts corrected internal timebase");

    gui_rf_preview_worker_set_format(w, GUI_RF_NTSC);
    uint64_t probe_generation = 0;
    check(!gui_rf_preview_worker_take_frame(w, frame, GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4,
                                           &probe_generation, NULL, NULL, NULL),
          "format request immediately invalidates old frame");
    check(submit_marker(w, 80000000u, 0, 2, true, 25), "new rate/epoch submit");
    check(wait_frame(w, frame, &generation, &width, &height, &stats) && height == 4 && frame[0] == 25,
          "worker applies requested format and new source rate");
    check(stats.source_rate_hz == 80000000u && atomic_load(&latest_first_sample) == 0,
          "new epoch does not retain old output sample cursor");

    gui_rf_preview_worker_reset(w);
    probe_generation = 0;
    check(!gui_rf_preview_worker_take_frame(w, frame, GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4,
                                           &probe_generation, NULL, NULL, NULL),
          "explicit reset immediately invalidates published frame");
    check(submit_marker(w, 20000000u, 0, 3, false, 27), "20 MSPS epoch submit");
    check(wait_frame(w, frame, &generation, &width, &height, &stats) && frame[0] == 27,
          "20 MSPS recovers after explicit reset");
    gui_rf_preview_worker_destroy(w);
}

static void test_unsupported_rate(void) {
    gui_rf_preview_worker_t *w = gui_rf_preview_worker_create(GUI_RF_PAL);
    check(w != NULL, "unsupported-rate worker creation");
    if (!w) return;
    unsigned int before = atomic_load(&create_calls);
    for (unsigned int i = 0; i < 12; i++)
        check(submit_marker(w, 28636360u, i * 32u, 4, false, 29), "unsupported-ratio block queued");
    gui_rf_preview_stats_t stats;
    uint64_t until = get_time_ms() + 3000;
    do {
        gui_rf_preview_worker_get_stats(w, &stats);
        if (strstr(stats.decode.reason, "Unsupported")) break;
        thrd_sleep_ms(1);
    } while (get_time_ms() < until);
    thrd_sleep_ms(20);
    check(strstr(stats.decode.reason, "Unsupported") != NULL, "unsupported rate reported");
    check(atomic_load(&create_calls) == before + 1, "unsupported ratio is configured once, never retried per block");
    unsigned int attempts = atomic_load(&create_calls);
    check(submit_marker(w, 8000000u, 0, 6, true, 29), "below-minimum rate metadata queued");
    until = get_time_ms() + 3000;
    do {
        gui_rf_preview_worker_get_stats(w, &stats);
        if (stats.source_rate_hz == 8000000u) break;
        thrd_sleep_ms(1);
    } while (get_time_ms() < until);
    check(stats.source_rate_hz == 8000000u && strstr(stats.decode.reason, "Unsupported") &&
          atomic_load(&create_calls) == attempts,
          "below-minimum source rate is reported without creating a resampler");
    gui_rf_preview_worker_destroy(w);
}

static void test_queue_overload(uint8_t *frame) {
    gui_rf_preview_worker_t *w = gui_rf_preview_worker_create(GUI_RF_PAL);
    check(w != NULL, "overflow worker creation");
    if (!w) return;
    int16_t *samples = calloc(65536, sizeof(*samples));
    check(samples != NULL, "overflow fixture allocation");
    if (!samples) { gui_rf_preview_worker_destroy(w); return; }
    samples[0] = 31;
    atomic_store(&process_entered, false);
    atomic_store(&hold_process, true);
    gui_rf_block_info_t info = { 20000000u, 0, 5, false };
    check(gui_rf_preview_worker_submit(w, samples, 65536, &info), "blocking fixture initial submit");
    uint64_t until = get_time_ms() + 3000;
    while (!atomic_load(&process_entered) && get_time_ms() < until) thrd_sleep_ms(1);
    check(atomic_load(&process_entered), "worker reached blocked test decoder");
    check(!gui_rf_preview_worker_wait_idle(w, 0),
          "idle includes in-flight decoder work, even with no later slots");
    unsigned int accepted = 0, rejected = 0;
    uint64_t start = get_time_ms();
    for (unsigned int i = 1; i <= 100; i++) {
        info.start_sample = (uint64_t)i * 65536;
        if (gui_rf_preview_worker_submit(w, samples, 65536, &info)) accepted++; else rejected++;
    }
    check(get_time_ms() - start < 1000, "submit completes while decoder is deliberately blocked");
    check(rejected > 0 && accepted < 64, "input-duration bound limits 20 MSPS queue to less than 200 ms");
    atomic_store(&hold_process, false);
    check(gui_rf_preview_worker_wait_idle(w, 3000), "idle waits until in-flight work and old queue finish");
    gui_rf_preview_stats_t stats;
    until = get_time_ms() + 3000;
    do {
        gui_rf_preview_worker_get_stats(w, &stats);
        if (stats.blocks_dropped >= accepted + rejected) break;
        thrd_sleep_ms(1);
    } while (get_time_ms() < until);
    check(stats.blocks_dropped >= accepted + rejected, "overflow rejects current block and discards old queued blocks");
    uint64_t generation = 0;
    int width, height;
    info.start_sample = 101u * 65536u;
    samples[0] = 33;
    until = get_time_ms() + 3000;
    bool submitted = false;
    do {
        submitted = gui_rf_preview_worker_submit(w, samples, 65536, &info);
        if (submitted) break;
        thrd_sleep_ms(1);
    } while (get_time_ms() < until);
    /* Reuse producer memory immediately: worker must own a complete copy. */
    memset(samples, 0, 65536 * sizeof(*samples));
    check(submitted && wait_frame(w, frame, &generation, &width, &height, &stats) && frame[0] == 33,
          "fresh frame after overflow owns copied input and excludes stale queued data");
    free(samples);
    gui_rf_preview_worker_destroy(w);
}

static bool wait_budget(gui_rf_preview_worker_t *w) {
    uint64_t until = get_time_ms() + 3000;
    do {
        gui_rf_preview_stats_t stats;
        gui_rf_preview_worker_get_stats(w, &stats);
        if (stats.budget_waiting) return true;
        thrd_sleep_ms(1);
    } while (get_time_ms() < until);
    return false;
}

static void test_pause_and_counters(uint8_t *frame) {
    check(gui_rf_preview_worker_set_budget(0), "offline budget configuration");
    gui_rf_preview_worker_t *w = gui_rf_preview_worker_create(GUI_RF_PAL);
    check(w != NULL, "pause worker creation");
    if (!w) return;
    uint64_t generation = 0;
    int width = 0, height = 0;
    gui_rf_preview_stats_t stats;
    check(submit_marker(w, 20000000u, 0, 1, false, 41), "initial frozen-frame fixture");
    check(gui_rf_preview_worker_wait_idle(w, 3000) &&
          wait_frame(w, frame, &generation, &width, &height, &stats), "initial frame published");
    check(stats.total_frames == 1 && stats.total_fields == 2 && stats.total_hsyncs == 625 &&
          stats.total_fields_accepted == 2 && stats.total_fields_rejected == 1 &&
          stats.total_reject_parity == 1 && stats.total_color_losses == 1,
          "frame and diagnostic counters accumulate without callback/snapshot double count");

    atomic_store(&process_entered, false);
    atomic_store(&hold_process, true);
    check(submit_marker(w, 20000000u, 32, 1, false, 43), "in-flight pause fixture");
    uint64_t until = get_time_ms() + 3000;
    while (!atomic_load(&process_entered) && get_time_ms() < until) thrd_sleep_ms(1);
    check(atomic_load(&process_entered), "pause fixture entered decoder");
    for (unsigned int i = 2; i < 7; i++)
        check(submit_marker(w, 20000000u, i * 32u, 1, false, 45), "queued pause fixture");
    gui_rf_preview_worker_set_paused(w, true);
    check(!gui_rf_preview_worker_submit(w, NULL, 65536, NULL),
          "pause rejects before touching payload or metadata");
    atomic_store(&hold_process, false);
    check(gui_rf_preview_worker_wait_idle(w, 3000), "pause drains queue including in-flight block");
    uint64_t fresh_view_generation = 0;
    check(gui_rf_preview_worker_take_frame(w, frame, GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4,
                                           &fresh_view_generation, &width, &height, &stats) && frame[0] == 41,
          "paused fresh UI viewer can copy original frozen generation");
    check(stats.paused && stats.pause_drops == 6 && stats.blocks_dropped == 0 && stats.total_frames == 2,
          "pause drops separate from queue drops, completed in-flight decode counted");
    check(stats.total_fields == 4 && stats.total_fields_rejected == 2,
          "pause reset preserves diagnostic session totals");

    gui_rf_preview_worker_set_paused(w, false);
    int16_t samples[32] = {47};
    gui_rf_block_info_t info = {20000000u, 7000, 1, false};
    check(gui_rf_preview_worker_submit(w, samples, 32, &info), "immediate resume submission");
    memset(samples, 0, sizeof(samples));
    check(gui_rf_preview_worker_wait_idle(w, 3000) &&
          wait_frame(w, frame, &generation, &width, &height, &stats) && frame[0] == 47,
          "resume retains new epoch payload ownership and reacquires");
    check(!stats.paused && stats.total_frames == 3 && stats.total_fields == 6 &&
          stats.total_fields_accepted == 6 && stats.total_vsync_candidates == 9,
          "resume reset does not erase cumulative counters");
    gui_rf_preview_worker_reset(w);
    check(submit_marker(w, 20000000u, 0, 2, false, 49), "explicit-reset counter fixture");
    check(gui_rf_preview_worker_wait_idle(w, 3000), "counter fixture drained");
    gui_rf_preview_worker_get_stats(w, &stats);
    check(stats.total_frames == 4 && stats.total_fields == 8 && stats.total_hsyncs == 2500 &&
          stats.total_reject_parity == 4, "explicit reset retains all session totals");
    thrd_sleep_ms(1100);
    gui_rf_preview_worker_get_stats(w, &stats);
    check(stats.fps > 0 && stats.fps < 4.0, "FPS uses actual frame count over elapsed interval");
    thrd_sleep_ms(1100);
    gui_rf_preview_worker_get_stats(w, &stats);
    check(stats.fps == 0.0, "quiet decode interval reports zero FPS");
    check(stats.priority_lowered, "worker explicitly lowers its own scheduling priority");
    gui_rf_preview_worker_destroy(w);
}

static void test_shared_budget_and_interrupt(void) {
    check(gui_rf_preview_worker_set_budget(1), "restore default shared budget");
    atomic_store(&busy_ms, 50);
    atomic_store(&max_active_processes, 0);
    gui_rf_preview_worker_t *a = gui_rf_preview_worker_create(GUI_RF_PAL);
    gui_rf_preview_worker_t *b = gui_rf_preview_worker_create(GUI_RF_NTSC);
    check(a && b, "shared-budget workers created");
    if (a && b) {
        check(submit_marker(a, 20000000u, 0, 1, false, 51) &&
              submit_marker(b, 20000000u, 0, 1, false, 53), "two workers submit independently");
        check(gui_rf_preview_worker_wait_idle(a, 3000) && gui_rf_preview_worker_wait_idle(b, 3000),
              "both shared-budget workers finish");
        check(atomic_load(&max_active_processes) == 1,
              "two workers cannot multiply the shared active CPU permit");
        gui_rf_preview_stats_t sa, sb;
        gui_rf_preview_worker_get_stats(a, &sa);
        gui_rf_preview_worker_get_stats(b, &sb);
        check(sa.processing_cpu_ms + sb.processing_cpu_ms > 20 &&
              sa.processing_wall_ms + sb.processing_wall_ms > 50,
              "thread CPU and processing wall costs are published");
    }
    gui_rf_preview_worker_destroy(a);
    gui_rf_preview_worker_destroy(b);

    check(gui_rf_preview_worker_set_budget(.05), "strict budget wait fixture");
    atomic_store(&busy_ms, 80);
    gui_rf_preview_worker_t *w = gui_rf_preview_worker_create(GUI_RF_PAL);
    check(w != NULL, "budget-stop worker created");
    if (w) {
        check(submit_marker(w, 20000000u, 0, 1, false, 55) &&
              submit_marker(w, 20000000u, 32, 1, false, 57), "budget-wait fixture input");
        check(wait_budget(w), "worker enters shared CPU-debt wait");
        uint64_t begin = get_time_ms();
        gui_rf_preview_worker_set_paused(w, true);
        check(gui_rf_preview_worker_wait_idle(w, 1000), "pause interrupts budget wait and drains input");
        check(get_time_ms() - begin < 500, "pause is bounded independently of long CPU debt");
        gui_rf_preview_stats_t stats;
        gui_rf_preview_worker_get_stats(w, &stats);
        check(stats.paused && !stats.budget_waiting && stats.pause_drops >= 1,
              "pause settles wait state and dropped queue");
        gui_rf_preview_worker_set_paused(w, false);
        check(submit_marker(w, 20000000u, 64, 1, false, 59), "resume into existing global debt");
        check(wait_budget(w), "resume respects existing shared CPU debt");
        begin = get_time_ms();
        gui_rf_preview_worker_destroy(w);
        check(get_time_ms() - begin < 500, "destroy interrupts budget wait without waiting for debt");
    }
    atomic_store(&busy_ms, 0);
    check(gui_rf_preview_worker_set_budget(1), "restore default budget after tests");
    w = gui_rf_preview_worker_create(GUI_RF_PAL);
    check(w != NULL, "worker after stopped waiter created");
    if (w) {
        check(submit_marker(w, 20000000u, 0, 1, false, 61) &&
              gui_rf_preview_worker_wait_idle(w, 1000),
              "destroy cancels FIFO waiter rather than blocking future workers");
        gui_rf_preview_worker_destroy(w);
    }

    check(gui_rf_preview_worker_set_budget(.05), "control-wait fixture budget");
    atomic_store(&busy_ms, 80);
    w = gui_rf_preview_worker_create(GUI_RF_PAL);
    check(w != NULL, "control-wait worker created");
    if (w) {
        check(submit_marker(w, 20000000u, 0, 1, false, 63) &&
              submit_marker(w, 20000000u, 32, 1, false, 65), "control-wait fixture input");
        check(wait_budget(w), "control fixture enters budget wait");
        uint64_t begin = get_time_ms();
        gui_rf_preview_worker_set_format(w, GUI_RF_NTSC);
        check(gui_rf_preview_worker_wait_idle(w, 1000) && get_time_ms() - begin < 500,
              "format control interrupts budget wait and releases obsolete queued slot");
        gui_rf_preview_stats_t stats;
        gui_rf_preview_worker_get_stats(w, &stats);
        check(stats.total_frames == 1 && stats.total_fields == 2 && !stats.budget_waiting &&
              stats.blocks_dropped == 1, "format reset preserves totals and cancels budget ownership");
        check(gui_rf_preview_worker_set_budget(0), "unpace offline recovery after control");
        atomic_store(&busy_ms, 0);
        check(submit_marker(w, 20000000u, 64, 1, false, 67) &&
              gui_rf_preview_worker_wait_idle(w, 1000), "new format resumes after cancelled wait");
        gui_rf_preview_worker_get_stats(w, &stats);
        check(stats.total_frames == 2 && stats.total_fields == 4, "post-control counter recovery");
        gui_rf_preview_worker_destroy(w);
    }
    atomic_store(&busy_ms, 0);
    check(gui_rf_preview_worker_set_budget(1), "final default budget restoration");
}

static void test_long_running_counters(void) {
    /* Seed the DSP substitute near both 32-bit limits, then use ordinary
     * callbacks and the production worker's accumulation/reset paths. */
    const uint64_t boundaries[] = {INT32_MAX, UINT32_MAX};
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        uint64_t boundary = boundaries[i];
        gui_rf_preview_worker_t *w = gui_rf_preview_worker_create(GUI_RF_PAL);
        check(w != NULL, "long-running counter worker created");
        if (!w) continue;
        atomic_store(&counter_seed, boundary);
        gui_rf_preview_stats_t stats;
        for (unsigned int block = 0; block < 2; block++) {
            check(submit_marker(w, 20000000u, block * 32u, 1, false, 77) &&
                  gui_rf_preview_worker_wait_idle(w, 1000), "counter boundary input drains");
            gui_rf_preview_worker_get_stats(w, &stats);
            uint64_t hsyncs = boundary + block * UINT64_C(625);
            uint64_t fields = boundary + block * UINT64_C(2);
            check(stats.decode.hsync_count == hsyncs && stats.decode.vsync_count == fields &&
                  stats.decode.fields_decoded == fields, "decode counters cross 32-bit limits exactly");
            check(stats.total_hsyncs == hsyncs && stats.total_vsyncs == fields &&
                  stats.total_fields == fields && stats.total_frames == block + 1u,
                  "callback and status snapshot accumulate large counts only once");
        }
        check(submit_marker(w, 20000000u, 64, 2, true, 79) &&
              gui_rf_preview_worker_wait_idle(w, 1000), "counter boundary epoch reset drains");
        gui_rf_preview_worker_get_stats(w, &stats);
        check(stats.decode.hsync_count == 625 && stats.decode.vsync_count == 2 &&
              stats.decode.fields_decoded == 2, "new epoch starts fresh decode counters");
        check(stats.total_hsyncs == boundary + 1250 && stats.total_vsyncs == boundary + 4 &&
              stats.total_fields == boundary + 4 && stats.total_frames == 3,
              "epoch reset preserves large session totals without recounting");
        gui_rf_preview_worker_destroy(w);
        atomic_store(&counter_seed, 0);
    }
}

static void test_measured_shared_budget(void) {
    check(gui_rf_preview_worker_set_budget(.5), "measured half-core budget");
    atomic_store(&busy_ms, 20);
    gui_rf_preview_worker_t *workers[2] = {
        gui_rf_preview_worker_create(GUI_RF_PAL), gui_rf_preview_worker_create(GUI_RF_NTSC)
    };
    check(workers[0] && workers[1], "measured shared-budget workers created");
    if (workers[0] && workers[1]) {
        uint64_t offset[2] = {0, 0};
        uint64_t start = get_time_us();
        while (get_time_us() - start < 2500000u) {
            for (unsigned int i = 0; i < 2; i++) {
                if (!gui_rf_preview_worker_wait_idle(workers[i], 0)) continue;
                if (submit_marker(workers[i], 20000000u, offset[i], 1, false, 71)) offset[i] += 32;
            }
            thrd_sleep_ms(1);
        }
        check(gui_rf_preview_worker_wait_idle(workers[0], 2000) &&
              gui_rf_preview_worker_wait_idle(workers[1], 2000), "measured budget fixtures drain");
        double wall_ms = (double)(get_time_us() - start) / 1000.0;
        gui_rf_preview_stats_t a, b;
        gui_rf_preview_worker_get_stats(workers[0], &a);
        gui_rf_preview_worker_get_stats(workers[1], &b);
        double cpu_ms = a.processing_cpu_ms + b.processing_cpu_ms;
        double ratio = cpu_ms / wall_ms;
        printf("Shared 0.5-core budget: %.1f ms wall, %.1f ms thread CPU, %.3f cores, %llu/%llu frames\n",
               wall_ms, cpu_ms, ratio, (unsigned long long)a.total_frames,
               (unsigned long long)b.total_frames);
        check(a.total_frames >= 4 && b.total_frames >= 4,
              "both continuous sources progress under FIFO shared budget");
        /* Soft pacing, OS bookkeeping and final in-flight block are deliberately
         * tolerated. This is a bounded software scheduling regression, not a
         * hardware capture guarantee or proof of a hard instantaneous quota. */
        check(ratio <= .62, "multiple workers share half-core long-interval CPU allowance");
        check(a.budget_wait_ms + b.budget_wait_ms > 500,
              "measured work is actually paced rather than silently unbudgeted");
    }
    gui_rf_preview_worker_destroy(workers[0]);
    gui_rf_preview_worker_destroy(workers[1]);
    atomic_store(&busy_ms, 0);
    check(gui_rf_preview_worker_set_budget(1), "measured test restores default budget");
}

static void test_fast_block_handoffs(void) {
    check(gui_rf_preview_worker_set_budget(1), "fast-block handoff budget");
    atomic_store(&busy_ms, 0);
    gui_rf_preview_worker_t *a = gui_rf_preview_worker_create(GUI_RF_PAL);
    gui_rf_preview_worker_t *b = gui_rf_preview_worker_create(GUI_RF_NTSC);
    check(a && b, "fast-block workers created");
    if (a && b) {
        atomic_store(&process_entered, false);
        atomic_store(&hold_process, true);
        check(submit_marker(a, 20000000u, 0, 1, false, 73), "fast-block active worker fixture");
        uint64_t until = get_time_ms() + 3000;
        while (!atomic_load(&process_entered) && get_time_ms() < until) thrd_sleep_ms(1);
        check(atomic_load(&process_entered), "first fast-block worker owns permit");
        check(submit_marker(b, 20000000u, 0, 1, false, 75) && wait_budget(b),
              "second fast-block worker joins FIFO before handoff");
        for (unsigned int i = 1; i < 32; i++) {
            check(submit_marker(a, 20000000u, i * 32u, 1, false, 73), "queued fast-block A payload");
            check(submit_marker(b, 20000000u, i * 32u, 1, false, 75), "queued fast-block B payload");
        }
        /* No subsequent input events are supplied during drain. Contenders
         * must progress on permit handoff, not depend on a future input block.
         * This is an inexpensive synthetic queue/lifecycle regression, not a
         * real RF throughput or guaranteed scheduling latency benchmark. */
        thrd_sleep_ms(20);
        atomic_store(&hold_process, false);
        check(gui_rf_preview_worker_wait_idle(a, 3000) &&
              gui_rf_preview_worker_wait_idle(b, 3000), "fast FIFO handoffs drain without new input");
        gui_rf_preview_stats_t sa, sb;
        gui_rf_preview_worker_get_stats(a, &sa);
        gui_rf_preview_worker_get_stats(b, &sb);
        check(sa.total_frames == 32 && sb.total_frames == 32 &&
              sa.blocks_dropped == 0 && sb.blocks_dropped == 0,
              "both fast-block queues progress without stale-age loss at permit handoff");
    }
    atomic_store(&hold_process, false);
    gui_rf_preview_worker_destroy(a);
    gui_rf_preview_worker_destroy(b);
}

static void test_parallel_budget(void) {
    check(gui_rf_preview_worker_set_budget(2),"parallel workers budget");
    atomic_store(&busy_ms,0);atomic_store(&hold_process,true);atomic_store(&active_processes,0);
    gui_rf_preview_worker_t *a=gui_rf_preview_worker_create(GUI_RF_PAL),*b=gui_rf_preview_worker_create(GUI_RF_NTSC);
    check(a && b,"parallel fixture workers");
    if(a && b) {
        check(submit_marker(a,20000000u,0,1,false,1) && submit_marker(b,20000000u,0,1,false,2),"parallel fixture input");
        uint64_t until=get_time_ms()+3000;while(atomic_load(&active_processes)<2 && get_time_ms()<until) thrd_sleep_ms(1);
        check(atomic_load(&active_processes)==2,"two-core budget really runs two video consumers");
    }
    atomic_store(&hold_process,false);
    check((!a || gui_rf_preview_worker_wait_idle(a,3000)) && (!b || gui_rf_preview_worker_wait_idle(b,3000)),"parallel fixtures drain");
    gui_rf_preview_worker_destroy(a);gui_rf_preview_worker_destroy(b);
}
static void test_independent_audio(gui_rf_profile_t profile) {
    check(gui_rf_preview_worker_set_budget(0),"independent audio offline budget");
    gui_rf_preview_worker_t *w=gui_rf_preview_worker_create_profile(profile);
    check(w!=NULL,"HiFi worker has independent consumer");if(!w) return;
    gui_rf_preview_worker_set_audio_callback(w,audio_test_callback,NULL);
    unsigned int initial_calls=atomic_load(&audio_calls);
    for(unsigned int i=0;i<8;i++) check(submit_marker(w,20000000u,i*32u,1,false,80),"initial file-like eight-block batch");
    check(gui_rf_preview_worker_wait_idle(w,3000) && atomic_load(&audio_calls)==initial_calls+8,
          "first stream-boundary reset preserves every legal prequeued audio block");
    atomic_store(&audio_entered,false);atomic_store(&hold_audio,true);
    unsigned int video_before=atomic_load(&process_calls),callbacks=atomic_load(&audio_callbacks);
    check(submit_marker(w,20000000u,256,1,false,81),"independent audio input");
    uint64_t until=get_time_ms()+3000;while(!atomic_load(&audio_entered) && get_time_ms()<until) thrd_sleep_ms(1);
    check(atomic_load(&audio_entered) && wait_calls(video_before+1),"video completes while audio consumer remains blocked");
    check(!gui_rf_preview_worker_wait_idle(w,0),"wait_idle includes in-flight audio");
    gui_rf_preview_worker_set_audio_enabled(w,false);atomic_store(&hold_audio,false);
    check(gui_rf_preview_worker_wait_idle(w,3000) && atomic_load(&audio_callbacks)==callbacks,"disable suppresses in-flight old generation PCM");
    gui_rf_preview_worker_set_audio_enabled(w,true);
    check(submit_marker(w,20000000u,288,1,false,83) && gui_rf_preview_worker_wait_idle(w,3000),"audio restarts at next copied block");
    check(atomic_load(&audio_marker)==83 && atomic_load(&audio_first)==288 && atomic_load(&audio_end)==320,"audio copied payload and 20MSPS timeline remain exact");
    /* A disable/enable between resample entry and enqueue must not retag the
     * old payload with the new audio generation. */
    atomic_store(&resample_entered,false);atomic_store(&hold_resample,true);
    callbacks=atomic_load(&audio_callbacks);
    check(submit_marker(w,20000000u,320,1,false,84),"inflight resample generation fixture");
    until=get_time_ms()+3000;while(!atomic_load(&resample_entered) && get_time_ms()<until) thrd_sleep_ms(1);
    check(atomic_load(&resample_entered),"old block begins resampling");
    gui_rf_preview_worker_set_audio_enabled(w,false);gui_rf_preview_worker_set_audio_enabled(w,true);
    atomic_store(&hold_resample,false);
    check(gui_rf_preview_worker_wait_idle(w,3000) && atomic_load(&audio_callbacks)==callbacks,"reenable cannot relabel inflight old RF audio");
    int16_t *wide=calloc(65536,sizeof(*wide));check(wide!=NULL,"maximum-ratio audio input allocation");
    if(wide) {
        wide[0]=89;unsigned int audio_before=atomic_load(&audio_calls);
        gui_rf_block_info_t info={16000000u,0,2,true};
        check(gui_rf_preview_worker_submit(w,wide,65536,&info),"16MSPS maximum RF block submits");free(wide);
        check(gui_rf_preview_worker_wait_idle(w,3000),"maximum-ratio audio split drains");
        check(atomic_load(&audio_calls)==audio_before+3 && atomic_load(&audio_first)==65536 &&
              atomic_load(&audio_end)==81920 && atomic_load(&audio_marker)==89,"81920 resampled RF samples split into exact 32768/32768/16384 audio blocks");
    }
    atomic_store(&audio_entered,false);atomic_store(&hold_audio,true);
    check(submit_marker(w,20000000u,96,3,true,85),"audio overload initial input");
    until=get_time_ms()+3000;while(!atomic_load(&audio_entered) && get_time_ms()<until) thrd_sleep_ms(1);
    video_before=atomic_load(&process_calls);
    for(unsigned int i=0;i<40;i++) {
        check(submit_marker(w,20000000u,128+i*32u,3,false,87),"audio overload input");
        check(wait_calls(video_before+i+1),"audio queue pressure never stalls video");
    }
    gui_rf_preview_stats_t stats;gui_rf_preview_worker_get_stats(w,&stats);
    check(stats.audio_blocks_dropped>0 && stats.audio_active,"bounded audio queue reports its independent overload");
    callbacks=atomic_load(&audio_callbacks);atomic_store(&hold_audio,false);
    check(gui_rf_preview_worker_wait_idle(w,3000),"audio overload drains with re-calibration");
    check(atomic_load(&audio_callbacks)>=callbacks,"post-overload consumer safely settles");
    /* Hold an old video block and its old audio, queue more old RF, then
     * submit a seek. No queued/inflight old RF may claim the new source epoch. */
    atomic_store(&hold_process,true);atomic_store(&process_entered,false);
    atomic_store(&hold_audio,true);atomic_store(&audio_entered,false);
    check(submit_marker(w,20000000u,1408,3,false,91),"seek old inflight fixture");
    until=get_time_ms()+3000;while((!atomic_load(&process_entered) || !atomic_load(&audio_entered)) && get_time_ms()<until) thrd_sleep_ms(1);
    check(atomic_load(&process_entered) && atomic_load(&audio_entered),"seek fixture holds both old consumers");
    for(unsigned int i=0;i<4;i++) check(submit_marker(w,20000000u,1440+i*32u,3,false,93),"seek queues old RF");
    callbacks=atomic_load(&audio_callbacks);atomic_store(&expected_audio_marker,99);atomic_store(&wrong_audio_callbacks,0);
    check(submit_marker(w,20000000u,0,4,true,99),"seek publishes new source generation immediately");
    atomic_store(&hold_process,false);atomic_store(&hold_audio,false);
    check(gui_rf_preview_worker_wait_idle(w,3000) && atomic_load(&wrong_audio_callbacks)==0 &&
          atomic_load(&audio_callbacks)==callbacks+1,"seek suppresses inflight and queued old RF PCM without relabeling");
    atomic_store(&expected_audio_marker,0);
    gui_rf_preview_worker_set_audio_enabled(w,false);
    check(gui_rf_preview_worker_wait_idle(w,3000),"disabled audio flush callback settles");
    gui_rf_preview_worker_get_stats(w,&stats);
    check(!stats.audio_enabled && !stats.audio_active && stats.processing_cpu_ms>=stats.audio_cpu_ms &&
          stats.processing_wall_ms>=stats.audio_wall_ms,"aggregate CPU/wall includes audio and reports disabled state");
    gui_rf_preview_worker_destroy(w);check(gui_rf_preview_worker_set_budget(1),"audio fixture restores default budget");
}

static void test_superbeta_optional_audio(void) {
    check(gui_rf_preview_worker_set_budget(0),"SuperBeta profile fixture offline budget");
    gui_rf_preview_worker_t *w=gui_rf_preview_worker_create_profile(GUI_RF_PROFILE_NTSC_SUPERBETA);
    check(w!=NULL,"video-only SuperBeta worker created");if(!w) return;
    gui_rf_preview_worker_set_audio_callback(w,audio_test_callback,NULL);
    unsigned int calls=atomic_load(&audio_calls),video=atomic_load(&process_calls);
    check(submit_marker(w,20000000u,0,1,false,101) && gui_rf_preview_worker_wait_idle(w,3000),
          "video-only SuperBeta processes RF");
    check(atomic_load(&audio_calls)==calls && atomic_load(&process_calls)==video+1,
          "SuperBeta without HiFi performs zero audio DSP while audio flag defaults true");
    gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI);
    check(gui_rf_preview_worker_wait_idle(w,3000),"SuperBeta optional HiFi profile applies");
    check(submit_marker(w,20000000u,32,1,false,103) && gui_rf_preview_worker_wait_idle(w,3000),
          "SuperBeta HiFi starts its audio consumer after profile transition");
    check(atomic_load(&audio_calls)==calls+1 && atomic_load(&audio_marker)==103,
          "SuperBeta HiFi decodes enabled audio from its current RF block");
    gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_SUPERBETA);
    check(gui_rf_preview_worker_wait_idle(w,3000),"SuperBeta optional HiFi removal applies");
    check(submit_marker(w,20000000u,64,1,false,105) && gui_rf_preview_worker_wait_idle(w,3000),
          "video-only SuperBeta continues after removing optional HiFi");
    gui_rf_preview_stats_t stats;gui_rf_preview_worker_get_stats(w,&stats);
    check(atomic_load(&audio_calls)==calls+1 && stats.audio_enabled && !stats.audio_active,
          "removing SuperBeta HiFi stops audio independently of the monitor flag");
    gui_rf_preview_worker_destroy(w);check(gui_rf_preview_worker_set_budget(1),"SuperBeta fixture restores default budget");
}

/* The wide RF front-end is necessary for S-VHS even though its picture
 * decoder retains the VHS path. Check the actual worker's converter choice,
 * clock and profile transitions independently of the physical RF tests. */
static void test_svhs_frontend(void) {
    check(gui_rf_preview_worker_set_budget(0),"S-VHS offline budget");
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS};
    for(size_t p=0;p<sizeof(profiles)/sizeof(profiles[0]);++p) {
        gui_rf_preview_worker_t *w=gui_rf_preview_worker_create_profile(profiles[p]);
        check(w!=NULL,"S-VHS worker creation");if(!w)continue;
        gui_rf_preview_worker_set_audio_callback(w,audio_test_callback,NULL);
        unsigned video=atomic_load(&process_calls),audio=atomic_load(&audio_calls);
        unsigned narrow=atomic_load(&narrow_resample_calls);
        check(submit_marker(w,16000000u,0,1,false,141) && gui_rf_preview_worker_wait_idle(w,3000),
            "insufficient S-VHS source rate drains safely");
        gui_rf_preview_stats_t stats;gui_rf_preview_worker_get_stats(w,&stats);
        check(strstr(stats.decode.reason,"S-VHS") && strstr(stats.decode.reason,"20 MSPS") &&
            atomic_load(&process_calls)==video,"S-VHS rejects missing source bandwidth");
        check(submit_marker(w,20000000u,0,2,true,143) && gui_rf_preview_worker_wait_idle(w,3000),
            "native 20MSPS S-VHS accepted");
        check(atomic_load(&latest_first_sample)==0 && atomic_load(&latest_video_count)==32 &&
            atomic_load(&latest_video_marker)==1143,"native S-VHS uses the wide video stream on the 20M clock");
        check(submit_marker(w,40000000u,4000,3,true,145) && gui_rf_preview_worker_wait_idle(w,3000),
            "40MSPS S-VHS accepted");
        check(atomic_load(&latest_first_sample)==2000 && atomic_load(&latest_video_count)==16 &&
            atomic_load(&latest_video_marker)==1145,"40MSPS S-VHS is downsampled to wide-passband 20MSPS");
        check(submit_marker(w,80000000u,8000,4,true,147) && gui_rf_preview_worker_wait_idle(w,3000),
            "80MSPS S-VHS accepted");
        check(atomic_load(&latest_first_sample)==2000 && atomic_load(&latest_video_count)==8 &&
            atomic_load(&latest_video_marker)==1147,"80MSPS S-VHS retains the 20M media clock");
        check(atomic_load(&audio_calls)==audio && atomic_load(&narrow_resample_calls)==narrow,
            "S-VHS performs no Beta audio DSP or unnecessary narrow-band conversion");
        unsigned created=atomic_load(&create_calls);
        gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_PAL_VHS);
        check(gui_rf_preview_worker_wait_idle(w,3000) &&
            submit_marker(w,80000000u,8032,4,false,149) && gui_rf_preview_worker_wait_idle(w,3000),
            "S-VHS to ordinary VHS at unchanged source rate");
        check(atomic_load(&latest_first_sample)==2008 && atomic_load(&latest_video_marker)==149 &&
            atomic_load(&create_calls)==created+1,"leaving S-VHS restores the ordinary converter response");
        created=atomic_load(&create_calls);
        gui_rf_preview_worker_set_profile(w,profiles[p]);
        check(gui_rf_preview_worker_wait_idle(w,3000) &&
            submit_marker(w,80000000u,8064,4,false,151) && gui_rf_preview_worker_wait_idle(w,3000),
            "ordinary VHS to S-VHS at unchanged source rate");
        check(atomic_load(&latest_first_sample)==2016 && atomic_load(&latest_video_marker)==1151 &&
            atomic_load(&create_calls)==created+2,"S-VHS reselects the wide converter despite equal output rates");
        created=atomic_load(&create_calls);
        gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_ED_BETA);
        check(gui_rf_preview_worker_wait_idle(w,3000) &&
            submit_marker(w,80000000u,8096,4,false,153) && gui_rf_preview_worker_wait_idle(w,3000),
            "S-VHS to ED starts the independent Hi-Fi consumer");
        check(atomic_load(&audio_calls)==audio+1 && atomic_load(&audio_marker)==153 &&
            atomic_load(&create_calls)==created,"ED reuses the wide converter and decodes current audio");
        audio=atomic_load(&audio_calls);narrow=atomic_load(&narrow_resample_calls);
        gui_rf_preview_worker_set_profile(w,profiles[p]);
        check(gui_rf_preview_worker_wait_idle(w,3000) &&
            submit_marker(w,80000000u,8128,4,false,155) && gui_rf_preview_worker_wait_idle(w,3000),
            "ED to S-VHS clears previous Beta audio state");
        gui_rf_preview_worker_get_stats(w,&stats);
        check(atomic_load(&latest_first_sample)==2032 && atomic_load(&latest_video_marker)==1155 &&
            atomic_load(&audio_calls)==audio && atomic_load(&narrow_resample_calls)==narrow &&
            atomic_load(&create_calls)==created && !stats.audio_active,
            "S-VHS reuses wide video without stale ED audio or narrow conversion");
        gui_rf_preview_worker_destroy(w);
    }
    check(gui_rf_preview_worker_set_budget(1),"S-VHS restores budget");
}

static void test_ed_frontends(void) {
    check(gui_rf_preview_worker_set_budget(0),"ED dual-front-end offline budget");
    gui_rf_preview_worker_t *w=gui_rf_preview_worker_create_profile(GUI_RF_PROFILE_NTSC_ED_BETA);
    check(w!=NULL,"ED worker creation");if(!w)return;
    gui_rf_preview_worker_set_audio_callback(w,audio_test_callback,NULL);
    unsigned video=atomic_load(&process_calls),audio=atomic_load(&audio_calls),created=atomic_load(&create_calls);
    check(submit_marker(w,16000000u,0,1,false,109) && gui_rf_preview_worker_wait_idle(w,3000),"low-rate ED input drains safely");
    gui_rf_preview_stats_t stats;gui_rf_preview_worker_get_stats(w,&stats);
    check(strstr(stats.decode.reason,"20 MSPS") && atomic_load(&process_calls)==video &&
        atomic_load(&audio_calls)==audio && atomic_load(&create_calls)==created,
        "ED rejects insufficient source bandwidth without video or audio upsampling");
    check(submit_marker(w,20000000u,0,1,true,109) && gui_rf_preview_worker_wait_idle(w,3000),"native20 MSPS ED source is accepted");
    check(atomic_load(&latest_first_sample)==0 && atomic_load(&latest_video_count)==32 &&
        atomic_load(&latest_video_marker)==1109 && atomic_load(&audio_marker)==109,
        "native20 ED retains distinct video and audio front-ends on the same sample clock");
    check(submit_marker(w,40000000u,4000,2,true,111) && gui_rf_preview_worker_wait_idle(w,3000),"ED 40 MSPS source");
    check(atomic_load(&latest_first_sample)==2000 && atomic_load(&latest_video_count)==16 &&
        atomic_load(&latest_video_marker)==1111,"ED video receives wide-band samples on the20 MSPS axis");
    if(atomic_load(&audio_first)!=2000 || atomic_load(&audio_count)!=16 || atomic_load(&audio_end)!=2016 ||
        atomic_load(&audio_marker)!=111) {
        gui_rf_preview_worker_get_stats(w,&stats);
        fprintf(stderr,"ED narrow observed first=%llu count=%llu end=%llu marker=%d audio_calls=%u drops=%llu\n",
            (unsigned long long)atomic_load(&audio_first),(unsigned long long)atomic_load(&audio_count),
            (unsigned long long)atomic_load(&audio_end),atomic_load(&audio_marker),atomic_load(&audio_calls),
            (unsigned long long)stats.audio_blocks_dropped);
    }
    check(atomic_load(&audio_first)==2000 && atomic_load(&audio_count)==16 && atomic_load(&audio_end)==2016 &&
        atomic_load(&audio_marker)==111,"ED Hi-Fi receives only the independent 20 MSPS samples and clock");
    check(submit_marker(w,40000000u,100000,3,true,113) && gui_rf_preview_worker_wait_idle(w,3000),"ED source epoch/gap");
    check(atomic_load(&latest_first_sample)==50000 && atomic_load(&audio_first)==50000,
        "ED source gap resets and reanchors both front-ends independently");
    created=atomic_load(&create_calls);audio=atomic_load(&audio_calls);
    gui_rf_preview_worker_set_audio_enabled(w,false);
    check(submit_marker(w,40000000u,100032,3,false,115) && gui_rf_preview_worker_wait_idle(w,3000),"ED video while audio muted");
    check(atomic_load(&latest_first_sample)==50016 && atomic_load(&audio_calls)==audio,"mute does not halt wide video");
    gui_rf_preview_worker_set_audio_enabled(w,true);
    check(submit_marker(w,40000000u,100064,3,false,117) && gui_rf_preview_worker_wait_idle(w,3000),"ED audio reenabled");
    check(atomic_load(&audio_first)==50032 && atomic_load(&latest_first_sample)==50032 &&
        atomic_load(&create_calls)==created,"unmute reanchors narrow audio without recreating converters or changing wide video");
    gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_BETAMAX_HIFI);
    check(gui_rf_preview_worker_wait_idle(w,3000) &&
        submit_marker(w,40000000u,100096,3,false,119) && gui_rf_preview_worker_wait_idle(w,3000),"ED to ordinary Beta at unchanged source rate");
    check(atomic_load(&latest_first_sample)==50048 && atomic_load(&latest_video_count)==16 &&
        atomic_load(&latest_video_marker)==119 && atomic_load(&create_calls)==created+1,
        "leaving ED rebuilds the legacy front-end even without a source-rate change");
    created=atomic_load(&create_calls);
    gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_ED_BETA);
    check(gui_rf_preview_worker_wait_idle(w,3000) &&
        submit_marker(w,40000000u,100128,3,false,121) && gui_rf_preview_worker_wait_idle(w,3000),"ordinary Beta to ED at unchanged source rate");
    check(atomic_load(&latest_first_sample)==50064 && atomic_load(&latest_video_count)==16 &&
        atomic_load(&audio_first)==50064 && atomic_load(&create_calls)==created+2,
        "entering ED rebuilds both filter paths despite equal20 MSPS output rates");
    check(submit_marker(w,80000000u,8000,4,true,123) && gui_rf_preview_worker_wait_idle(w,3000),"ED 80 MSPS input");
    check(atomic_load(&latest_first_sample)==2000 && atomic_load(&latest_video_count)==8 &&
        atomic_load(&audio_first)==2000 && atomic_load(&audio_count)==8,"80 MSPS feeds distinct video/audio20 MSPS streams");
    gui_rf_preview_worker_get_stats(w,&stats);
    check(stats.latency_ms>=TEST_ED_DELAY_SAMPLES*1000.0/20000000.0,
        "ED converter delay is accounted on the public20 MSPS frame axis");
    check(submit_marker(w,16000000u,2048,5,true,125) && gui_rf_preview_worker_wait_idle(w,3000),"ED returns to insufficient source rate");
    gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_BETAMAX_HIFI);
    check(gui_rf_preview_worker_wait_idle(w,3000) &&
        submit_marker(w,16000000u,2080,5,false,127) && gui_rf_preview_worker_wait_idle(w,3000),"legacy profile recovers at the same formerly rejected source rate");
    check(atomic_load(&latest_first_sample)==2600 && atomic_load(&audio_first)==2600,
        "rejected ED configuration cannot leave legacy conversion unavailable");
    gui_rf_preview_worker_destroy(w);check(gui_rf_preview_worker_set_budget(1),"ED fixture restores budget");
}

static void test_ed_buffered_frontends(bool delay_wide) {
    atomic_store(&delay_wide_once,delay_wide);atomic_store(&delay_audio_once,!delay_wide);
    check(gui_rf_preview_worker_set_budget(0),"ED buffered-front-end offline budget");
    gui_rf_preview_worker_t *w=gui_rf_preview_worker_create_profile(GUI_RF_PROFILE_NTSC_ED_BETA);
    check(w!=NULL,"ED buffered worker creation");if(!w)goto cleanup;
    gui_rf_preview_worker_set_audio_callback(w,audio_test_callback,NULL);
    unsigned video=atomic_load(&process_calls),audio=atomic_load(&audio_calls);
    const uint64_t origin=54002; /* Both20M converters deliberately have different buffered availability. */
    check(submit_marker(w,54000000u,origin,8,true,129) && gui_rf_preview_worker_wait_idle(w,3000),"ED first buffered conversion");
    check(atomic_load(&process_calls)==video+(delay_wide?0:1) &&
        atomic_load(&audio_calls)==audio+(delay_wide?1:0),"one front-end may produce zero while the other advances");
    check(submit_marker(w,54000000u,origin+32,8,false,129) && gui_rf_preview_worker_wait_idle(w,3000),"ED second buffered conversion");
    const uint64_t v0=origin*20000000u/54000000u,a0=origin*20000000u/54000000u;
    check(atomic_load(&latest_first_sample)==v0+(delay_wide?0:11) &&
        atomic_load(&latest_video_count)==(delay_wide?23:12),"video cursor follows its actual produced count independently of audio availability");
    check(atomic_load(&audio_first)==a0+(delay_wide?11:0) &&
        atomic_load(&audio_count)==(delay_wide?12:23),"audio cursor follows independent buffered output at 20 MSPS");
    check(atomic_load(&latest_video_marker)==1129 && atomic_load(&audio_marker)==129,
        "asymmetric buffering never substitutes the narrow samples for wide video or vice versa");
    check(submit_marker(w,54000000u,origin+64,8,false,131) && gui_rf_preview_worker_wait_idle(w,3000),"ED buffered cursors continue");
    check(atomic_load(&latest_first_sample)==v0+23 && atomic_load(&audio_first)==a0+23,
        "both cursors stay continuous after unequal per-call availability");
    const uint64_t gap=108005;
    check(submit_marker(w,54000000u,gap,9,true,133) && gui_rf_preview_worker_wait_idle(w,3000) &&
        submit_marker(w,54000000u,gap+32,9,false,133) && gui_rf_preview_worker_wait_idle(w,3000),"ED buffered gap resets both histories");
    check(atomic_load(&latest_first_sample)==gap*20000000u/54000000u+(delay_wide?0:11) &&
        atomic_load(&audio_first)==gap*20000000u/54000000u+(delay_wide?11:0),
        "gap drops old buffered tails and reanchors both noninteger output grids");
    gui_rf_preview_worker_destroy(w);
cleanup:
    atomic_store(&delay_wide_once,false);atomic_store(&delay_audio_once,false);
    check(gui_rf_preview_worker_set_budget(1),"buffered ED fixture restores budget");
}


/* ED has a separate wide video converter. Muting must remove all narrow
 * converter work, then discard buffered audio and reanchor its 20M cursor
 * when enabled again, without resetting or replaying wide video. */
static void test_ed_muted_frontend(void) {
    check(gui_rf_preview_worker_set_budget(0),"muted ED front-end offline budget");
    atomic_store(&delay_audio_once,true);
    gui_rf_preview_worker_t *w=gui_rf_preview_worker_create_profile(GUI_RF_PROFILE_NTSC_ED_BETA);
    check(w!=NULL,"muted ED worker creation");if(!w)goto cleanup;
    gui_rf_preview_worker_set_audio_callback(w,audio_test_callback,NULL);
    const uint64_t origin=54002;
    unsigned audio=atomic_load(&audio_calls);
    check(submit_marker(w,54000000u,origin,20,true,201) && gui_rf_preview_worker_wait_idle(w,3000),
        "ED narrow converter retains a deliberately buffered old audio tail");
    check(atomic_load(&audio_calls)==audio,"old buffered tail has not produced audio");
    unsigned narrow=atomic_load(&narrow_resample_calls),wide=atomic_load(&wide_resample_calls);
    unsigned video=atomic_load(&process_calls),resets=atomic_load(&reset_calls);
    gui_rf_preview_worker_set_audio_enabled(w,false);
    check(submit_marker(w,54000000u,origin+32,20,false,203) && gui_rf_preview_worker_wait_idle(w,3000) &&
        submit_marker(w,54000000u,origin+64,20,false,205) && gui_rf_preview_worker_wait_idle(w,3000),
        "muted ED consumes only wide RF across consecutive blocks");
    gui_rf_preview_stats_t stats;gui_rf_preview_worker_get_stats(w,&stats);
    check(atomic_load(&narrow_resample_calls)==narrow && atomic_load(&wide_resample_calls)==wide+2 &&
        atomic_load(&process_calls)==video+2 && atomic_load(&reset_calls)==resets &&
        atomic_load(&audio_calls)==audio && !stats.audio_enabled,
        "muting skips narrow resampling while preserving wide decode and controls");
    unsigned narrow_resets=atomic_load(&narrow_reset_calls);
    gui_rf_preview_worker_set_audio_enabled(w,true);
    check(submit_marker(w,54000000u,origin+96,20,false,207) && gui_rf_preview_worker_wait_idle(w,3000),
        "unmute restarts ED narrow conversion at fresh source RF");
    check(atomic_load(&narrow_reset_calls)>narrow_resets && atomic_load(&audio_calls)==audio,
        "unmute discarded the old pending tail and permits fresh converter buffering");
    check(submit_marker(w,54000000u,origin+128,20,false,207) && gui_rf_preview_worker_wait_idle(w,3000),
        "fresh buffered ED audio becomes available");
    const uint64_t resumed=(origin+96)*20000000u/54000000u;
    check(atomic_load(&audio_calls)==audio+1 && atomic_load(&audio_marker)==207 &&
        atomic_load(&audio_first)==resumed && atomic_load(&audio_count)==23 &&
        atomic_load(&audio_end)==resumed+23 && atomic_load(&reset_calls)==resets,
        "audio resumes on its source-derived 20M clock without stale payload or video reset");
    check(atomic_load(&latest_first_sample)==origin*20000000u/54000000u+47,
        "muting and narrow converter buffering never alter the independent wide cursor");
    gui_rf_preview_worker_set_audio_enabled(w,false);
    gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_BETAMAX_HIFI);
    check(gui_rf_preview_worker_wait_idle(w,3000),"muted ED to legacy profile applies");
    narrow=atomic_load(&narrow_resample_calls);wide=atomic_load(&wide_resample_calls);
    audio=atomic_load(&audio_calls);
    check(submit_marker(w,40000000u,400000,21,true,209) && gui_rf_preview_worker_wait_idle(w,3000),
        "muted legacy video still converts its narrow RF");
    check(atomic_load(&narrow_resample_calls)==narrow+1 && atomic_load(&wide_resample_calls)==wide &&
        atomic_load(&latest_first_sample)==200000 && atomic_load(&latest_video_count)==16 &&
        atomic_load(&latest_video_marker)==209 && atomic_load(&audio_calls)==audio,
        "narrow work is never skipped when it supplies legacy video");
    gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_ED_BETA);
    check(gui_rf_preview_worker_wait_idle(w,3000),"muted legacy to ED profile applies");
    narrow=atomic_load(&narrow_resample_calls);
    check(submit_marker(w,40000000u,400032,21,false,211) && gui_rf_preview_worker_wait_idle(w,3000),
        "returning to muted ED resumes only wide video");
    check(atomic_load(&narrow_resample_calls)==narrow && atomic_load(&latest_first_sample)==200016 &&
        atomic_load(&latest_video_marker)==1211 && atomic_load(&audio_calls)==audio,
        "profile transition retains muted ED conversion savings");
    gui_rf_preview_worker_set_audio_enabled(w,true);
    check(submit_marker(w,40000000u,400064,21,false,213) && gui_rf_preview_worker_wait_idle(w,3000),
        "reenabling audio after muted profile changes");
    check(atomic_load(&audio_calls)==audio+1 && atomic_load(&audio_marker)==213 &&
        atomic_load(&audio_first)==200032 && atomic_load(&latest_first_sample)==200032,
        "profile and mute changes resume only current audio on the correct clock");
    gui_rf_preview_worker_destroy(w);
cleanup:
    atomic_store(&delay_audio_once,false);
    check(gui_rf_preview_worker_set_budget(1),"muted ED fixture restores budget");
}


/* A flush callback holds the audio consumer after it captured its generation.
 * New RF then advances both producer and video generations and queues a fresh
 * audio slot. The consumer must refresh its old local generation, not discard
 * that newly current slot as if it were stale. */
static void test_ed_audio_generation_handoff(void) {
    check(gui_rf_preview_worker_set_budget(0),"ED audio generation handoff offline budget");
    gui_rf_preview_worker_t *w=gui_rf_preview_worker_create_profile(GUI_RF_PROFILE_NTSC_ED_BETA);
    check(w!=NULL,"ED audio generation handoff worker");if(!w)goto cleanup;
    gui_rf_preview_worker_set_audio_callback(w,audio_test_callback,NULL);
    atomic_store(&audio_flush_entered,false);atomic_store(&hold_audio_flush,true);
    gui_rf_preview_worker_set_audio_enabled(w,false);
    gui_rf_preview_worker_set_audio_enabled(w,true);
    uint64_t until=get_time_ms()+3000;
    while(!atomic_load(&audio_flush_entered) && get_time_ms()<until) thrd_sleep_ms(1);
    check(atomic_load(&audio_flush_entered),"audio consumer holds a captured generation during flush");
    unsigned video=atomic_load(&process_calls),audio=atomic_load(&audio_calls);
    check(submit_marker(w,40000000u,8000,22,true,219) && wait_calls(video+1),
        "new ED source advances and enqueues audio while the old generation is held");
    atomic_store(&hold_audio_flush,false);
    check(gui_rf_preview_worker_wait_idle(w,3000),"ED generation handoff drains");
    check(atomic_load(&audio_calls)==audio+1 && atomic_load(&audio_marker)==219 &&
        atomic_load(&audio_first)==4000 && atomic_load(&audio_count)==16 && atomic_load(&audio_end)==4016,
        "newest audio slot survives consumer generation refresh with its exact 20M clock");
    gui_rf_preview_worker_destroy(w);
cleanup:
    atomic_store(&hold_audio_flush,false);
    check(gui_rf_preview_worker_set_budget(1),"ED generation handoff restores budget");
}

int main(void) {
    atomic_init(&hold_process, false);
    atomic_init(&process_entered, false);
    atomic_init(&process_calls, 0);
    atomic_init(&reset_calls, 0);
    atomic_init(&create_calls, 0);
    atomic_init(&busy_ms, 0);
    atomic_init(&active_processes, 0);
    atomic_init(&max_active_processes, 0);
    atomic_init(&latest_first_sample, 0);
    atomic_init(&counter_seed, 0);
    uint8_t *frame = malloc(GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4);
    if (!frame) return 1;
    test_frames_and_commands(frame);
    test_unsupported_rate();
    test_queue_overload(frame);
    test_pause_and_counters(frame);
    test_shared_budget_and_interrupt();
    test_long_running_counters();
    test_measured_shared_budget();
    test_fast_block_handoffs();
    test_parallel_budget();
    test_superbeta_optional_audio();
    test_svhs_frontend();
    test_ed_frontends();
    test_ed_buffered_frontends(true);test_ed_buffered_frontends(false);
    test_ed_muted_frontend();
    test_ed_audio_generation_handoff();
    test_independent_audio(GUI_RF_PROFILE_NTSC_BETAMAX_HIFI);
    test_independent_audio(GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI);
    free(frame);
    if (!failures) puts("RF preview worker tests passed");
    return failures ? 1 : 0;
}
