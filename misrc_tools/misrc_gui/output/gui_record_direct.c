/*
 * MISRC GUI - Direct native RAW record path (implementation)
 *
 * See gui_record_direct.h for the ordering/tail-drain contract this file
 * implements. Depends only on the buffer manager and threading helpers, so
 * the unit harness compiles it without the GUI.
 */

#include "gui_record_direct.h"

#include "../../common/threading.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// Same record-path policy as the extraction thread: a very short wait
// window so transient full-buffer spikes do not immediately force spill
// mode on a single scheduler hiccup.
static const backpressure_policy_t s_direct_write_policy = {
    .max_wait_attempts = 2,
    .wait_timeout_ms = 1,
    .log_first_wait = false,
    .log_drops = false,
};

static void gui_record_direct_log(const gui_record_direct_ctx_t *ctx,
                                  const char *level, const char *fmt, ...) {
    if (!ctx || ctx->cb.log == NULL) {
        return;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';
    ctx->cb.log(ctx->cb.user, ctx->channel, level ? level : "INFO", msg);
}

static void gui_record_direct_notify_write_error(const gui_record_direct_ctx_t *ctx, bool active) {
    if (ctx && ctx->cb.set_write_error) {
        ctx->cb.set_write_error(ctx->cb.user, ctx->channel, active);
    }
}

static void gui_record_direct_account_write(gui_record_direct_ctx_t *ctx, size_t bytes) {
    if (!ctx || bytes == 0) {
        return;
    }
    if (ctx->bytes_written_total) {
        atomic_fetch_add(ctx->bytes_written_total, bytes);
    }
    if (ctx->bytes_written_channel) {
        atomic_fetch_add(ctx->bytes_written_channel, bytes);
    }
}

void gui_record_direct_tap_enable(gui_record_direct_ctx_t *ctx) {
    if (!ctx) return;
    atomic_store(&ctx->tap_inflight, 0);
    atomic_store(&ctx->tap_enabled, true);
}

void gui_record_direct_tap_disable(gui_record_direct_ctx_t *ctx) {
    if (!ctx) return;
    atomic_store(&ctx->tap_enabled, false);
}

void gui_record_direct_tap_wait_idle(gui_record_direct_ctx_t *ctx) {
    if (!ctx) return;
    while (atomic_load(&ctx->tap_inflight) != 0) {
        thrd_sleep_ms(1);
    }
}

// Push body shared by the live-checked push and the iteration-latched
// push. No in-flight management here: the callers own the tap handshake.
static gui_record_direct_push_result_t gui_record_direct_push_inner(
    gui_record_direct_ctx_t *ctx, const uint8_t *bytes, size_t len,
    uint32_t frame_index) {
    if (ctx->bytes_read_total) {
        atomic_fetch_add(ctx->bytes_read_total, len);
    }

    gui_record_direct_push_result_t result = GUI_RECORD_DIRECT_PUSH_DROPPED;

    // Ordering rule: while the spill backlog is non-zero, new bytes go to the
    // spill so they queue behind the not-yet-drained spill data. The writer
    // drains the older ringbuffer contents first, so the byte stream stays
    // in order even under backpressure. If the spill itself fails while a
    // backlog exists, the bytes are dropped (never queued into the ring
    // ahead of older spill data).
    uint64_t backlog = 0;
    if (ctx->cb.spill_backlog) {
        backlog = ctx->cb.spill_backlog(ctx->cb.user, ctx->channel);
    }
    if (backlog > 0) {
        if (ctx->cb.spill_enqueue &&
            ctx->cb.spill_enqueue(ctx->cb.user, ctx->channel, bytes, len, frame_index)) {
            result = GUI_RECORD_DIRECT_PUSH_SPILL;
        }
        return result;
    }

    void *dst = bufmgr_write_begin(ctx->bufmgr, (buffer_id_t)ctx->buf_id, len, &s_direct_write_policy);
    if (dst) {
        memcpy(dst, bytes, len);
        bufmgr_write_end(ctx->bufmgr, (buffer_id_t)ctx->buf_id, len);
        result = GUI_RECORD_DIRECT_PUSH_RING;
    } else if (ctx->cb.spill_enqueue &&
               ctx->cb.spill_enqueue(ctx->cb.user, ctx->channel, bytes, len, frame_index)) {
        // Spilled after the policy wait: undo the drop statistic that
        // bufmgr_write_begin just counted (mirrors the extraction path).
        atomic_uint_fast32_t *drops = &ctx->bufmgr->stats[ctx->buf_id].write_drops;
        uint32_t drops_now = atomic_load(drops);
        if (drops_now > 0) {
            atomic_fetch_sub(drops, 1);
        }
        result = GUI_RECORD_DIRECT_PUSH_SPILL;
    }

    return result;
}

gui_record_direct_push_result_t gui_record_direct_push(gui_record_direct_ctx_t *ctx,
                                                       const uint8_t *bytes, size_t len,
                                                       uint32_t frame_index) {
    if (!ctx || !bytes || len == 0) {
        return GUI_RECORD_DIRECT_PUSH_IDLE;
    }

    // Handshake: mark in-flight, THEN check enabled, so the stopper's
    // disable + wait-idle sequence can never miss an active push.
    atomic_fetch_add(&ctx->tap_inflight, 1);
    if (!atomic_load(&ctx->tap_enabled)) {
        atomic_fetch_sub(&ctx->tap_inflight, 1);
        return GUI_RECORD_DIRECT_PUSH_IDLE;
    }

    gui_record_direct_push_result_t result =
        gui_record_direct_push_inner(ctx, bytes, len, frame_index);
    atomic_fetch_sub(&ctx->tap_inflight, 1);
    return result;
}

bool gui_record_direct_tap_iteration_enter(gui_record_direct_ctx_t *ctx) {
    if (!ctx) return false;
    // Handshake: mark in-flight, THEN check enabled, so the stopper's
    // disable + wait-idle sequence waits for this whole gated iteration
    // (tap_iteration_leave is only reached after the gated pushes have
    // completed) and can never miss a latched push.
    atomic_fetch_add(&ctx->tap_inflight, 1);
    return atomic_load(&ctx->tap_enabled);
}

void gui_record_direct_tap_iteration_leave(gui_record_direct_ctx_t *ctx) {
    if (!ctx) return;
    atomic_fetch_sub(&ctx->tap_inflight, 1);
}

gui_record_direct_push_result_t gui_record_direct_push_latched(
    gui_record_direct_ctx_t *ctx, bool latched_on,
    const uint8_t *bytes, size_t len, uint32_t frame_index) {
    if (!ctx || !bytes || len == 0 || !latched_on) {
        return GUI_RECORD_DIRECT_PUSH_IDLE;
    }
    // The paired tap_iteration_enter already marked this channel in-flight
    // (the stopper's wait-idle covers this push); honor the iteration-latched
    // decision instead of the live tap_enabled flag so a record start/stop
    // handshake landing between the per-channel pushes cannot split the
    // channels by one chunk.
    return gui_record_direct_push_inner(ctx, bytes, len, frame_index);
}

int gui_record_direct_writer_thread(void *ctx_ptr) {
    gui_record_direct_ctx_t *ctx = (gui_record_direct_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->bufmgr || !ctx->file) {
        return 0;
    }

    // Boost thread priority to avoid backpressure when window is minimized.
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    uint8_t *block = (uint8_t *)malloc(GUI_RECORD_DIRECT_MAX_BLOCK_BYTES);
    if (!block) {
        gui_record_direct_log(ctx, "ERROR",
                              "Direct RAW writer failed to allocate the block buffer");
        return 0;
    }

    fprintf(stderr, "[DIRECT] Writer thread %c started\n", ctx->channel == 0 ? 'A' : 'B');

    bool have_pending = false;
    size_t pending_len = 0, pending_offset = 0;
    uint64_t last_retry_ms = 0;
    uint64_t write_err_count = 0;

    while (1) {
        bool stopping = !ctx->writer_draining || atomic_load(ctx->writer_draining);

        if (have_pending) {
            // The output file failed mid-write: the failed block is held here
            // (it is the oldest unread data), retried every ~2 s. Nothing
            // else is read while it is held, so the stream cannot reorder.
            uint64_t now_ms = get_time_ms();
            bool retry_due = (last_retry_ms == 0) || now_ms < last_retry_ms ||
                             (now_ms - last_retry_ms) >= 2000;
            if (stopping && !retry_due) {
                // Final bounded best-effort attempt at stop, so an output
                // file that became writable in the meantime still receives
                // the held block. Exactly one attempt; never a hang at join.
                retry_due = true;
            }
            if (retry_due) {
                last_retry_ms = now_ms;
                clearerr(ctx->file);
                bool ok;
                if (pending_offset < pending_len) {
                    pending_offset += fwrite(block + pending_offset, 1, pending_len - pending_offset, ctx->file);
                    ok = pending_offset == pending_len && !ferror(ctx->file);
                } else {
                    ok = fflush(ctx->file) == 0;
                }
                if (ok) {
                    have_pending = false;
                    gui_record_direct_notify_write_error(ctx, false);
                    gui_record_direct_account_write(ctx, pending_len);
                    gui_record_direct_log(ctx, "INFO",
                                          "Direct RAW output file write recovered");
                    fprintf(stderr, "[DIRECT] Channel %c write recovered\n",
                            ctx->channel == 0 ? 'A' : 'B');
                } else {
                    clearerr(ctx->file);
                    write_err_count++;
                    gui_record_direct_log(ctx, "ERROR",
                                          "Direct RAW write error persists (#%llu): output file may be locked",
                                          (unsigned long long)write_err_count);
                }
            }
            if (have_pending) {
                // On stop, give the oldest block one final attempt. If it
                // succeeded, continue draining the newer queued blocks.
                if (stopping) break;
                thrd_sleep_ms(10);
            }
            continue;
        }

        // Normal drain: ringbuffer first (oldest data), then the spill
        // backlog. Variable length: whatever is available, capped at
        // GUI_RECORD_DIRECT_MAX_BLOCK_BYTES, so the final partial block at
        // stop is drained too (no fixed-block tail loss).
        // Once a spill backlog is observed, sample the ring after it:
        // the producer may have published older ring bytes just before the spill.
        uint64_t backlog = ctx->cb.spill_backlog ? ctx->cb.spill_backlog(ctx->cb.user, ctx->channel) : 0;
        size_t fill = bufmgr_fill_level(ctx->bufmgr, (buffer_id_t)ctx->buf_id);
        const void *data = NULL;
        size_t len = 0;
        bool from_ring = false;

        if (fill > 0) {
            size_t want = (fill < GUI_RECORD_DIRECT_MAX_BLOCK_BYTES)
                          ? fill : GUI_RECORD_DIRECT_MAX_BLOCK_BYTES;
            void *p = bufmgr_read_begin(ctx->bufmgr, (buffer_id_t)ctx->buf_id, want, 0);
            if (p) {
                data = p;
                len = want;
                from_ring = true;
            }
        }
        if (data == NULL && backlog && ctx->cb.spill_read) {
            size_t want = ((uint64_t)GUI_RECORD_DIRECT_MAX_BLOCK_BYTES < backlog)
                          ? GUI_RECORD_DIRECT_MAX_BLOCK_BYTES : (size_t)backlog;
            if (ctx->cb.spill_read(ctx->cb.user, ctx->channel, block, want)) {
                data = block;
                len = want;
            }
        }

        if (data == NULL) {
            if (stopping) {
                break;
            }
            // Ring is empty here and, had the spill backlog been non-zero,
            // the spill read above would have had data. The next push lands
            // in the ring, so waiting on the ring data event is sufficient.
            (void)bufmgr_wait_data(ctx->bufmgr, (buffer_id_t)ctx->buf_id, 10);
            continue;
        }

        size_t written = fwrite(data, 1, len, ctx->file);
        bool ok = written == len && !ferror(ctx->file);
        if (from_ring) {
            // Keep the failed block before returning ring memory to the producer.
            if (!ok) memcpy(block, data, len);
            bufmgr_read_end(ctx->bufmgr, (buffer_id_t)ctx->buf_id, len);
        }

        if (ok) {
            gui_record_direct_account_write(ctx, len);
            continue;
        }

        // Retry only the unwritten suffix of the oldest block before reading
        // any newer ring or spill bytes.
        clearerr(ctx->file);
        pending_len = len;
        pending_offset = written;
        have_pending = true;
        gui_record_direct_notify_write_error(ctx, true);
        write_err_count++;
        if (write_err_count <= 5 || (write_err_count % 1000) == 0) {
            gui_record_direct_log(ctx, "ERROR",
                                  "Direct RAW write error (#%llu): output file may be locked",
                                  (unsigned long long)write_err_count);
            fprintf(stderr, "[DIRECT] write error channel %d (#%llu)\n",
                    ctx->channel, (unsigned long long)write_err_count);
        }
    }

    free(block);
    fprintf(stderr, "[DIRECT] Writer thread %c exiting\n", ctx->channel == 0 ? 'A' : 'B');
    return 0;
}
