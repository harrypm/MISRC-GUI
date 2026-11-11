/*
 * MISRC GUI - Direct native RAW record path
 *
 * Byte-exact passthrough recording for capture backends whose native device
 * stream is already the output format (CXADC with FLAC off and per-channel
 * resampling off). The device capture thread pushes the exact bytes it read
 * into the channel's record ringbuffer (BUF_RECORD_A/B) via
 * gui_record_direct_push(); gui_record_direct_writer_thread fwrites those
 * bytes unchanged, so the output file is the card's byte stream 1:1
 * (8-bit mode -> unsigned .u8 bytes, tenbit mode -> unsigned .u16 words).
 *
 * Ordering contract (producer + writer):
 * - Producer: while the spill backlog is non-zero, new bytes go to the spill
 *   so they queue behind the not-yet-drained spill data.
 * - Writer: drain the ringbuffer first (oldest data), then the spill.
 * Together these keep the byte stream in order under backpressure. The
 * legacy path can reorder because it retries the ringbuffer as soon as any
 * space appears while older data still sits in the spill file.
 *
 * Tail drain: the writer reads whatever is available (variable length), so
 * the final partial block at recording stop is written too. The legacy
 * fixed-block RAW writer breaks at stop with less than one full block left
 * and loses it.
 *
 * The module has no raylib/gui_app dependency so this contract is
 * unit-testable (test/gui_record_direct_harness.c): every session
 * interaction (spill file, capture log, accounting atomics, write-error
 * flag) is injected through the context.
 */

#ifndef GUI_RECORD_DIRECT_H
#define GUI_RECORD_DIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>

#include "../../common/buffer_manager.h"

// Writer read granularity cap (matches the legacy RAW writer block size).
#define GUI_RECORD_DIRECT_MAX_BLOCK_BYTES ((size_t)65536 * 32)

// Result of one producer push.
typedef enum {
    GUI_RECORD_DIRECT_PUSH_IDLE = 0,   // tap disabled: nothing recorded
    GUI_RECORD_DIRECT_PUSH_RING,       // accepted into the record ringbuffer
    GUI_RECORD_DIRECT_PUSH_SPILL,      // accepted into the spill fallback
    GUI_RECORD_DIRECT_PUSH_DROPPED,    // lost (ring full + spill failed)
} gui_record_direct_push_result_t;

// Injected session interactions. Production implementations live in
// gui_record.c (spill temp file, capture log, write-error flags); the unit
// harness provides in-memory equivalents.
typedef struct gui_record_direct_callbacks {
    // Spill-file fallback (byte stream per channel; mirrors gui_record.c).
    bool (*spill_enqueue)(void *user, int channel, const void *bytes, size_t len,
                          uint32_t frame_index);
    bool (*spill_read)(void *user, int channel, void *dst, size_t len);
    uint64_t (*spill_backlog)(void *user, int channel);
    // Output-file write-error flag (drives the UI "write error" indicator).
    void (*set_write_error)(void *user, int channel, bool active);
    // Capture-log sink. May be NULL (no logging).
    void (*log)(void *user, int channel, const char *level, const char *message);
    void *user;
} gui_record_direct_callbacks_t;

typedef struct gui_record_direct_ctx {
    buffer_manager_t *bufmgr;  // required
    int buf_id;                // BUF_RECORD_A or BUF_RECORD_B
    int channel;               // 0 = A, 1 = B
    FILE *file;                // output file (required)

    // The finalizer sets this after all record producers have stopped.
    const atomic_bool *writer_draining;

    // Accounting targets (optional; NULL = skip).
    atomic_uint_fast64_t *bytes_read_total;      // producer: bytes accepted from the device
    atomic_uint_fast64_t *bytes_written_total;    // writer: total output bytes
    atomic_uint_fast64_t *bytes_written_channel;  // writer: per-channel output bytes

    gui_record_direct_callbacks_t cb;

    // Tap handshake. Producer: mark in-flight, then read enabled. Stopper:
    // clear enabled, then wait for in-flight == 0 before draining, so no
    // push is still running when the writer performs its final drain.
    atomic_bool tap_enabled;
    atomic_int tap_inflight;
} gui_record_direct_ctx_t;

// Producer side (device capture thread). Pushes the exact bytes read from
// the card into the channel's record path (no decode, no pairing, no
// truncation). Backpressure mirrors the extraction thread: short wait, then
// spill, then drop (reported as PUSH_DROPPED for the caller to log).
gui_record_direct_push_result_t gui_record_direct_push(gui_record_direct_ctx_t *ctx,
                                                        const uint8_t *bytes, size_t len,
                                                        uint32_t frame_index);

// Iteration-scoped tap gate (multi-channel lockstep capture threads): the
// CXADC capture thread must decide ONCE per iteration whether that
// iteration's chunks are recorded, so a record start/stop handshake landing
// between the per-channel pushes cannot split the channels by one chunk
// (observed: a 64 KiB A/B count difference). enter() marks the channel's tap
// in-flight and returns the latched enabled state; the caller pushes every
// gated channel with push_latched(latched_on) or none, then leaves. The
// in-flight marking makes the stopper's disable + wait-idle sequence wait
// for the whole gated iteration (the leave is only reached after the gated
// pushes completed), so no latched push can run after the writers' final
// drain.
bool gui_record_direct_tap_iteration_enter(gui_record_direct_ctx_t *ctx);
void gui_record_direct_tap_iteration_leave(gui_record_direct_ctx_t *ctx);
gui_record_direct_push_result_t gui_record_direct_push_latched(
    gui_record_direct_ctx_t *ctx, bool latched_on,
    const uint8_t *bytes, size_t len, uint32_t frame_index);

// Writer thread (started by the record layer instead of the legacy RAW
// writer for direct channels). fwrites the bytes unchanged; variable-length
// reads; drains ring + spill remainder at stop including the final partial
// block. On output-file write failure it holds the failed block in memory
// (oldest unread data) and retries the output every ~2 s, preserving order.
int gui_record_direct_writer_thread(void *ctx_ptr);

// Tap lifecycle (record layer). enable() arms the tap for the next push;
// disable()+tap_wait_idle() is the stop-side handshake.
void gui_record_direct_tap_enable(gui_record_direct_ctx_t *ctx);
void gui_record_direct_tap_disable(gui_record_direct_ctx_t *ctx);
// Block until no producer push is in flight (bounded by the write policy).
void gui_record_direct_tap_wait_idle(gui_record_direct_ctx_t *ctx);

#endif // GUI_RECORD_DIRECT_H
