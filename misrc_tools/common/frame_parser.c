/*
 * MISRC Common - Frame Parser and Capture Handler Implementation
 *
 * Shared frame parsing, sync tracking, and capture context infrastructure
 * for hsdaoh callbacks.
 */

#include "frame_parser.h"
#include <hsdaoh_crc.h>
#include "fast_crc16.h"
#include "misrc_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
    #if defined(__APPLE__) || defined(__MACH__)
        #include <libkern/OSByteOrder.h>
        #define le16toh(x) OSSwapLittleToHostInt16(x)
        #define le32toh(x) OSSwapLittleToHostInt32(x)
    #elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
        #include <sys/endian.h>
        #define le16toh(x) letoh16(x)
        #define le32toh(x) letoh32(x)
    #else
        #include <endian.h>
        /* Some libcs only expose leXXtoh when certain feature macros are enabled.
         * Provide a fallback to avoid implicit-declaration + link errors. */
        #ifndef le16toh
            #if __BYTE_ORDER == __LITTLE_ENDIAN
                #define le16toh(x) (x)
                #define le32toh(x) (x)
            #else
                #include <byteswap.h>
                #define le16toh(x) bswap_16(x)
                #define le32toh(x) bswap_32(x)
            #endif
        #endif
    #endif
#else
    /* Windows is always little-endian */
    #define le16toh(x) (x)
    #define le32toh(x) (x)
#endif

static uint16_t frame_compute_line_crc(const uint8_t *line_dat, unsigned int width_words,
                                       bool has_stream_id)
{
    (void)has_stream_id;
    if (width_words == 0) {
        return 0;
    }
    size_t line_bytes = width_words * sizeof(uint16_t);
    return misrc_crc16_ccitt_fast(line_dat, line_bytes);
}

static int frame_crc_error_tolerance(void)
{
    static int cached = -1;
    if (cached != -1) {
        return cached;
    }

    cached = 24; /* default tolerance for minor line-level CRC noise */
    const char *v = getenv("MISRC_CRC_TOLERANCE");
    if (v && v[0]) {
        char *end = NULL;
        long parsed = strtol(v, &end, 10);
        if (end != v && *end == '\0' && parsed >= 0 && parsed <= 10000) {
            cached = (int)parsed;
        }
    }
    return cached;
}

static int frame_idle_error_tolerance(void)
{
    static int cached = -1;
    if (cached != -1) {
        return cached;
    }

    cached = 24; /* default tolerance for recurring low-count idle jitter */
    const char *v = getenv("MISRC_IDLE_TOLERANCE");
    if (v && v[0]) {
        char *end = NULL;
        long parsed = strtol(v, &end, 10);
        if (end != v && *end == '\0' && parsed >= 0 && parsed <= 10000) {
            cached = (int)parsed;
        }
    }
    return cached;
}

static unsigned int frame_idle_desync_threshold(unsigned int height)
{
    /* Large idle-error bursts are typically validator state desync after
     * discontinuities, not sustained payload corruption. Use a conservative
     * threshold tied to frame height (roughly 25% of lines, minimum 64). */
    unsigned int threshold = height / 4;
    if (threshold < 64) {
        threshold = 64;
    }
    return threshold;
}
/*-----------------------------------------------------------------------------
 * Line Parsing
 *-----------------------------------------------------------------------------*/

parsed_line_t frame_parse_line(const uint8_t *line_dat, int width,
                                bool has_stream_id, bool has_crc)
{
    parsed_line_t result = {0};
    const uint16_t *words = (const uint16_t *)line_dat;
    int trailer_words = 1 + (has_stream_id ? 1 : 0) + (has_crc ? 1 : 0);

    if (width <= trailer_words) {
        result.valid = false;
        return result;
    }

    /* Extract payload length from last word, apply 12-bit mask */
    result.payload_len = le16toh(words[width - 1]) & 0x0FFF;

    /* Extract CRC from second-to-last word if present */
    if (has_crc) {
        result.crc = le16toh(words[width - 2]);
    }

    /* Extract stream ID from third-to-last word if present */
    if (has_stream_id) {
        result.stream_id = le16toh(words[width - 3]) & 0x0FFF;
    }

    /* Validate payload length against available payload area (exclude trailer words) */
    result.valid = (result.payload_len <= (uint16_t)(width - trailer_words));

    return result;
}

/*-----------------------------------------------------------------------------
 * Frame Sync Tracking
 *-----------------------------------------------------------------------------*/

void frame_sync_init(frame_sync_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

frame_sync_result_t frame_check_sync(frame_sync_state_t *state,
                                      uint16_t framecounter,
                                      unsigned int sync_threshold)
{
    /* Check for duplicate frame */
    if (framecounter == state->last_frame_cnt) {
        return FRAME_SYNC_DUPLICATE;
    }

    /* Check for missed frames */
    uint16_t expected = (state->last_frame_cnt + 1) & 0xFFFF;
    bool in_order = (framecounter == expected);

    if (in_order) {
        state->in_order_cnt++;
    } else {
        state->in_order_cnt = 0;
    }

    state->last_frame_cnt = framecounter;

    /* Check if we just acquired sync */
    if (!state->stream_synced && state->in_order_cnt > sync_threshold) {
        state->stream_synced = true;
        state->non_sync_cnt = 0;
        return FRAME_SYNC_ACQUIRED;
    }

    if (!in_order && state->stream_synced) {
        return FRAME_SYNC_MISSED;
    }

    return FRAME_SYNC_OK;
}

/*-----------------------------------------------------------------------------
 * CRC Validation
 *-----------------------------------------------------------------------------*/

void frame_crc_init(frame_crc_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

bool frame_check_crc(frame_crc_state_t *state, const uint8_t *line_dat,
                     int width, uint16_t received_crc, enum crc_config crc_config)
{
    if (crc_config == CRC_NONE) {
        return true;  /* No CRC checking */
    }

    uint16_t expected_crc;

    if (crc_config == CRC16_1_LINE) {
        expected_crc = state->last_crc[0];
    } else {
        /* CRC16_2_LINE */
        expected_crc = state->last_crc[1];
    }

    /* Update running CRC values */
    state->last_crc[1] = state->last_crc[0];
    state->last_crc[0] = frame_compute_line_crc(line_dat, (unsigned int)width, false);

    return (received_crc == expected_crc);
}

/*-----------------------------------------------------------------------------
 * Idle Count Validation
 *-----------------------------------------------------------------------------*/

void frame_idle_init(frame_idle_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

int frame_check_idle(frame_idle_state_t *state, const uint8_t *line_dat,
                     uint16_t payload_len, int width,
                     bool has_stream_id, bool has_crc)
{
    /* Calculate idle field length */
    int idle_len = (width - 1) - payload_len;
    if (has_stream_id) idle_len--;
    if (has_crc) idle_len--;

    if (idle_len < 0) {
        return 1;
    }
    if (idle_len == 0) {
        return 0;
    }

    /* Check idle counts using hsdaoh library function */
    const uint16_t *idle_start = (const uint16_t *)line_dat + payload_len;
    return hsdaoh_check_idle_cnt(&state->idle_cnt, (uint16_t *)idle_start, (uint16_t)idle_len);
}

/*-----------------------------------------------------------------------------
 * Combined Frame Validation State
 *-----------------------------------------------------------------------------*/

void frame_parser_init(frame_parser_state_t *state)
{
    frame_sync_init(&state->sync);
    frame_crc_init(&state->crc);
    frame_idle_init(&state->idle);
    state->frames_since_error = 0;
    state->line_validation_reprime_frames = 0;
}

/*-----------------------------------------------------------------------------
 * High-Level Frame Processing
 *-----------------------------------------------------------------------------*/

frame_process_result_t frame_process(frame_parser_state_t *state,
                                      const uint8_t *buf,
                                      unsigned int width,
                                      unsigned int height,
                                      const metadata_t *meta,
                                      unsigned int sync_threshold)
{
    frame_process_result_t result = {0};
    result.sync_result = FRAME_SYNC_OK;
    result.valid = false;
    result.report_errors = false;
    int idle_error_count = 0;
    int crc_error_count = 0;
    bool suppress_line_errors = false;

    /* Validate magic number (need le32toh for big-endian systems) */
    if (le32toh(meta->magic) != HSDAOH_MAGIC) {
        state->sync.stream_synced = false;
        state->sync.in_order_cnt = 0;
        state->sync.non_sync_cnt++;
        state->line_validation_reprime_frames = 0;
        result.sync_result = FRAME_SYNC_LOST;
        return result;
    }

    /* Check for duplicate frame */
    if (meta->framecounter == state->sync.last_frame_cnt) {
        result.sync_result = FRAME_SYNC_DUPLICATE;
        return result;
    }

    /* Check frame ordering */
    uint16_t expected = (state->sync.last_frame_cnt + 1) & 0xFFFF;
    bool in_order = (meta->framecounter == expected);

    if (in_order) {
        state->sync.in_order_cnt++;
    } else {
        state->sync.in_order_cnt = 0;
        if (state->sync.stream_synced) {
            result.sync_result = FRAME_SYNC_MISSED;
            /* A framecounter gap breaks CRC and idle running-state continuity.
             * Re-prime validators and suppress line-error counting for this frame. */
            frame_crc_init(&state->crc);
            frame_idle_init(&state->idle);
            state->line_validation_reprime_frames = 1;
        }
    }

    state->sync.last_frame_cnt = meta->framecounter;
    suppress_line_errors = (state->line_validation_reprime_frames > 0);

    /* Parse all lines - always parse to update CRC/idle state */
    bool has_stream_id = (meta->flags & FLAG_STREAM_ID_PRESENT) != 0;
    bool has_crc = (meta->crc_config != CRC_NONE);

    for (unsigned int line = 0; line < height; line++) {
        const uint8_t *line_dat = buf + (width * sizeof(uint16_t) * line);

        parsed_line_t parsed = frame_parse_line(line_dat, (int)width,
                                                 has_stream_id, has_crc);

        if (!parsed.valid) {
            /* Invalid payload length - frame is corrupt */
            if (!state->sync.stream_synced) {
                state->sync.non_sync_cnt++;
            }
            return result;
        }

        /* Check idle counts */
        int idle_err = frame_check_idle(&state->idle, line_dat,
                                        parsed.payload_len, (int)width,
                                        has_stream_id, has_crc);
        if (!suppress_line_errors) {
            result.error_count += idle_err;
            idle_error_count += idle_err;
        }

        /* Check CRC if enabled - always update state, only count errors when synced */
        if (has_crc) {
            uint16_t expected_crc = (meta->crc_config == CRC16_1_LINE)
                                    ? state->crc.last_crc[0]
                                    : state->crc.last_crc[1];

            if (!suppress_line_errors &&
                parsed.crc != expected_crc &&
                state->sync.stream_synced) {
                result.error_count++;
                crc_error_count++;
            }

            /* Always update CRC state */
            state->crc.last_crc[1] = state->crc.last_crc[0];
            state->crc.last_crc[0] = frame_compute_line_crc(line_dat, width, has_stream_id);
        }

        /* Accumulate payload sizes by stream (only when synced) */
        if (parsed.payload_len > 0 && state->sync.stream_synced) {
            if (parsed.stream_id == 0) {
                result.stream0_bytes += parsed.payload_len * sizeof(uint16_t);
            } else if (parsed.stream_id == 1) {
                result.stream1_bytes += parsed.payload_len * sizeof(uint16_t);
            }
        }
    }
    if (state->line_validation_reprime_frames > 0) {
        state->line_validation_reprime_frames--;
    }
    result.crc_errors = crc_error_count;
    result.idle_errors = idle_error_count;

    /* Handle errors - only count when synced */
    if (result.error_count > 0 && state->sync.stream_synced) {
        int crc_tol = frame_crc_error_tolerance();
        int idle_tol = frame_idle_error_tolerance();
        bool tolerate_crc_only = (idle_error_count == 0 &&
                                  crc_error_count == result.error_count &&
                                  crc_tol > 0 &&
                                  crc_error_count <= crc_tol);
        bool tolerate_idle_only = (crc_error_count == 0 &&
                                   idle_error_count == result.error_count &&
                                   idle_tol > 0 &&
                                   idle_error_count <= idle_tol);
        bool tolerate_mixed_low = (idle_error_count > 0 &&
                                   crc_error_count > 0 &&
                                   idle_tol > 0 &&
                                   crc_tol > 0 &&
                                   idle_error_count <= idle_tol &&
                                   crc_error_count <= crc_tol &&
                                   result.error_count <= (idle_tol + crc_tol));
        unsigned int idle_desync_threshold = frame_idle_desync_threshold(height);
        bool idle_desync_burst = (idle_error_count > 0 &&
                                  (unsigned int)idle_error_count >= idle_desync_threshold &&
                                  crc_error_count <= crc_tol);
        if (misrc_debug_enabled()) {
            fprintf(stderr,
                    "[FP] framecounter=%u errors total=%d idle=%d crc=%d idle_tol=%d crc_tol=%d tolerate_idle_only=%s tolerate_crc_only=%s tolerate_mixed_low=%s idle_desync_burst=%s idle_desync_threshold=%u\n",
                    meta->framecounter,
                    result.error_count,
                    idle_error_count,
                    crc_error_count,
                    idle_tol,
                    crc_tol,
                    tolerate_idle_only ? "yes" : "no",
                    tolerate_crc_only ? "yes" : "no",
                    tolerate_mixed_low ? "yes" : "no",
                    idle_desync_burst ? "yes" : "no",
                    idle_desync_threshold);
        }

        if (idle_desync_burst) {
            /* Re-prime line validators without escalating a one-frame idle
             * burst to a capture-stop event. Treat this frame as invalid. */
            frame_crc_init(&state->crc);
            frame_idle_init(&state->idle);
            state->line_validation_reprime_frames = 2;
            state->frames_since_error = 0;
            result.error_count = 0;
            result.report_errors = false;
            result.valid = false;
            return result;
        }

        if (!tolerate_crc_only && !tolerate_idle_only && !tolerate_mixed_low) {
            result.report_errors = true;
            state->frames_since_error = 0;
            return result;  /* Frame has non-tolerable errors, don't mark as valid */
        }
    }

    state->frames_since_error++;

    /* Check for sync acquisition (after processing frame, like CLI) */
    if (!state->sync.stream_synced && result.error_count == 0 &&
        state->sync.in_order_cnt > sync_threshold) {
        state->sync.stream_synced = true;
        state->sync.non_sync_cnt = 0;
        result.sync_result = FRAME_SYNC_ACQUIRED;
        /* Don't set valid yet - first synced frame is for priming */
        return result;
    }

    if (state->sync.stream_synced) {
        result.valid = true;
    }

    return result;
}

size_t frame_copy_payloads_cb(const uint8_t *buf,
                               unsigned int width,
                               unsigned int height,
                               const metadata_t *meta,
                               uint8_t *out_stream0,
                               uint8_t *out_stream1,
                               frame_payload_cb_t callback,
                               void *ctx)
{
    bool has_stream_id = (meta->flags & FLAG_STREAM_ID_PRESENT) != 0;
    bool has_crc = (meta->crc_config != CRC_NONE);

    size_t offset0 = 0;
    size_t offset1 = 0;

    for (unsigned int line = 0; line < height; line++) {
        const uint8_t *line_dat = buf + (width * sizeof(uint16_t) * line);

        parsed_line_t parsed = frame_parse_line(line_dat, (int)width,
                                                 has_stream_id, has_crc);

        if (!parsed.valid || parsed.payload_len == 0) {
            continue;
        }

        size_t payload_bytes = parsed.payload_len * sizeof(uint16_t);

        /* Call filter callback if provided */
        if (callback && !callback(ctx, parsed.stream_id, line_dat, payload_bytes)) {
            continue;
        }

        if (parsed.stream_id == 0 && out_stream0) {
            memcpy(out_stream0 + offset0, line_dat, payload_bytes);
            offset0 += payload_bytes;
        } else if (parsed.stream_id == 1 && out_stream1) {
            memcpy(out_stream1 + offset1, line_dat, payload_bytes);
            offset1 += payload_bytes;
        }
    }

    return offset0 + offset1;
}

size_t frame_copy_payloads(const uint8_t *buf,
                           unsigned int width,
                           unsigned int height,
                           const metadata_t *meta,
                           uint8_t *out_stream0,
                           uint8_t *out_stream1)
{
    return frame_copy_payloads_cb(buf, width, height, meta,
                                   out_stream0, out_stream1, NULL, NULL);
}

/*-----------------------------------------------------------------------------
 * Capture Handler Initialization
 *-----------------------------------------------------------------------------*/

void capture_handler_init(capture_handler_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    frame_parser_init(&ctx->frame_state);
    atomic_store(&ctx->capture_rf, false);
    atomic_store(&ctx->capture_audio, false);
}

void capture_handler_reset_audio_sync(capture_handler_ctx_t *ctx)
{
    ctx->audio_sync_stage1 = false;
    ctx->audio_sync_stage2 = false;
}

/*-----------------------------------------------------------------------------
 * Default Callbacks
 *-----------------------------------------------------------------------------*/

void capture_handler_default_progress(void *user_ctx, unsigned int non_sync_cnt)
{
    (void)user_ctx;

    if (non_sync_cnt % 5 == 0) {
        fprintf(stderr, "\033[A\33[2K\r Received %u frames without sync...\n",
                non_sync_cnt + 1);
    }

    if (non_sync_cnt == 500) {
        fprintf(stderr, " Received more than 500 corrupted frames! Check connection!\n");
    }
}

/*-----------------------------------------------------------------------------
 * Sync Event Handling
 *-----------------------------------------------------------------------------*/

bool capture_handler_process_sync_event(capture_handler_ctx_t *ctx,
                                         frame_sync_result_t sync_result,
                                         const metadata_t *meta,
                                         bool was_synced)
{
    /* Call user callback if set */
    if (ctx->sync_event_cb) {
        ctx->sync_event_cb(ctx->user_ctx, sync_result, meta, was_synced);
    }

    switch (sync_result) {
        case FRAME_SYNC_LOST:
            /* Reset audio sync state when sync is lost */
            capture_handler_reset_audio_sync(ctx);
            return false;

        case FRAME_SYNC_DUPLICATE:
            return false;

        case FRAME_SYNC_MISSED:
            /* Caller may want to log this but can continue processing */
            break;

        case FRAME_SYNC_ACQUIRED:
            /* Reset audio sync state for clean start */
            capture_handler_reset_audio_sync(ctx);

            /* Check if audio is available when requested */
            if (atomic_load(&ctx->capture_audio)) {
                if (!(meta->flags & FLAG_STREAM_ID_PRESENT)) {
                    /* Audio requested but not available in stream */
                    if (ctx->msg_cb) {
                        ctx->msg_cb(ctx->user_ctx, 3,
                                   "MISRC does not transmit audio, cannot capture audio!\n");
                    }
                    /* Caller should handle this - we continue but audio won't work */
                }
            }
            break;

        case FRAME_SYNC_OK:
            break;
    }

    return true;
}

/*-----------------------------------------------------------------------------
 * Audio Sync Filter
 *-----------------------------------------------------------------------------*/

bool capture_handler_audio_filter(void *ctx, int stream_id,
                                   const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;

    capture_handler_ctx_t *cap = (capture_handler_ctx_t *)ctx;

    if (stream_id == 0) {
        /* RF stream */
        /* Always copy RF when requested.
         *
         * IMPORTANT: Do not gate RF on audio sync state.
         * The ordering of RF vs audio payload lines inside a frame is not guaranteed.
         * Gating RF until the first audio line can cause partial RF payload copies
         * while still committing the full expected RF byte count to the ringbuffer.
         */
        return atomic_load(&cap->capture_rf);
    } else if (stream_id == 1) {
        /* Audio stream */
        if (!atomic_load(&cap->capture_audio))
            return false;

        if (cap->audio_sync_stage2) {
            /* Fully synced - copy audio normally */
            return true;
        }

        if (cap->audio_sync_stage1) {
            /* Stage 1 -> Stage 2: second audio line seen */
            cap->audio_sync_stage2 = true;
            if (cap->audio_sync_cb) {
                cap->audio_sync_cb(cap->user_ctx, true);
            }
            /* Skip this transition frame */
            return false;
        } else {
            /* Stage 0 -> Stage 1: first audio line seen */
            cap->audio_sync_stage1 = true;
            /* Skip this frame */
            return false;
        }
    }

    /* Unknown stream ID */
    return false;
}
