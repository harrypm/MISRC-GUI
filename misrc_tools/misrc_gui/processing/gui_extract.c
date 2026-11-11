/*
 * MISRC hsdaoh-rp2350 GUI - Sample Extraction and Display Processing
 *
 * Continuous extraction thread that runs from capture start to capture stop.
 * - Always reads from capture ringbuffer
 * - Always updates display buffers for GUI
 * - When recording enabled, also writes to record ringbuffers
 */

#include "gui_extract.h"
#include "../net/gui_monitor_transport.h"
#include "../net/gui_net.h"
#include "../net/gui_monitor_video_server.h"
#include "../net/gui_remote_display.h"
#include "gui_rf_stream_marks.h"
#include "../core/gui_app.h"
#include "../output/gui_record.h"
#include "../visualization/gui_oscilloscope.h"
#include "../signal/gui_cvbs.h"
#include "../../common/extract.h"
#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"
#include "../../common/misrc_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

// Buffer sizes
#define BUFFER_READ_SIZE 65536
#define RF_EXTRACT_INPUT_ALIGNMENT 16u
#define RF_EXTRACT_SIMD_SAMPLES 8u

// Extraction buffers (page-aligned for SSE/AVX)
static int16_t *s_buf_a = NULL;
static int16_t *s_buf_b = NULL;
static uint8_t *s_buf_aux = NULL;
static conv_function_t s_extract_fn = NULL;
static bool s_initialized = false;

static bool s_b_present = true; // Runtime capability set at capture start

// Scratch buffers for recording conversions
static int8_t *s_tmp8_a = NULL;
static int8_t *s_tmp8_b = NULL;
static int32_t *s_tmp32_a = NULL;
static int32_t *s_tmp32_b = NULL;

// Note: Record ringbuffers now managed by buffer manager (BUF_RECORD_A, BUF_RECORD_B)

// Extraction thread state
static thrd_t s_extract_thread;
static bool s_extract_thread_running = false;
static atomic_bool s_extract_stop_requested = false;
static gui_app_t *s_extract_app = NULL;
// Note: s_capture_rb removed - now using app->buffers (buffer_manager)

// Recording state (atomic for thread-safe access)
enum { RECORD_PUBLISH_OPEN = 1u, RECORD_PUBLISH_ACTIVE = 2u,
       RECORD_PUBLISH_DRAIN = 4u, RECORD_PUBLISH_BOUNDED = 8u };
/* Upper bits identify the recording session. A publisher preempted before
 * taking ownership cannot resume into a later session. */
static atomic_uint s_record_publish_state;
static atomic_flag s_record_cursor_guard = ATOMIC_FLAG_INIT;
static uint64_t s_record_start_byte;
static atomic_uint_fast64_t s_record_stop_byte;
static atomic_bool s_record_drain_failed;
static atomic_bool s_use_flac = false;
static atomic_uchar s_rf_bits_a = 16;
static atomic_uchar s_rf_bits_b = 16;
// Direct native RAW channels: their record ringbuffer is fed by the
// capture-side tap (gui_record_direct_tap_push), so the extraction thread
// must not write those rings (single producer per ring).
static atomic_bool s_direct_channel_a = false;
static atomic_bool s_direct_channel_b = false;

static void record_cursor_lock(void) {
    while (atomic_flag_test_and_set_explicit(&s_record_cursor_guard, memory_order_acquire))
        thrd_sleep_ms(1);
}

static void record_cursor_unlock(void) {
    atomic_flag_clear_explicit(&s_record_cursor_guard, memory_order_release);
}

static uint64_t record_source_tail(const gui_app_t *app) {
    return atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].bytes_written);
}

static conv_16to8_t s_conv_16to8 = NULL;
static conv_16to32_t s_conv_16to8to32 = NULL;

// Event signaling for producer/consumer synchronization
static rb_event_t s_data_event;       // Signaled when new data is available in ringbuffer
static rb_event_t s_space_event;      // Signaled when space becomes available in ringbuffer
static bool s_events_initialized = false;

// Note: Record buffer events now managed by buffer manager

// Periodic buffer stats logging interval (in frames, ~1 frame = 65536 samples at 40MHz = 1.64ms)
// Log every ~2 seconds = 1220 frames
#define BUFMGR_LOG_INTERVAL 1220

// Sideband stamps stay aligned with the playback RF FIFO across seek, loop,
// padding and queue drops. 8192 blocks exceed the maximum 1 GiB RF FIFO.
#define RF_STAMP_CAPACITY 8192u
typedef struct { gui_rf_block_info_t info[2]; size_t counts[2]; } rf_input_stamp_t;
static rf_input_stamp_t s_rf_stamps[RF_STAMP_CAPACITY];
static atomic_uint_fast64_t s_stamp_write, s_stamp_read, s_stamp_forwarded;
static bool s_tagged_input;
bool gui_extract_stamp_playback(const gui_rf_block_info_t info[2], const size_t counts[2]) {
    uint64_t write = atomic_load_explicit(&s_stamp_write, memory_order_relaxed);
    uint64_t read = atomic_load_explicit(&s_stamp_read, memory_order_acquire);
    if (!s_tagged_input || write-read >= RF_STAMP_CAPACITY) return false;
    rf_input_stamp_t *stamp = &s_rf_stamps[write % RF_STAMP_CAPACITY];
    memcpy(stamp->info, info, sizeof(stamp->info));
    memcpy(stamp->counts, counts, sizeof(stamp->counts));
    atomic_store_explicit(&s_stamp_write, write+1, memory_order_release);
    return true;
}
static bool read_rf_stamp(rf_input_stamp_t *stamp) {
    uint64_t read = atomic_load_explicit(&s_stamp_read, memory_order_relaxed);
    if (read == atomic_load_explicit(&s_stamp_write, memory_order_acquire)) return false;
    *stamp = s_rf_stamps[read % RF_STAMP_CAPACITY];
    atomic_store_explicit(&s_stamp_read, read+1, memory_order_release);
    return true;
}
/* Independent mixed-rate playback: one stamp ring per channel. The producer
 * publishes each channel's block info into its own ring so the extraction
 * thread can feed the preview workers honest per-channel rate/position
 * metadata (there is no shared paired axis in this mode). */
typedef struct { gui_rf_block_info_t info; size_t count; } rf_channel_stamp_t;
static rf_channel_stamp_t s_channel_stamps[2][RF_STAMP_CAPACITY];
static atomic_uint_fast64_t s_channel_stamp_write[2], s_channel_stamp_read[2];

uint64_t gui_extract_playback_stamps_written(int channel) {
    return channel < 0 ? atomic_load(&s_stamp_write) : atomic_load(&s_channel_stamp_write[channel]);
}

uint64_t gui_extract_playback_stamps_forwarded(int channel) {
    /* Independent consumers retire their stamp after forwarding; the paired
     * consumer releases its stamp early, so publish its RF boundary separately. */
    return channel < 0 ? atomic_load(&s_stamp_forwarded) : atomic_load(&s_channel_stamp_read[channel]);
}
bool gui_extract_stamp_playback_channel(int channel,
                                        const gui_rf_block_info_t *info,
                                        size_t count) {
    if (channel < 0 || channel > 1 || !info || !count || count > BUFFER_READ_SIZE) return false;
    uint64_t write = atomic_load_explicit(&s_channel_stamp_write[channel],
                                          memory_order_relaxed);
    uint64_t read = atomic_load_explicit(&s_channel_stamp_read[channel],
                                         memory_order_acquire);
    if (!s_tagged_input || write-read >= RF_STAMP_CAPACITY) return false;
    rf_channel_stamp_t *stamp = &s_channel_stamps[channel][write % RF_STAMP_CAPACITY];
    stamp->info = *info;
    stamp->count = count;
    atomic_store_explicit(&s_channel_stamp_write[channel], write+1,
                          memory_order_release);
    return true;
}
static bool peek_rf_stamp_channel(int channel, rf_channel_stamp_t *stamp) {
    uint64_t read = atomic_load_explicit(&s_channel_stamp_read[channel],
                                         memory_order_relaxed);
    if (read == atomic_load_explicit(&s_channel_stamp_write[channel],
                                     memory_order_acquire)) return false;
    *stamp = s_channel_stamps[channel][read % RF_STAMP_CAPACITY];
    return true;
}
static gui_rf_stream_marks_t *s_rf_marks;
static bool s_rf_observer_seen, s_rf_seen_b;
static uint32_t s_rf_seen_parser, s_rf_seen_missed, s_rf_seen_drops;
static uint64_t s_rf_seen_epoch;
static void observe_rf_write(void *context, uint64_t byte_position, size_t bytes) {
    gui_app_t *app = context;
    if (!app || !s_rf_marks) return;
    uint32_t parser = atomic_load(&app->parser_error_count);
    uint32_t missed = atomic_load(&app->missed_frame_count);
    uint32_t drops = atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].write_drops);
    uint64_t epoch = atomic_load(&app->rf_stream_epoch);
    bool has_b = app->capture_has_channel_b;
    bool gap = !s_rf_observer_seen || (has_b != s_rf_seen_b || parser != s_rf_seen_parser || missed != s_rf_seen_missed ||
                                    drops != s_rf_seen_drops || epoch != s_rf_seen_epoch);
    if (gap && epoch == s_rf_seen_epoch) epoch = atomic_fetch_add(&app->rf_stream_epoch,1)+1;
    gui_rf_block_info_t info[2] = {{0}};
    uint32_t shared_rate = atomic_load(&app->sample_rate);
    for (int ch=0; ch<2; ++ch) {
        info[ch].sample_rate_hz = atomic_load(&app->rf_source_rate_hz[ch]);
        if (!info[ch].sample_rate_hz) info[ch].sample_rate_hz = shared_rate;
        info[ch].epoch = epoch;
    }
    if (!has_b) info[1].sample_rate_hz = 0;
    // A parser may compact valid payload around an unknown position in this
    // write. Exclude that entire write from preview; recording is untouched.
    gui_rf_stream_marks_observe(s_rf_marks,byte_position,bytes,info,gap);
    s_rf_observer_seen = true; s_rf_seen_b = has_b;
    s_rf_seen_parser = parser; s_rf_seen_missed = missed; s_rf_seen_drops = drops; s_rf_seen_epoch = epoch;
}
void gui_extract_prepare_stream(gui_app_t *app) {
    if (!app || app->is_capturing || s_extract_thread_running) return;
    bufmgr_reset(&app->buffers,BUF_CAPTURE_RF);
    bufmgr_reset(&app->buffers,BUF_DISPLAY);
    atomic_store(&s_stamp_write,0); atomic_store(&s_stamp_read,0);
    atomic_store(&s_stamp_forwarded,0);
    for (int ch = 0; ch < 2; ++ch) {
        atomic_store(&s_channel_stamp_write[ch], 0);
        atomic_store(&s_channel_stamp_read[ch], 0);
        bufmgr_reset(&app->buffers, ch ? BUF_CAPTURE_RF_B : BUF_CAPTURE_RF_A);
        bufmgr_reset(&app->buffers, ch ? BUF_DISPLAY_B : BUF_DISPLAY_A);
    }
    // A new session starts in the paired pipeline; the playback commit
    // re-enables independent mode for a mixed-rate set after this returns.
    atomic_store(&app->playback_independent_mode, false);
    atomic_store(&app->playback_channel_paused[0], false);
    atomic_store(&app->playback_channel_paused[1], false);
    if (!s_rf_marks) s_rf_marks = gui_rf_stream_marks_create();
    gui_rf_stream_marks_reset(s_rf_marks);
    s_rf_observer_seen = false;
    atomic_fetch_add(&app->rf_stream_epoch,1);
    atomic_store(&app->rf_source_rate_hz[0],0); atomic_store(&app->rf_source_rate_hz[1],0);
    atomic_store(&app->rf_preview_input_supported,true);
    // Stock display container until a producer publishes a per-channel value
    // (playback commit/detector raises a direct-domain 16-bit channel to 4096).
    atomic_store(&app->rf_display_full_scale[0],2048);
    atomic_store(&app->rf_display_full_scale[1],2048);
    // No extraction producer is running here. Clear pressure from the prior
    // session so a new capture does not inherit its frozen-preview state.
    const gui_rf_block_info_t empty_info[2] = {{0}};
    if (!app->headless_server) gui_video_submit_rf(app,NULL,NULL,0,0,empty_info,true);
    app->buffers.rf_write_observer = observe_rf_write;
    app->buffers.rf_write_observer_context = app;
}
// Per-channel clip/peak counters for the independent loop (gui_extract_update_stats
// always touches BOTH channels' atomics, so a single-channel call would zero
// the other channel's peaks).
static void independent_channel_stats(gui_app_t *app, int channel,
                                      const int16_t *samples, size_t num_samples) {
    size_t clip_pos = 0, clip_neg = 0;
    uint16_t peak_pos = 0, peak_neg = 0;
    size_t peak_samples = (num_samples < 1000) ? num_samples : 1000;
    // This channel's REAL container full scale (a direct-domain 16-bit file
    // is NOT 12-bit: clip must be judged against its own rails, not 2047).
    uint32_t full_scale = atomic_load(&app->rf_display_full_scale[channel]);
    if (full_scale < 1) full_scale = 2048;
    int32_t clip_hi = (int32_t)full_scale - 1;
    int32_t clip_lo = -(int32_t)full_scale;
    // Peak atomics stay on the common 12-bit convention (VU meters and the
    // level-autostop helpers normalize against 2048), so native-domain peaks
    // are rescaled into that container for the meters only.
    float meter_scale = 2048.0f / (float)full_scale;
    for (size_t i = 0; i < num_samples; i++) {
        int16_t v = samples[i];
        if (v >= clip_hi) clip_pos++;
        else if (v <= clip_lo) clip_neg++;
        if (i < peak_samples) {
            if (v > 0 && (uint16_t)v > peak_pos) peak_pos = (uint16_t)v;
            if (v < 0 && (uint16_t)(-v) > peak_neg) peak_neg = (uint16_t)(-v);
        }
    }
    uint16_t meter_pos = (uint16_t)((float)peak_pos * meter_scale + 0.5f);
    uint16_t meter_neg = (uint16_t)((float)peak_neg * meter_scale + 0.5f);
    if (channel == 0) {
        atomic_fetch_add(&app->clip_count_a_pos, clip_pos);
        atomic_fetch_add(&app->clip_count_a_neg, clip_neg);
        atomic_store(&app->peak_a_pos, meter_pos);
        atomic_store(&app->peak_a_neg, meter_neg);
    } else {
        atomic_fetch_add(&app->clip_count_b_pos, clip_pos);
        atomic_fetch_add(&app->clip_count_b_neg, clip_neg);
        atomic_store(&app->peak_b_pos, meter_pos);
        atomic_store(&app->peak_b_neg, meter_neg);
    }
}

// Independent mixed-rate playback extraction (player only).
//
// The producer wrote each file's display-domain int16 samples to its own
// FIFO (BUF_CAPTURE_RF_A/B) at that file's true rate. There is no shared
// sample axis and no 32-bit packing: this loop consumes each channel
// independently, forwards per-channel blocks to the display FIFOs and the
// RF preview workers with honest per-channel rate/position stamps, and
// skips the record path entirely (recording is impossible during
// playback - the Record control is the pause toggle there).
static int extraction_thread_independent(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;
    uint32_t frame_count = 0;

    fprintf(stderr, "[EXTRACT] Independent mixed-rate playback extraction started\n");

    while (1) {
        if (atomic_load(&s_extract_stop_requested) || atomic_load(&do_exit)) break;
        if (!s_extract_app || !s_extract_app->is_capturing) break;

        bool any_data = false;
        for (int ch = 0; ch < 2; ++ch) {
            buffer_id_t rf_id = ch ? BUF_CAPTURE_RF_B : BUF_CAPTURE_RF_A;
            buffer_id_t disp_id = ch ? BUF_DISPLAY_B : BUF_DISPLAY_A;
            rf_channel_stamp_t stamp;
            if (!peek_rf_stamp_channel(ch, &stamp)) continue;
            // Mixed-rate batches and the last batch can be shorter than the
            // nominal block. Read the exact FIFO span owned by this stamp.
            size_t count = stamp.count;
            size_t read_bytes = count * sizeof(int16_t);
            void *buf = bufmgr_read_begin(&s_extract_app->buffers, rf_id, read_bytes, 10);
            if (!buf) continue;
            any_data = true;

            const int16_t *samples = (const int16_t *)buf;

            // Preview feed with THIS channel's true rate/position metadata.
            gui_rf_block_info_t info[2] = {{0}, {0}};
            info[ch] = stamp.info;
            gui_video_submit_rf(s_extract_app,
                                ch == 0 ? samples : NULL,
                                ch == 1 ? samples : NULL,
                                ch == 0 ? count : 0,
                                ch == 1 ? count : 0,
                                info,
                                true);

            // Per-channel stats (clips/peaks/counters)
            if (count > 0) {
                independent_channel_stats(s_extract_app, ch, samples, count);
                if (ch == 0) {
                    atomic_fetch_add(&s_extract_app->samples_a, count);
                } else {
                    atomic_fetch_add(&s_extract_app->samples_b, count);
                }
                atomic_fetch_add(&s_extract_app->total_samples, count);
            }

            // Display FIFO (lossy): forward the block for the display thread.
            // Zero-count (metadata failure) blocks are dropped like the
            // paired path's preview-unsafe ranges.
            if (count > 0) {
                size_t write_bytes = count * sizeof(int16_t);
                uint8_t *disp = (uint8_t *)bufmgr_write_begin(
                    &s_extract_app->buffers, disp_id, write_bytes, NULL);
                if (disp) {
                    memcpy(disp, samples, write_bytes);
                    bufmgr_write_end(&s_extract_app->buffers, disp_id, write_bytes);
                }
            }

            bufmgr_read_end(&s_extract_app->buffers, rf_id, read_bytes);
            atomic_fetch_add_explicit(&s_channel_stamp_read[ch], 1, memory_order_release);
        }

        if (!any_data) {
            // Both FIFOs idle (paused channels / EOF): keep the loop alive
            // while the session is active.
            thrd_sleep_ms(5);
            continue;
        }

        frame_count++;
        if (frame_count % BUFMGR_LOG_INTERVAL == 0) {
            bufmgr_log_periodic(&s_extract_app->buffers);
        }
    }

    fprintf(stderr, "[EXTRACT] Independent mixed-rate playback extraction exiting\n");
    return 0;
}

enum { CLIENT_RF_DISCARD_IDLE, CLIENT_RF_DISCARD_REQUESTED, CLIENT_RF_DISCARD_DONE };

bool gui_extract_discard_client_rf(gui_app_t *app) {
    int state = CLIENT_RF_DISCARD_IDLE;
    if (atomic_compare_exchange_strong(&app->net_rf_client_discard_state, &state,
                                       CLIENT_RF_DISCARD_REQUESTED) || state != CLIENT_RF_DISCARD_DONE)
        return false;
    atomic_store(&app->net_rf_client_discard_state, CLIENT_RF_DISCARD_IDLE);
    return true;
}

// Extraction thread - runs continuously from capture start to stop
// Always updates display/stats, conditionally writes to record ringbuffers
static int extraction_thread(void *ctx) {
    (void)ctx;
    const bool rtl_input = atomic_load(&s_extract_app->rf_source_is_rtlsdr);
    size_t clip[2] = {0, 0};
    uint16_t peak[2] = {0, 0};
    uint32_t frame_count = 0;
    uint64_t source_offset = 0;
    bool have_mapping = false, previous_swap = false, previous_b = false;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    fprintf(stderr, "[EXTRACT] Continuous extraction thread started\n");

    while (1) {
        // Check for exit
        if (atomic_load(&s_extract_stop_requested) || atomic_load(&do_exit)) {
            break;
        }
        unsigned pending = atomic_load(&s_record_publish_state);
        uint64_t stop_byte = atomic_load(&s_record_stop_byte);
        if ((pending & RECORD_PUBLISH_DRAIN) && source_offset * sizeof(uint32_t) >= stop_byte) {
            atomic_fetch_and(&s_record_publish_state, ~RECORD_PUBLISH_DRAIN);
            pending &= ~RECORD_PUBLISH_DRAIN;
        }
        if (!(pending & RECORD_PUBLISH_DRAIN) &&
            atomic_load(&s_extract_app->net_rf_client_discard_state) == CLIENT_RF_DISCARD_REQUESTED) {
            // The producer is paused. Only this consumer may advance head,
            // after any block already in extraction has finished publishing.
            buffer_manager_t *buffers = &s_extract_app->buffers;
            size_t stale = bufmgr_fill_level(buffers, BUF_CAPTURE_RF);
            if (stale) bufmgr_read_end(buffers, BUF_CAPTURE_RF, stale);
            source_offset += stale / sizeof(uint32_t);
            // A short tail can leave the next block misaligned for the SIMD
            // extractor. Rewind the empty ring, retaining the byte counters
            // used to match RF metadata to the logical stream position.
            ringbuffer_t *rf = &buffers->buffers[BUF_CAPTURE_RF];
            atomic_store(&rf->head, 0);
            atomic_store(&rf->tail, 0);
            atomic_store(&s_extract_app->net_rf_client_discard_state, CLIENT_RF_DISCARD_DONE);
        }
        const int16_t *mapped_a = NULL;
        const int16_t *mapped_b = NULL;
        bool recording_enabled = false;
        uint32_t ui_stride = 1;

        const uint64_t block_start = source_offset * sizeof(uint32_t);
        // A short final block is only peeked for recording. Preview will
        // consume it with the next full, aligned block in its own timeline.
        const bool tail_only = (pending & RECORD_PUBLISH_DRAIN) &&
            stop_byte - block_start < BUFFER_READ_SIZE * sizeof(uint32_t);
        size_t sample_count = BUFFER_READ_SIZE;
        if (tail_only) {
            sample_count = (size_t)(stop_byte - block_start) / sizeof(uint32_t);
        } else if (rtl_input) {
            // USB can return a partial transfer. Consume complete I/Q pairs
            // immediately so a short read doesn't add silence to recordings.
            buffer_manager_t *buffers = &s_extract_app->buffers;
            size_t available = bufmgr_fill_level(buffers, BUF_CAPTURE_RF);
            if (!available) {
                if (buffers->events_initialized[BUF_CAPTURE_RF])
                    bufmgr_wait_data(buffers, BUF_CAPTURE_RF, 20);
                else
                    thrd_sleep_ms(1);
                available = bufmgr_fill_level(buffers, BUF_CAPTURE_RF);
            }
            size_t available_pairs = available / sizeof(uint32_t);
            if (available_pairs < sample_count) sample_count = available_pairs;
        }
        size_t read_size = sample_count * sizeof(uint32_t);
        void *buf = read_size ? bufmgr_read_begin(&s_extract_app->buffers, BUF_CAPTURE_RF,
                                                  read_size, rtl_input ? 0 : 20) : NULL;
        if (!buf) {
            // No data available yet - check if we should exit
            if (!s_extract_app || !s_extract_app->is_capturing) {
                break;
            }
            // Buffer manager already waits with timeout, just continue loop
            continue;
        }

        bool rtl_unaligned = false;
        if (rtl_input && !tail_only) {
            size_t misalignment = (uintptr_t)buf % RF_EXTRACT_INPUT_ALIGNMENT;
            rtl_unaligned = misalignment != 0;
            if (rtl_unaligned) {
                // A short completion may leave the next FIFO read unaligned.
                // Consume only the scalar prefix so the bulk path realigns.
                size_t prefix = (RF_EXTRACT_INPUT_ALIGNMENT - misalignment) / sizeof(uint32_t);
                if (prefix < sample_count) sample_count = prefix;
                read_size = sample_count * sizeof(uint32_t);
            }
        }
        const size_t sample_bytes = sample_count * sizeof(int16_t);
        rf_input_stamp_t stamp = {0};
        bool preview_safe;
        bool block_has_b = s_extract_app->capture_has_channel_b;
        if (tail_only) {
            preview_safe = false;
        } else if (s_tagged_input) {
            preview_safe = read_rf_stamp(&stamp);
            if (preview_safe) block_has_b = stamp.counts[1] != 0;
        } else {
            preview_safe = gui_rf_stream_marks_read(s_rf_marks,source_offset*4,read_size,stamp.info);
            // An unsafe RF range can still carry known channel mapping. With
            // no metadata, preserve capture capability for raw recording.
            if (stamp.info[0].sample_rate_hz || stamp.info[1].sample_rate_hz)
                block_has_b = stamp.info[1].sample_rate_hz != 0;
            stamp.counts[0] = stamp.counts[1] = sample_count;
        }
        if (!preview_safe) {
            stamp.counts[0] = stamp.counts[1] = 0;
            stamp.info[0].discontinuity = stamp.info[1].discontinuity = true;
        }
        // Extract samples (always - this is the core work)
        // Prefer known per-block presence over the producer's newest flag, but
        // preview-only metadata failures must not silence the recorded B data.
        s_b_present = block_has_b;
        if (tail_only) {
            size_t tail_clip[2] = {0};
            extract_AB_C(buf, sample_count, tail_clip, s_buf_aux, s_buf_a, s_buf_b, peak);
        } else if (rtl_input) {
            size_t bulk = rtl_unaligned ? 0 : sample_count - sample_count % RF_EXTRACT_SIMD_SAMPLES;
            if (bulk) s_extract_fn(buf, bulk, clip, s_buf_aux, s_buf_a, s_buf_b, peak);
            if (bulk < sample_count) {
                extract_AB_C((uint32_t *)buf + bulk, sample_count - bulk, clip,
                             s_buf_aux + bulk, s_buf_a + bulk, s_buf_b + bulk, peak);
            }
        } else {
            s_extract_fn(buf, sample_count, clip, s_buf_aux, s_buf_a, s_buf_b, peak);
        }

        // Gated packed-word forensics (MISRC_DEBUG): immediately after the
        // extraction, compare any rail-valued OUTPUT against its SOURCE word
        // in the still-mapped ringbuffer read - pinpoints whether rails enter
        // via the producer's words or the extraction itself.
        if (misrc_debug_enabled()) {
            static int s_pack_left = 5;
            if (s_pack_left > 0) {
                int found = 0;
                for (size_t i = 0; i < sample_count && found < 4; i++) {
                    if (s_buf_a[i] >= 2047 || s_buf_a[i] <= -2048) {
                        if (found == 0) s_pack_left--;
                        uint32_t w = ((const uint32_t *)buf)[i];
                        fprintf(stderr,
                                "[PACKDBG] i=%zu out=%d word=0x%08x afield=%u (decodes %d) aux=%u bfield=%u\n",
                                i, (int)s_buf_a[i], (unsigned)w,
                                (unsigned)(w & 0xFFFu),
                                (int)(2047 - (int)(w & 0xFFFu)),
                                (unsigned)((w >> 12) & 0xFFu),
                                (unsigned)((w >> 20) & 0xFFFu));
                        found++;
                    }
                }
            }
        }

        if (!s_b_present) {
            // Prevent stale/uninitialized data from driving CH-B meters/display
            memset(s_buf_b, 0, sample_bytes);
        }
        bool swap_channels = false;
        if (s_extract_app && !s_tagged_input) {
            swap_channels = s_extract_app->is_recording
                            ? s_extract_app->capture_mode_runtime_misrc
                            : s_extract_app->user_capture_mode_misrc;
            // Hardware-variant override: some MISRC V1.5/V2.5 setups have the
            // analog channels wired opposite to the default MISRC mapping.
            // In that case, invert MISRC swap behavior so GUI/record channels
            // match the physical A/B inputs.
            if (swap_channels && s_extract_app->settings.misrc_v15_v25_ab_swap) {
                swap_channels = false;
            }
        }
        // Playback stamps already identify each file's absence and timeline
        // gaps. Ending B must not flush its buffered tail or reset playing A.
        bool mapping_gap = !s_tagged_input && have_mapping &&
                           (previous_swap != swap_channels || previous_b != s_b_present);
        have_mapping = true; previous_swap = swap_channels; previous_b = s_b_present;
        if (s_b_present && swap_channels) {
            mapped_a = s_buf_b;
            mapped_b = s_buf_a;
        } else {
            mapped_a = s_buf_a;
            mapped_b = s_b_present ? s_buf_b : NULL;
        }

        // Mark capture buffer as consumed via buffer manager
        if (!tail_only) {
            bufmgr_read_end(&s_extract_app->buffers, BUF_CAPTURE_RF, read_size);
            frame_count++;
        }
        record_cursor_lock();
        unsigned record_state = atomic_load(&s_record_publish_state);
        const bool bounded = (record_state & RECORD_PUBLISH_BOUNDED) != 0;
        uint64_t begin = block_start, end = block_start + read_size;
        const bool draining = (record_state & RECORD_PUBLISH_DRAIN) != 0;
        const uint64_t limit = atomic_load(&s_record_stop_byte);
        if (bounded && (record_state & RECORD_PUBLISH_OPEN)) {
            // Ring availability precedes the producer's absolute cursor
            // update. Do not admit a block ahead of the published cursor.
            while (record_source_tail(s_extract_app) < end && !atomic_load(&do_exit))
                thrd_sleep_ms(1);
        }
        if (bounded && begin < s_record_start_byte) begin = s_record_start_byte;
        if (draining && end > limit) end = limit;
        const bool drained = draining && end == limit;
        recording_enabled = end > begin && (record_state & (RECORD_PUBLISH_OPEN | RECORD_PUBLISH_DRAIN)) &&
            atomic_compare_exchange_strong(&s_record_publish_state, &record_state,
                                           record_state | RECORD_PUBLISH_ACTIVE);
        record_cursor_unlock();
        // Conditionally write to record buffers via buffer manager
        // Note: we always write raw int16 blocks to BUF_RECORD_A/B.
        // The writer threads (FLAC/RAW) handle RF bit depth conversion and optional soxr resampling.
        if (recording_enabled) {
            char disk_guard_msg[640] = {0};
            if (gui_record_check_disk_space_guard(s_extract_app, frame_count,
                                                  disk_guard_msg, sizeof(disk_guard_msg))) {
                unsigned prior = atomic_fetch_and(&s_record_publish_state,
                                                  ~(RECORD_PUBLISH_OPEN | RECORD_PUBLISH_DRAIN));
                if (prior & RECORD_PUBLISH_DRAIN) atomic_store(&s_record_drain_failed, true);
                recording_enabled = false;
                if (!atomic_load(&s_extract_app->dropout_stop_requested)) {
                    atomic_store(&s_extract_app->dropout_stop_reason, GUI_DROPOUT_DISK_SPACE);
                    atomic_store(&s_extract_app->dropout_stop_requested, true);
                }
                if (misrc_debug_enabled() && disk_guard_msg[0]) {
                    fprintf(stderr, "[EXTRACT] %s", disk_guard_msg);
                    fputc('\n', stderr);
                }
            } else {
                // Direct native channels are recorded by the capture-side
                // tap; skip their record writes so each record ringbuffer
                // has exactly one producer.
                bool want_a = s_extract_app->settings.capture_a &&
                              !atomic_load(&s_direct_channel_a);
                bool want_b = s_b_present && s_extract_app->settings.capture_b &&
                              !atomic_load(&s_direct_channel_b);
                bool drop_a = false, drop_b = false;
                const bool wanted[2] = {want_a, want_b};
                const bool record_swap = bounded && s_b_present &&
                    s_extract_app->capture_mode_runtime_misrc && !s_extract_app->settings.misrc_v15_v25_ab_swap;
                const int16_t *samples[2] = {
                    bounded ? (record_swap ? s_buf_b : s_buf_a) : mapped_a,
                    bounded ? (record_swap ? s_buf_a : s_buf_b) : mapped_b
                };
                const size_t record_offset = (size_t)(begin - block_start) / sizeof(uint32_t);
                const size_t record_bytes = (size_t)(end - begin) / sizeof(uint32_t) * sizeof(int16_t);
                bool *dropped[2] = {&drop_a, &drop_b};
                for (int channel = 0; channel < 2; ++channel) {
                    if (!wanted[channel]) continue;
                    char spill_error[256] = {0};
                    if (gui_record_push_converted(s_extract_app, channel, samples[channel] + record_offset,
                                                  record_bytes, frame_count, spill_error,
                                                  sizeof(spill_error))) continue;
                    *dropped[channel] = true;
                    char drop_msg[384];
                    snprintf(drop_msg, sizeof(drop_msg),
                             "Record data loss on channel %c at extract_frame=%u (spill enqueue failed%s%s)",
                             channel ? 'B' : 'A', frame_count,
                             spill_error[0] ? ": " : "", spill_error);
                    gui_record_log_capture_event(s_extract_app, "ERROR", drop_msg,
                                                 GUI_ERROR_CLASS_SYSTEM, 1);
                    if (misrc_debug_enabled()) fprintf(stderr, "%s\n", drop_msg);
                }

                if ((drop_a || drop_b) &&
                    s_extract_app->settings.stop_on_dropout &&
                    !atomic_load(&s_extract_app->dropout_stop_requested)) {
                    atomic_store(&s_extract_app->dropout_stop_reason, GUI_DROPOUT_BACKPRESSURE);
                    atomic_store(&s_extract_app->dropout_stop_requested, true);
                }
                if ((drop_a || drop_b) && atomic_load(&do_exit)) {
                    atomic_fetch_and(&s_record_publish_state, ~RECORD_PUBLISH_ACTIVE);
                    goto exit_thread;
                }
            }
            atomic_fetch_and(&s_record_publish_state, ~(RECORD_PUBLISH_ACTIVE | (drained ? RECORD_PUBLISH_DRAIN : 0u)));
        } else if (drained) {
            atomic_fetch_and(&s_record_publish_state, ~RECORD_PUBLISH_DRAIN);
        }
        if (tail_only) continue;

        if (recording_enabled) {
            bool record_a = s_extract_app->settings.capture_a;
            bool record_b = s_b_present && s_extract_app->settings.capture_b;
            unsigned int max_fill_pct = 0;
            if (record_a) {
                size_t cap_a = s_extract_app->buffers.buffers[BUF_RECORD_A].buffer_size;
                if (cap_a > 0) {
                    size_t fill_a = bufmgr_fill_level(&s_extract_app->buffers, BUF_RECORD_A);
                    unsigned int pct_a = (unsigned int)((fill_a * 100) / cap_a);
                    if (pct_a > max_fill_pct) max_fill_pct = pct_a;
                }
            }
            if (record_b) {
                size_t cap_b = s_extract_app->buffers.buffers[BUF_RECORD_B].buffer_size;
                if (cap_b > 0) {
                    size_t fill_b = bufmgr_fill_level(&s_extract_app->buffers, BUF_RECORD_B);
                    unsigned int pct_b = (unsigned int)((fill_b * 100) / cap_b);
                    if (pct_b > max_fill_pct) max_fill_pct = pct_b;
                }
            }

            if (max_fill_pct >= 85) {
                ui_stride = 8;
            } else if (max_fill_pct >= 70) {
                ui_stride = 4;
            } else if (max_fill_pct >= 50) {
                ui_stride = 2;
            } else {
                ui_stride = 1;
            }
        }

        // Signal that space is now available (buffer manager handles events internally)

        /* M-Server copies the latest ORIGINAL capture block only, at <=10 Hz.
         * Compression/resampling/FFT happen off the extraction worker in the
         * monitor HTTP thread, never blocking RF capture on slow clients. */
        if (gui_monitor_server_active()) {
            static uint64_t last_preview_ms;
            uint64_t now=get_time_ms();
            if(now-last_preview_ms>=100u){
                last_preview_ms=now;
                gui_rd_capture(mapped_a,mapped_b,sample_count,
                    atomic_load(&s_extract_app->sample_rate),(uint32_t)frame_count);
            }
        }

        // Write extracted samples to display buffer (lossy - OK to drop for display)
        // Display thread handles CVBS decode + oscilloscope processing
        // Frame layout: [samples_a: BUFFER_READ_SIZE * 2 bytes] [samples_b: BUFFER_READ_SIZE * 2 bytes]
        /* Headless M-Server builds previews directly from the captured samples
         * (gui_rd_capture above). There is no display thread to consume the
         * legacy BUF_DISPLAY queue; filling it wastes copies and creates a
         * perpetually full queue with drop counters increasing indefinitely.
         * Keep all other modes, including legacy headless Server, unchanged. */
        if ((frame_count % ui_stride) == 0 &&
            !(s_extract_app->headless_server && gui_monitor_server_active())) {
            const size_t display_channel_size = BUFFER_READ_SIZE * sizeof(int16_t);
            const size_t display_frame_size = display_channel_size * 2;
            uint8_t *display_buf = bufmgr_write_begin(&s_extract_app->buffers, BUF_DISPLAY,
                                                     display_frame_size, NULL);
            if (display_buf) {
                uint8_t *display_b = display_buf + display_channel_size;
                memcpy(display_buf, mapped_a, sample_bytes);
                if (sample_bytes < display_channel_size)
                    memset(display_buf + sample_bytes, 0, display_channel_size - sample_bytes);
                if (mapped_b) {
                    memcpy(display_b, mapped_b, sample_bytes);
                    if (sample_bytes < display_channel_size)
                        memset(display_b + sample_bytes, 0, display_channel_size - sample_bytes);
                } else {
                    memset(display_b, 0, display_channel_size);
                }

                bufmgr_write_end(&s_extract_app->buffers, BUF_DISPLAY, display_frame_size);
            }
            // If display buffer full, frame is silently dropped (lossy buffer policy)
        }

        // RF bypasses the lossy display stride. Only bounded copies happen here;
        // a full preview queue causes a source-position gap.
        if (s_b_present && swap_channels) {
            gui_rf_block_info_t info_tmp = stamp.info[0];
            stamp.info[0] = stamp.info[1]; stamp.info[1] = info_tmp;
            size_t count_tmp = stamp.counts[0];
            stamp.counts[0] = stamp.counts[1]; stamp.counts[1] = count_tmp;
        }
        stamp.info[0].discontinuity |= mapping_gap;
        stamp.info[1].discontinuity |= mapping_gap;
        if (!s_extract_app->headless_server)
            gui_video_submit_rf(s_extract_app, mapped_a, mapped_b, stamp.counts[0], stamp.counts[1],
                                  stamp.info, atomic_load(&s_extract_app->rf_preview_input_supported));
        if (s_tagged_input && preview_safe)
            atomic_store_explicit(&s_stamp_forwarded, atomic_load_explicit(&s_stamp_read, memory_order_relaxed),
                                  memory_order_release);
        /* Continuous, bounded headless video input. Capture/record never wait
         * for a decoder, H.264 encoder, or slow monitor connection. */
        if(gui_monitor_server_active())
            gui_monitor_video_server_capture(mapped_a,mapped_b,
                stamp.counts[0],stamp.counts[1],stamp.info,
                atomic_load(&s_extract_app->rf_preview_input_supported));
        source_offset += sample_count;
        // Update expensive UI stats at the same dynamic stride used for display.
        if ((frame_count % ui_stride) == 0) {
            gui_extract_update_stats(s_extract_app, mapped_a, mapped_b, sample_count);
        }

        // Gated diagnostics (~2 s): the live peak/clip counters exactly as
        // the status bar reads them (MISRC_DEBUG; automated level/clip
        // verification - dev note 2026-11-03).
        if ((frame_count % 560) == 0 && misrc_debug_enabled()) {
            fprintf(stderr,
                    "[STATS] A peak=%u/%u clip=%llu/%llu B peak=%u/%u clip=%llu/%llu\n",
                    (unsigned)atomic_load(&s_extract_app->peak_a_pos),
                    (unsigned)atomic_load(&s_extract_app->peak_a_neg),
                    (unsigned long long)atomic_load(&s_extract_app->clip_count_a_pos),
                    (unsigned long long)atomic_load(&s_extract_app->clip_count_a_neg),
                    (unsigned)atomic_load(&s_extract_app->peak_b_pos),
                    (unsigned)atomic_load(&s_extract_app->peak_b_neg),
                    (unsigned long long)atomic_load(&s_extract_app->clip_count_b_pos),
                    (unsigned long long)atomic_load(&s_extract_app->clip_count_b_neg));
        }

        // Sample counters always updated (cheap atomic ops)
        atomic_fetch_add(&s_extract_app->total_samples, sample_count);
        atomic_fetch_add(&s_extract_app->samples_a, sample_count);
        if (s_b_present) {
            atomic_fetch_add(&s_extract_app->samples_b, sample_count);
        }

        // Periodic buffer stats logging
        if (frame_count % BUFMGR_LOG_INTERVAL == 0) {
            bufmgr_log_periodic(&s_extract_app->buffers);
        }

        // Note: FFT is now processed from display samples in the render thread
        // (see gui_oscilloscope.c render_oscilloscope_channel split mode)
    }

exit_thread:
    record_cursor_lock();
    if (atomic_load(&s_record_publish_state) & RECORD_PUBLISH_DRAIN)
        atomic_store(&s_record_drain_failed, true);
    atomic_fetch_and(&s_record_publish_state, ~(RECORD_PUBLISH_DRAIN | RECORD_PUBLISH_ACTIVE));
    atomic_store(&s_extract_stop_requested, true);
    record_cursor_unlock();
    fprintf(stderr, "[EXTRACT] Continuous extraction thread exiting\n"); 
    return 0;
}

void gui_extract_init(void) {
    if (s_initialized) return;

    // Allocate 32-byte aligned buffers for SSE/AVX
    s_buf_a = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
    s_buf_b = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
    s_buf_aux = aligned_alloc(16, BUFFER_READ_SIZE);

    // Scratch buffers for conversions
    s_tmp8_a = (int8_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int8_t));
    s_tmp8_b = (int8_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int8_t));
    s_tmp32_a = (int32_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int32_t));
    s_tmp32_b = (int32_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int32_t));

    s_conv_16to8 = get_16to8_function();
    s_conv_16to8to32 = get_16to8to32_function();

    // Startup default only. gui_extract_start() always overwrites this from
    // runtime capability; this does not imply Channel B exists in upstream mode.
    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, (void*)1);
    s_b_present = true;



    // Initialize synchronization events
    if (!s_events_initialized) {
        if (rb_event_init(&s_data_event) == 0 && rb_event_init(&s_space_event) == 0) {
            s_events_initialized = true;
            fprintf(stderr, "[EXTRACT] Event signaling initialized\n");
        } else {
            fprintf(stderr, "[EXTRACT] Warning: Failed to initialize events, falling back to polling\n");
        }
    }

    // Note: Record buffer events now managed by buffer manager

    s_initialized = true;
}

void gui_extract_cleanup(void) {
    // Stop extraction thread if running
    gui_extract_stop();
    gui_rf_stream_marks_destroy(s_rf_marks);
    s_rf_marks = NULL;

    // Note: Record buffers now managed by buffer manager (cleanup via bufmgr_cleanup)

    // Destroy synchronization events
    if (s_events_initialized) {
        rb_event_destroy(&s_data_event);
        rb_event_destroy(&s_space_event);
        s_events_initialized = false;
    }

    // Free extraction buffers
    if (s_initialized) {
        if (s_buf_a) {
            aligned_free(s_buf_a);
            s_buf_a = NULL;
        }
        if (s_buf_b) {
            aligned_free(s_buf_b);
            s_buf_b = NULL;
        }
        if (s_buf_aux) {
            aligned_free(s_buf_aux);
            s_buf_aux = NULL;
        }
        if (s_tmp8_a) {
            aligned_free(s_tmp8_a);
            s_tmp8_a = NULL;
        }
        if (s_tmp8_b) {
            aligned_free(s_tmp8_b);
            s_tmp8_b = NULL;
        }
        if (s_tmp32_a) {
            aligned_free(s_tmp32_a);
            s_tmp32_a = NULL;
        }
        if (s_tmp32_b) {
            aligned_free(s_tmp32_b);
            s_tmp32_b = NULL;
        }
        s_initialized = false;
    }
}

static int extract_start_internal(gui_app_t *app, bool tagged) {
    if (s_extract_thread_running) {
        return 0;  // Already running
    }

    // Initialize extraction if needed
    gui_extract_init();

    // Configure extraction path from runtime capture capability.
    // For upstream backend, always use the AB extraction function so that
    // dual-ADC hardware (auto-detected via stream_id=1 mid-capture) has its
    // Channel B data unpacked correctly. When B is not present (single ADC),
    // bits[31:20] are zero and the extraction loop memsets s_buf_b to zero,
    // so display/recording are unaffected.
    bool upstream = (app && app->capture_backend_upstream);
    s_b_present = (app && app->capture_has_channel_b);
    bool use_ab_fn = s_b_present || upstream;
    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, use_ab_fn ? (void*)1 : NULL);
    if (!s_b_present && misrc_debug_enabled()) {
        fprintf(stderr, "[EXTRACT] Runtime A-only extraction enabled (upstream=%d)\n", upstream);
    }

    // Store context (uses app->buffers for capture ringbuffer via buffer manager)
    s_extract_app = app;
    s_tagged_input = tagged;
    atomic_store(&s_stamp_write, 0);
    atomic_store(&s_stamp_read, 0);
    atomic_store(&s_stamp_forwarded, 0);
    for (int ch = 0; ch < 2; ++ch) {
        atomic_store(&s_channel_stamp_write[ch], 0);
        atomic_store(&s_channel_stamp_read[ch], 0);
    }

    atomic_store(&s_record_publish_state, 0);
    atomic_store(&s_use_flac, false);
    atomic_store(&s_extract_stop_requested, false);
    atomic_store(&app->net_rf_client_discard_state, CLIENT_RF_DISCARD_IDLE);

    // Ensure buffers are initialized in buffer manager
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) < 0) {
        fprintf(stderr, "[EXTRACT] Failed to initialize capture buffer\n");
        return -1;
    }

    // Start extraction thread. Independent mixed-rate playback (player only)
    // runs the per-channel loop; every other session uses the paired loop.
    bool independent = app && atomic_load(&app->playback_independent_mode);
    if (thrd_create_with_priority(&s_extract_thread,
                                  independent ? extraction_thread_independent
                                              : extraction_thread,
                                  app,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[EXTRACT] Failed to create extraction thread\n");
        return -1;
    }

    s_extract_thread_running = true;
    fprintf(stderr, "[EXTRACT] Started %s extraction thread\n",
            independent ? "independent mixed-rate" : "continuous");
    return 0;
}

int gui_extract_start(gui_app_t *app) { return extract_start_internal(app, false); }
int gui_extract_start_tagged(gui_app_t *app) { return extract_start_internal(app, true); }

void gui_extract_stop(void) {
    if (!s_extract_thread_running) {
        return;
    }

    // Let a requested recording Stop publish the committed tail first.
    if (atomic_load(&s_record_publish_state) & RECORD_PUBLISH_DRAIN)
        (void)gui_extract_wait_recording_idle();
    // Disable new record publications before joining the extraction thread.
    atomic_fetch_and(&s_record_publish_state, ~RECORD_PUBLISH_OPEN);
    // Stop also works during startup failure, when is_capturing is still true,
    // and with a producer that continuously keeps the input FIFO nonempty.
    atomic_store(&s_extract_stop_requested, true);

    // Wait for thread to exit
    thrd_join(s_extract_thread, NULL);
    s_extract_thread_running = false;
    s_extract_app = NULL;

    fprintf(stderr, "[EXTRACT] Stopped continuous extraction thread\n");
}

bool gui_extract_is_running(void) {
    return s_extract_thread_running;
}

// Note: Record ringbuffers now accessed via app->buffers (buffer_manager)
// Use BUF_RECORD_A and BUF_RECORD_B with bufmgr_read_begin/bufmgr_read_end

void gui_extract_disable_recording(void) {
    atomic_fetch_and(&s_record_publish_state, ~RECORD_PUBLISH_OPEN);
}

void gui_extract_request_record_stop(void) {
    record_cursor_lock();
    unsigned state = atomic_load(&s_record_publish_state);
    if ((state & (RECORD_PUBLISH_OPEN | RECORD_PUBLISH_BOUNDED)) ==
                 (RECORD_PUBLISH_OPEN | RECORD_PUBLISH_BOUNDED)) {
        if (atomic_load(&s_extract_stop_requested)) {
            atomic_store(&s_record_drain_failed, true);
        } else {
            atomic_store(&s_record_stop_byte, record_source_tail(s_extract_app));
            atomic_fetch_or(&s_record_publish_state, RECORD_PUBLISH_DRAIN);
        }
    }
    gui_extract_disable_recording();
    record_cursor_unlock();
}

bool gui_extract_wait_recording_idle(void) {
    while (atomic_load(&s_record_publish_state) & (RECORD_PUBLISH_ACTIVE | RECORD_PUBLISH_DRAIN))
        thrd_sleep_ms(1);
    return !atomic_load(&s_record_drain_failed);
}

void gui_extract_finish_recording(void) {
    gui_extract_disable_recording();
    gui_extract_wait_recording_idle();
}

void gui_extract_set_recording(bool enabled, bool use_flac, uint8_t rf_bits_a, uint8_t rf_bits_b,
                               bool direct_a, bool direct_b) {
    if (!enabled) gui_extract_finish_recording();
    atomic_store(&s_use_flac, use_flac);
    atomic_store(&s_rf_bits_a, rf_bits_a);
    atomic_store(&s_rf_bits_b, rf_bits_b);
    atomic_store(&s_direct_channel_a, direct_a);
    atomic_store(&s_direct_channel_b, direct_b);
    if (enabled) {
        const gui_app_t *app = s_extract_app;
        const bool cxadc = app && app->selected_device >= 0 && app->selected_device < app->device_count &&
                           app->devices[app->selected_device].type == DEVICE_TYPE_CXADC;
        const bool bounded = app && !s_tagged_input && (app->settings.net_mode == GUI_NET_MODE_CLIENT || cxadc) &&
            ((app->settings.capture_a && !direct_a) || (app->settings.capture_b && !direct_b));
        record_cursor_lock();
        atomic_store(&s_record_drain_failed, false);
        unsigned generation = (atomic_load(&s_record_publish_state) & ~15u) + 16u;
        if (bounded) s_record_start_byte = record_source_tail(app);
        atomic_store(&s_record_publish_state, generation | RECORD_PUBLISH_OPEN |
                     (bounded ? RECORD_PUBLISH_BOUNDED : 0u));
        record_cursor_unlock();
    }
    fprintf(stderr, "[EXTRACT] Recording %s (FLAC: %s, bits A:%u B:%u, direct A:%s B:%s)\n",
            enabled ? "enabled" : "disabled",
            use_flac ? "yes" : "no",
            (unsigned)rf_bits_a, (unsigned)rf_bits_b,
            direct_a ? "yes" : "no", direct_b ? "yes" : "no");
}

void gui_extract_reset_record_rbs(gui_app_t *app) {
    // Reset record buffers via buffer manager
    if (app) {
        bufmgr_reset(&app->buffers, BUF_RECORD_A);
        bufmgr_reset(&app->buffers, BUF_RECORD_B);
    }
}

void gui_extract_init_record_rbs(gui_app_t *app) {
    // Initialize record buffers via buffer manager (for simulated capture that
    // doesn't go through gui_extract_start)
    if (app) {
        if (app->settings.capture_a) {
            bufmgr_ensure_init(&app->buffers, BUF_RECORD_A);
        }
        if (s_b_present && app->settings.capture_b) {
            bufmgr_ensure_init(&app->buffers, BUF_RECORD_B);
        }
        fprintf(stderr, "[EXTRACT] Record buffers initialized (for simulated capture)\n");
    }
}

bool gui_extract_is_recording(bool *use_flac) {
    if (use_flac) {
        *use_flac = atomic_load(&s_use_flac);
    }
    return (atomic_load(&s_record_publish_state) & RECORD_PUBLISH_OPEN) != 0;
}

extract_fn_t gui_extract_get_function(void) {
    if (!s_initialized) gui_extract_init();
    return (extract_fn_t)s_extract_fn;
}

int16_t *gui_extract_get_buf_a(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_a;
}

int16_t *gui_extract_get_buf_b(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_b;
}

uint8_t *gui_extract_get_buf_aux(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_aux;
}

void gui_extract_update_stats(gui_app_t *app, const int16_t *buf_a,
                              const int16_t *buf_b, size_t num_samples) {
    // Gated clip forensics (MISRC_DEBUG): the first few frames that count
    // any clip dump the offending values + positions + the buffer extremes,
    // so a false counter has a direct paper trail (dev note 2026-11-03:
    // playback counted 59k clips on a provably clean file).
    static int s_clip_forensics_left = 5;
    size_t clip_a_pos = 0, clip_a_neg = 0;
    size_t clip_b_pos = 0, clip_b_neg = 0;
    uint16_t peak_a_pos = 0, peak_a_neg = 0;
    uint16_t peak_b_pos = 0, peak_b_neg = 0;
     bool have_b = (buf_b != NULL); // buf_b may be NULL in upstream mode, so check before processing B channel stats                            
    // Peak detection uses first 1000 samples
    size_t peak_samples = (num_samples < 1000) ? num_samples : 1000;

    // Forensics capture: record offending (index, value) DURING the count
    // loop, then re-read them at print time - a value that changes between
    // the two reads proves a concurrent writer with hard indices.
    size_t dbg_idx[8];
    int16_t dbg_val[8];
    int dbg_n = 0;

     for (size_t i = 0; i < num_samples; i++) {
        int16_t sa = buf_a[i];

        // Clipping detection (12-bit ADC: +2047 is positive clip, -2048 is negative clip)
        if (sa >= 2047) clip_a_pos++;
        else if (sa <= -2048) clip_a_neg++;
        if ((sa >= 2047 || sa <= -2048) && dbg_n < 8) {
            dbg_idx[dbg_n] = i;
            dbg_val[dbg_n] = sa;
            dbg_n++;
        }

        // Peak detection (first N samples only) - A
        if (i < peak_samples) {
            if (sa > 0 && (uint16_t)sa > peak_a_pos) peak_a_pos = (uint16_t)sa;
            if (sa < 0 && (uint16_t)(-sa) > peak_a_neg) peak_a_neg = (uint16_t)(-sa);
        }

        // B only if present
        if (have_b) {
            int16_t sb = buf_b[i];

            if (sb >= 2047) clip_b_pos++;
            else if (sb <= -2048) clip_b_neg++;

            if (i < peak_samples) {
                if (sb > 0 && (uint16_t)sb > peak_b_pos) peak_b_pos = (uint16_t)sb;
                if (sb < 0 && (uint16_t)(-sb) > peak_b_neg) peak_b_neg = (uint16_t)(-sb);
            }
        }
    }


    // Gated clip forensics dump (see the note at the top of this function)
    if (misrc_debug_enabled() && s_clip_forensics_left > 0 &&
        (clip_a_pos > 0 || clip_a_neg > 0) && buf_a) {
        s_clip_forensics_left--;
        int16_t bmn = buf_a[0], bmx = buf_a[0];
        for (size_t i = 1; i < num_samples; i++) {
            if (buf_a[i] < bmn) bmn = buf_a[i];
            if (buf_a[i] > bmx) bmx = buf_a[i];
        }
        fprintf(stderr,
                "[CLIPDBG] n=%zu clips=%zu/%zu buf_minmax=[%d, %d] buf_a=%p\n",
                num_samples, clip_a_pos, clip_a_neg, (int)bmn, (int)bmx,
                (const void *)buf_a);
        for (int k = 0; k < dbg_n; k++) {
            fprintf(stderr, "[CLIPDBG]  idx=%zu counted=%d now=%d (changed=%s)\n",
                    dbg_idx[k], (int)dbg_val[k], (int)buf_a[dbg_idx[k]],
                    buf_a[dbg_idx[k]] != dbg_val[k] ? "YES" : "no");
        }
    }

    // Update atomic counters
    atomic_fetch_add(&app->clip_count_a_pos, clip_a_pos);
    atomic_fetch_add(&app->clip_count_a_neg, clip_a_neg);
    atomic_store(&app->peak_a_pos, peak_a_pos);
    atomic_store(&app->peak_a_neg, peak_a_neg);

    if (have_b) {
        atomic_fetch_add(&app->clip_count_b_pos, clip_b_pos);
        atomic_fetch_add(&app->clip_count_b_neg, clip_b_neg);
        atomic_store(&app->peak_b_pos, peak_b_pos);
        atomic_store(&app->peak_b_neg, peak_b_neg);
    } else {
        atomic_store(&app->peak_b_pos, 0);
        atomic_store(&app->peak_b_neg, 0);
        atomic_store(&app->clip_count_b_pos, 0);
        atomic_store(&app->clip_count_b_neg, 0);
    }

}

rb_event_t *gui_extract_get_data_event(void) {
    if (!s_events_initialized) return NULL;
    return &s_data_event;
}

rb_event_t *gui_extract_get_space_event(void) {
    if (!s_events_initialized) return NULL;
    return &s_space_event;
}

