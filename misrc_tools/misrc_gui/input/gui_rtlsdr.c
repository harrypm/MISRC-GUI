/*
 * MISRC GUI - RTL-SDR (RTL2832) Device Support Implementation
 *
 * Local USB via librtlsdr. Mirrors the FX3 backend lifecycle
 * (enumerate/open/start/stop/is_running) and the playback/CXADC capture-feed
 * pattern: pack 8-bit I/Q into the hsdaoh 32-bit capture format and write
 * BUF_CAPTURE_RF, letting the shared extraction thread produce display +
 * record + stats. Demodulation is handled by the separate Demod panel.
 */

#ifdef ENABLE_RTLSDR

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

// rtl-sdr.h only pulls stdint.h + rtl-sdr_export.h (no libusb.h, no Windows
// header conflict), so it is safe to include before raylib/gui headers.
#include <rtl-sdr.h>

#include "gui_rtlsdr.h"
#include "../output/gui_record.h"
#ifdef __ANDROID__
#include <unistd.h>
#include "../../common/libusb_compat.h"
#include "android_rtlsdr_usb.h"
#endif
#include "../signal/gui_cvbs.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

/* do_exit is defined in the core capture module; declared extern here like gui_fx3.c. */
extern atomic_int do_exit;

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------

// One rtlsdr_read_sync call reads this many bytes (must be a multiple of 512).
// 131072 bytes = 65536 interleaved I/Q pairs = 65536 packed uint32 samples.
#define RTL_READ_BYTES      (131072)

#define RTL_DEFAULT_RATE_HZ 2400000U
#ifdef __ANDROID__
// One full batch takes up to 292 ms at the lowest supported sample rate.
#define RTL_USB_READ_TIMEOUT_MS 500u
#endif
#define RTL_TUNE_STEP (UINT64_C(1) << 32)

//-----------------------------------------------------------------------------
// State
//-----------------------------------------------------------------------------

static rtlsdr_dev_t *s_rtlsdr_dev = NULL;
#ifdef __ANDROID__
static int s_rtlsdr_android_fd = -1;
#endif
static atomic_bool s_rtlsdr_running = false;
static atomic_uint s_rtlsdr_actual_rate;
static atomic_uint_fast64_t s_rtlsdr_record_tuning;
static void *s_rtlsdr_thread = NULL;

//-----------------------------------------------------------------------------
// Raw format encoding (mirrors gui_playback.c / extract.c decoding)
//   Bits 0-11:  Channel A (12-bit, stored as 2047 - sample)
//   Bits 12-19: AUX data (8 bits, 0)
//   Bits 20-31: Channel B (12-bit, stored as 2047 - sample)
//-----------------------------------------------------------------------------

static inline uint32_t rtl_encode_raw_sample(int16_t sample_a, int16_t sample_b) {
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;
    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    uint32_t aux = 0;
    return ch_a | (aux << 12) | (ch_b << 20);
}

//-----------------------------------------------------------------------------
// Enumeration
//-----------------------------------------------------------------------------

int gui_rtlsdr_enumerate(rtlsdr_device_info_t *devices, int max_devices) {
    if (!devices || max_devices <= 0) return 0;

#ifdef __ANDROID__
    android_rtlsdr_usb_device_t attached[ANDROID_RTL_USB_MAX_DEVICES];
    int capacity = max_devices < ANDROID_RTL_USB_MAX_DEVICES
        ? max_devices : ANDROID_RTL_USB_MAX_DEVICES;
    int count = android_rtlsdr_list_usb_devices(attached, capacity);
    for (int i = 0; i < count; ++i) {
        const char *name = rtlsdr_get_device_name_by_usb_id(attached[i].vid, attached[i].pid);
        devices[i].index = attached[i].id;
        devices[i].serial[0] = '\0';
        devices[i].blog_v4 = false;
        snprintf(devices[i].name, sizeof(devices[i].name), "%s (USB %d)",
                 name ? name : "RTL-SDR", attached[i].id);
    }
    return count;
#else
    uint32_t count = rtlsdr_get_device_count();
    int added = 0;
    for (uint32_t i = 0; i < count && added < max_devices; i++) {
        rtlsdr_device_info_t *dst = &devices[added];
        dst->index = (int)i;

        const char *name = rtlsdr_get_device_name(i);
        snprintf(dst->name, sizeof(dst->name), "%s", name ? name : "RTL-SDR");

        dst->serial[0] = '\0';
        dst->blog_v4 = false;
        char manufact[256] = {0};
        char product[256] = {0};
        char serial[256] = {0};
        if (rtlsdr_get_device_usb_strings(i, manufact, product, serial) == 0) {
            snprintf(dst->serial, sizeof(dst->serial), "%s", serial);
            dst->blog_v4 = strcmp(manufact, "RTLSDRBlog") == 0 && strcmp(product, "Blog V4") == 0;
        }
        added++;
    }
    return added;
#endif
}

//-----------------------------------------------------------------------------
// Open / Close
//-----------------------------------------------------------------------------

int gui_rtlsdr_open(gui_app_t *app, int device_index) {
    if (!app) return -1;
    gui_rtlsdr_close(app);

#ifdef __ANDROID__
    s_rtlsdr_android_fd = android_rtlsdr_open_usb_fd(device_index);
    if (s_rtlsdr_android_fd < 0) {
        fprintf(stderr, "[RTL-SDR] USB device %d is absent or permission is unavailable\n",
                device_index);
        return -1;
    }
    int r = rtlsdr_open_fd(&s_rtlsdr_dev, s_rtlsdr_android_fd);
#else
    int r = rtlsdr_open(&s_rtlsdr_dev, (uint32_t)device_index);
#endif
    if (r < 0 || !s_rtlsdr_dev) {
        fprintf(stderr, "[RTL-SDR] Failed to open device %d (err %d)\n", device_index, r);
        gui_rtlsdr_close(app);
        return -1;
    }

    uint32_t rate = app->settings.rtlsdr_sample_rate_hz;
    if (rate == 0) rate = RTL_DEFAULT_RATE_HZ;
    const int rate_result = rtlsdr_set_sample_rate(s_rtlsdr_dev, rate);
    if (rate_result < 0) {
        fprintf(stderr, "[RTL-SDR] Warning: set_sample_rate(%u) failed\n", rate);
    }

    if (rtlsdr_set_center_freq(s_rtlsdr_dev, (uint32_t)app->settings.rtlsdr_freq_hz) < 0) {
        fprintf(stderr, "[RTL-SDR] Warning: set_center_freq(%llu) failed\n",
                (unsigned long long)app->settings.rtlsdr_freq_hz);
    }

    // AGC: rtlsdr_set_agc_mode(1=on,0=off). Gain mode: 0=auto,1=manual.
    rtlsdr_set_agc_mode(s_rtlsdr_dev, app->settings.rtlsdr_agc ? 1 : 0);
    if (app->settings.rtlsdr_gain_mode == 1) {
        const int mode_result = rtlsdr_set_tuner_gain_mode(s_rtlsdr_dev, 1);
        const int gain_result = rtlsdr_set_tuner_gain(s_rtlsdr_dev, app->settings.rtlsdr_gain_tenths_db);
#ifdef ENABLE_RTLSDR_BLOG_V4_VGA
        if (rtlsdr_is_blog_v4(s_rtlsdr_dev)) {
            const int index = app->settings.rtlsdr_vga_index;
            r = mode_result < 0 ? mode_result : gain_result;
            if (r == 0) r = rtlsdr_set_blog_v4_vga_index(s_rtlsdr_dev, index);
            if (r < 0) {
                fprintf(stderr, "[RTL-SDR] Failed to apply Blog V4 manual VGA index 0x%02X (err %d)\n",
                        (unsigned)index, r);
                gui_rtlsdr_close(app);
                return GUI_RTLSDR_VGA_FAILED;
            }
            fprintf(stderr, "[RTL-SDR] Blog V4 manual VGA index: 0x%02X\n", (unsigned)index);
        }
#else
        (void)mode_result;
        (void)gain_result;
#endif
    } else {
        rtlsdr_set_tuner_gain_mode(s_rtlsdr_dev, 0);  // auto
    }

    rtlsdr_set_offset_tuning(s_rtlsdr_dev, app->settings.rtlsdr_offset_corr ? 1 : 0);
    rtlsdr_reset_buffer(s_rtlsdr_dev);
    // The getter can return a cached rate even after a failed USB register write.
    atomic_store(&s_rtlsdr_actual_rate, rate_result == 0 ? rtlsdr_get_sample_rate(s_rtlsdr_dev) : 0);
    atomic_store(&app->rf_source_rtlsdr_tuning, RTL_TUNE_STEP | rtlsdr_get_center_freq(s_rtlsdr_dev));

    fprintf(stderr, "[RTL-SDR] Opened device %d: %u Hz, freq %llu Hz, agc=%d, gain_mode=%d\n",
            device_index, rate, (unsigned long long)app->settings.rtlsdr_freq_hz,
            app->settings.rtlsdr_agc ? 1 : 0, app->settings.rtlsdr_gain_mode);
    return 0;
}

void gui_rtlsdr_close(gui_app_t *app) {
    (void)app;
    atomic_store(&s_rtlsdr_record_tuning, 0);
    atomic_store(&s_rtlsdr_actual_rate, 0);
    if (s_rtlsdr_dev) {
        rtlsdr_close(s_rtlsdr_dev);
        s_rtlsdr_dev = NULL;
    }
#ifdef __ANDROID__
    if (s_rtlsdr_android_fd >= 0) {
        close(s_rtlsdr_android_fd);
        s_rtlsdr_android_fd = -1;
    }
#endif
}

//-----------------------------------------------------------------------------
// Capture thread
//-----------------------------------------------------------------------------

static bool rtlsdr_interrupt_recording_on_read_gap(gui_app_t *app, int status, int bytes, bool timed_out) {
    if (status >= 0 || (timed_out && bytes > 0)) return false;
    atomic_store(&s_rtlsdr_record_tuning, 0);
    gui_record_rtlsdr_gap(app);
    return true;
}

static int rtlsdr_capture_thread(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    uint8_t *in = (uint8_t *)malloc(RTL_READ_BYTES + 1u);
    if (!in) {
        atomic_store(&s_rtlsdr_record_tuning, 0);
        gui_record_rtlsdr_gap(app);
        gui_record_mark_source_loss();
        fprintf(stderr, "[RTL-SDR] Failed to allocate capture buffer\n");
        atomic_store(&s_rtlsdr_running, false);
        return -1;
    }

    fprintf(stderr, "[RTL-SDR] Capture thread started\n");
    atomic_store(&app->stream_synced, true);
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    uint64_t batch_count = 0, preview_sample = 0, last_tuning = 0;
    uint64_t settling_tuning = 0;
    size_t settling_pairs = 0;
    unsigned int carry = 0;
    bool preview_gap = true;
    const uint32_t rate = atomic_load(&app->sample_rate);
    const bool record_rate_valid = atomic_load(&s_rtlsdr_actual_rate) != 0;
    while (atomic_load(&s_rtlsdr_running) && !atomic_load(&do_exit)) {
        const uint64_t tuning = atomic_load(&app->rf_source_rtlsdr_tuning);
        const bool record_settled = (uint32_t)tuning && tuning == atomic_load(&s_rtlsdr_record_tuning);
        if (!(uint32_t)tuning && tuning != last_tuning) {
            // Reset on the I/Q producer, after any earlier block submission.
            gui_video_reset_rtlsdr(app);
            last_tuning = tuning;
            preview_gap = true;
        }
        int n_read = 0;
#ifdef __ANDROID__
        int r = rtlsdr_read_sync_timeout(s_rtlsdr_dev, in + carry, RTL_READ_BYTES,
                                         &n_read, RTL_USB_READ_TIMEOUT_MS);
        const bool timed_out = r == LIBUSB_ERROR_TIMEOUT;
#else
        int r = rtlsdr_read_sync(s_rtlsdr_dev, in + carry, RTL_READ_BYTES, &n_read);
        const bool timed_out = false;
#endif
        if (!atomic_load(&s_rtlsdr_running) || atomic_load(&do_exit)) break;
        if (rtlsdr_interrupt_recording_on_read_gap(app, r, n_read, timed_out)) {
            settling_pairs = 0;
            preview_gap = true;
            if (timed_out) continue;
            gui_record_mark_source_loss();
            carry = 0;
            fprintf(stderr, "[RTL-SDR] read_sync error %d\n", r);
            thrd_sleep_ms(10);
            continue;
        }
        if (n_read <= 0) {
            thrd_sleep_ms(1);
            continue;
        }

        // The transfer may time out with valid bytes. Keep a trailing I
        // byte for the next read so a partial completion cannot swap I/Q.
        size_t received = (size_t)n_read + carry;
        carry = (unsigned int)(received & 1u);
        if (received < 2u) continue;
        const size_t pairs = received / 2;
        // Record only reads started after settling, independently of the display FIFO.
        if (record_rate_valid && (uint32_t)tuning &&
            tuning == atomic_load(&app->rf_source_rtlsdr_tuning)) {
            if (record_settled) {
                gui_record_rtlsdr_push(app, in, pairs, tuning);
            } else {
                // Drain a full batch after connecting or retuning, even if Android delivers it
                // in pieces. Retuning does not reset the USB byte phase, so keep
                // carrying any trailing I byte into the next read.
                if (settling_tuning != tuning) {
                    settling_tuning = tuning;
                    settling_pairs = 0;
                }
                settling_pairs += pairs;
                if (settling_pairs >= RTL_READ_BYTES / 2u)
                    atomic_store(&s_rtlsdr_record_tuning, tuning);
            }
        } else if (record_settled) {
            gui_record_rtlsdr_gap(app);
        }
        const size_t write_bytes = pairs * sizeof(uint32_t);
        uint32_t *out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF, write_bytes, NULL);
        if (out) {
            for (size_t i = 0; i < pairs; i++) {
                int16_t a = (int16_t)(((int)in[i * 2] - 128) * 16);
                int16_t b = (int16_t)(((int)in[i * 2 + 1] - 128) * 16);
                out[i] = rtl_encode_raw_sample(a, b);
            }
            bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, write_bytes);
        } else {
            atomic_fetch_add(&app->rb_drop_count, 1);
            if (atomic_load(&app->rb_drop_count) <= 5) {
                fprintf(stderr, "[RTL-SDR] Warning: BUF_CAPTURE_RF full, data dropped\n");
            }
        }

        /* Use the original unpadded I/Q bytes. The ordinary display/record
         * extraction can swap the two channels when MISRC mode is selected. */
        const uint32_t center_hz = (uint32_t)tuning;
        const bool stable_tuning = center_hz && tuning == atomic_load(&app->rf_source_rtlsdr_tuning);
        if (stable_tuning && last_tuning && tuning != last_tuning) {
            /* The receiver may still have old I/Q in its USB FIFO. */
            last_tuning = tuning;
            preview_gap = true;
        } else if (out && pairs && stable_tuning && atomic_load(&s_rtlsdr_running)) {
            const gui_rf_block_info_t info = {
                .sample_rate_hz = rate,
                .start_sample = preview_sample,
                .epoch = atomic_load(&app->rf_stream_epoch),
                .discontinuity = preview_gap || tuning != last_tuning,
                .iq_center_hz = center_hz
            };
            gui_video_submit_rtlsdr(app, in, pairs, &info);
            last_tuning = tuning;
            preview_gap = false;
        } else {
            preview_gap = true;
        }
        preview_sample += pairs;
        if (carry) in[0] = in[received - 1u];
        if (timed_out) preview_gap = true;

        atomic_store(&app->last_callback_time_ms, get_time_ms());
        batch_count++;
    }

    atomic_store(&s_rtlsdr_record_tuning, 0);
    fprintf(stderr, "[RTL-SDR] Capture thread exiting after %llu batches\n",
            (unsigned long long)batch_count);
    free(in);
    return 0;
}

//-----------------------------------------------------------------------------
// Start / Stop
//-----------------------------------------------------------------------------

int gui_rtlsdr_start(gui_app_t *app) {
    if (!app) return -1;
    fprintf(stderr, "[RTL-SDR] Starting capture\n");
    if (!s_rtlsdr_dev) {
        fprintf(stderr, "[RTL-SDR] No device open\n");
        return -1;
    }

    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

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

    uint32_t rate = rtlsdr_get_sample_rate(s_rtlsdr_dev);
    if (!rate) {
        rate = app->settings.rtlsdr_sample_rate_hz;
        if (!rate) rate = RTL_DEFAULT_RATE_HZ;
    }
    atomic_store(&app->sample_rate, rate);

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[RTL-SDR] Failed to initialize capture ringbuffer\n");
        gui_app_set_status(app, "Failed to initialize capture buffer");
        return -1;
    }

    // A prior stop can leave an unread tail in the shared packed FIFO.
    bufmgr_reset(&app->buffers, BUF_CAPTURE_RF);
    atomic_store(&app->rf_source_is_rtlsdr, true);
    atomic_store(&s_rtlsdr_running, true);
    app->is_capturing = true;

    // Extraction thread: reads BUF_CAPTURE_RF -> BUF_DISPLAY + BUF_RECORD_* + stats.
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[RTL-SDR] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        atomic_store(&app->rf_source_is_rtlsdr, false);
        atomic_store(&s_rtlsdr_running, false);
        app->is_capturing = false;
        return -1;
    }

    // Display thread: processes BUF_DISPLAY for oscilloscope/FFT/waterfall/demod.
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[RTL-SDR] Failed to start display thread (non-fatal)\n");
        }
    }

    thrd_t thread;
    if (thrd_create_with_priority(&thread,
                                   rtlsdr_capture_thread,
                                   app,
                                   THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[RTL-SDR] Failed to create capture thread\n");
        gui_extract_stop();
        if (app->display_thread) gui_display_thread_stop(app->display_thread);
        atomic_store(&app->rf_source_is_rtlsdr, false);
        atomic_store(&s_rtlsdr_running, false);
        app->is_capturing = false;
        return -1;
    }
    s_rtlsdr_thread = (void *)(uintptr_t)thread;

    gui_app_set_status(app, "RTL-SDR capture running");
    return 0;
}

void gui_rtlsdr_stop(gui_app_t *app) {
    if (!atomic_load(&s_rtlsdr_running) && !s_rtlsdr_thread) return;
    fprintf(stderr, "[RTL-SDR] Stopping capture\n");

    app->is_capturing = false;
    atomic_store(&s_rtlsdr_running, false);

    if (s_rtlsdr_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)s_rtlsdr_thread;
        thrd_join(thread, NULL);
        s_rtlsdr_thread = NULL;
    }

    if (app->display_thread) gui_display_thread_stop(app->display_thread);
    gui_extract_stop();
    atomic_store(&app->rf_source_is_rtlsdr, false);

    gui_rtlsdr_close(app);
    atomic_store(&app->stream_synced, false);
    gui_app_set_status(app, "RTL-SDR capture stopped");
}

bool gui_rtlsdr_is_running(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_rtlsdr_running);
}

bool gui_rtlsdr_get_rf_source(const gui_app_t *app, uint32_t *rate, uint64_t *tuning) {
    if (!app || !rate || !tuning || !atomic_load(&s_rtlsdr_running))
        return false;
    const uint64_t current_tuning = atomic_load(&app->rf_source_rtlsdr_tuning);
    const uint32_t actual_rate = atomic_load(&s_rtlsdr_actual_rate);
    if (!actual_rate || !(uint32_t)current_tuning ||
        current_tuning != atomic_load(&s_rtlsdr_record_tuning))
        return false;
    *rate = actual_rate;
    *tuning = current_tuning;
    return true;
}

int gui_rtlsdr_set_frequency(gui_app_t *app, uint64_t hz) {
    if (!app) return -1;
    if (gui_record_rtlsdr_active()) {
        gui_app_set_status(app, "Stop Hi-Fi RF recording before retuning");
        return -1;
    }
    // Persist so the next capture start uses the new frequency even if not live.
    app->settings.rtlsdr_freq_hz = hz;
    gui_settings_save(&app->settings);
    // Live retune only if the device is open and capture is running.
    if (s_rtlsdr_dev && atomic_load(&s_rtlsdr_running)) {
        const uint64_t tuning =
            (atomic_load(&app->rf_source_rtlsdr_tuning) & ~(RTL_TUNE_STEP - 1)) + RTL_TUNE_STEP;
        atomic_store(&app->rf_source_rtlsdr_tuning, tuning);
        atomic_store(&s_rtlsdr_record_tuning, 0);
        int r = rtlsdr_set_center_freq(s_rtlsdr_dev, (uint32_t)hz);
        atomic_store(&app->rf_source_rtlsdr_tuning, tuning | rtlsdr_get_center_freq(s_rtlsdr_dev));
        if (r < 0) {
            fprintf(stderr, "[RTL-SDR] live retune to %llu Hz failed (err %d)\n",
                    (unsigned long long)hz, r);
            return -1;
        }
        fprintf(stderr, "[RTL-SDR] live retune to %llu Hz\n", (unsigned long long)hz);
    }
    return 0;
}

#endif // ENABLE_RTLSDR
