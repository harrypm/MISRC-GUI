/*
 * MISRC GUI - Simulated Device (Test Signal Generator)
 *
 * Streams the Test Gen signal through the FULL capture pipeline like a live
 * capture (waveform, FFT, Video panel decode, VHS FM preview, recording):
 *
 *   Channel A: hacktv-accurate composite CVBS (gui_testgen_frame)
 *   Channel B: VHS-family tape RF (gui_testgen_rf_frame) - FM luma plus
 *              color-under QAM (PAL/NTSC) or the divided-by-four SECAM
 *              chroma FM, matching the RF preview decoder profiles so the
 *              VHS FM panel decodes it live.
 *
 * Both channels follow app->settings.video_system (0=PAL, 1=NTSC,
 * 2=SECAM, 3=MESECAM); a change detected in the capture loop regenerates
 * the frames and restarts the stream. The tape format
 * (app->settings.gen_tape_format) parameterizes the RF modulation ONLY:
 * a format change regenerates and restarts the RF stream while CH1's
 * composite stream keeps flowing continuously - the format cycle is
 * decoupled from the CVBS channel (user request 2026-11-03). TWO frames
 * per system are pre-generated and streamed alternately: SECAM's D'r/D'b
 * line cadence and ident type alternate every
 * frame (a single repeated frame would hold one cadence and break field
 * identification).
 */

#include "gui_simulated.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../signal/gui_testgen.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

//-----------------------------------------------------------------------------
// Raw Format Encoding
//-----------------------------------------------------------------------------

// Encode int16_t samples to the raw 32-bit capture format
// This mirrors the decoding done in extract.c
// Raw format:
//   Bits 0-11:  Channel A (12-bit, stored as 2047 - sample)
//   Bits 12-19: AUX data (8 bits, we set to 0)
//   Bits 20-31: Channel B (12-bit, stored as 2047 - sample)
static inline uint32_t encode_raw_sample(int16_t sample_a, int16_t sample_b) {
    // Clamp samples to 12-bit signed range
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;

    // Encode: store as (2047 - sample) to match extract.c decoding
    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    uint32_t aux = 0;  // No AUX data during simulation

    return ch_a | (aux << 12) | (ch_b << 20);
}

//-----------------------------------------------------------------------------
// Simulated Capture Thread
//-----------------------------------------------------------------------------

static int simulated_capture_thread(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;

    int16_t *buf_a = (int16_t *)malloc(SIM_BUFFER_SIZE * sizeof(int16_t));
    int16_t *buf_b = (int16_t *)malloc(SIM_BUFFER_SIZE * sizeof(int16_t));

    if (!buf_a || !buf_b) {
        fprintf(stderr, "[SIM] Failed to allocate buffers\n");
        free(buf_a);
        free(buf_b);
        return -1;
    }

    /* Pre-generated frames. cvbs_frame and rf_frame each hold TWO frames
     * (indices 0 and 1) streamed alternately; both pairs have the same
     * sample count. */
    int sim_system = -1;   /* force first generation */
    int sim_format = -1;   /* force first generation */
    int16_t *cvbs_frame = NULL;
    int16_t *rf_frame = NULL;
    size_t frame_samples = 0;      /* samples in ONE frame */
    size_t stream_samples = 0;     /* total across both frames */
    /* Per-channel streaming positions. Both frame pairs have the same
     * length, but the positions are kept independent so a tape-FORMAT
     * change (RF-only) restarts just the RF stream - CH1's composite
     * signal keeps flowing without a discontinuity. A system change
     * regenerates both pairs and resets both positions together. */
    uint64_t cvbs_stream_pos = 0;  /* composite position within its pair */
    uint64_t rf_stream_pos = 0;    /* RF position within its pair */

    fprintf(stderr, "[SIM] Simulated capture thread started at %d MSPS (writing to BUF_CAPTURE_RF)\n", SIM_SAMPLE_RATE / 1000000);
    fprintf(stderr, "[SIM] Channel A: hacktv-accurate CVBS (video_system setting)\n");
    fprintf(stderr, "[SIM] Channel B: VHS-family tape RF, FM luma + chroma (video_system setting)\n");

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, SIM_SAMPLE_RATE);

    uint64_t batch_count = 0;

    while (atomic_load(&app->sim_running)) {
        /* System (PAL/NTSC/SECAM/MESECAM) and tape format follow the app's
         * settings. A system change regenerates BOTH channels; a format
         * change regenerates the RF frames only (see the branches below). */
        int want_system = gui_testgen_clamp_system(app->settings.video_system);
        int want_format = gui_testgen_clamp_format(app->settings.gen_tape_format);
        /* Channel mapping: the extraction thread swaps raw A/B for MISRC-mode
         * captures (the display's CH1 is fed from raw slot B), and this
         * device emulates the MISRC capture chain. Write the generator
         * channels into the raw slots that land on the DISPLAY's CH1/CH2
         * after that mapping: CH1 = composite CVBS, CH2 = tape RF. Mirrors
         * gui_extract.c's swap logic, including the recording-time runtime
         * latch and the v1.5/v2.5 wiring override. */
        bool swap_ab = app->is_recording ? app->capture_mode_runtime_misrc
                                         : app->user_capture_mode_misrc;
        if (swap_ab && app->settings.misrc_v15_v25_ab_swap) {
            swap_ab = false;
        }

        if (want_system != sim_system) {
            /* System change (or first batch): BOTH channels' signals
             * change - composite scan geometry and RF modulation alike -
             * so regenerate both pairs and restart both streams in step. */
            sim_system = want_system;
            sim_format = want_format;
            size_t need = gui_testgen_frame_samples(
                (gui_testgen_system_t)sim_system, SIM_SAMPLE_RATE);
            if (need > 0) {
                int16_t *grown_cvbs = (int16_t *)realloc(cvbs_frame,
                                                    2 * need * sizeof(int16_t));
                int16_t *grown_rf = (int16_t *)realloc(rf_frame,
                                                    2 * need * sizeof(int16_t));
                if (grown_cvbs && grown_rf) {
                    cvbs_frame = grown_cvbs;
                    rf_frame = grown_rf;
                    frame_samples = need;
                    stream_samples = 2 * need;
                    gui_testgen_frame(
                        (gui_testgen_system_t)sim_system, cvbs_frame, need,
                        SIM_SAMPLE_RATE, 0);
                    gui_testgen_frame(
                        (gui_testgen_system_t)sim_system, cvbs_frame + need,
                        need, SIM_SAMPLE_RATE, 1);
                    gui_testgen_rf_frame(
                        (gui_testgen_system_t)sim_system,
                        (gui_testgen_format_t)sim_format, rf_frame, need,
                        SIM_SAMPLE_RATE, 0);
                    gui_testgen_rf_frame(
                        (gui_testgen_system_t)sim_system,
                        (gui_testgen_format_t)sim_format, rf_frame + need,
                        need, SIM_SAMPLE_RATE, 1);
                    cvbs_stream_pos = 0;
                    rf_stream_pos = 0;
                    fprintf(stderr, "[SIM] Regenerated %s %s frames "
                            "(2x %zu samples at %u MSPS): CH1 CVBS + CH2 RF\n",
                            gui_testgen_system_name((gui_testgen_system_t)sim_system),
                            gui_testgen_format_name((gui_testgen_format_t)sim_format),
                            need, SIM_SAMPLE_RATE / 1000000);
                } else {
                    free(grown_cvbs);
                    free(grown_rf);
                    /* Keep any previous valid pair; on the first failure
                     * there is nothing to stream. */
                    if (!cvbs_frame || !rf_frame) {
                        frame_samples = 0;
                        stream_samples = 0;
                    }
                }
            }
        } else if (want_format != sim_format) {
            /* Format-only change: the tape format parameterizes the RF
             * modulation ONLY. Regenerate just the RF frame pair and
             * restart its stream; CH1's composite pair is untouched (same
             * system, same content) and keeps streaming continuously -
             * the format cycle never disturbs the CVBS channel. */
            sim_format = want_format;
            if (frame_samples > 0 && rf_frame) {
                gui_testgen_rf_frame(
                    (gui_testgen_system_t)sim_system,
                    (gui_testgen_format_t)sim_format, rf_frame,
                    frame_samples, SIM_SAMPLE_RATE, 0);
                gui_testgen_rf_frame(
                    (gui_testgen_system_t)sim_system,
                    (gui_testgen_format_t)sim_format, rf_frame + frame_samples,
                    frame_samples, SIM_SAMPLE_RATE, 1);
                rf_stream_pos = 0;
                fprintf(stderr, "[SIM] Regenerated %s %s RF frames "
                        "(2x %zu samples at %u MSPS): CH2 RF only, "
                        "CH1 CVBS continuous\n",
                        gui_testgen_system_name((gui_testgen_system_t)sim_system),
                        gui_testgen_format_name((gui_testgen_format_t)sim_format),
                        frame_samples, SIM_SAMPLE_RATE / 1000000);
            }
        }

        for (int i = 0; i < SIM_BUFFER_SIZE; i++) {
            /* Channel A (CVBS): 2170 scale - measured from the real EBU
             * HackDAC reference FLAC (per-bar plateau analysis of the
             * decoded s16 data, converted to the 12-bit tool domain):
             *   real sync tip    = -10500 int16 -> reads -0.32
             *   real white bar   = +24300 int16 -> reads +0.74
             *   real chroma peak = clips at +0.99 (8-bit MAX)
             * buf_a = hacktv_value * 2170 reproduces all of them.
             * Channel B (tape RF): ORIGINAL 1024 scale - DO NOT CHANGE.
             * The RF decode is frequency/envelope-based (the decoder's
             * gates are relative), so the absolute scale is free; 1024
             * keeps the ~0.72-0.87 generator amplitude inside the 12-bit
             * domain with headroom. */
            if (stream_samples > 0) {
                size_t at_cvbs = (size_t)(cvbs_stream_pos % (uint64_t)stream_samples);
                size_t at_rf = (size_t)(rf_stream_pos % (uint64_t)stream_samples);
                int16_t cvbs_s = (int16_t)(((double)cvbs_frame[at_cvbs] / 32767.0) * 2170.0);
                int16_t rf_s = (int16_t)(((double)rf_frame[at_rf] / 32767.0) * 1024.0);
                if (swap_ab) {
                    buf_a[i] = rf_s;   /* raw A -> display CH2 (tape RF) */
                    buf_b[i] = cvbs_s; /* raw B -> display CH1 (CVBS) */
                } else {
                    buf_a[i] = cvbs_s;
                    buf_b[i] = rf_s;
                }
                cvbs_stream_pos++;
                rf_stream_pos++;
            } else {
                buf_a[i] = 0;
                buf_b[i] = 0;
            }
        }

        // Encode generated samples to raw 32-bit format and write to BUF_CAPTURE_RF
        // The extraction thread will read this, update statistics, display, CVBS, and recording
        size_t raw_bytes = SIM_BUFFER_SIZE * sizeof(uint32_t);
        uint32_t *raw_buf = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                                           raw_bytes, NULL);
        if (raw_buf) {
            // Encode int16 samples to raw format
            for (int i = 0; i < SIM_BUFFER_SIZE; i++) {
                raw_buf[i] = encode_raw_sample(buf_a[i], buf_b[i]);
            }
            bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, raw_bytes);
        } else {
            // Buffer full - this shouldn't happen often with proper sizing
            fprintf(stderr, "[SIM] Warning: BUF_CAPTURE_RF full, frame dropped\n");
        }

        // Update timestamp (extraction thread updates other statistics)
        atomic_store(&app->last_callback_time_ms, get_time_ms());

        batch_count++;
        thrd_sleep_ms(SIM_UPDATE_INTERVAL_MS);
    }

    fprintf(stderr, "[SIM] Simulated capture thread exiting after %llu batches\n",
            (unsigned long long)batch_count);

    free(buf_a);
    free(buf_b);
    free(cvbs_frame);
    free(rf_frame);

    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_simulated_start(gui_app_t *app) {
    fprintf(stderr, "[SIM] Starting simulated capture\n");
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    // Reset statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, SIM_SAMPLE_RATE);
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    // Reset display buffers
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Start thread
    atomic_store(&app->sim_running, true);
    app->is_capturing = true;

    // Start extraction thread - reads from BUF_CAPTURE_RF, writes to BUF_DISPLAY
    // Also handles statistics, peak detection, and recording if enabled
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[SIM] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        atomic_store(&app->sim_running, false);
        app->is_capturing = false;
        return -1;
    }

    // Start display thread - processes BUF_DISPLAY for oscilloscope/CVBS
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[SIM] Failed to start display thread (non-fatal)\n");
            // Non-fatal - display will use legacy path
        }
    }

    // Start simulated capture thread - generates samples and writes to BUF_CAPTURE_RF
    thrd_t thread;
    if (thrd_create(&thread, simulated_capture_thread, app) != thrd_success) {
        fprintf(stderr, "[SIM] Failed to create simulated capture thread\n");
        gui_extract_stop();
        if (app->display_thread) {
            gui_display_thread_stop(app->display_thread);
        }
        atomic_store(&app->sim_running, false);
        app->is_capturing = false;
        return -1;
    }
    app->sim_thread = (void *)(uintptr_t)thread;

    gui_app_set_status(app, "Simulated capture running");

    return 0;
}

void gui_simulated_stop(gui_app_t *app) {
    if (!atomic_load(&app->sim_running)) return;

    fprintf(stderr, "[SIM] Stopping simulated capture\n");

    // Set is_capturing to false BEFORE stopping extraction thread
    // The extraction thread checks this flag to know when to exit
    app->is_capturing = false;

    atomic_store(&app->sim_running, false);

    // Stop simulated capture thread first (it writes to BUF_CAPTURE_RF)
    if (app->sim_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)app->sim_thread;
        thrd_join(thread, NULL);
        app->sim_thread = NULL;
    }

    // Stop display thread (reads from BUF_DISPLAY written by extraction)
    if (app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }

    // Stop extraction thread (reads BUF_CAPTURE_RF, writes BUF_DISPLAY)
    gui_extract_stop();

    atomic_store(&app->stream_synced, false);

    gui_app_set_status(app, "Simulated capture stopped");
}

bool gui_simulated_is_running(gui_app_t *app) {
    return atomic_load(&app->sim_running);
}
