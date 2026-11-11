/*
 * Unit harness for the direct native RAW record path (gui_record_direct.c).
 *
 * Drives the REAL producer push (gui_record_direct_push) and writer thread
 * (gui_record_direct_writer_thread) through a REAL buffer manager +
 * ringbuffer, with an in-memory spill stub and counting log / write-error
 * callbacks, and asserts:
 *   - the tap handshake: pushes while the tap is disabled are ignored
 *   - the spill fallback is used once the ring is full (deterministic: the
 *     producer pushes more than the ring holds before the writer starts)
 *   - the output file is byte-identical to the pushed pattern, which proves
 *     both the producer/consumer ordering (ring first, spill behind) and
 *     byte-exactness for odd/varied push sizes (1..999999 bytes)
 *   - the final partial block is drained at stop (no fixed-block tail loss)
 *   - write-error recovery: with the output on /dev/full (unbuffered) the
 *     writer holds the failed block, and after the output file is replaced
 *     it recovers and writes every byte in order
 *
 * Registered as the meson test 'gui_record_direct' and compiled+run by
 * ci_guard_tests.py (check_raw_direct_writer_runtime).
 */

#include "../misrc_gui/output/gui_record_direct.h"
#include "../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failures;

static void expect_true(bool condition, const char *message) {
    g_checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        g_failures++;
    }
}

/* ---- in-memory spill stub (byte FIFO per channel) ---- */

static uint8_t *g_spill_data[2];
static size_t g_spill_len[2];
static size_t g_spill_read_off[2];
static size_t g_spill_enqueue_count[2];

static bool stub_spill_enqueue(void *user, int channel, const void *bytes, size_t len,
                               uint32_t frame_index) {
    (void)user;
    (void)frame_index;
    if (channel < 0 || channel > 1 || !bytes || len == 0) return false;
    if (g_spill_read_off[channel] > 0) {
        memmove(g_spill_data[channel],
                g_spill_data[channel] + g_spill_read_off[channel],
                g_spill_len[channel] - g_spill_read_off[channel]);
        g_spill_len[channel] -= g_spill_read_off[channel];
        g_spill_read_off[channel] = 0;
    }
    uint8_t *grown = (uint8_t *)realloc(g_spill_data[channel], g_spill_len[channel] + len);
    if (!grown) return false;
    g_spill_data[channel] = grown;
    memcpy(g_spill_data[channel] + g_spill_len[channel], bytes, len);
    g_spill_len[channel] += len;
    g_spill_enqueue_count[channel]++;
    return true;
}

static bool stub_spill_read(void *user, int channel, void *dst, size_t len) {
    (void)user;
    if (channel < 0 || channel > 1 || !dst) return false;
    if (g_spill_len[channel] - g_spill_read_off[channel] < len) return false;
    memcpy(dst, g_spill_data[channel] + g_spill_read_off[channel], len);
    g_spill_read_off[channel] += len;
    return true;
}

static uint64_t stub_spill_backlog(void *user, int channel) {
    (void)user;
    if (channel < 0 || channel > 1) return 0;
    return (uint64_t)(g_spill_len[channel] - g_spill_read_off[channel]);
}

/* ---- counting log / write-error stubs ---- */

static int g_log_count[2];
static bool g_write_error_state[2];
static int g_write_error_transitions[2];

static void stub_log(void *user, int channel, const char *level, const char *message) {
    (void)user;
    (void)level;
    (void)message;
    if (channel < 0 || channel > 1) return;
    g_log_count[channel]++;
}

static void stub_set_write_error(void *user, int channel, bool active) {
    (void)user;
    if (channel < 0 || channel > 1) return;
    if (g_write_error_state[channel] != active) {
        g_write_error_transitions[channel]++;
    }
    g_write_error_state[channel] = active;
}

static void reset_stubs(void) {
    for (int i = 0; i < 2; i++) {
        free(g_spill_data[i]);
        g_spill_data[i] = NULL;
        g_spill_len[i] = 0;
        g_spill_read_off[i] = 0;
        g_spill_enqueue_count[i] = 0;
        g_log_count[i] = 0;
        g_write_error_state[i] = false;
        g_write_error_transitions[i] = 0;
    }
}

/* ---- deterministic pattern + file verification ---- */

static uint8_t pattern_byte(size_t index) {
    return (uint8_t)((index * 131u + 7u) & 0xFFu);
}

static void fill_pattern(uint8_t *dst, size_t start, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = pattern_byte(start + i);
    }
}

static bool file_matches_pattern(FILE *fp, size_t total) {
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    size_t verified = 0;
    size_t mismatches = 0;
    static uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != pattern_byte(verified + i)) {
                if (mismatches < 4) {
                    fprintf(stderr,
                            "MISMATCH at byte %zu: got %u want %u\n",
                            verified + i, (unsigned)buf[i],
                            (unsigned)pattern_byte(verified + i));
                }
                mismatches++;
            }
        }
        verified += n;
    }
    if (verified != total) {
        fprintf(stderr, "file size mismatch: got %zu want %zu\n", verified, total);
        return false;
    }
    return mismatches == 0;
}

/* ---- buffer manager with small record rings ---- */

static buffer_manager_t g_mgr;

static bool init_mgr(size_t record_size) {
    buffer_config_t cfg[BUF_COUNT];
    memset(&cfg, 0, sizeof(cfg));
    static const char *const names[BUF_COUNT] = {
        "capture_rf", "capture_audio", "record_a", "record_b", "display"
    };
    size_t sizes[BUF_COUNT] = {
        1024 * 1024, 256 * 1024, record_size, record_size, 256 * 1024
    };
    for (int i = 0; i < BUF_COUNT; i++) {
        cfg[i].name = names[i];
        cfg[i].size = sizes[i];
        cfg[i].lazy_init = true;
    }
    if (bufmgr_init_custom(&g_mgr, cfg) != 0) return false;
    if (bufmgr_ensure_init(&g_mgr, BUF_RECORD_A) != 0) return false;
    if (bufmgr_ensure_init(&g_mgr, BUF_RECORD_B) != 0) return false;
    return true;
}

static uint8_t g_push_buf[1024 * 1024];

/* Test 1: handshake, spill fallback under ring-full, ordered byte-exact
 * drain including the odd tail (3 x 2 MiB + 12345 bytes, odd chunk sizes). */
static void run_passthrough_test(void) {
    expect_true(init_mgr(64 * 1024), "buffer manager init (64 KiB record rings)");

    FILE *out = tmpfile();
    expect_true(out != NULL, "tmpfile for passthrough output");

    atomic_bool writer_draining = false;
    atomic_uint_fast64_t total_read;
    atomic_uint_fast64_t total_written;
    atomic_uint_fast64_t ch_written;
    atomic_init(&total_read, 0);
    atomic_init(&total_written, 0);
    atomic_init(&ch_written, 0);

    gui_record_direct_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.bufmgr = &g_mgr;
    ctx.buf_id = BUF_RECORD_A;
    ctx.channel = 0;
    ctx.file = out;
    ctx.writer_draining = &writer_draining;
    ctx.bytes_read_total = &total_read;
    ctx.bytes_written_total = &total_written;
    ctx.bytes_written_channel = &ch_written;
    ctx.cb.user = NULL;
    ctx.cb.spill_enqueue = stub_spill_enqueue;
    ctx.cb.spill_read = stub_spill_read;
    ctx.cb.spill_backlog = stub_spill_backlog;
    ctx.cb.set_write_error = stub_set_write_error;
    ctx.cb.log = stub_log;

    // Handshake: pushes while the tap is disabled are ignored entirely.
    gui_record_direct_tap_disable(&ctx);
    fill_pattern(g_push_buf, 0, 8);
    gui_record_direct_push_result_t r =
        gui_record_direct_push(&ctx, g_push_buf, 8, 0);
    expect_true(r == GUI_RECORD_DIRECT_PUSH_IDLE, "tap disabled -> push is IDLE");
    expect_true(atomic_load(&total_read) == 0, "tap disabled -> no bytes counted");

    gui_record_direct_tap_enable(&ctx);

    // Push more than the ring holds with no consumer yet: the ring fills,
    // then every further push must go to the spill (producer ordering rule).
    const size_t total = 3 * GUI_RECORD_DIRECT_MAX_BLOCK_BYTES + 12345;
    const size_t chunks[] = {65536, 65537, 100000, 1, 3, 999999, 123456, 7};
    size_t pushed = 0;
    uint32_t frame = 0;
    size_t ci = 0;
    while (pushed < total) {
        size_t n = chunks[ci % (sizeof(chunks) / sizeof(chunks[0]))];
        ci++;
        if (n > total - pushed) n = total - pushed;
        fill_pattern(g_push_buf, pushed, n);
        gui_record_direct_push_result_t pr =
            gui_record_direct_push(&ctx, g_push_buf, n, frame++);
        expect_true(pr == GUI_RECORD_DIRECT_PUSH_RING ||
                    pr == GUI_RECORD_DIRECT_PUSH_SPILL,
                    "push accepted (ring or spill)");
        pushed += n;
    }
    expect_true(g_spill_enqueue_count[0] > 0,
                "spill fallback used when the ring is full");

    // Now start the writer: it must drain ring + spill in order, including
    // the odd tail, then exit once recording stops.
    thrd_t writer;
    expect_true(thrd_create(&writer, gui_record_direct_writer_thread, &ctx) == thrd_success,
                "writer thread started");
    atomic_store(&writer_draining, true);
    thrd_join(writer, NULL);

    expect_true(atomic_load(&total_read) == total, "tap counted every pushed byte");
    expect_true(atomic_load(&total_written) == total, "writer accounted every output byte");
    expect_true(atomic_load(&ch_written) == total, "per-channel accounting matches");
    expect_true(file_matches_pattern(out, total),
                "output file is byte-identical to the pattern (ordering + odd tail drain)");
    expect_true(!g_write_error_state[0], "no write errors in the happy path");

    fclose(out);
    bufmgr_cleanup(&g_mgr);
    memset(&g_mgr, 0, sizeof(g_mgr));
}

/* Test 2: write-error recovery on /dev/full (unbuffered so every fwrite
 * fails for real): the failed block is held and retried; after the output
 * file is replaced the writer recovers and writes every byte in order. */
static void run_write_error_recovery_test(void) {
    FILE *full = fopen("/dev/full", "wb");
    if (!full) {
        printf("SKIP: /dev/full not available (write-error recovery test)\n");
        return;
    }
    // Unbuffered so even the smallest fwrite goes to the device and fails,
    // instead of "succeeding" into a stdio buffer that is never flushed.
    setvbuf(full, NULL, _IONBF, 0);

    expect_true(init_mgr(64 * 1024), "buffer manager init (write-error test)");

    FILE *out = tmpfile();
    expect_true(out != NULL, "tmpfile for recovery output");

    atomic_bool writer_draining = false;
    atomic_uint_fast64_t total_read;
    atomic_uint_fast64_t total_written;
    atomic_uint_fast64_t ch_written;
    atomic_init(&total_read, 0);
    atomic_init(&total_written, 0);
    atomic_init(&ch_written, 0);

    gui_record_direct_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.bufmgr = &g_mgr;
    ctx.buf_id = BUF_RECORD_A;
    ctx.channel = 0;
    ctx.file = full;
    ctx.writer_draining = &writer_draining;
    ctx.bytes_read_total = &total_read;
    ctx.bytes_written_total = &total_written;
    ctx.bytes_written_channel = &ch_written;
    ctx.cb.user = NULL;
    ctx.cb.spill_enqueue = stub_spill_enqueue;
    ctx.cb.spill_read = stub_spill_read;
    ctx.cb.spill_backlog = stub_spill_backlog;
    ctx.cb.set_write_error = stub_set_write_error;
    ctx.cb.log = stub_log;

    gui_record_direct_tap_enable(&ctx);
    thrd_t writer;
    expect_true(thrd_create(&writer, gui_record_direct_writer_thread, &ctx) == thrd_success,
                "writer thread started (write-error test)");

    const size_t total = 3 * 1024 * 1024 + 777;
    const size_t chunks[] = {65536, 65537, 100000, 1, 3, 999999, 123456, 7};
    size_t pushed = 0;
    uint32_t frame = 0;
    size_t ci = 0;
    while (pushed < total) {
        size_t n = chunks[ci % (sizeof(chunks) / sizeof(chunks[0]))];
        ci++;
        if (n > total - pushed) n = total - pushed;
        fill_pattern(g_push_buf, pushed, n);
        (void)gui_record_direct_push(&ctx, g_push_buf, n, frame++);
        pushed += n;
    }

    // Wait until the writer has reported the write-error state.
    for (int i = 0; i < 500 && !g_write_error_state[0]; i++) {
        thrd_sleep_ms(10);
    }
    expect_true(g_write_error_state[0], "writer entered the write-error state on /dev/full");

    // The output becomes writable again. The writer re-reads ctx.file each
    // loop, and its ~2 s retry throttle picks up the new file, retries the
    // held block first, then drains the rest in order. Wait for the
    // recovery to be observed before stopping the recording.
    ctx.file = out;
    for (int i = 0; i < 1000 && g_write_error_state[0]; i++) {
        thrd_sleep_ms(10);
    }
    expect_true(!g_write_error_state[0], "write error cleared after the output file became writable");

    atomic_store(&writer_draining, true);
    thrd_join(writer, NULL);
    expect_true(atomic_load(&total_written) == total, "recovered writer accounted all bytes");
    expect_true(file_matches_pattern(out, total),
                "recovered output is byte-identical (held block first, then ordered drain)");

    fclose(out);
    fclose(full);
    bufmgr_cleanup(&g_mgr);
    memset(&g_mgr, 0, sizeof(g_mgr));
}

int main(void) {
    reset_stubs();
    run_passthrough_test();
    reset_stubs();
    run_write_error_recovery_test();
    reset_stubs();

    printf("PASS: direct RAW record harness (%d checks, %d failures)\n",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
