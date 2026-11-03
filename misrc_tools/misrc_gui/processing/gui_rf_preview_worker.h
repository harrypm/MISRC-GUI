#ifndef GUI_RF_PREVIEW_WORKER_H
#define GUI_RF_PREVIEW_WORKER_H

#include "../signal/gui_rf_preview.h"
#include "../signal/gui_beta_hifi.h"

typedef struct gui_rf_preview_worker gui_rf_preview_worker_t;
/* Audio end_sample always uses the normalized 20 MSPS axis, including EDBeta.
 * EDBeta video has a separate wide-passband 20 MSPS converter and retains
 * public 20 MSPS frame timestamps. Beta Hi-Fi uses its ordinary converter. */
typedef void (*gui_rf_preview_audio_callback_t)(void *context, const float *stereo,
                                                size_t frames, uint64_t end_sample);

typedef struct {
    uint32_t sample_rate_hz;
    uint64_t start_sample;
    uint64_t epoch;
    bool discontinuity;
} gui_rf_block_info_t;

typedef struct {
    gui_rf_decode_status_t decode;
    gui_beta_hifi_status_t hifi;
    uint32_t source_rate_hz;
    uint64_t blocks_dropped;
    uint64_t resets;
    uint64_t generation;
    double decode_ms;
    double latency_ms;
    double fps; /* Decoded frames / actual wall interval, refreshed >=1s. */
    uint64_t pause_drops;
    uint64_t total_frames;
    uint64_t total_fields;
    uint64_t total_hsyncs;
    uint64_t total_vsyncs;
    uint64_t total_vsync_candidates;
    uint64_t total_fields_accepted;
    uint64_t total_fields_rejected;
    uint64_t total_reject_cadence;
    uint64_t total_reject_parity;
    uint64_t total_reject_line_map;
    uint64_t total_color_losses;
    /* Cumulative CPU includes worker dispatch/check overhead at completion
     * snapshots. Sleeping contributes no CPU. Fallback is conservative wall. */
    double processing_cpu_ms;
    double processing_wall_ms;
    double audio_cpu_ms,audio_wall_ms;
    uint64_t audio_blocks_dropped;
    bool audio_active,audio_enabled;
    double budget_wait_ms;
    double cpu_percent; /* Same wall interval as fps; 100% is one logical core. */
    bool paused;
    bool budget_waiting;
    bool cpu_time_is_wall;
    bool priority_lowered; /* OS accepted the per-thread background request. */
} gui_rf_preview_stats_t;

/* Shared by all workers in this process, default 1.0 logical core. Accepts
 * 0.05..8.0; actual DSP demand depends on profile, hardware and signal;
 * 0 disables pacing for offline maximum-speed validation only.
 * This is a soft processing budget, not an OS quota. A running 65536-source-
 * sample block cannot be interrupted; short intervals can exceed the rate due
 * to block granularity and OS accounting/scheduling. Windows keeps at most
 * 15.625ms global CPU credit, but each worker's CPU counter can independently
 * lag by an OS accounting quantum; this is not a hard aggregate burst bound.
 * FIFO arbitration admits ceil(budget) concurrent blocks (maximum eight),
 * sharing a four-ms CPU base token capacity plus one bounded coarse-accounting
 * quantum per live registered consumer (maximum 64 counters) and active-block
 * costs. Registration/unregistration never forgives debt. A valid zero CPU
 * snapshot retains elapsed credit for its later measured quantum; no wall time
 * is charged as CPU while the OS counter works. Producer submit never waits.
 * Configuration is safe while workers run. */
bool gui_rf_preview_worker_set_budget(double logical_cores);

gui_rf_preview_worker_t *gui_rf_preview_worker_create(gui_rf_format_t format);
gui_rf_preview_worker_t *gui_rf_preview_worker_create_profile(gui_rf_profile_t profile);
void gui_rf_preview_worker_set_profile(gui_rf_preview_worker_t *worker, gui_rf_profile_t profile);
/* Defaults true for offline probes. GUI enables only its selected audible
 * HiFi route. Changing it invalidates queued/in-flight audio immediately. */
void gui_rf_preview_worker_set_audio_enabled(gui_rf_preview_worker_t *worker,bool enabled);
/* Install once before publishing/submitting this worker. Called only by its
 * audio consumer thread; NULL/zero frames flushes on a stream discontinuity. */
void gui_rf_preview_worker_set_audio_callback(gui_rf_preview_worker_t *worker,
                                              gui_rf_preview_audio_callback_t callback,
                                              void *context);
void gui_rf_preview_worker_destroy(gui_rf_preview_worker_t *worker);
void gui_rf_preview_worker_set_format(gui_rf_preview_worker_t *worker,
                                     gui_rf_format_t format);
/* Frame display mode: Active (default) or Full (VBI + burst with decoded
 * chroma); applies on the DSP thread between input blocks. Survives
 * worker resets and rebinds. */
void gui_rf_preview_worker_set_frame_mode(gui_rf_preview_worker_t *worker,
                                         bool full_frame);
void gui_rf_preview_worker_reset(gui_rf_preview_worker_t *worker);
/* Atomic, nonblocking pressure control. Paused submissions are rejected before
 * copying. Worker drains old input/reset state, preserving the latest complete
 * frame for a frozen UI; resume reacquires from new input. */
void gui_rf_preview_worker_set_paused(gui_rf_preview_worker_t *worker, bool paused);
/* Never waits for queue space or a decoder/render mutex. A rejected block is
 * reported as a gap at the next accepted block. Caller retains input ownership. */
bool gui_rf_preview_worker_submit(gui_rf_preview_worker_t *worker,
                                  const int16_t *samples, size_t count,
                                  const gui_rf_block_info_t *info);
/* Copies the newest complete interlaced frame, if generation has changed.
 * Destination capacity is bytes, at least MAX_WIDTH*MAX_HEIGHT*4. */
/* Optional borrowed RGBA32 frame sink. The callback is invoked on the RF
 * preview worker after a decoded frame; it must be fast and nonblocking.
 * Set context lifetime must cover worker callbacks; stop/join before freeing. */
void gui_rf_preview_worker_set_monitor_frame_sink(gui_rf_preview_worker_t *worker,
    void (*callback)(void *, const uint8_t *, unsigned, unsigned, uint64_t),
    void *context);

bool gui_rf_preview_worker_take_frame(gui_rf_preview_worker_t *worker,
                                      uint8_t *rgba, size_t capacity,
                                      uint64_t *generation, int *width,
                                      int *height, gui_rf_preview_stats_t *stats);
void gui_rf_preview_worker_get_stats(gui_rf_preview_worker_t *worker,
                                    gui_rf_preview_stats_t *stats);

/* Offline/test callers only. Waits for all previously submitted slots to finish
 * decoding and publishing. The capture producer must never call this function. */
bool gui_rf_preview_worker_wait_idle(gui_rf_preview_worker_t *worker,
                                     uint32_t timeout_ms);

#endif
