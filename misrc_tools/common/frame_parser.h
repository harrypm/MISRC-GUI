/*
 * MISRC Common - Frame Parser and Capture Handler
 *
 * Shared frame parsing, sync tracking, and capture context infrastructure
 * for hsdaoh callbacks. Used by both CLI (misrc_capture) and GUI (gui_capture).
 *
 * This module provides:
 * - Low-level frame parsing (line parsing, CRC, idle validation)
 * - Frame sync tracking and acquisition
 * - High-level capture context with callbacks
 * - Audio sync state machine
 */

#ifndef MISRC_FRAME_PARSER_H
#define MISRC_FRAME_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#include <hsdaoh.h>
#include <hsdaoh_raw.h>
#include "ringbuffer.h"

/*-----------------------------------------------------------------------------
 * Line Parsing
 *-----------------------------------------------------------------------------*/

/* Parsed line data from a single video line */
typedef struct {
    uint16_t payload_len;   /* Payload length in 16-bit words (12-bit value) */
    uint16_t crc;           /* CRC value from line (if present) */
    uint16_t stream_id;     /* Stream ID (0=RF, 1=audio, if present) */
    bool valid;             /* True if payload_len is within bounds */
} parsed_line_t;

/* Parse a single line from frame data
 *
 * @param line_dat      Pointer to start of line data
 * @param width         Width of line in 16-bit words
 * @param has_stream_id True if stream ID field is present (FLAG_STREAM_ID_PRESENT)
 * @param has_crc       True if CRC field is present (crc_config != CRC_NONE)
 * @return Parsed line data
 */
parsed_line_t frame_parse_line(const uint8_t *line_dat, int width,
                                bool has_stream_id, bool has_crc);

/*-----------------------------------------------------------------------------
 * Frame Sync Tracking
 *-----------------------------------------------------------------------------*/

/* State for frame synchronization tracking */
typedef struct {
    uint16_t last_frame_cnt;    /* Last seen frame counter */
    unsigned int in_order_cnt;  /* Consecutive in-order frames */
    bool stream_synced;         /* True when synchronized */
    unsigned int non_sync_cnt;  /* Frames received without sync */
} frame_sync_state_t;

/* Initialize frame sync state */
void frame_sync_init(frame_sync_state_t *state);

/* Result of frame sync check */
typedef enum {
    FRAME_SYNC_OK,              /* Frame is in order */
    FRAME_SYNC_DUPLICATE,       /* Duplicate frame (same counter) */
    FRAME_SYNC_MISSED,          /* Missed one or more frames */
    FRAME_SYNC_LOST,            /* Lost sync (invalid magic) */
    FRAME_SYNC_ACQUIRED         /* Just acquired sync */
} frame_sync_result_t;

/* Check frame synchronization
 *
 * @param state           Sync state to update
 * @param framecounter    Frame counter from metadata
 * @param sync_threshold  Number of in-order frames required to sync (typically 4)
 * @return Sync result
 */
frame_sync_result_t frame_check_sync(frame_sync_state_t *state,
                                      uint16_t framecounter,
                                      unsigned int sync_threshold);

/*-----------------------------------------------------------------------------
 * CRC Validation
 *-----------------------------------------------------------------------------*/

/* State for CRC validation (maintains running CRC values) */
typedef struct {
    uint16_t last_crc[2];       /* Last two CRC values for 2-line mode */
} frame_crc_state_t;

/* Initialize CRC state */
void frame_crc_init(frame_crc_state_t *state);

/* Validate CRC for a line
 *
 * @param state         CRC state to update
 * @param line_dat      Pointer to start of line data
 * @param width         Width of line in 16-bit words
 * @param received_crc  CRC value received in line
 * @param crc_config    CRC configuration from metadata (enum crc_config)
 * @return True if CRC is valid (or no CRC checking), false if mismatch
 */
bool frame_check_crc(frame_crc_state_t *state, const uint8_t *line_dat,
                     int width, uint16_t received_crc, enum crc_config crc_config);

/*-----------------------------------------------------------------------------
 * Idle Count Validation
 *-----------------------------------------------------------------------------*/

/* State for idle count validation */
typedef struct {
    uint16_t idle_cnt;          /* Running idle count state */
} frame_idle_state_t;

/* Initialize idle state */
void frame_idle_init(frame_idle_state_t *state);

/* Check idle counts in a line
 *
 * @param state         Idle state to update
 * @param line_dat      Pointer to start of line data
 * @param payload_len   Payload length in 16-bit words
 * @param width         Width of line in 16-bit words
 * @param has_stream_id True if stream ID field is present
 * @param has_crc       True if CRC field is present
 * @return Number of idle errors found
 */
int frame_check_idle(frame_idle_state_t *state, const uint8_t *line_dat,
                     uint16_t payload_len, int width,
                     bool has_stream_id, bool has_crc);

/*-----------------------------------------------------------------------------
 * Combined Frame Validation State
 *-----------------------------------------------------------------------------*/

/* Combined state for all frame validation */
typedef struct {
    frame_sync_state_t sync;
    frame_crc_state_t crc;
    frame_idle_state_t idle;
    unsigned int frames_since_error;
    unsigned int line_validation_reprime_frames;
} frame_parser_state_t;

/* Initialize all frame parser state */
void frame_parser_init(frame_parser_state_t *state);

/*-----------------------------------------------------------------------------
 * High-Level Frame Processing
 *-----------------------------------------------------------------------------*/

/* Result of processing a complete frame */
typedef struct {
    frame_sync_result_t sync_result;    /* Sync status (OK, acquired, missed, etc.) */
    size_t stream0_bytes;               /* Payload bytes for stream 0 (RF) */
    size_t stream1_bytes;               /* Payload bytes for stream 1 (audio) */
    int error_count;                    /* Number of CRC/idle errors in frame */
    int crc_errors;                     /* CRC category for actionable diagnosis */
    int idle_errors;                    /* Idle category for actionable diagnosis */
    bool valid;                         /* True if frame can be processed */
    bool report_errors;                 /* True if errors should be reported (after priming) */
} frame_process_result_t;

/* Process a complete frame: validate magic, check sync, parse all lines
 *
 * @param state         Parser state to update
 * @param buf           Frame buffer from hsdaoh callback
 * @param width         Width in 16-bit words
 * @param height        Height (number of lines)
 * @param meta          Metadata extracted from frame
 * @param sync_threshold Number of in-order frames required for sync (typically 4)
 * @return Processing result with payload sizes and error info
 *
 * This function handles:
 * - Magic number validation
 * - Frame sync tracking (duplicates, ordering)
 * - Line parsing with CRC and idle validation
 * - Priming period for CRC (first 2 frames after sync don't count errors)
 */
frame_process_result_t frame_process(frame_parser_state_t *state,
                                      const uint8_t *buf,
                                      unsigned int width,
                                      unsigned int height,
                                      const metadata_t *meta,
                                      unsigned int sync_threshold);

/* Copy payload data from frame to output buffers
 *
 * @param buf           Frame buffer from hsdaoh callback
 * @param width         Width in 16-bit words
 * @param height        Height (number of lines)
 * @param meta          Metadata extracted from frame
 * @param out_stream0   Output buffer for stream 0 (RF), or NULL to skip
 * @param out_stream1   Output buffer for stream 1 (audio), or NULL to skip
 * @return Total bytes copied (stream0 + stream1)
 *
 * Call this after frame_process() returns valid=true to copy payload data.
 */
size_t frame_copy_payloads(const uint8_t *buf,
                           unsigned int width,
                           unsigned int height,
                           const metadata_t *meta,
                           uint8_t *out_stream0,
                           uint8_t *out_stream1);

/*-----------------------------------------------------------------------------
 * Callback-based Payload Copying
 *-----------------------------------------------------------------------------*/

/* Callback for per-line payload handling
 *
 * @param ctx           User context passed to frame_copy_payloads_cb
 * @param stream_id     Stream ID (0=RF, 1=audio)
 * @param data          Pointer to payload data
 * @param len           Length of payload in bytes
 * @return true to copy this payload, false to skip
 */
typedef bool (*frame_payload_cb_t)(void *ctx, int stream_id,
                                    const uint8_t *data, size_t len);

/* Copy payload data with per-line callback for filtering
 *
 * @param buf           Frame buffer from hsdaoh callback
 * @param width         Width in 16-bit words
 * @param height        Height (number of lines)
 * @param meta          Metadata extracted from frame
 * @param out_stream0   Output buffer for stream 0 (RF), or NULL to skip
 * @param out_stream1   Output buffer for stream 1 (audio), or NULL to skip
 * @param callback      Callback to decide whether to copy each payload (or NULL for all)
 * @param ctx           User context passed to callback
 * @return Total bytes copied (stream0 + stream1)
 *
 * The callback is called before each payload is copied. Return true to copy,
 * false to skip. This allows filtering or custom handling per-line.
 */
size_t frame_copy_payloads_cb(const uint8_t *buf,
                               unsigned int width,
                               unsigned int height,
                               const metadata_t *meta,
                               uint8_t *out_stream0,
                               uint8_t *out_stream1,
                               frame_payload_cb_t callback,
                               void *ctx);

/*-----------------------------------------------------------------------------
 * Capture Handler Callback Types
 *-----------------------------------------------------------------------------*/

/* Message callback - for logging messages
 *
 * @param user_ctx      User context
 * @param level         Message level (0=info, 1=warning, 2=error, 3=critical)
 * @param fmt           Format string
 * @param ...           Format arguments
 */
typedef void (*capture_msg_cb_t)(void *user_ctx, int level, const char *fmt, ...);

/* Sync progress callback - called while waiting for sync
 *
 * @param user_ctx      User context
 * @param non_sync_cnt  Number of frames received without sync
 */
typedef void (*capture_sync_progress_cb_t)(void *user_ctx, unsigned int non_sync_cnt);

/* Sync event callback - called on sync state changes
 *
 * @param user_ctx      User context
 * @param result        Sync result (ACQUIRED, LOST, MISSED, etc.)
 * @param meta          Frame metadata (for info display)
 * @param was_synced    True if was synced before this frame
 */
typedef void (*capture_sync_event_cb_t)(void *user_ctx, frame_sync_result_t result,
                                         const metadata_t *meta, bool was_synced);

/* Audio sync callback - called when audio sync state changes
 *
 * @param user_ctx      User context
 * @param synced        True when RF and audio are now synced
 */
typedef void (*capture_audio_sync_cb_t)(void *user_ctx, bool synced);

/*-----------------------------------------------------------------------------
 * Capture Handler Context
 *-----------------------------------------------------------------------------*/

typedef struct {
    /* Ringbuffer pointers (caller owns allocation) */
    ringbuffer_t *rb_rf;              /* RF data ringbuffer (required for capture) */
    ringbuffer_t *rb_audio;           /* Audio data ringbuffer (optional, NULL if no audio) */

    /* Frame parser state */
    frame_parser_state_t frame_state;

    /* Capture configuration */
    atomic_bool capture_rf;           /* Whether to capture RF stream */
    atomic_bool capture_audio;        /* Whether to capture audio stream */

    /* Audio sync state machine (two-stage sync)
     * Stage 1: First audio line seen, RF can start
     * Stage 2: Second audio line seen, audio fully synced */
    bool audio_sync_stage1;           /* First audio line seen */
    bool audio_sync_stage2;           /* Fully synced */

    /* Callbacks (all optional - can be NULL) */
    capture_msg_cb_t msg_cb;          /* For logging messages */
    capture_sync_progress_cb_t progress_cb;  /* For "waiting for sync" updates */
    capture_sync_event_cb_t sync_event_cb;   /* For sync state changes */
    capture_audio_sync_cb_t audio_sync_cb;   /* For audio sync notification */

    /* User context passed to all callbacks */
    void *user_ctx;
} capture_handler_ctx_t;

/*-----------------------------------------------------------------------------
 * Capture Handler Initialization
 *-----------------------------------------------------------------------------*/

/* Initialize capture handler context
 *
 * @param ctx           Context to initialize
 *
 * Sets all fields to safe defaults. Caller should then set:
 * - rb_rf (required for capture)
 * - rb_audio (optional for audio capture)
 * - capture_rf, capture_audio flags
 * - callbacks as needed
 * - user_ctx for callbacks
 */
void capture_handler_init(capture_handler_ctx_t *ctx);

/* Reset audio sync state machine
 *
 * @param ctx           Context to reset
 *
 * Call this when sync is lost or when starting a new capture session.
 */
void capture_handler_reset_audio_sync(capture_handler_ctx_t *ctx);

/*-----------------------------------------------------------------------------
 * Default Callbacks (for CLI-style output)
 *-----------------------------------------------------------------------------*/

/* Default sync progress callback - prints to stderr
 *
 * @param user_ctx      Unused
 * @param non_sync_cnt  Number of frames received without sync
 *
 * Prints progress every 5 frames and warns at 500 frames.
 * Can be used directly as progress_cb or called from custom callback.
 */
void capture_handler_default_progress(void *user_ctx, unsigned int non_sync_cnt);

/*-----------------------------------------------------------------------------
 * Sync Event Handling
 *-----------------------------------------------------------------------------*/

/* Process sync event and determine whether to continue
 *
 * @param ctx           Capture handler context
 * @param sync_result   Sync result from frame_process()
 * @param meta          Frame metadata (for info display)
 * @param was_synced    True if was synced before this frame
 * @return True if callback should continue processing, false to return early
 *
 * Handles LOST, DUPLICATE, MISSED, ACQUIRED, OK cases.
 * Calls sync_event_cb if set.
 * Returns false for LOST and DUPLICATE (caller should return).
 * Also resets audio sync state on LOST.
 */
bool capture_handler_process_sync_event(capture_handler_ctx_t *ctx,
                                         frame_sync_result_t sync_result,
                                         const metadata_t *meta,
                                         bool was_synced);

/*-----------------------------------------------------------------------------
 * Audio Sync Filter
 *-----------------------------------------------------------------------------*/

/* Audio sync filter callback for use with frame_copy_payloads_cb()
 *
 * @param ctx           Capture handler context (cast from void*)
 * @param stream_id     Stream ID (0=RF, 1=audio)
 * @param data          Pointer to payload data (unused)
 * @param len           Length of payload in bytes (unused)
 * @return True to copy this payload, false to skip
 *
 * Implements two-stage audio sync state machine:
 * - Stage 0: Wait for first audio line, skip all data
 * - Stage 1: First audio seen, copy RF, skip audio
 * - Stage 2: Second audio seen, copy both RF and audio
 *
 * Usage:
 *   frame_copy_payloads_cb(buf, width, height, meta,
 *                          out_rf, out_audio,
 *                          capture_handler_audio_filter, ctx);
 */
bool capture_handler_audio_filter(void *ctx, int stream_id,
                                   const uint8_t *data, size_t len);

#endif /* MISRC_FRAME_PARSER_H */
