/*
 * MISRC GUI - Display Thread
 *
 * Dedicated thread for display processing (CVBS, oscilloscope, FFT).
 * Decouples display processing from the critical recording path.
 *
 * Architecture:
 *   Extraction Thread --[BUF_DISPLAY]--> Display Thread --> Render Thread
 *
 * The display thread reads from BUF_DISPLAY (lossy buffer) and performs:
 *   - CVBS video decoding
 *   - Oscilloscope sample preparation (trigger, resampling)
 *   - Statistics updates for display
 *
 * If the display thread is slow, frames are dropped from BUF_DISPLAY
 * without affecting the recording path.
 */

#ifndef GUI_DISPLAY_THREAD_H
#define GUI_DISPLAY_THREAD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/* Forward declarations */
struct gui_app;
struct buffer_manager;

/*
 * Display sample buffer - holds prepared samples for render thread
 */
#define DISPLAY_SAMPLE_COUNT 65536

typedef struct {
    int16_t *samples_a;          /* Channel A samples (DISPLAY_SAMPLE_COUNT) */
    int16_t *samples_b;          /* Channel B samples (DISPLAY_SAMPLE_COUNT) */
    size_t sample_count;         /* Valid A-side samples in the latest publish */
    size_t sample_count_b;       /* Valid B-side samples (independent mode) */
    uint32_t sample_rate;        /* Sample rate for display calculations */
    atomic_bool ready;           /* New samples available for render thread */
    atomic_uint_fast64_t frame;  /* Frame counter for debugging */
} display_samples_t;

/*
 * Display thread state
 */
typedef struct display_thread {
    /* Thread handle */
    void *thread;                /* thrd_t */
    atomic_bool running;         /* Thread running flag */
    atomic_bool stop_requested;  /* Request thread to stop */

    /* Buffer manager reference */
    struct buffer_manager *bufmgr;

    /* App reference (for CVBS, stats, etc.) */
    struct gui_app *app;

    /* Output samples for render thread */
    display_samples_t samples;

    /* Statistics */
    atomic_uint_fast64_t frames_processed;
    atomic_uint_fast64_t frames_dropped;  /* Frames we couldn't process in time */
} display_thread_t;

/*
 * Initialize display thread state (allocates buffers)
 *
 * @param dt    Display thread state
 * @return 0 on success, -1 on error
 */
int gui_display_thread_init(display_thread_t *dt);

/*
 * Cleanup display thread state (frees buffers)
 *
 * @param dt    Display thread state
 */
void gui_display_thread_cleanup(display_thread_t *dt);

/*
 * Start the display thread
 *
 * @param dt        Display thread state
 * @param app       Application state (for CVBS decoders, etc.)
 * @param bufmgr    Buffer manager (for reading BUF_DISPLAY)
 * @return 0 on success, -1 on error
 */
int gui_display_thread_start(display_thread_t *dt,
                              struct gui_app *app,
                              struct buffer_manager *bufmgr);

/*
 * Stop the display thread (waits for thread to exit)
 *
 * @param dt    Display thread state
 */
void gui_display_thread_stop(display_thread_t *dt);

/*
 * Check if new display samples are available
 *
 * Called by render thread to check if display thread has new data.
 *
 * @param dt    Display thread state
 * @return true if new samples available
 */
bool gui_display_thread_samples_ready(display_thread_t *dt);

/*
 * Acquire display samples for rendering
 *
 * Called by render thread. Returns pointers to the sample buffers.
 * The ready flag is cleared atomically.
 *
 * @param dt        Display thread state
 * @param out_a     Output pointer for channel A samples
 * @param out_b     Output pointer for channel B samples
 * @param out_count Output for number of samples
 * @return true if samples acquired, false if no new samples
 */
bool gui_display_thread_acquire_samples(display_thread_t *dt,
                                         const int16_t **out_a,
                                         const int16_t **out_b,
                                         size_t *out_count);

/*
 * Get display thread statistics
 */
void gui_display_thread_get_stats(display_thread_t *dt,
                                   uint64_t *frames_processed,
                                   uint64_t *frames_dropped);

#endif /* GUI_DISPLAY_THREAD_H */
