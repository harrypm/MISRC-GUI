/*
 * MISRC GUI - Display Thread Implementation
 *
 * Dedicated thread for display processing, decoupled from recording path.
 */

#include "gui_display_thread.h"
#include "../core/gui_app.h"
#include "../signal/gui_cvbs.h"
#include "../visualization/gui_histogram_panel.h"
#include "../visualization/gui_oscilloscope.h"
#include "../visualization/panel_interface.h"
#include "../../common/buffer_manager.h"
#include "../../common/buffer.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* External do_exit flag */
extern atomic_int do_exit;

/* Frame size: 65536 samples per channel, 2 bytes per sample, 2 channels */
#define DISPLAY_FRAME_SAMPLES 65536
#define DISPLAY_FRAME_SIZE (DISPLAY_FRAME_SAMPLES * sizeof(int16_t) * 2)
#define DISPLAY_CHANNEL_SIZE (DISPLAY_FRAME_SAMPLES * sizeof(int16_t))

/*
 * Display thread main function
 */
static int display_thread_func(void *ctx) {
    display_thread_t *dt = (display_thread_t *)ctx;
    gui_app_t *app = dt->app;
    buffer_manager_t *bufmgr = dt->bufmgr;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    fprintf(stderr, "[DISPLAY] Display thread started\n");

    while (!atomic_load(&dt->stop_requested) && !atomic_load(&do_exit)) {
        /* Read from display buffer with timeout */
        void *buf = bufmgr_read_begin(bufmgr, BUF_DISPLAY, DISPLAY_FRAME_SIZE, 50);

        if (!buf) {
            /* Timeout - no new data, loop and check for exit */
            continue;
        }

        /* Split into channels */
        const int16_t *samples_a = (const int16_t *)buf;
        const int16_t *samples_b = (const int16_t *)((uint8_t *)buf + DISPLAY_CHANNEL_SIZE);

        /* Process all panels via unified vtable dispatch
         * This handles histogram, FFT, CVBS, and any other panel types that process raw samples.
         */
        uint32_t sample_rate = atomic_load(&app->sample_rate);
        panel_process_all(app, samples_a, samples_b, DISPLAY_FRAME_SAMPLES, sample_rate);

        /* Copy samples to output buffer for render thread */
        memcpy(dt->samples.samples_a, samples_a, DISPLAY_CHANNEL_SIZE);
        memcpy(dt->samples.samples_b, samples_b, DISPLAY_CHANNEL_SIZE);
        dt->samples.sample_count = DISPLAY_FRAME_SAMPLES;
        dt->samples.sample_rate = atomic_load(&app->sample_rate);

        /* Mark read as complete - signals space available */
        bufmgr_read_end(bufmgr, BUF_DISPLAY, DISPLAY_FRAME_SIZE);

        /* Signal render thread that new samples are ready */
        atomic_store(&dt->samples.ready, true);
        atomic_fetch_add(&dt->samples.frame, 1);
        atomic_fetch_add(&dt->frames_processed, 1);
    }

    atomic_store(&dt->running, false);
    fprintf(stderr, "[DISPLAY] Display thread exiting\n");
    return 0;
}

/*
 * Initialize display thread state
 */
int gui_display_thread_init(display_thread_t *dt) {
    if (!dt) return -1;

    memset(dt, 0, sizeof(*dt));

    /* Allocate sample buffers (32-byte aligned for SIMD) */
    dt->samples.samples_a = (int16_t *)aligned_alloc(32, DISPLAY_CHANNEL_SIZE);
    dt->samples.samples_b = (int16_t *)aligned_alloc(32, DISPLAY_CHANNEL_SIZE);

    if (!dt->samples.samples_a || !dt->samples.samples_b) {
        fprintf(stderr, "[DISPLAY] Failed to allocate sample buffers\n");
        gui_display_thread_cleanup(dt);
        return -1;
    }

    memset(dt->samples.samples_a, 0, DISPLAY_CHANNEL_SIZE);
    memset(dt->samples.samples_b, 0, DISPLAY_CHANNEL_SIZE);

    atomic_store(&dt->samples.ready, false);
    atomic_store(&dt->samples.frame, 0);
    atomic_store(&dt->running, false);
    atomic_store(&dt->stop_requested, false);
    atomic_store(&dt->frames_processed, 0);
    atomic_store(&dt->frames_dropped, 0);

    fprintf(stderr, "[DISPLAY] Display thread state initialized\n");
    return 0;
}

/*
 * Cleanup display thread state
 */
void gui_display_thread_cleanup(display_thread_t *dt) {
    if (!dt) return;

    /* Ensure thread is stopped */
    gui_display_thread_stop(dt);

    /* Free sample buffers */
    if (dt->samples.samples_a) {
        aligned_free(dt->samples.samples_a);
        dt->samples.samples_a = NULL;
    }
    if (dt->samples.samples_b) {
        aligned_free(dt->samples.samples_b);
        dt->samples.samples_b = NULL;
    }

    fprintf(stderr, "[DISPLAY] Display thread state cleaned up\n");
}

/*
 * Start the display thread
 */
int gui_display_thread_start(display_thread_t *dt,
                              struct gui_app *app,
                              struct buffer_manager *bufmgr) {
    if (!dt || !app || !bufmgr) return -1;

    if (atomic_load(&dt->running)) {
        fprintf(stderr, "[DISPLAY] Thread already running\n");
        return 0;
    }

    /* Ensure BUF_DISPLAY is initialized */
    if (bufmgr_ensure_init(bufmgr, BUF_DISPLAY) < 0) {
        fprintf(stderr, "[DISPLAY] Failed to initialize display buffer\n");
        return -1;
    }

    dt->app = app;
    dt->bufmgr = bufmgr;
    atomic_store(&dt->stop_requested, false);
    atomic_store(&dt->running, true);

    /* Reset statistics */
    atomic_store(&dt->frames_processed, 0);
    atomic_store(&dt->frames_dropped, 0);

    /* Create thread */
    thrd_t *thread = (thrd_t *)malloc(sizeof(thrd_t));
    if (!thread) {
        atomic_store(&dt->running, false);
        return -1;
    }

    if (thrd_create_with_priority(thread,
                                  display_thread_func,
                                  dt,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[DISPLAY] Failed to create display thread\n");
        free(thread);
        atomic_store(&dt->running, false);
        return -1;
    }

    dt->thread = thread;
    fprintf(stderr, "[DISPLAY] Display thread started\n");
    return 0;
}

/*
 * Stop the display thread
 */
void gui_display_thread_stop(display_thread_t *dt) {
    if (!dt || !dt->thread) return;

    if (!atomic_load(&dt->running)) {
        return;
    }

    /* Signal thread to stop */
    atomic_store(&dt->stop_requested, true);

    /* Wait for thread to exit */
    thrd_t *thread = (thrd_t *)dt->thread;
    thrd_join(*thread, NULL);

    free(thread);
    dt->thread = NULL;
    atomic_store(&dt->running, false);

    fprintf(stderr, "[DISPLAY] Display thread stopped (processed %llu frames)\n",
            (unsigned long long)atomic_load(&dt->frames_processed));
}

/*
 * Check if new display samples are available
 */
bool gui_display_thread_samples_ready(display_thread_t *dt) {
    if (!dt) return false;
    return atomic_load(&dt->samples.ready);
}

/*
 * Acquire display samples for rendering
 */
bool gui_display_thread_acquire_samples(display_thread_t *dt,
                                         const int16_t **out_a,
                                         const int16_t **out_b,
                                         size_t *out_count) {
    if (!dt) return false;

    /* Atomically check and clear ready flag */
    if (!atomic_exchange(&dt->samples.ready, false)) {
        return false;
    }

    if (out_a) *out_a = dt->samples.samples_a;
    if (out_b) *out_b = dt->samples.samples_b;
    if (out_count) *out_count = dt->samples.sample_count;

    return true;
}

/*
 * Get display thread statistics
 */
void gui_display_thread_get_stats(display_thread_t *dt,
                                   uint64_t *frames_processed,
                                   uint64_t *frames_dropped) {
    if (!dt) return;

    if (frames_processed) {
        *frames_processed = atomic_load(&dt->frames_processed);
    }
    if (frames_dropped) {
        *frames_dropped = atomic_load(&dt->frames_dropped);
    }
}
