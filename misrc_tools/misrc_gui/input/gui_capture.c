/*
 * MISRC - hsdaoh-rp2350 GUI - Capture Integration
 * NEW-n-improved with extra sparkles
 * Uses the same ringbuffer and extraction pattern as misrc_capture.c
 */

#include <stdarg.h>
#include <stdatomic.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "gui_capture.h"
#include "../core/gui_app.h"
#include "../visualization/gui_text.h"
#include "../output/gui_record.h"
#include "../processing/gui_extract.h"
#include "../visualization/gui_oscilloscope.h"
#include "../visualization/gui_phosphor_rt.h"
#include "../visualization/gui_fft.h"
#include "gui_simulated.h"
#include "gui_playback.h"
#include "gui_cxadc.h"
#ifdef ENABLE_FX3
#include "gui_fx3.h"
#endif
#ifdef ENABLE_DDD
#include "gui_ddd.h"
#endif
#include "../visualization/gui_panel.h"
#include "../visualization/panel_interface.h"
#include "../visualization/gui_histogram_panel.h"
#include "../signal/gui_cvbs.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include <hsdaoh.h>
#include "../../common/hsdaoh_compat.h"
#if defined(_WIN32)
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#endif
#include "../../misrc_capture/simple_capture/simple_capture.h"
#if defined(_WIN32)
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#endif

#ifndef HSDAOH_UPSTREAM
// Frame-based mode only (MISRC)
#include <hsdaoh_raw.h>
#include "../../common/frame_parser.h"
#endif

#include "../../common/extract.h"
#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"
#include "../../common/threading.h"
// #include "../../common/frame_parser.h"
#include "../../common/device_enum.h"
#include "../../common/buffer_manager.h"
#include "../../common/misrc_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>
#include <time.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

// Define M_PI if not available (Windows compatibility)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Buffer sizes (platform-agnostic byte sizing)
#define BUFFER_READ_SIZE 65536
#define BUFFER_TOTAL_SIZE ((size_t)256 * 1024 * 1024)  // 256MB capture ringbuffer
#define BUFFER_AUDIO_TOTAL_SIZE ((size_t)256 * 1024 * 1024)
#define DEFAULT_AUDIO_SAMPLE_RATE 78125U

// Note: All capture buffers now managed by buffer manager (BUF_CAPTURE_RF, BUF_CAPTURE_AUDIO)
// Capture handler context (includes frame parser state)
#ifndef HSDAOH_UPSTREAM
static capture_handler_ctx_t s_capture_handler;
#else
// Upstream mode audio enable/disable (used by gui_capture_set_audio_capture + upstream callback)
static atomic_bool s_upstream_capture_audio = ATOMIC_VAR_INIT(true);
#endif

#if defined(__APPLE__)
static IOPMAssertionID s_idle_sleep_assertion_id = kIOPMNullAssertionID;
static IOPMAssertionID s_display_sleep_assertion_id = kIOPMNullAssertionID;

static void gui_capture_hold_power_assertions(void)
{
    if (s_idle_sleep_assertion_id == kIOPMNullAssertionID) {
        IOReturn r = IOPMAssertionCreateWithName(kIOPMAssertionTypeNoIdleSleep,
                                                 kIOPMAssertionLevelOn,
                                                 CFSTR("MISRC GUI active capture"),
                                                 &s_idle_sleep_assertion_id);
        if (r != kIOReturnSuccess) {
            fprintf(stderr, "[GUI] Failed to create NoIdleSleep assertion: 0x%x\n", r);
            s_idle_sleep_assertion_id = kIOPMNullAssertionID;
        }
    }
    if (s_display_sleep_assertion_id == kIOPMNullAssertionID) {
        IOReturn r = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                                 kIOPMAssertionLevelOn,
                                                 CFSTR("MISRC GUI active capture"),
                                                 &s_display_sleep_assertion_id);
        if (r != kIOReturnSuccess) {
            fprintf(stderr, "[GUI] Failed to create display-sleep assertion: 0x%x\n", r);
            s_display_sleep_assertion_id = kIOPMNullAssertionID;
        }
    }
}

static void gui_capture_release_power_assertions(void)
{
    if (s_idle_sleep_assertion_id != kIOPMNullAssertionID) {
        IOPMAssertionRelease(s_idle_sleep_assertion_id);
        s_idle_sleep_assertion_id = kIOPMNullAssertionID;
    }
    if (s_display_sleep_assertion_id != kIOPMNullAssertionID) {
        IOPMAssertionRelease(s_display_sleep_assertion_id);
        s_display_sleep_assertion_id = kIOPMNullAssertionID;
    }
}
#else
static void gui_capture_hold_power_assertions(void) {}
static void gui_capture_release_power_assertions(void) {}
#endif

// Message callback for hsdaoh
// Enable with hsdaoh change to support callbacks
#ifdef HSDAOH_UPSTREAM
static void gui_hsdaoh_cache_message(gui_app_t *app, enum hsdaoh_msg_level level, const char *msg)
{
    // try-lock: never block the hsdaoh thread
    if (atomic_flag_test_and_set(&app->hs_msg_lock)) {
        return; // drop if UI is copying at the same time
    }

    strncpy(app->hs_msg_buf, msg, sizeof(app->hs_msg_buf) - 1);
    app->hs_msg_buf[sizeof(app->hs_msg_buf) - 1] = '\0';
    atomic_store(&app->hs_msg_level, (int)level);
    atomic_store(&app->hs_msg_pending, true);

    atomic_flag_clear(&app->hs_msg_lock);
}
#endif


static bool gui_message_is_data_damaging(enum hsdaoh_msg_level level, const char *message)
{
    if (!message || !message[0]) {
        return false;
    }

    if (level == HSDAOH_ERROR || level == HSDAOH_CRITICAL) {
        return true;
    }

    if (level != HSDAOH_WARNING) {
        return false;
    }

    return strstr(message, "Lost sync to HDMI input stream") != NULL ||
           strstr(message, "Missed at least one frame") != NULL ||
           strstr(message, "frame errors") != NULL ||
           strstr(message, "Buffer dropped due to overrun") != NULL ||
           strstr(message, "corrupted frame") != NULL ||
           strstr(message, "corrupted frames") != NULL;
}

// #ifndef HSDAOH_UPSTREAM
static void gui_message_callback(void *ctx, enum hsdaoh_msg_level level, const char *format, ...) {
    gui_app_t *app = (gui_app_t *)ctx;
    if (!app) return;

    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    const char *level_str = "INFO";
    if (level == HSDAOH_WARNING) level_str = "WARN";
    else if (level == HSDAOH_ERROR) level_str = "ERROR";
    else if (level == HSDAOH_CRITICAL) level_str = "CRITICAL";
    bool level_is_error = (level == HSDAOH_ERROR || level == HSDAOH_CRITICAL);

    if (!app->is_capturing) {
        if (level_is_error || misrc_debug_enabled()) {
            fprintf(stderr, "[%s] %s", level_str, buffer);
        }
        return;
    }

    if (gui_message_is_data_damaging(level, buffer)) {
        gui_record_log_capture_event(app, level_str, buffer, GUI_ERROR_CLASS_SYSTEM, 1);
    }
    
#ifdef HSDAOH_UPSTREAM
    /*
     * Upstream mode: do NOT push GUI updates from this callback.
     * Just translate hsdaoh stderr-style messages into app state/counters.    
    */
    if (strstr(buffer, "Lost sync to HDMI input stream")) {
        atomic_store(&app->stream_synced, false);
    } else if (strstr(buffer, "Synchronized to HDMI input stream")) {
        atomic_store(&app->stream_synced, true);
    } else if (strstr(buffer, "Missed at least one frame")) {
        /* only count missed frames once we were already synced */
        if (atomic_load(&app->stream_synced))
            atomic_fetch_add(&app->missed_frame_count, 1);
    } else if (strstr(buffer, "frame errors")) {
        if (!level_is_error) {
            int n = 0;
            if (sscanf(buffer, "%d frame errors", &n) == 1 && n > 0)
                gui_app_count_parser_errors(app, (uint32_t)n);
            else
                gui_app_count_parser_errors(app, 1);
        }
    } else if (strstr(buffer, "Buffer dropped due to overrun")) {
        atomic_fetch_add(&app->rb_drop_count, 1);
    }
    // 2) Cache the last message for the UI thread to apply (no GUI mutation here)
    gui_hsdaoh_cache_message(app, level, buffer);
   
    // Print to console only when debug enabled, except always show ERROR/CRITICAL

    if (level_is_error || misrc_debug_enabled()){
        fprintf(stderr, "[%s] %s", level_str, buffer);
    }
#else
    /*
     * Frame mode (Stefan/MISRC): keep original behaviour.
     * Status is only pushed to GUI for ERROR/CRITICAL.
     */
    if (level_is_error) {
        gui_app_set_status(app, buffer);
    }


    if (level_is_error || misrc_debug_enabled()) {
        fprintf(stderr, "[%s] %s", level_str, buffer);
    }
#endif
}

// Debug counter
static int s_callback_count = 0;
static uint32_t s_capture_last_wait_count = 0;
static uint32_t s_capture_last_drop_count = 0;
static uint32_t s_capture_last_logged_drop_total = 0;
static uint32_t s_capture_missed_streak = 0;
static bool s_capture_missed_burst_reported = false;
static uint64_t s_capture_prev_callback_time_ms = 0;
#ifdef HSDAOH_UPSTREAM
static uint64_t s_diag_last_emit_ms = 0;
static uint32_t s_diag_last_frame_count = 0;
static uint32_t s_diag_last_error_count = 0;
static uint32_t s_diag_last_parser_error_count = 0;
static uint32_t s_diag_last_system_error_count = 0;
static uint32_t s_diag_last_missed_count = 0;
static uint32_t s_diag_last_wait_count = 0;
static uint32_t s_diag_last_drop_count = 0;
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#define MISRC_THREAD_LOCAL __declspec(thread)
#else
#define MISRC_THREAD_LOCAL _Thread_local
#endif
// Callback-gap monitoring for optional dropout-stop behavior.
static const uint64_t s_capture_gap_resync_threshold_ms = 1000;
static const uint32_t s_capture_missed_streak_report_threshold = 3;
static const backpressure_policy_t s_capture_rf_write_policy = {
    // Allow short transient extraction stalls without declaring capture drops.
    .max_wait_attempts = 1000,
    .wait_timeout_ms = 1,
    .log_first_wait = true,
    .log_drops = true,
};
static const backpressure_policy_t s_capture_audio_write_policy = {
    .max_wait_attempts = 8,
    .wait_timeout_ms = 1,
    .log_first_wait = true,
    .log_drops = true,
};

static inline uint32_t gui_capture_normalize_sample_rate_hz(uint32_t raw_srate)
{
    if (raw_srate == 0) return 0;

    /* hsdaoh can report RF sample-rate in kHz (e.g. 40000 for 40 MSPS)
     * while other paths report Hz (e.g. 40000000). Normalize to Hz. */
    uint64_t hz = raw_srate;
    if (raw_srate <= 100000U) {
        hz = (uint64_t)raw_srate * 1000ULL;
    }

    /* Reject obviously corrupted values so UI/status does not latch garbage. */
    if (hz < 1000000ULL || hz > 200000000ULL) {
        return 0;
    }

    return (uint32_t)hz;
}

static inline void gui_capture_update_backpressure_counters(gui_app_t *app)
{
    if (!app) return;

    uint32_t waits = atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].write_waits);
    uint32_t drops = atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].write_drops);

    if (waits > s_capture_last_wait_count) {
        atomic_fetch_add(&app->rb_wait_count, waits - s_capture_last_wait_count);
    }
    if (drops > s_capture_last_drop_count) {
        atomic_fetch_add(&app->rb_drop_count, drops - s_capture_last_drop_count);
    }

    s_capture_last_wait_count = waits;
    s_capture_last_drop_count = drops;
}

static inline void gui_capture_request_dropout_stop(gui_app_t *app, gui_dropout_reason_t reason)
{
    if (!app || !app->settings.stop_on_dropout) return;
    atomic_store(&app->dropout_stop_reason, (uint32_t)reason);
    atomic_store(&app->dropout_stop_requested, true);
}

static inline void gui_capture_promote_callback_priority_once(void)
{
    static MISRC_THREAD_LOCAL bool s_capture_callback_thread_priority_set = false;
    if (!s_capture_callback_thread_priority_set) {
        /* Callback thread is the capture-ingest hot path. */
        thrd_set_priority(THRD_PRIORITY_CRITICAL);
        s_capture_callback_thread_priority_set = true;
    }
}
#ifdef HSDAOH_UPSTREAM
static inline void gui_capture_reset_realtime_diag_state(void)
{
    s_diag_last_emit_ms = 0;
    s_diag_last_frame_count = 0;
    s_diag_last_error_count = 0;
    s_diag_last_parser_error_count = 0;
    s_diag_last_system_error_count = 0;
    s_diag_last_missed_count = 0;
    s_diag_last_wait_count = 0;
    s_diag_last_drop_count = 0;
}

static void gui_capture_emit_realtime_diag(gui_app_t *app, uint64_t now_ms)
{
    if (!app) return;
    if (!app->is_capturing) {
        gui_capture_reset_realtime_diag_state();
        return;
    }

    uint32_t frame_count = atomic_load(&app->frame_count);
    uint32_t error_count = atomic_load(&app->error_count);
    uint32_t parser_error_count = atomic_load(&app->parser_error_count);
    uint32_t system_error_count = atomic_load(&app->system_error_count);
    uint32_t missed_count = atomic_load(&app->missed_frame_count);
    uint32_t wait_count = atomic_load(&app->rb_wait_count);
    uint32_t drop_count = atomic_load(&app->rb_drop_count);
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    bool synced = atomic_load(&app->stream_synced);
    uint64_t last_cb_ms = atomic_load(&app->last_callback_time_ms);
    uint64_t callback_age_ms = (now_ms >= last_cb_ms) ? (now_ms - last_cb_ms) : 0;

    if (s_diag_last_emit_ms == 0 || now_ms < s_diag_last_emit_ms) {
        s_diag_last_emit_ms = now_ms;
        s_diag_last_frame_count = frame_count;
        s_diag_last_error_count = error_count;
        s_diag_last_parser_error_count = parser_error_count;
        s_diag_last_system_error_count = system_error_count;
        s_diag_last_missed_count = missed_count;
        s_diag_last_wait_count = wait_count;
        s_diag_last_drop_count = drop_count;
        return;
    }

    uint64_t elapsed_ms = now_ms - s_diag_last_emit_ms;
    if (elapsed_ms < 1000) {
        return;
    }

    uint32_t frame_delta = frame_count - s_diag_last_frame_count;
    uint32_t error_delta = error_count - s_diag_last_error_count;
    uint32_t parser_error_delta = parser_error_count - s_diag_last_parser_error_count;
    uint32_t system_error_delta = system_error_count - s_diag_last_system_error_count;
    uint32_t missed_delta = missed_count - s_diag_last_missed_count;
    uint32_t wait_delta = wait_count - s_diag_last_wait_count;
    uint32_t drop_delta = drop_count - s_diag_last_drop_count;
    double fps = (elapsed_ms > 0) ? ((double)frame_delta * 1000.0 / (double)elapsed_ms) : 0.0;

    fprintf(stderr,
            "[DIAG] sync=%s srate=%uHz fps=%.1f frame=+%u err=+%u parser=+%u system=+%u missed=+%u wait=+%u drop=+%u cb_age=%" PRIu64 "ms totals(f=%u,e=%u,p=%u,s=%u,m=%u,w=%u,d=%u)\n",
            synced ? "OK" : "NO",
            sample_rate,
            fps,
            frame_delta,
            error_delta,
            parser_error_delta,
            system_error_delta,
            missed_delta,
            wait_delta,
            drop_delta,
            callback_age_ms,
            frame_count,
            error_count,
            parser_error_count,
            system_error_count,
            missed_count,
            wait_count,
            drop_count);

    s_diag_last_emit_ms = now_ms;
    s_diag_last_frame_count = frame_count;
    s_diag_last_error_count = error_count;
    s_diag_last_parser_error_count = parser_error_count;
    s_diag_last_system_error_count = system_error_count;
    s_diag_last_missed_count = missed_count;
    s_diag_last_wait_count = wait_count;
    s_diag_last_drop_count = drop_count;
}
#endif

void gui_capture_set_audio_capture(bool enabled)
{
#ifdef HSDAOH_UPSTREAM 
    atomic_store(&s_upstream_capture_audio, enabled);

#else   
    // MISRC frame-based mode: audio capture is part of the capture handler 
    atomic_store(&s_capture_handler.capture_audio, enabled);

    if (enabled) {
        // Ensure clean audio sync ramp when enabling audio mid-capture.
        capture_handler_reset_audio_sync(&s_capture_handler);
    }
#endif
}

static void gui_capture_apply_cxadc_profile(gui_app_t *app, int card_count)
{
    if (!app) return;
    if (card_count < 1) card_count = 1;
    if (card_count > 2) card_count = 2;

    bool changed = false;

    // Fixed RF assumptions for CXADC mode.
    if (app->settings.rf_bits_a != 8) {
        app->settings.rf_bits_a = 8;
        changed = true;
    }
    if (app->settings.rf_bits_b != 8) {
        app->settings.rf_bits_b = 8;
        changed = true;
    }
    // Keep user-selected resample modes/rates and RF A toggle in CXADC mode.
    // Only clamp RF-B capture when there is no second CXADC card source.
    if (card_count < 2 && app->settings.capture_b) {
        app->settings.capture_b = false;
        changed = true;
    }

    // Card mapping is fixed: card 0 -> Channel A, card 1 -> Channel B.
    if (app->user_capture_mode_misrc) {
        app->user_capture_mode_misrc = false;
        changed = true;
    }
    if (!app->is_recording && app->capture_mode_runtime_misrc) {
        app->capture_mode_runtime_misrc = false;
        changed = true;
    }

    // 3-channel clockgen mapping defaults (CH1/CH2/CH3=headswitch).
    static const char *audio_labels[4] = { "audio1", "audio2", "headswitch", "" };
    for (int i = 0; i < 4; i++) {
        if (strcmp(app->settings.audio_1ch_labels[i], audio_labels[i]) != 0) {
            snprintf(app->settings.audio_1ch_labels[i],
                     sizeof(app->settings.audio_1ch_labels[i]),
                     "%s",
                     audio_labels[i]);
            changed = true;
        }
    }

    if (!app->settings.audio_output_tags[1][0]) {
        snprintf(app->settings.audio_output_tags[1],
                 sizeof(app->settings.audio_output_tags[1]),
                 "%s",
                 "audio12");
        changed = true;
    }
    if (!app->settings.audio_output_tags[2][0]) {
        snprintf(app->settings.audio_output_tags[2],
                 sizeof(app->settings.audio_output_tags[2]),
                 "%s",
                 "headswitch");
        changed = true;
    }

    if (card_count > 1) {
        if (!app->settings.enable_audio_2ch_12) {
            app->settings.enable_audio_2ch_12 = true;
            changed = true;
        }
        if (!app->settings.enable_audio_1ch[2]) {
            app->settings.enable_audio_1ch[2] = true;
            changed = true;
        }
    }

    if (changed) {
        gui_settings_save(&app->settings);
    }
}

/*-----------------------------------------------------------------------------
 * GUI-Specific Capture Handler Callbacks
 *-----------------------------------------------------------------------------*/
#ifndef HSDAOH_UPSTREAM
static void gui_sync_event_cb(void *user_ctx, frame_sync_result_t result,
                               const metadata_t *meta, bool was_synced)
{
    (void)meta;
    gui_app_t *app = (gui_app_t *)user_ctx;

    switch (result) {
        case FRAME_SYNC_LOST:
            if (was_synced && misrc_debug_enabled()) {
                fprintf(stderr, "[CB] Lost sync to HDMI input stream\n");
            }
            atomic_store(&app->stream_synced, false);
            if (was_synced) {
                gui_record_log_capture_event(app, "ERROR", "Lost sync to HDMI input stream",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
            }
            s_capture_missed_streak = 0;
            s_capture_missed_burst_reported = false;
            break;
        case FRAME_SYNC_MISSED:
            if (misrc_debug_enabled()) {
                // Match CLI usefulness: include framecounter. Note expected counter isn't directly
                // available here because frame_process() already updated last_frame_cnt.
                if (meta) {
                    fprintf(stderr, "[CB] Missed at least one frame, fcnt %u\n", (unsigned)meta->framecounter);
                } else {
                    fprintf(stderr, "[CB] Missed frame(s)\n");
                }
            }
            s_capture_missed_streak++;
            if (s_capture_missed_streak >= s_capture_missed_streak_report_threshold &&
                !s_capture_missed_burst_reported) {
                atomic_fetch_add(&app->missed_frame_count, 1);
                s_capture_missed_burst_reported = true;
                gui_record_log_capture_event(app, "WARN", "Persistent missed-frame burst detected",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                gui_capture_request_dropout_stop(app, GUI_DROPOUT_MISSED_FRAME);
            }
            break;
        case FRAME_SYNC_ACQUIRED:
            if (misrc_debug_enabled()) {
                fprintf(stderr, "[CB] Synchronized to HDMI input stream\n");
            }
            atomic_store(&app->stream_synced, true);
            s_capture_missed_streak = 0;
            s_capture_missed_burst_reported = false;
            break;
        case FRAME_SYNC_DUPLICATE:
            s_capture_missed_streak = 0;
            s_capture_missed_burst_reported = false;
            break;
        case FRAME_SYNC_OK:
            s_capture_missed_streak = 0;
            s_capture_missed_burst_reported = false;
            break;
    }
}

static void gui_capture_configure_handler(gui_app_t *app, bool reset_parser_state)
{
    /*
     * Parser/reconnect ownership contract:
     * - capture_handler_init() (parser/sync reset) is allowed only at capture
     *   lifecycle boundaries, never from callback error paths.
     * - Authorized reset sites:
     *   1) gui_app_init() for initial baseline
     *   2) gui_app_start_capture() only after stream startup succeeds
     *   3) gui_app_stop_capture() when a session ends (including reconnect teardown)
     *
     * This keeps parser state transitions deterministic across auto-reconnect.
     */
    if (reset_parser_state) {
        capture_handler_init(&s_capture_handler);
    }

    // Ringbuffers are managed by buffer manager in GUI path.
    s_capture_handler.rb_rf = NULL;
    s_capture_handler.rb_audio = NULL;

    // Keep RF/audio capture enabled by default for monitoring/extraction continuity.
    atomic_store(&s_capture_handler.capture_rf, true);
    atomic_store(&s_capture_handler.capture_audio, true);

    s_capture_handler.sync_event_cb = gui_sync_event_cb;
    s_capture_handler.user_ctx = app;
}


/*-----------------------------------------------------------------------------
 * Main Capture Callback
 *-----------------------------------------------------------------------------*/

// Main capture callback - writes raw data to ringbuffer (like reference implementation)
void gui_capture_callback(void *data_info_ptr) {
    if (!data_info_ptr) return;
    hsdaoh_data_info_t *data_info = (hsdaoh_data_info_t *)data_info_ptr;
    if (!data_info->ctx) return;
    gui_app_t *app = (gui_app_t *)data_info->ctx;
    gui_capture_promote_callback_priority_once();

    s_callback_count++;

    if (atomic_load(&do_exit)) return;
    if (!app) return;
    if (!data_info->buf) return;
    if (!app->is_capturing) return;
    // Any callback activity means the capture path is still alive.
    uint64_t now_ms = get_time_ms();
    atomic_store(&app->last_callback_time_ms, now_ms);
    if (s_capture_prev_callback_time_ms != 0) {
        uint64_t elapsed = now_ms - s_capture_prev_callback_time_ms;
        if (now_ms < s_capture_prev_callback_time_ms) {
            elapsed = (UINT64_MAX - s_capture_prev_callback_time_ms) + now_ms + 1;
        }
        if (elapsed > s_capture_gap_resync_threshold_ms) {
            if (app->settings.stop_on_dropout) {
                char gap_msg[160];
                snprintf(gap_msg, sizeof(gap_msg),
                         "Capture callback gap detected: %" PRIu64 " ms (dropout stop requested)",
                         elapsed);
                gui_record_log_capture_event(app, "ERROR", gap_msg, GUI_ERROR_CLASS_SYSTEM, 1);
                gui_capture_request_dropout_stop(app, GUI_DROPOUT_CALLBACK_GAP);
                s_capture_prev_callback_time_ms = now_ms;
                return;
            }
            if (misrc_debug_enabled()) {
                fprintf(stderr,
                        "[CB] Callback gap %" PRIu64 "ms detected\n",
                        elapsed);
            }
        }
    }
    s_capture_prev_callback_time_ms = now_ms;
    if (data_info->width == 0 || data_info->height == 0) return;

    if (data_info->device_error) {
        // Do not force parser sync-state mutation here; parser resets belong to
        // capture lifecycle boundaries (start/stop), not callback error handling.
        atomic_store(&app->stream_synced, false);
        capture_handler_reset_audio_sync(&s_capture_handler);
        s_capture_missed_streak = 0;
        s_capture_missed_burst_reported = false;
        gui_record_log_capture_event(app, "ERROR", "Capture callback reported device_error",
                                     GUI_ERROR_CLASS_SYSTEM, 1);
        gui_capture_request_dropout_stop(app, GUI_DROPOUT_DEVICE_ERROR);
        return;
    }

    // Extract metadata from frame
    metadata_t meta;
    hsdaoh_extract_metadata(data_info->buf, &meta, data_info->width);

    bool was_synced = s_capture_handler.frame_state.sync.stream_synced;

    // Process frame using shared parser
    frame_process_result_t result = frame_process(&s_capture_handler.frame_state,
                                                   data_info->buf,
                                                   data_info->width,
                                                   data_info->height,
                                                   &meta, 4);

    // Handle sync state changes using shared handler
    if (!capture_handler_process_sync_event(&s_capture_handler, result.sync_result,
                                             &meta, was_synced)) {
        return;  // LOST or DUPLICATE - stop processing
    }

    // Don't process until synced
    if (!s_capture_handler.frame_state.sync.stream_synced) {
        return;
    }


    atomic_fetch_add(&app->frame_count, 1);

    // Handle errors
    if (result.error_count > 0 && result.report_errors) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "Frame parser error burst: %d errors in callback frame", result.error_count);
        gui_record_log_capture_event(app, "ERROR", err_msg, GUI_ERROR_CLASS_PARSER, (uint32_t)result.error_count);
        if (misrc_debug_enabled()) {
            fprintf(stderr, "[CB] %d frame errors, %u frames since last error\n",
                    result.error_count, s_capture_handler.frame_state.frames_since_error);
        }
        if (app->settings.stop_on_dropout) {
            gui_capture_request_dropout_stop(app, GUI_DROPOUT_FRAME_ERROR);
        }
        return;  // Discard frame with errors
    }

    // Don't process if no payload
    if (!result.valid || result.stream0_bytes == 0) {
        return;
    }

    // Update sample rate only from valid payload frames.
    if (meta.stream_info[0].srate > 0) {
        uint32_t sample_rate_hz = gui_capture_normalize_sample_rate_hz(meta.stream_info[0].srate);
        if (sample_rate_hz > 0) {
            atomic_store(&app->sample_rate, sample_rate_hz);
        }
    }

    uint8_t *buf_out = NULL;
    uint8_t *buf_out_audio = NULL;

    // Write to capture ringbuffer via buffer manager
    // Buffer manager handles backpressure according to default policy for BUF_CAPTURE_RF
    buf_out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF, result.stream0_bytes, &s_capture_rf_write_policy);
    gui_capture_update_backpressure_counters(app);
    if (!buf_out) {
        uint32_t total_drops = atomic_load(&app->rb_drop_count);
        uint32_t delta_drops = (total_drops > s_capture_last_logged_drop_total)
                                 ? (total_drops - s_capture_last_logged_drop_total)
                                 : 1;
        s_capture_last_logged_drop_total = total_drops;
        uint32_t current_frame = atomic_load(&app->frame_count);
        char drop_msg[192];
        snprintf(drop_msg, sizeof(drop_msg),
                 "Capture RF backpressure drop: +%u (total=%u) frame=%u",
                 delta_drops, total_drops, current_frame);
        gui_record_log_capture_event(app, "ERROR", drop_msg, GUI_ERROR_CLASS_SYSTEM, delta_drops);
        if (misrc_debug_enabled()) {
            fprintf(stderr, "[CB] %s\n", drop_msg);
        }
        if (app->settings.stop_on_dropout) {
            gui_capture_request_dropout_stop(app, GUI_DROPOUT_BACKPRESSURE);
        }
        return;
    }

    // If audio capture enabled, reserve space in audio buffer via buffer manager
    if (atomic_load(&s_capture_handler.capture_audio) && result.stream1_bytes > 0) {
        buf_out_audio = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO, result.stream1_bytes, &s_capture_audio_write_policy);
        // buf_out_audio may be NULL if buffer full
    }

    // Copy payloads (RF + optional audio) with shared audio sync filtering
    frame_copy_payloads_cb(data_info->buf, data_info->width, data_info->height,
                           &meta, buf_out, buf_out_audio,
                           capture_handler_audio_filter, &s_capture_handler);

    bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, result.stream0_bytes);
    if (buf_out_audio) {
        bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, result.stream1_bytes);
    }

    // Signal that new data is available (data events are managed by buffer manager)
    bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);

    if (s_callback_count <= 3 && misrc_debug_enabled()) {
        fprintf(stderr, "[CB] Wrote %zu bytes to ringbuffer\n", result.stream0_bytes);
    }
}

static void gui_simple_capture_callback(sc_data_info_t *sc_data_info)
{
    if (!sc_data_info) {
        return;
    }

    hsdaoh_data_info_t hs_data_info;
    memset(&hs_data_info, 0, sizeof(hs_data_info));
    hs_data_info.ctx = sc_data_info->ctx;
    hs_data_info.buf = (unsigned char *)sc_data_info->data;
    hs_data_info.width = sc_data_info->width;
    hs_data_info.height = sc_data_info->height;
    hs_data_info.len = sc_data_info->len;

    gui_capture_callback(&hs_data_info);
}
//#endif /* HSDAOH_UPSTREAM */
#else /* HSDAOH_UPSTREAM */
/*-----------------------------------------------------------------------------
 * Upstream helpers (rp2350 / upstream path)
 *-----------------------------------------------------------------------------*/
    static inline void gui_upstream_mark_synced(gui_app_t *app)
    {
        bool was = atomic_exchange(&app->stream_synced, true);
        if (!was && misrc_debug_enabled()) {
            fprintf(stderr, "[CB] Upstream stream active\n");
        }
    }

#endif /* HSDAOH_UPSTREAM */


// Initialize application
void gui_app_init(gui_app_t *app) {
    // Initialize panel registry and register all panel types
    // This must happen before any panel state is created
    panel_registry_init();
    gui_waveform_panel_register();
    gui_fft_panel_register();
    gui_cvbs_panel_register();
    gui_histogram_panel_register();

    // Initialize per-channel display buffers
    memset(app->display_samples_a, 0, sizeof(app->display_samples_a));
    memset(app->display_samples_b, 0, sizeof(app->display_samples_b));
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Initialize simulated device state
    app->sim_thread = NULL;
    atomic_store(&app->sim_running, false);

    // Initialize playback device state
    atomic_store(&app->playback_running, false);

#ifdef ENABLE_FX3
    // Initialize FX3 device state
    app->fx3_dev = NULL;
    app->fx3_thread = NULL;
    atomic_store(&app->fx3_running, false);
#endif

#ifdef ENABLE_DDD
    // Initialize DdD device state
    app->ddd_dev = NULL;
    app->ddd_thread = NULL;
    atomic_store(&app->ddd_running, false);
#endif

    app->vu_a.level_pos = 0;
    app->vu_a.level_neg = 0;
    app->vu_a.peak_pos = 0;
    app->vu_a.peak_neg = 0;
    app->vu_a.peak_hold_time_pos = 0;
    app->vu_a.peak_hold_time_neg = 0;

    app->vu_b.level_pos = 0;
    app->vu_b.level_neg = 0;
    app->vu_b.peak_pos = 0;
    app->vu_b.peak_neg = 0;
    app->vu_b.peak_hold_time_pos = 0;
    app->vu_b.peak_hold_time_neg = 0;
    app->user_capture_mode_misrc = app->settings.misrc_mode;
    app->capture_mode_runtime_misrc = app->user_capture_mode_misrc;
    app->settings.misrc_mode = app->user_capture_mode_misrc;

    strcpy(app->status_message, "Initializing...");

    for (int i = 0; i < 4; i++) {
        atomic_store(&app->audio_peak[i], 0);
    }
    // Support hsdaoh-rp2350 error/stats handling
    atomic_store(&app->hs_msg_pending, false);
    atomic_store(&app->hs_msg_level, (int)HSDAOH_INFO);
    atomic_flag_clear(&app->hs_msg_lock);
    app->hs_msg_buf[0] = '\0';
    app->hs_ui_last_poll_ms = 0;

    // Initialize sample rate to default (will be updated when device connects)
    atomic_store(&app->sample_rate, DEFAULT_SAMPLE_RATE);
    atomic_store(&app->audio_sample_rate, DEFAULT_AUDIO_SAMPLE_RATE);
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    app->low_signal_time = 0.0f;
    app->low_signal_armed = false;
    TraceLog(LOG_INFO, "APP INIT: sample_rate set to %u", DEFAULT_SAMPLE_RATE);

    // Initialize trigger state for channel A
    app->trigger_a.enabled = false;
    app->trigger_a.level = 0;
    app->trigger_a.zoom_scale = ZOOM_SCALE_DEFAULT;
    app->trigger_a.trigger_display_pos = -1;
    atomic_store(&app->trigger_a.display_width, DISPLAY_BUFFER_SIZE);  // Will be updated by renderer
    app->trigger_a.scope_mode = SCOPE_MODE_PHOSPHOR;  // Phosphor mode by default
    app->trigger_a.trigger_mode = TRIGGER_MODE_RISING;  // Rising edge by default
    app->trigger_a.trigger_source = TRIGGER_SOURCE_CH1;
    app->trigger_a.phosphor_color = PHOSPHOR_COLOR_HEATMAP;  // Opacity mode by default

    // Initialize trigger state for channel B
    app->trigger_b.enabled = false;
    app->trigger_b.level = 0;
    app->trigger_b.zoom_scale = ZOOM_SCALE_DEFAULT;
    app->trigger_b.trigger_display_pos = -1;
    atomic_store(&app->trigger_b.display_width, DISPLAY_BUFFER_SIZE);  // Will be updated by renderer
    app->trigger_b.scope_mode = SCOPE_MODE_PHOSPHOR;  // Phosphor mode by default
    app->trigger_b.trigger_mode = TRIGGER_MODE_RISING;  // Rising edge by default
    app->trigger_b.trigger_source = TRIGGER_SOURCE_CH1;
    app->trigger_b.phosphor_color = PHOSPHOR_COLOR_HEATMAP;  // Opacity mode by default

    // Initialize phosphor display state
    app->phosphor_a = (phosphor_rt_t *)calloc(1, sizeof(phosphor_rt_t));
    app->phosphor_b = (phosphor_rt_t *)calloc(1, sizeof(phosphor_rt_t));

    // Set phosphor config (render textures are created lazily on first use)
    if (app->phosphor_a) {
        app->phosphor_a->config.decay_rate = SCOPE_DECAY_RATE;
        app->phosphor_a->config.hit_increment = SCOPE_HIT_INCREMENT;
        app->phosphor_a->config.bloom_intensity = SCOPE_BLOOM;
        memcpy(app->phosphor_a->config.channel_color, PHOSPHOR_CHANNEL_COLOR_A, sizeof(float) * 3);
    }
    if (app->phosphor_b) {
        app->phosphor_b->config.decay_rate = SCOPE_DECAY_RATE;
        app->phosphor_b->config.hit_increment = SCOPE_HIT_INCREMENT;
        app->phosphor_b->config.bloom_intensity = SCOPE_BLOOM;
        memcpy(app->phosphor_b->config.channel_color, PHOSPHOR_CHANNEL_COLOR_B, sizeof(float) * 3);
    }

    // Initialize panel configuration lock (must be clear before use)
    atomic_flag_clear(&app->panel_config_lock);

    // Initialize panel configuration (new panel abstraction system)
    app->panel_config_a.split = true;
    app->panel_config_a.left_view = PANEL_VIEW_WAVEFORM;
    app->panel_config_a.right_view = PANEL_VIEW_FFT;
    app->panel_config_a.left_state = panel_create_view_state(PANEL_VIEW_WAVEFORM);
    app->panel_config_a.right_state = panel_create_view_state(PANEL_VIEW_FFT);

    app->panel_config_b.split = true;
    app->panel_config_b.left_view = PANEL_VIEW_WAVEFORM;
    app->panel_config_b.right_view = PANEL_VIEW_FFT;
    app->panel_config_b.left_state = panel_create_view_state(PANEL_VIEW_WAVEFORM);
    app->panel_config_b.right_state = panel_create_view_state(PANEL_VIEW_FFT);

    // Note: All buffers (BUF_CAPTURE_RF, BUF_CAPTURE_AUDIO, etc.) are initialized
    // by buffer manager automatically on first use

    // Initialize parser/capture-handler baseline once at app init.
#ifndef HSDAOH_UPSTREAM   // MISRC mode only
    gui_capture_configure_handler(app, true);
#endif


    // Initialize centralized buffer manager
    if (bufmgr_init(&app->buffers) != 0) {
        fprintf(stderr, "Failed to initialize buffer manager\n");
    }

    // Initialize display thread (allocated, started on capture start)
    app->display_thread = (display_thread_t *)calloc(1, sizeof(display_thread_t));
    if (app->display_thread) {
        if (gui_display_thread_init(app->display_thread) != 0) {
            fprintf(stderr, "Failed to initialize display thread state\n");
            free(app->display_thread);
            app->display_thread = NULL;
        }
    }

    // Set app for text rendering
    gui_text_set_app(app);
}

// Cleanup application
void gui_app_cleanup(gui_app_t *app) {
    if (app->is_capturing) {
        gui_app_stop_capture(app);
    }

    // Note: All buffers now managed by buffer manager (cleanup via bufmgr_cleanup)

    // Cleanup extraction subsystem
    gui_extract_cleanup();

    // Cleanup phosphor buffers and textures
    if (app->phosphor_a) {
        phosphor_rt_cleanup(app->phosphor_a);
        free(app->phosphor_a);
        app->phosphor_a = NULL;
    }
    if (app->phosphor_b) {
        phosphor_rt_cleanup(app->phosphor_b);
        free(app->phosphor_b);
        app->phosphor_b = NULL;
    }

    // Note: CVBS decoders are now owned by panel state (left_state/right_state)
    // and cleaned up via panel_config_cleanup() below

    // Cleanup panel configurations (includes FFT, CVBS, histogram, waveform state)
    // Per-panel resamplers are cleaned up by vtable destroy() functions
    panel_config_cleanup(&app->panel_config_a);
    panel_config_cleanup(&app->panel_config_b);

    // Cleanup oscilloscope static resources
    gui_oscilloscope_cleanup();

    // Cleanup display thread
    if (app->display_thread) {
        gui_display_thread_cleanup(app->display_thread);
        free(app->display_thread);
        app->display_thread = NULL;
    }

    // Cleanup buffer manager
    bufmgr_cleanup(&app->buffers);
}

// Enumerate available capture devices
void gui_app_enumerate_devices(gui_app_t *app) {
    app->device_count = 0;
    fprintf(stderr, "[GUI] enumerate_devices: count=%d\n", app->device_count); //debugging
    // Use shared device enumeration (hsdaoh + simple_capture + optionally FX3)
    misrc_device_list_t devices;
    misrc_device_list_init(&devices);
#if defined(ENABLE_DDD)
    int count = misrc_device_enumerate_ddd(&devices, true, app->settings.discover_simple_capture, true);
#elif defined(ENABLE_FX3)
    int count = misrc_device_enumerate_fx3(&devices, true, app->settings.discover_simple_capture, true);
#else
    int count = misrc_device_enumerate(&devices, true, app->settings.discover_simple_capture);
#endif

    if (count < 0) {
        gui_app_set_status(app, "Device enumeration failed");
        misrc_device_list_free(&devices);
        return;
    }

    // Copy devices to GUI format
    for (size_t i = 0; i < devices.count && app->device_count < MAX_DEVICES; i++) {
        misrc_device_info_t *src = &devices.devices[i];
        device_info_t *dst = &app->devices[app->device_count];

        // Format name with type prefix for simple_capture devices
        if (src->type == MISRC_DEVICE_TYPE_SIMPLE_CAPTURE) {
            snprintf(dst->name, sizeof(dst->name), "[%s] %s",
                     device_get_simple_capture_short_name(), src->name);
            dst->type = DEVICE_TYPE_SIMPLE_CAPTURE;
            dst->index = -1;
            // Store device_id in serial field for simple_capture
            snprintf(dst->serial, sizeof(dst->serial), "%s", src->device_id);
        }
#ifdef ENABLE_FX3
        else if (src->type == MISRC_DEVICE_TYPE_FX3) {
            snprintf(dst->name, sizeof(dst->name), "[FX3] %s", src->name);
            dst->type = DEVICE_TYPE_FX3;
            dst->index = src->index;
            snprintf(dst->serial, sizeof(dst->serial), "%s", src->device_id);
        }
#endif
#ifdef ENABLE_DDD
        else if (src->type == MISRC_DEVICE_TYPE_DDD) {
            snprintf(dst->name, sizeof(dst->name), "[DdD] %s", src->name);
            dst->type = DEVICE_TYPE_DDD;
            dst->index = src->index;
            snprintf(dst->serial, sizeof(dst->serial), "%s", src->device_id);
        }
#endif
        else {
            snprintf(dst->name, sizeof(dst->name), "%s", src->name);
            dst->type = DEVICE_TYPE_HSDAOH;
            dst->index = src->index;
            dst->serial[0] = '\0';
        }

        app->device_count++;
    }

    misrc_device_list_free(&devices);

    // Add CXADC mode option if cards are detected on host.
    int cxadc_cards = gui_cxadc_detect_cards();
    if (cxadc_cards > 0 && app->device_count < MAX_DEVICES) {
        device_info_t *dst = &app->devices[app->device_count];
        if (cxadc_cards > 1) {
            snprintf(dst->name, sizeof(dst->name), "[CXADC] CXADC Clockgen");
            dst->index = 2;
        } else {
            snprintf(dst->name, sizeof(dst->name), "[CXADC] CXADC");
            dst->index = 1;
        }
        snprintf(dst->serial, sizeof(dst->serial), "CXADC_%dCARD", dst->index);
        dst->type = DEVICE_TYPE_CXADC;
        app->device_count++;
    }

    // Always add simulated device at the end
    if (app->device_count < MAX_DEVICES) {
        device_info_t *dst = &app->devices[app->device_count];
        snprintf(dst->name, sizeof(dst->name), "[Simulated] Test Signal");
        snprintf(dst->serial, sizeof(dst->serial), "SIM001");
        dst->type = DEVICE_TYPE_SIMULATED;
        dst->index = -1;
        app->device_count++;
    }

    // Add playback device option
    if (app->device_count < MAX_DEVICES) {
        device_info_t *dst = &app->devices[app->device_count];
        snprintf(dst->name, sizeof(dst->name), "[Playback] FLAC Files");
        snprintf(dst->serial, sizeof(dst->serial), "PLAYBACK");
        dst->type = DEVICE_TYPE_PLAYBACK;
        dst->index = -1;
        app->device_count++;
    }

    if (app->device_count == 0) {
        gui_app_set_status(app, "No capture devices found");
    } else {
        if (app->selected_device < 0 || app->selected_device >= app->device_count) {
            app->selected_device = 0;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Found %d device(s)", app->device_count);
        gui_app_set_status(app, msg);
    }
}

#ifdef HSDAOH_UPSTREAM // Upstream mode callback - writes raw data to ringbuffer (like reference implementation)
static void gui_capture_upstream_callback(hsdaoh_data_info_t *data_info) //
{
    if (!data_info || !data_info->ctx) return;
    gui_capture_promote_callback_priority_once();

    gui_app_t *app = (gui_app_t *)data_info->ctx;
    if (!app || !data_info->buf || data_info->len == 0) return;

    atomic_store(&app->last_callback_time_ms, get_time_ms());

    // We are receiving valid upstream data -> consider link synced
    //atomic_store(&app->stream_synced, true);    
     
    if (data_info->stream_id == 0) {
        gui_upstream_mark_synced(app); //UPDATE 2 - hsdaoh - status from metadata only - Update 3 put it back ---delete ****
        const uint16_t *samples = (const uint16_t *)data_info->buf;
        size_t sample_count = data_info->len / sizeof(uint16_t);

        size_t packed_bytes = sample_count * sizeof(uint32_t);
        uint8_t *out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF, packed_bytes, &s_capture_rf_write_policy);
        gui_capture_update_backpressure_counters(app);
        if (!out) {
            uint32_t total_drops = atomic_load(&app->rb_drop_count);
            uint32_t delta_drops = (total_drops > s_capture_last_logged_drop_total)
                                     ? (total_drops - s_capture_last_logged_drop_total)
                                     : 1;
            s_capture_last_logged_drop_total = total_drops;
            uint32_t current_frame = atomic_load(&app->frame_count);
            char drop_msg[192];
            snprintf(drop_msg, sizeof(drop_msg),
                     "Capture RF backpressure drop: +%u (total=%u) frame=%u",
                     delta_drops, total_drops, current_frame);
            gui_record_log_capture_event(app, "ERROR", drop_msg, GUI_ERROR_CLASS_SYSTEM, delta_drops);
            if (misrc_debug_enabled()) {
                fprintf(stderr, "[CB] %s\n", drop_msg);
            }
            if (app->settings.stop_on_dropout) {
                gui_capture_request_dropout_stop(app, GUI_DROPOUT_BACKPRESSURE);
            }
            return;
        }

        uint32_t *packed = (uint32_t *)out;
        for (size_t i = 0; i < sample_count; i++) {
            uint16_t a = samples[i] & 0x0FFF;
            packed[i] = (uint32_t)a;
        }

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, packed_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);

        if (data_info->srate > 0) {
            uint32_t sample_rate_hz = gui_capture_normalize_sample_rate_hz(data_info->srate);
            if (sample_rate_hz > 0) {
                atomic_store(&app->sample_rate, sample_rate_hz);
            }
        }

        atomic_fetch_add(&app->frame_count, 1);
        return;
    }

    if (data_info->stream_id == 2) {
        if (!atomic_load(&s_upstream_capture_audio)) return;

        if (data_info->len < 6) return;
        
        size_t frames = data_info->len / 6;
        if (frames == 0) return;
        
        if (frames > SIZE_MAX / 12) {
            fprintf(stderr, "[AUDIO] Frame count overflow\n");
            return;
        }
        
        size_t padded_len = frames * 12;
        
        uint8_t *out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO, padded_len, &s_capture_audio_write_policy);
        if (!out) return;
        
        memset(out, 0, padded_len);
        
        const uint8_t *src = data_info->buf;
        for (size_t i = 0; i < frames; i++) {
            memcpy(out + i*12, src + i*6, 6);
        }
        
        bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, padded_len);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
        
        if (data_info->srate > 0) {
            static int rate_logged = 0;
            if (!rate_logged) {
                fprintf(stderr, "[AUDIO] Sample rate: %u Hz\n", data_info->srate);
                rate_logged = 1;
            }
        }
        
        return;
    }

}
#endif /* HSDAOH_UPSTREAM */


// Start capture (+60ln from orig adding upstream callbk)
int gui_app_start_capture(gui_app_t *app) {
    fprintf(stderr, "[GUI] gui_app_start_capture called\n");

    if (app->is_capturing) {
        fprintf(stderr, "[GUI] Already capturing\n");
        return 0;
    }

    if (app->device_count == 0) {
        fprintf(stderr, "[GUI] No devices available\n");
        gui_app_set_status(app, "No devices available");
        return -1;
    }

    device_info_t *dev = &app->devices[app->selected_device];
    fprintf(stderr, "[GUI] Selected device: %s (type %d, index %d)\n", dev->name, dev->type, dev->index);

    // Handle simulated device separately
    if (dev->type == DEVICE_TYPE_SIMULATED) {
        int sim_rc = gui_simulated_start(app);
        if (sim_rc == 0) {
            gui_capture_hold_power_assertions();
        }
        return sim_rc;
    }

    // Handle playback device
    if (dev->type == DEVICE_TYPE_PLAYBACK) {
        const char *file_a = app->settings.playback_file_a[0] ? app->settings.playback_file_a : NULL;
        const char *file_b = app->settings.playback_file_b[0] ? app->settings.playback_file_b : NULL;
        if (!file_a && !file_b) {
            gui_app_set_status(app, "No playback files selected");
            return -1;
        }
        int playback_rc = gui_playback_start(app, file_a, file_b);
        if (playback_rc == 0) {
            gui_capture_hold_power_assertions();
        }
        return playback_rc;
    }

    if (dev->type == DEVICE_TYPE_CXADC) {
        int cxadc_cards = dev->index;
        if (cxadc_cards < 1) cxadc_cards = 1;
        if (cxadc_cards > 2) cxadc_cards = 2;
        gui_capture_apply_cxadc_profile(app, cxadc_cards);
        int cxadc_rc = gui_cxadc_start(app, cxadc_cards);
        if (cxadc_rc == 0) {
            bool prev_runtime_mode = app->capture_mode_runtime_misrc;
            app->capture_mode_runtime_misrc = app->user_capture_mode_misrc;
            TraceLog(LOG_INFO,
                     "MODE TRACE: source=gui_app_start_capture_cxadc latch_runtime old=%s new=%s user=%s",
                     prev_runtime_mode ? "MISRC" : "HSDAOH",
                     app->capture_mode_runtime_misrc ? "MISRC" : "HSDAOH",
                     app->user_capture_mode_misrc ? "MISRC" : "HSDAOH");
            app->capture_start_time = GetTime();
            app->reconnect_pending = false;
            app->reconnect_attempts = 0;
            {
                time_t t = time(NULL);
                struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
                localtime_s(&tmv, &t);
#else
                localtime_r(&t, &tmv);
#endif
                snprintf(app->capture_timestamp, sizeof(app->capture_timestamp), "%04d.%02d.%02d_%02d.%02d.%02d",
                         (tmv.tm_year + 1900),
                         tmv.tm_mon + 1,
                         tmv.tm_mday,
                         tmv.tm_hour,
                         tmv.tm_min,
                         tmv.tm_sec);
            }
            gui_capture_hold_power_assertions();
        }
        return cxadc_rc;
    }

#ifdef ENABLE_FX3
    // Handle FX3 device
    if (dev->type == DEVICE_TYPE_FX3) {
        proc_set_priority(PROC_PRIORITY_ABOVE);
        thrd_set_priority(THRD_PRIORITY_CRITICAL);
        // Open FX3 device first
        int r = gui_fx3_open(app, dev->index);
        if (r < 0) {
            gui_app_set_status(app, "Failed to open FX3 device");
            proc_set_priority(PROC_PRIORITY_NORMAL);
            return -1;
        }
        int fx3_rc = gui_fx3_start(app);
        if (fx3_rc == 0) {
            gui_capture_hold_power_assertions();
        } else {
            proc_set_priority(PROC_PRIORITY_NORMAL);
        }
        return fx3_rc;
    }
#endif

#ifdef ENABLE_DDD
    // Handle DdD device
    if (dev->type == DEVICE_TYPE_DDD) {
        proc_set_priority(PROC_PRIORITY_ABOVE);
        thrd_set_priority(THRD_PRIORITY_CRITICAL);
        // Open DdD device first
        int r = gui_ddd_open(app, dev->index);
        if (r < 0) {
            gui_app_set_status(app, "Failed to open DdD device");
            proc_set_priority(PROC_PRIORITY_NORMAL);
            return -1;
        }
        int ddd_rc = gui_ddd_start(app);
        if (ddd_rc == 0) {
            gui_capture_hold_power_assertions();
        } else {
            proc_set_priority(PROC_PRIORITY_NORMAL);
        }
        return ddd_rc;
    }
#endif

    // Ensure capture buffer is initialized via buffer manager
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[GUI] Failed to initialize capture ringbuffer\n");
        gui_app_set_status(app, "Failed to initialize capture buffer");
        return -1;
    }
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
    atomic_store(&app->sample_rate, DEFAULT_SAMPLE_RATE);
    atomic_store(&app->audio_sample_rate, DEFAULT_AUDIO_SAMPLE_RATE);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    app->low_signal_time = 0.0f;
    app->low_signal_armed = false;
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);
    app->last_recording_duration_s = 0.0;
    s_capture_last_wait_count = 0;
    s_capture_last_drop_count = 0;
    s_capture_last_logged_drop_total = 0;
    s_capture_missed_streak = 0;
    s_capture_missed_burst_reported = false;
    s_capture_prev_callback_time_ms = 0;

    // Reset display buffers (per-channel)
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Reset callback counter; parser state is reset only after stream starts successfully.
    s_callback_count = 0;
 
// Open device
    int r = -1;
    app->hs_dev = NULL;
    app->sc_dev = NULL;

    if (dev->type == DEVICE_TYPE_SIMPLE_CAPTURE) {
#ifdef HSDAOH_UPSTREAM
        gui_app_set_status(app, "Simple-capture unavailable in HSDAOH_UPSTREAM build");
        return -1;
#else
        fprintf(stderr, "[GUI] Opening %s device %s...\n", sc_get_impl_name(), dev->serial);
        proc_set_priority(PROC_PRIORITY_ABOVE);
        thrd_set_priority(THRD_PRIORITY_CRITICAL);
        r = sc_start_capture(dev->serial, 1920, 1080, SC_CODEC_YUYV, 60, 1,
                             gui_simple_capture_callback, app, &app->sc_dev);
        if (r < 0 || !app->sc_dev) {
            fprintf(stderr, "[GUI] sc_start_capture failed: %d\n", r);
            gui_app_set_status(app, "Failed to open simple-capture device");
            app->sc_dev = NULL;
            proc_set_priority(PROC_PRIORITY_NORMAL);
            return -1;
        }
#endif
    } else {
#ifdef HSDAOH_UPSTREAM
        fprintf(stderr, "[GUI] Opening device index %d (HSDAOH_UPSTREAM)...\n", dev->index);
        proc_set_priority(PROC_PRIORITY_ABOVE);
        thrd_set_priority(THRD_PRIORITY_CRITICAL);

        r = hsdaoh_open(&app->hs_dev, dev->index);
        if (r < 0 || !app->hs_dev) {
            fprintf(stderr, "[GUI] RP-hsdaoh_open failed: %d\n", r);
            if (r == -3) {
                gui_app_set_status(app, "Permission denied opening MS2130 via libusb; run misrc_gui with sudo");
                proc_set_priority(PROC_PRIORITY_NORMAL);
                return -3;
            } else {
                gui_app_set_status(app, "Failed to open device");
            }
            app->hs_dev = NULL;
            proc_set_priority(PROC_PRIORITY_NORMAL);
            return -1;
        }
        hsdaoh_set_msg_callback(app->hs_dev, gui_message_callback, app);
#else
        fprintf(stderr, "[GUI] Opening hsdaoh device...\n");
        proc_set_priority(PROC_PRIORITY_ABOVE);
        thrd_set_priority(THRD_PRIORITY_CRITICAL);
        r = hsdaoh_open(&app->hs_dev, dev->index);
        if (r < 0 || !app->hs_dev) {
            fprintf(stderr, "[GUI] hsdaoh_open failed: %d\n", r);
            if (r == -3) {
                gui_app_set_status(app, "Permission denied opening MS2130 via libusb; run misrc_gui with sudo");
                proc_set_priority(PROC_PRIORITY_NORMAL);
                return -3;
            } else {
                gui_app_set_status(app, "Failed to open device");
            }
            app->hs_dev = NULL;
            proc_set_priority(PROC_PRIORITY_NORMAL);
            return -1;
        }

        hsdaoh_set_msg_callback(app->hs_dev, gui_message_callback, app);
        hsdaoh_raw_callback(app->hs_dev, true);
#endif

        fprintf(stderr, "[GUI] Starting stream...\n");

#ifdef HSDAOH_UPSTREAM
        r = MISRC_HSDAOH_START_STREAM(app->hs_dev, gui_capture_upstream_callback, app);
#else
        r = MISRC_HSDAOH_START_STREAM(app->hs_dev, (hsdaoh_read_cb_t)gui_capture_callback, app);
#endif

        if (r < 0) {
            fprintf(stderr, "[GUI] hsdaoh_start_stream failed: %d\n", r);
            gui_app_set_status(app, "Failed to start stream");
            hsdaoh_close(app->hs_dev);
            app->hs_dev = NULL;
            proc_set_priority(PROC_PRIORITY_NORMAL);
            return -1;
        }
    }

#ifndef HSDAOH_UPSTREAM
    // Capture lifecycle boundary: keep parser setup/reset work after stream
    // startup succeeds, not on pre-open or failed-start paths.
    gui_capture_configure_handler(app, true);
#endif

        bool prev_runtime_mode = app->capture_mode_runtime_misrc;
        app->capture_mode_runtime_misrc = app->user_capture_mode_misrc;
        TraceLog(LOG_INFO,
                 "MODE TRACE: source=gui_app_start_capture latch_runtime old=%s new=%s user=%s",
                 prev_runtime_mode ? "MISRC" : "HSDAOH",
                 app->capture_mode_runtime_misrc ? "MISRC" : "HSDAOH",
                 app->user_capture_mode_misrc ? "MISRC" : "HSDAOH");
        app->is_capturing = true;
        app->capture_start_time = GetTime();
        app->reconnect_pending = false;
        app->reconnect_attempts = 0;

#if defined(__APPLE__)
    /* hsdaoh_start_stream() / sc_start_capture() have now spawned their
     * internal libusb/libuvc transport threads. Walk the task one more time
     * and force every thread (ours + third-party) off the timeshare class so
     * CLPC stops parking them on the E-cluster. */
    macos_promote_all_task_threads();
#endif

    // Capture-start timestamp (used for auto naming if enabled)
    {
        time_t t = time(NULL);
        struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        snprintf(app->capture_timestamp, sizeof(app->capture_timestamp), "%04d.%02d.%02d_%02d.%02d.%02d",
                 (tmv.tm_year + 1900),
                 tmv.tm_mon + 1,
                 tmv.tm_mday,
                 tmv.tm_hour,
                 tmv.tm_min,
                 tmv.tm_sec);
    }

    // Start audio thread for monitoring/draining (keeps audio always-on during capture).
    // gui_audio_start() will only open/write files when app->is_recording is true.
    (void)gui_audio_start(app, &app->buffers);

    // Start the extraction thread - runs continuously from capture start
    r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[GUI] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        if (app->hs_dev) {
            hsdaoh_stop_stream(app->hs_dev);
            hsdaoh_close(app->hs_dev);
            app->hs_dev = NULL;
        }
        if (app->sc_dev) {
            sc_stop_capture(app->sc_dev);
            app->sc_dev = NULL;
        }
        app->is_capturing = false;
        proc_set_priority(PROC_PRIORITY_NORMAL);
        return -1;
    }

    // Start the display thread - processes BUF_DISPLAY for oscilloscope/CVBS
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[GUI] Failed to start display thread (non-fatal)\n");
            // Non-fatal - display will use legacy path
        }
    }
    gui_capture_hold_power_assertions();

#if defined(__APPLE__)
    /* Final sweep: catch any late-created dispatch/workqueue/Metal helper
     * threads that spun up during extraction/display thread init. */
    macos_promote_all_task_threads();
#endif

    gui_app_set_status(app, "Capturing...");

    return 0;
}

// Stop capture
void gui_app_stop_capture(gui_app_t *app) {
    if (!app->is_capturing) {
        return;
    }

    // Return process scheduling to default once capture mode ends.
    proc_set_priority(PROC_PRIORITY_NORMAL);

    if (app->is_recording) {
        gui_app_stop_recording(app);
    }
    gui_capture_release_power_assertions();

    // Check if this is a simulated, playback, or FX3 capture
    device_info_t *dev = &app->devices[app->selected_device];
    if (dev->type == DEVICE_TYPE_SIMULATED) {
        gui_simulated_stop(app);
        gui_app_clear_display(app);
        return;
    }
    if (dev->type == DEVICE_TYPE_PLAYBACK) {
        gui_playback_stop(app);
        gui_app_clear_display(app);
        return;
    }
    if (dev->type == DEVICE_TYPE_CXADC) {
        gui_cxadc_stop(app);
        app->capture_timestamp[0] = '\0';
        gui_app_clear_display(app);
        return;
    }
#ifdef ENABLE_FX3
    if (dev->type == DEVICE_TYPE_FX3) {
        gui_fx3_stop(app);
        gui_app_clear_display(app);
        return;
    }
#endif
#ifdef ENABLE_DDD
    if (dev->type == DEVICE_TYPE_DDD) {
        gui_ddd_stop(app);
        gui_app_clear_display(app);
        return;
    }
#endif

    // Set is_capturing to false BEFORE stopping extraction thread
    // The extraction thread checks this flag to know when to exit
    app->is_capturing = false;
    app->capture_timestamp[0] = '\0';

    // Stop display thread first (it reads from BUF_DISPLAY written by extraction)
    if (app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }

    // Stop audio monitoring thread
    gui_audio_stop(app);

    // Stop extraction thread before closing device
    gui_extract_stop();

    if (app->hs_dev) {
        hsdaoh_stop_stream(app->hs_dev);
        hsdaoh_close(app->hs_dev);
        app->hs_dev = NULL;
    }
    if (app->sc_dev) {
        sc_stop_capture(app->sc_dev);
        app->sc_dev = NULL;
    }

    atomic_store(&app->stream_synced, false);
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    app->low_signal_time = 0.0f;
    app->low_signal_armed = false;
    s_capture_missed_streak = 0;
    s_capture_missed_burst_reported = false;
    s_capture_prev_callback_time_ms = 0;
    s_capture_last_logged_drop_total = 0;
#ifndef HSDAOH_UPSTREAM
    // Capture lifecycle boundary: keep parser teardown/reset work in stop
    // cleanup so reconnects begin from a clean baseline.
    gui_capture_configure_handler(app, true);
#endif

    // Print capture summary with backpressure stats
    uint32_t frames = atomic_load(&app->frame_count);
    uint32_t missed = atomic_load(&app->missed_frame_count);
    uint32_t errors_total = atomic_load(&app->error_count);
    uint32_t parser_errors = atomic_load(&app->parser_error_count);
    uint32_t system_errors = atomic_load(&app->system_error_count);
    uint32_t waits = atomic_load(&app->rb_wait_count);
    uint32_t drops = atomic_load(&app->rb_drop_count);
    fprintf(stderr, "[GUI] Capture stopped: %u frames, %u missed, %u errors (parser=%u, system=%u), %u waits, %u drops\n",
            frames, missed, errors_total, parser_errors, system_errors, waits, drops);

    // Clear display to show "No Signal"
    gui_app_clear_display(app);

    gui_app_set_status(app, "Capture stopped");
}

// Note: Audio buffer now accessed via app->buffers (buffer_manager)
// Use BUF_CAPTURE_AUDIO with bufmgr_read_begin/bufmgr_read_end

// Recording wrappers - delegate to gui_record module
int gui_app_start_recording(gui_app_t *app) {
    return gui_record_start(app);
}

void gui_app_stop_recording(gui_app_t *app) {
    gui_record_stop(app);
}

// Helper to update one direction of VU meter (pos or neg)
static void update_vu_direction(float *level, float *peak, float *peak_hold_time,
                                 float target, float dt) {
    // Fast attack, slow release
    if (target > *level) {
        *level = target;
    } else {
        *level += (target - *level) * dt / VU_RELEASE_TIME;
    }

    // Peak hold
    if (target > *peak) {
        *peak = target;
        *peak_hold_time = 0;
    } else {
        *peak_hold_time += dt;
        if (*peak_hold_time > PEAK_HOLD_DURATION) {
            *peak -= dt * PEAK_DECAY_RATE;
            if (*peak < 0) *peak = 0;
        }
    }
}

// Update VU meters - tracks positive and negative separately for AC signals
void gui_app_update_vu_meters(gui_app_t *app, float dt) {
    // Get current peak values (separate pos/neg)
    uint16_t peak_a_pos = atomic_load(&app->peak_a_pos);
    uint16_t peak_a_neg = atomic_load(&app->peak_a_neg);
    uint16_t peak_b_pos = atomic_load(&app->peak_b_pos);
    uint16_t peak_b_neg = atomic_load(&app->peak_b_neg);

    // Convert to normalized levels (0-1), using 2048 as full scale
    float level_a_pos = (float)peak_a_pos / 2048.0f;
    float level_a_neg = (float)peak_a_neg / 2048.0f;
    float level_b_pos = (float)peak_b_pos / 2048.0f;
    float level_b_neg = (float)peak_b_neg / 2048.0f;

    // Clamp to 1.0
    if (level_a_pos > 1.0f) level_a_pos = 1.0f;
    if (level_a_neg > 1.0f) level_a_neg = 1.0f;
    if (level_b_pos > 1.0f) level_b_pos = 1.0f;
    if (level_b_neg > 1.0f) level_b_neg = 1.0f;

    // Update channel A (positive and negative separately)
    update_vu_direction(&app->vu_a.level_pos, &app->vu_a.peak_pos,
                        &app->vu_a.peak_hold_time_pos, level_a_pos, dt);
    update_vu_direction(&app->vu_a.level_neg, &app->vu_a.peak_neg,
                        &app->vu_a.peak_hold_time_neg, level_a_neg, dt);

    // Update channel B (positive and negative separately)
    update_vu_direction(&app->vu_b.level_pos, &app->vu_b.peak_pos,
                        &app->vu_b.peak_hold_time_pos, level_b_pos, dt);
    update_vu_direction(&app->vu_b.level_neg, &app->vu_b.peak_neg,
                        &app->vu_b.peak_hold_time_neg, level_b_neg, dt);
}

// Update display buffer (called from main thread)
// Note: Display is now updated by the continuous extraction thread
void gui_app_update_display_buffer(gui_app_t *app) {
    (void)app;
    // No-op: extraction thread handles display updates continuously
}

// Clear display buffer and reset VU meters (called when device disconnects)
void gui_app_clear_display(gui_app_t *app) {
    // Clear display samples (per-channel)
    memset(app->display_samples_a, 0, sizeof(app->display_samples_a));
    memset(app->display_samples_b, 0, sizeof(app->display_samples_b));
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Reset VU meters
    app->vu_a.level_pos = 0;
    app->vu_a.level_neg = 0;
    app->vu_a.peak_pos = 0;
    app->vu_a.peak_neg = 0;
    app->vu_a.peak_hold_time_pos = 0;
    app->vu_a.peak_hold_time_neg = 0;

    app->vu_b.level_pos = 0;
    app->vu_b.level_neg = 0;
    app->vu_b.peak_pos = 0;
    app->vu_b.peak_neg = 0;
    app->vu_b.peak_hold_time_pos = 0;
    app->vu_b.peak_hold_time_neg = 0;

    // Reset peak values
    atomic_store(&app->peak_a_pos, 0);
    atomic_store(&app->peak_a_neg, 0);
    atomic_store(&app->peak_b_pos, 0);
    atomic_store(&app->peak_b_neg, 0);

    // Reset stream sync status
    atomic_store(&app->stream_synced, false);
}

/* Prevent GUI lock with hsdaoh error handling added */
    void gui_app_set_status(gui_app_t *app, const char *message)
    {
        strncpy(app->status_message, message,
                sizeof(app->status_message) - 1);
        app->status_message[sizeof(app->status_message) - 1] = '\0';
        app->status_message_time = GetTime();
    }
void gui_capture_poll_hsdaoh_status(gui_app_t *app)
{
    if (!app) return;

    uint64_t now = get_time_ms();
#ifdef HSDAOH_UPSTREAM
    gui_capture_emit_realtime_diag(app, now);
#endif

    // Poll rate: 2 seconds (non-realtime, as requested)
    if (app->hs_ui_last_poll_ms != 0 && (now - app->hs_ui_last_poll_ms) < 2000) {
        return;
    }
    app->hs_ui_last_poll_ms = now;

    if (!atomic_load(&app->hs_msg_pending)) {
        return;
    }

    char local[512];

    // Copy cached message (try-lock; if locked, skip this poll tick)
    if (atomic_flag_test_and_set(&app->hs_msg_lock)) {
        return;
    }
    strncpy(local, app->hs_msg_buf, sizeof(local) - 1);
    local[sizeof(local) - 1] = '\0';
    atomic_store(&app->hs_msg_pending, false);
    atomic_flag_clear(&app->hs_msg_lock);

    // UI thread updates GUI-visible text
    if (local[0] != '\0') {
        gui_app_set_status(app, local);
    }
}

// Check if device has timed out (no callbacks for too long)
bool gui_capture_device_timeout(gui_app_t *app, uint32_t timeout_ms) {
    if (!app->is_capturing) return false;

    uint64_t last_cb = atomic_load(&app->last_callback_time_ms);
    uint64_t now = get_time_ms();
    // If time moved backwards (e.g., non-monotonic fallback), reset baseline.
    // This avoids treating a backward jump as a giant timeout.
    if (now < last_cb) {
        atomic_store(&app->last_callback_time_ms, now);
        return false;
    }

    return (now - last_cb) > timeout_ms;
}
