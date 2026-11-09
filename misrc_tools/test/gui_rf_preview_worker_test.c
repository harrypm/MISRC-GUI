/* Worker lifecycle tests substitute video and Beta DSP. The VHS Hi-Fi
 * contracts exercise the production audio decoder with synthetic RF. */
#include "../misrc_gui/processing/gui_rf_preview_worker.h"
#include "../misrc_gui/signal/gui_rf_resampler.h"
#include "../misrc_gui/signal/gui_vhs_hifi.h"
#include "../common/threading.h"

#include <math.h>
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
static atomic_uint finished_rf_tail_samples, finished_wide_tail_samples;
static atomic_uint busy_ms;
static atomic_uint active_processes;
static atomic_uint max_active_processes;
static atomic_uint_fast64_t latest_first_sample,latest_video_count;
static atomic_int latest_video_marker;
static atomic_bool delay_wide_once,delay_audio_once;
static atomic_uint_fast64_t counter_seed;
static atomic_uint_fast64_t frame_end_override;
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
        profile==GUI_RF_PROFILE_NTSC_ED_BETA || gui_rf_profile_is_audio_only(profile);
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
    uint64_t public_end = atomic_exchange(&frame_end_override, 0);
    if (!public_end) public_end = first + count;
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
bool gui_rf_resampler_finish(gui_rf_resampler_t *r,float *output,size_t capacity,size_t *output_count) {
    size_t n = r->pending < capacity ? r->pending : capacity;
    for (size_t i = 0; i < n; ++i) output[i] = r->pending_marker;
    r->pending -= n;
    *output_count = n;
    atomic_fetch_add(r->ed_beta ? &finished_wide_tail_samples : &finished_rf_tail_samples, (unsigned)n);
    return true;
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

static void test_ld_presentation_clock(void) {
    gui_rf_preview_worker_t *w = gui_rf_preview_worker_create_profile(GUI_RF_PROFILE_NTSC_LASERDISC);
    check(w != NULL, "LD presentation worker created");
    if (!w) return;
    check(gui_rf_preview_worker_set_budget(0), "LD presentation fixture offline budget");
    gui_rf_preview_worker_set_audio_enabled(w, false);
    gui_rf_preview_worker_set_ld_sync(w, true);
    const uint64_t start = 20u * GUI_RF_DECODE_RATE_HZ;
    const uint32_t rate = GUI_RF_DECODE_RATE_HZ;
    for (unsigned i = 0; i < 4; ++i) {
        check(submit_marker(w, rate, start + 32 * i, 1, false, (int16_t)(11 + i)) &&
              gui_rf_preview_worker_wait_idle(w, 3000), "LD frame is produced");
    }

    uint8_t rgba[8 * 6 * 4];
    uint64_t generation = 0;
    gui_rf_preview_stats_t stats;
    check(gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, false, 0) && stats.frame_end_sample == start + 32,
          "LD uses delayed video while waiting for the audio clock");
    check(gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, true, start + 32) && stats.frame_end_sample == start + 64 &&
          rgba[0] == (uint8_t)(1000 + 12),
          "LD audio clock at a frame boundary selects the next frame");
    check(!gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, true, start), "LD clock cannot rewind visible video within a stream");

    check(gui_rf_preview_worker_set_ld_audio(w, GUI_RF_LD_AUDIO_DIGITAL) &&
          !gui_rf_preview_worker_set_ld_audio(w, GUI_RF_LD_AUDIO_DIGITAL),
          "LD reports only actual audio track changes");
    check(gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, true, start + 65) && stats.frame_end_sample == start + 96,
          "LD switch preserves video history");
    gui_rf_preview_worker_set_ld_audio(w, GUI_RF_LD_AUDIO_AC3);
    const uint64_t delayed_end = start + 1000000;
    atomic_store(&frame_end_override, delayed_end);
    check(submit_marker(w, rate, start + 128, 1, false, 15) &&
          gui_rf_preview_worker_wait_idle(w, 3000), "LD frame after video hole is produced");
    check(gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, true, start + 128) && stats.frame_end_sample == start + 128,
          "LD uses prior picture across a video hole");
    check(!gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, true, start + 300000), "LD never presents the next picture early across a video hole");
    check(gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, true, delayed_end - 600000) && stats.frame_end_sample == delayed_end,
          "LD shows the next picture once its frame interval starts");

    atomic_store(&hold_process, true);
    atomic_store(&process_entered, false);
    check(submit_marker(w, rate, 0, 2, true, 21), "LD backward seek is queued");
    uint64_t until = get_time_ms() + 3000;
    while (!atomic_load(&process_entered) && get_time_ms() < until) thrd_sleep_ms(1);
    check(!gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, true, start + 96), "LD cannot present old history after seek");
    atomic_store(&hold_process, false);
    check(gui_rf_preview_worker_wait_idle(w, 3000), "LD backward seek completes");
    check(gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, true, 0) && stats.frame_end_sample == 32 && rgba[0] == (uint8_t)(1000 + 21),
          "LD presents new history after a backward seek");
    gui_rf_preview_worker_set_audio_enabled(w, true);
    check(submit_marker(w, rate, 32, 2, false, 22) &&
          gui_rf_preview_worker_wait_idle(w, 3000), "LD has a newer decoded picture at EOF");
    check(!gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, false, 0), "LD fallback is still behind the newest picture without an audio clock");
    gui_rf_preview_worker_set_ld_sync(w, false);
    gui_rf_preview_worker_set_audio_enabled(w, false);
    check(gui_rf_preview_worker_take_ld_frame(w, rgba, sizeof(rgba), &generation, NULL, NULL,
          &stats, false, 0) && stats.frame_end_sample == 64 && rgba[0] == (uint8_t)(1000 + 22),
          "LD presents the final picture when EOF ends audio synchronization");
    gui_rf_preview_worker_destroy(w);
    check(gui_rf_preview_worker_set_budget(1), "LD presentation fixture restores default budget");
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
static unsigned seek_output_resets;
static void seek_reset_output(const void *worker) {
    // The fixture holds the old PCM callback at its processing generation.
    check(atomic_load(&hold_audio) && atomic_load(&audio_entered) &&
          !gui_rf_preview_worker_audio_is_current(worker),
          "old PCM is already ineligible when the monitor queue is reset");
    ++seek_output_resets;
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
    const int16_t seek_samples[32] = {99};
    const gui_rf_block_info_t seek_info = {20000000u, 0, 4, true};
    unsigned prior_resets = seek_output_resets;
    check(gui_rf_preview_worker_submit_with_audio_reset(w, seek_samples, 32, &seek_info, seek_reset_output),
          "seek publishes new source generation immediately");
    check(seek_output_resets == prior_resets + 1, "seek resets the monitor synchronously");
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

static void test_vhs_hifi_audio_only(void) {
    enum { input_count = 65536 };
    int16_t *input = calloc(input_count, sizeof(*input));
    gui_rf_preview_worker_t *w = gui_rf_preview_worker_create_profile(GUI_RF_PROFILE_NTSC_VHS_HIFI);
    check(input && w, "VHS Hi-Fi audio-only worker created");
    if (input && w) {
        check(gui_rf_preview_worker_set_budget(0), "VHS Hi-Fi fixture offline budget");
        gui_rf_preview_worker_set_audio_callback(w, audio_test_callback, NULL);
        unsigned video = atomic_load(&process_calls), audio = atomic_load(&audio_callbacks);
        gui_rf_block_info_t info = {20000000u, 0, 1, false};
        check(gui_rf_preview_worker_submit(w, input, input_count, &info) &&
              gui_rf_preview_worker_wait_idle(w, 3000), "VHS Hi-Fi input drains");
        check(atomic_load(&audio_callbacks) > audio && atomic_load(&process_calls) == video,
              "VHS Hi-Fi publishes PCM without calling the picture decoder");

        gui_rf_preview_worker_set_profile(w, GUI_RF_PROFILE_PAL_VHS_HIFI);
        check(gui_rf_preview_worker_wait_idle(w, 3000), "VHS Hi-Fi PAL profile applies");
        audio = atomic_load(&audio_callbacks);
        info.start_sample = input_count;
        check(gui_rf_preview_worker_submit(w, input, input_count, &info) &&
              gui_rf_preview_worker_wait_idle(w, 3000), "PAL VHS Hi-Fi input drains");
        gui_rf_preview_stats_t stats;
        gui_rf_preview_worker_get_stats(w, &stats);
        check(strstr(stats.hifi.reason, "PAL VHS Hi-Fi") &&
              atomic_load(&audio_callbacks) > audio && atomic_load(&process_calls) == video,
              "switching to PAL replaces the VHS Hi-Fi demodulator and still skips video");

        unsigned conversions = atomic_load(&narrow_resample_calls);
        const uint32_t native_rates[] = {8000000u, 8192000u, 10000000u};
        for (size_t i = 0; i < sizeof(native_rates) / sizeof(native_rates[0]); ++i) {
            uint32_t rate = native_rates[i];
            info = (gui_rf_block_info_t){rate, rate, i + 2, true};
            audio = atomic_load(&audio_callbacks);
            check(gui_rf_preview_worker_submit(w, input, input_count, &info) &&
                  gui_rf_preview_worker_wait_idle(w, 3000), "native VHS Hi-Fi input drains");
            gui_rf_preview_worker_get_stats(w, &stats);
            uint64_t end = GUI_RF_DECODE_RATE_HZ + (uint64_t)input_count * GUI_RF_DECODE_RATE_HZ / rate;
            check(stats.vhs_hifi_rf_rate_hz == rate && stats.hifi.input_samples == input_count &&
                  atomic_load(&audio_callbacks) > audio && atomic_load(&audio_end) == end,
                  "native VHS Hi-Fi preserves its RF rate and publishes on the normalized media clock");
        }
        check(atomic_load(&narrow_resample_calls) == conversions && atomic_load(&process_calls) == video,
              "native VHS Hi-Fi bypasses the RF resampler and picture decoder");

        gui_rf_preview_worker_set_profile(w, GUI_RF_PROFILE_PAL_VHS);
        check(gui_rf_preview_worker_wait_idle(w, 3000), "low-rate video profile change drains");
        gui_rf_preview_worker_get_stats(w, &stats);
        check(!stats.vhs_hifi_rf_rate_hz && strstr(stats.decode.reason, "Unsupported RF rate"),
              "native Hi-Fi rate does not enable low-rate video decoding");
        gui_rf_preview_worker_set_profile(w, GUI_RF_PROFILE_PAL_VHS_HIFI);
        check(gui_rf_preview_worker_wait_idle(w, 3000), "native Hi-Fi profile restored");
        gui_rf_preview_worker_get_stats(w, &stats);
        check(stats.vhs_hifi_rf_rate_hz == info.sample_rate_hz,
              "native audio is restored without a sample rate change");

        gui_rf_preview_worker_set_audio_enabled(w, false);
        audio = atomic_load(&audio_callbacks);
        info.start_sample += input_count;
        info.discontinuity = false;
        check(gui_rf_preview_worker_submit(w, input, input_count, &info) &&
              gui_rf_preview_worker_wait_idle(w, 3000), "muted VHS Hi-Fi input drains");
        check(atomic_load(&narrow_resample_calls) == conversions &&
              atomic_load(&process_calls) == video && atomic_load(&audio_callbacks) == audio,
              "muted native audio-only worker skips audio and picture decoding");
        check(gui_rf_preview_worker_set_budget(1), "VHS Hi-Fi fixture restores default budget");
    }
    gui_rf_preview_worker_destroy(w);
    free(input);
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
    const int16_t preview[32] = {102};
    const gui_rf_block_info_t scrub = {.sample_rate_hz = 20000000u, .start_sample = 32,
                                     .epoch = 2, .discontinuity = true, .preview_only = true};
    check(gui_rf_preview_worker_submit(w, preview, 32, &scrub) &&
          gui_rf_preview_worker_wait_idle(w, 3000), "paused seek preroll reaches picture decoder");
    check(atomic_load(&latest_video_marker) == 102 && atomic_load(&audio_calls) == calls,
          "queued seek preroll stays silent even after the monitor enables audio");
    check(submit_marker(w,20000000u,32,3,true,103) && gui_rf_preview_worker_wait_idle(w,3000),
          "SuperBeta HiFi starts its audio consumer after profile transition");
    check(atomic_load(&audio_calls)==calls+1 && atomic_load(&audio_marker)==103,
          "SuperBeta HiFi decodes enabled audio from its current RF block");
    gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_SUPERBETA);
    check(gui_rf_preview_worker_wait_idle(w,3000),"SuperBeta optional HiFi removal applies");
    check(submit_marker(w,20000000u,64,3,false,105) && gui_rf_preview_worker_wait_idle(w,3000),
          "video-only SuperBeta continues after removing optional HiFi");
    gui_rf_preview_stats_t stats;gui_rf_preview_worker_get_stats(w,&stats);
    check(atomic_load(&audio_calls)==calls+1 && stats.audio_enabled && !stats.audio_active,
          "removing SuperBeta HiFi stops audio independently of the monitor flag");
    gui_rf_preview_worker_destroy(w);check(gui_rf_preview_worker_set_budget(1),"SuperBeta fixture restores default budget");
}

/* Wideband profiles retain their RF passband on the 20 MHz media clock.
 * Check converter transitions independently of the physical RF tests. */
static void test_wideband_frontend(void) {
    check(gui_rf_preview_worker_set_budget(0),"wideband video offline budget");
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS,
        GUI_RF_PROFILE_PAL_LASERDISC,GUI_RF_PROFILE_NTSC_LASERDISC};
    for(size_t p=0;p<sizeof(profiles)/sizeof(profiles[0]);++p) {
        gui_rf_preview_worker_t *w=gui_rf_preview_worker_create_profile(profiles[p]);
        check(w!=NULL,"wideband video worker creation");if(!w)continue;
        gui_rf_preview_worker_set_audio_callback(w,audio_test_callback,NULL);
        gui_rf_preview_worker_set_audio_enabled(w,false);
        unsigned video=atomic_load(&process_calls),audio=atomic_load(&audio_calls);
        unsigned narrow=atomic_load(&narrow_resample_calls);
        check(submit_marker(w,16000000u,0,1,false,141) && gui_rf_preview_worker_wait_idle(w,3000),
            "insufficient wideband video source rate drains safely");
        gui_rf_preview_stats_t stats;gui_rf_preview_worker_get_stats(w,&stats);
        const char *label=p<2 ? "S-VHS" : "LaserDisc";
        check(strstr(stats.decode.reason,label) && strstr(stats.decode.reason,"20 MSPS") &&
            atomic_load(&process_calls)==video,"wideband video rejects missing source bandwidth");
        check(submit_marker(w,20000000u,0,2,true,143) && gui_rf_preview_worker_wait_idle(w,3000),
            "native 20MSPS wideband video accepted");
        check(atomic_load(&latest_first_sample)==0 && atomic_load(&latest_video_count)==32 &&
            atomic_load(&latest_video_marker)==1143,"native wideband video uses the wide video stream on the 20M clock");
        check(submit_marker(w,40000000u,4000,3,true,145) && gui_rf_preview_worker_wait_idle(w,3000),
            "40MSPS wideband video accepted");
        check(atomic_load(&latest_first_sample)==2000 && atomic_load(&latest_video_count)==16 &&
            atomic_load(&latest_video_marker)==1145,"40MSPS wideband video is downsampled to wide-passband 20MSPS");
        check(submit_marker(w,80000000u,8000,4,true,147) && gui_rf_preview_worker_wait_idle(w,3000),
            "80MSPS wideband video accepted");
        check(atomic_load(&latest_first_sample)==2000 && atomic_load(&latest_video_count)==8 &&
            atomic_load(&latest_video_marker)==1147,"80MSPS wideband video retains the 20M media clock");
        check(atomic_load(&audio_calls)==audio && atomic_load(&narrow_resample_calls)==narrow,
            "wideband video performs no Beta audio DSP or unnecessary narrow-band conversion");
        unsigned created=atomic_load(&create_calls);
        gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_PAL_VHS);
        check(gui_rf_preview_worker_wait_idle(w,3000) &&
            submit_marker(w,80000000u,8032,4,false,149) && gui_rf_preview_worker_wait_idle(w,3000),
            "wideband video to ordinary VHS at unchanged source rate");
        check(atomic_load(&latest_first_sample)==2008 && atomic_load(&latest_video_marker)==149 &&
            atomic_load(&create_calls)==created+1,"leaving wideband video restores the ordinary converter response");
        created=atomic_load(&create_calls);
        gui_rf_preview_worker_set_profile(w,profiles[p]);
        check(gui_rf_preview_worker_wait_idle(w,3000) &&
            submit_marker(w,80000000u,8064,4,false,151) && gui_rf_preview_worker_wait_idle(w,3000),
            "ordinary VHS to wideband video at unchanged source rate");
        check(atomic_load(&latest_first_sample)==2016 && atomic_load(&latest_video_marker)==1151 &&
            atomic_load(&create_calls)==created+2,"wideband video reselects the wide converter despite equal output rates");
        created=atomic_load(&create_calls);
        gui_rf_preview_worker_set_profile(w,GUI_RF_PROFILE_NTSC_ED_BETA);
        gui_rf_preview_worker_set_audio_enabled(w,true);
        check(gui_rf_preview_worker_wait_idle(w,3000) &&
            submit_marker(w,80000000u,8096,4,false,153) && gui_rf_preview_worker_wait_idle(w,3000),
            "wideband video to ED starts the independent Hi-Fi consumer");
        check(atomic_load(&audio_calls)==audio+1 && atomic_load(&audio_marker)==153 &&
            atomic_load(&create_calls)==created,"ED reuses the wide converter and decodes current audio");
        audio=atomic_load(&audio_calls);narrow=atomic_load(&narrow_resample_calls);
        gui_rf_preview_worker_set_audio_enabled(w,false);
        gui_rf_preview_worker_set_profile(w,profiles[p]);
        check(gui_rf_preview_worker_wait_idle(w,3000) &&
            submit_marker(w,80000000u,8128,4,false,155) && gui_rf_preview_worker_wait_idle(w,3000),
            "ED to wideband video clears previous Beta audio state");
        gui_rf_preview_worker_get_stats(w,&stats);
        check(atomic_load(&latest_first_sample)==2032 && atomic_load(&latest_video_marker)==1155 &&
            atomic_load(&audio_calls)==audio && atomic_load(&narrow_resample_calls)==narrow &&
            atomic_load(&create_calls)==created && !stats.audio_active,
            "wideband video reuses wide video without stale ED audio or narrow conversion");
        gui_rf_preview_worker_destroy(w);
    }
    check(gui_rf_preview_worker_set_budget(1),"wideband video restores budget");
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

#define TEST_HIFI_PI 3.14159265358979323846
#define TEST_HIFI_IQ_BLOCK 8191

typedef struct {
    uint64_t end_sample, block_end;
    size_t frames;
    double re[2][2], im[2][2], peak;
    bool monotonic, locked;
    uint64_t resets;
} hifi_iq_capture_t;

static const double hifi_iq_tones[2] = {1000.0, 1700.0};

static void generate_hifi_iq(double time, double offset_hz, float *i, float *q) {
    double left = 2.0 * TEST_HIFI_PI * (offset_hz - 200000.0) * time -
                  25000.0 / hifi_iq_tones[0] * cos(2.0 * TEST_HIFI_PI * hifi_iq_tones[0] * time);
    double right = 2.0 * TEST_HIFI_PI * (offset_hz + 200000.0) * time -
                   25000.0 / hifi_iq_tones[1] * cos(2.0 * TEST_HIFI_PI * hifi_iq_tones[1] * time);
    *i = (float)(0.3 * (cos(left) + cos(right)));
    *q = (float)(0.3 * (sin(left) + sin(right)));
}

static void capture_hifi_iq(void *context, const float *pcm, size_t frames,
                            uint64_t end_sample, const gui_beta_hifi_status_t *status) {
    if (!pcm || !frames) return;
    hifi_iq_capture_t *audio = context;
    audio->monotonic &= end_sample > audio->end_sample && end_sample <= audio->block_end;
    audio->end_sample = end_sample;
    if (status) {
        audio->locked = status->locked;
        audio->resets = status->resets;
    }
    for (size_t j = 0; j < frames; ++j, ++audio->frames) {
        audio->peak = fmax(audio->peak, fmax(fabs(pcm[2 * j]), fabs(pcm[2 * j + 1])));
        if (audio->frames < GUI_BETA_HIFI_AUDIO_RATE_HZ / 10) continue;
        for (unsigned tone = 0; tone < 2; ++tone) {
            double angle = 2.0 * TEST_HIFI_PI * hifi_iq_tones[tone] *
                           audio->frames / GUI_BETA_HIFI_AUDIO_RATE_HZ;
            double cosine = cos(angle), sine = sin(angle);
            for (unsigned channel = 0; channel < 2; ++channel) {
                audio->re[channel][tone] += pcm[2 * j + channel] * cosine;
                audio->im[channel][tone] += pcm[2 * j + channel] * sine;
            }
        }
    }
}

static double hifi_iq_tone_power(const hifi_iq_capture_t *audio, unsigned channel, unsigned tone) {
    return audio->re[channel][tone] * audio->re[channel][tone] +
           audio->im[channel][tone] * audio->im[channel][tone];
}

static void check_hifi_iq_stereo(const hifi_iq_capture_t *audio, size_t expected_frames) {
    const double minimum_power = (double)expected_frames * expected_frames * 1e-6;
    for (unsigned channel = 0; channel < 2; ++channel) {
        double own = hifi_iq_tone_power(audio, channel, channel);
        double other = hifi_iq_tone_power(audio, channel, 1 - channel);
        check(own > minimum_power && own > 20.0 * other,
              "VHS Hi-Fi IQ recovers the expected independent tone in each stereo channel");
    }
}

static void test_vhs_hifi_iq_system(gui_rf_format_t format, uint32_t rate, uint64_t center) {
    gui_vhs_hifi_decoder_t *decoder = gui_vhs_hifi_create_iq(format, rate, center);
    check(decoder != NULL, "VHS Hi-Fi IQ decoder accepts both carriers in band");
    if (!decoder) return;

    const uint64_t first_sample = UINT64_C(0x100000017);
    const size_t sample_count = rate / 4;
    const double midpoint = format == GUI_RF_PAL ? 1600000.0 : 1500000.0;
    const double offset_hz = midpoint - (double)center;
    float i_data[TEST_HIFI_IQ_BLOCK], q_data[TEST_HIFI_IQ_BLOCK];
    hifi_iq_capture_t audio = {.end_sample = first_sample, .monotonic = true};
    for (size_t offset = 0; offset < sample_count;) {
        size_t count = sample_count - offset;
        if (count > TEST_HIFI_IQ_BLOCK) count = TEST_HIFI_IQ_BLOCK;
        for (size_t i = 0; i < count; ++i)
            generate_hifi_iq((double)(offset + i) / rate, offset_hz, &i_data[i], &q_data[i]);
        audio.block_end = first_sample + offset + count;
        gui_vhs_hifi_process_iq(decoder, i_data, q_data, count, first_sample + offset,
                                capture_hifi_iq, &audio);
        offset += count;
    }

    const size_t expected = GUI_BETA_HIFI_AUDIO_RATE_HZ / 4;
    check(audio.locked && audio.monotonic && audio.end_sample == first_sample + sample_count,
          "VHS Hi-Fi IQ locks and reports exclusive positions on its native 64-bit source clock");
    check(audio.frames == expected, "VHS Hi-Fi IQ yields 48 kHz over irregular input chunks");
    check_hifi_iq_stereo(&audio, expected);

    /* A new native sample position marks a dropped capture block. */
    uint64_t gap_start = audio.end_sample + 2 * TEST_HIFI_IQ_BLOCK;
    uint64_t previous_resets = audio.resets;
    memset(i_data, 0, sizeof(i_data));
    memset(q_data, 0, sizeof(q_data));
    audio = (hifi_iq_capture_t){.end_sample = gap_start, .block_end = gap_start + TEST_HIFI_IQ_BLOCK,
                                .monotonic = true};
    gui_vhs_hifi_process_iq(decoder, i_data, q_data, TEST_HIFI_IQ_BLOCK, gap_start,
                            capture_hifi_iq, &audio);
    check(audio.frames > 0 && audio.monotonic && !audio.locked && audio.peak == 0.0 &&
          audio.resets == previous_resets + 1,
          "VHS Hi-Fi IQ resets and mutes stale PCM across a capture gap");
    gui_vhs_hifi_destroy(decoder);
}

static void test_vhs_hifi_iq(void) {
    check(!gui_vhs_hifi_iq_supported(GUI_RF_NTSC, 250000u, 1500000u) &&
          !gui_vhs_hifi_iq_supported(GUI_RF_NTSC, 1024000u, 1650000u),
          "VHS Hi-Fi IQ rejects rate or tuning that excludes a carrier sideband");
    test_vhs_hifi_iq_system(GUI_RF_NTSC, 1024000u, 1500000u);
    test_vhs_hifi_iq_system(GUI_RF_NTSC, 1200000u, 1590000u);
    test_vhs_hifi_iq_system(GUI_RF_PAL, 1536000u, 1600000u);
    test_vhs_hifi_iq_system(GUI_RF_NTSC, 2048000u, 1500000u);
    test_vhs_hifi_iq_system(GUI_RF_PAL, 2400000u, 1600000u);
}

static void capture_worker_hifi_iq(void *context, const float *pcm, size_t frames,
                                   uint64_t end_sample) {
    capture_hifi_iq(context, pcm, frames, end_sample, NULL);
}

static void test_vhs_hifi_iq_worker(void) {
    gui_rf_preview_worker_t *worker = gui_rf_preview_worker_create_profile(GUI_RF_PROFILE_NTSC_VHS_HIFI);
    check(worker != NULL, "RTL-SDR VHS Hi-Fi worker created");
    if (!worker) return;

    check(gui_rf_preview_worker_set_budget(0), "RTL-SDR VHS Hi-Fi fixture offline budget");
    const uint32_t rate = 1024000u;
    const uint64_t start = UINT64_C(0x100000017);
    const size_t sample_count = rate / 4;
    const size_t expected_frames = GUI_BETA_HIFI_AUDIO_RATE_HZ / 4;
    const uint64_t end = (start + sample_count) * GUI_RF_DECODE_RATE_HZ / rate;
    uint8_t iq[2 * TEST_HIFI_IQ_BLOCK];
    hifi_iq_capture_t audio = {.end_sample = start * GUI_RF_DECODE_RATE_HZ / rate,
                               .block_end = end, .monotonic = true};
    gui_rf_preview_worker_set_audio_callback(worker, capture_worker_hifi_iq, &audio);

    const unsigned video = atomic_load(&process_calls);
    const unsigned narrow = atomic_load(&narrow_resample_calls);
    const unsigned wide = atomic_load(&wide_resample_calls);
    bool drained = true;
    for (size_t offset = 0; offset < sample_count;) {
        size_t count = sample_count - offset;
        if (count > TEST_HIFI_IQ_BLOCK) count = TEST_HIFI_IQ_BLOCK;
        for (size_t i = 0; i < count; ++i) {
            float in_phase, quadrature;
            generate_hifi_iq((double)(offset + i) / rate, 0.0, &in_phase, &quadrature);
            iq[2 * i] = (uint8_t)lroundf(127.5f * (1.0f + in_phase));
            iq[2 * i + 1] = (uint8_t)lroundf(127.5f * (1.0f + quadrature));
        }
        gui_rf_block_info_t info = {.sample_rate_hz = rate, .start_sample = start + offset,
                                    .epoch = 1, .iq_center_hz = 1500000u};
        drained = gui_rf_preview_worker_submit_iq(worker, iq, count, &info, NULL) &&
                  gui_rf_preview_worker_wait_idle(worker, 3000);
        check(drained, "RTL-SDR VHS Hi-Fi worker drains interleaved IQ");
        if (!drained) break;
        offset += count;
    }

    if (drained) {
        gui_rf_preview_stats_t stats;
        gui_rf_preview_worker_get_stats(worker, &stats);
        check(audio.frames == expected_frames && audio.monotonic && audio.end_sample == end,
              "RTL-SDR VHS Hi-Fi publishes 48 kHz PCM on the normalized 64-bit media clock");
        check_hifi_iq_stereo(&audio, expected_frames);
        check(stats.source_rate_hz == rate && stats.vhs_hifi_rf_rate_hz == rate &&
              stats.hifi.locked && stats.hifi.input_samples == sample_count &&
              !stats.blocks_dropped && !stats.audio_blocks_dropped,
              "RTL-SDR VHS Hi-Fi worker locks at the native IQ rate without dropping input");

        /* Continuous sample index and epoch: only the changed center forces reacquisition. */
        gui_rf_block_info_t retune = {.sample_rate_hz = rate, .start_sample = start + sample_count,
                                      .epoch = 1, .iq_center_hz = 1525000u};
        const uint64_t retune_end = (retune.start_sample + TEST_HIFI_IQ_BLOCK) *
                                    GUI_RF_DECODE_RATE_HZ / rate;
        audio = (hifi_iq_capture_t){.end_sample = end, .block_end = retune_end, .monotonic = true};
        memset(iq, 128, sizeof(iq));
        drained = gui_rf_preview_worker_submit_iq(worker, iq, TEST_HIFI_IQ_BLOCK, &retune, NULL) &&
                  gui_rf_preview_worker_wait_idle(worker, 3000);
        check(drained, "RTL-SDR VHS Hi-Fi worker drains after a center change");
        if (drained) {
            gui_rf_preview_worker_get_stats(worker, &stats);
            check(!stats.hifi.locked && stats.hifi.input_samples == TEST_HIFI_IQ_BLOCK &&
                  audio.frames > 0 && audio.monotonic && audio.end_sample == retune_end && audio.peak == 0.0,
                  "RTL-SDR VHS Hi-Fi retune clears the previous lock and mutes stale PCM");
        }
    }

    gui_rf_preview_worker_destroy(worker);
    check(atomic_load(&process_calls) == video && atomic_load(&narrow_resample_calls) == narrow &&
          atomic_load(&wide_resample_calls) == wide,
          "RTL-SDR VHS Hi-Fi bypasses video and real RF resampling");
    check(gui_rf_preview_worker_set_budget(1), "RTL-SDR VHS Hi-Fi fixture restores default budget");
}

static void test_ld_rf_tail_finish(bool audible) {
    gui_rf_preview_worker_t *w = gui_rf_preview_worker_create_profile(GUI_RF_PROFILE_NTSC_LASERDISC);
    check(w != NULL, "LD RF tail worker allocation");
    if (!w) return;
    enum { count = 4096 };
    int16_t samples[count] = {0};
    gui_rf_block_info_t info = {.sample_rate_hz = 54000000u, .epoch = 42};
    atomic_store(&finished_rf_tail_samples, 0);
    atomic_store(&finished_wide_tail_samples, 0);
    atomic_store(&delay_audio_once, true);
    atomic_store(&delay_wide_once, true);
    unsigned frames_before = atomic_load(&process_calls);
    gui_rf_preview_worker_set_audio_enabled(w, audible);
    check(gui_rf_preview_worker_submit(w, samples, count, &info) &&
          gui_rf_preview_worker_wait_idle(w, 3000), "LD final RF block has reached the converter");
    check(atomic_load(&finished_rf_tail_samples) == 0 &&
          atomic_load(&finished_wide_tail_samples) == 0, "waiting for input does not drain RF");
    uint64_t deadline = get_time_ms() + 3000;
    bool done;
    while (!(done = gui_rf_preview_worker_finish_ld_video(w)) && get_time_ms() < deadline)
        thrd_sleep_ms(1);
    check(done && atomic_load(&finished_rf_tail_samples) == 0 &&
          atomic_load(&finished_wide_tail_samples) == (unsigned)(count * 20 / 54) &&
          atomic_load(&process_calls) == frames_before + 1,
          "LD EOF drains the picture RF independently of audio monitoring");
    deadline = get_time_ms() + 3000;
    while (!(done = gui_rf_preview_worker_finish_ld_audio(w)) && get_time_ms() < deadline)
        thrd_sleep_ms(1);
    check(done && atomic_load(&finished_rf_tail_samples) == (audible ? (unsigned)(count * 20 / 54) : 0) &&
          atomic_load(&finished_wide_tail_samples) == (unsigned)(count * 20 / 54) &&
          atomic_load(&process_calls) == frames_before + 1,
          "LD EOF drains the selected audio RF without decoding the picture tail twice");
    atomic_store(&delay_audio_once, false);
    atomic_store(&delay_wide_once, false);
    gui_rf_preview_worker_destroy(w);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--vhs-hifi-iq-only") == 0) {
        test_vhs_hifi_iq();
        if (!failures) puts("PAL/NTSC VHS Hi-Fi complex I/Q tests passed");
        return failures ? 1 : 0;
    }
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
    atomic_init(&frame_end_override, 0);
    uint8_t *frame = malloc(GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4);
    if (!frame) return 1;
    test_frames_and_commands(frame);
    test_ld_presentation_clock();
    test_ld_rf_tail_finish(false);
    test_ld_rf_tail_finish(true);
    test_unsupported_rate();
    test_queue_overload(frame);
    test_pause_and_counters(frame);
    test_shared_budget_and_interrupt();
    test_long_running_counters();
    test_measured_shared_budget();
    test_fast_block_handoffs();
    test_parallel_budget();
    test_vhs_hifi_audio_only();
    test_vhs_hifi_iq();
    test_vhs_hifi_iq_worker();
    test_superbeta_optional_audio();
    test_wideband_frontend();
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
