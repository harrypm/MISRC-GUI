#include "buffer_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIB ((size_t)1024 * 1024 * 1024ULL)
#define MIB ((size_t)1024 * 1024ULL)

typedef struct {
    size_t sizes[8];
    int count;
} init_trace_t;

static init_trace_t g_record_a_trace = {{0}, 0};
static init_trace_t g_record_b_trace = {{0}, 0};

static init_trace_t *trace_for_name(const char *name) {
    if (strcmp(name, "record_a") == 0) return &g_record_a_trace;
    if (strcmp(name, "record_b") == 0) return &g_record_b_trace;
    return NULL;
}

static int expect_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "ASSERTION FAILED: %s\n", message);
        return 1;
    }
    return 0;
}

static int expect_trace(const char *name, const init_trace_t *trace,
                        const size_t *expected, int expected_count) {
    if (expect_true(trace->count == expected_count, "unexpected attempt count") != 0) {
        fprintf(stderr, "  %s attempts: expected=%d got=%d\n", name, expected_count, trace->count);
        return 1;
    }
    for (int i = 0; i < expected_count; i++) {
        if (expect_true(trace->sizes[i] == expected[i], "unexpected attempted allocation size") != 0) {
            fprintf(stderr, "  %s attempt[%d]: expected=%zu got=%zu\n",
                    name, i, expected[i], trace->sizes[i]);
            return 1;
        }
    }
    return 0;
}

static void set_cfg(buffer_config_t *cfg, const char *name, size_t size) {
    cfg->name = name;
    cfg->size = size;
    cfg->lazy_init = true;
}

/* ----- ringbuffer stubs ----- */

int rb_init(ringbuffer_t *rb, char *name, size_t size) {
    init_trace_t *trace = trace_for_name(name);
    if (trace && trace->count < (int)(sizeof(trace->sizes) / sizeof(trace->sizes[0]))) {
        trace->sizes[trace->count++] = size;
    }

    /* Simulate the observed production failure mode:
     * large record mapping attempts fail with return code 4. */
    if (trace && size > ((size_t)2 * GIB)) {
        return 4;
    }

    rb->buffer = NULL;
#ifdef _WIN32
    rb->_buffer2 = NULL;
#endif
    rb->buffer_size = size;
    rb->fd = -1;
    atomic_store(&rb->head, 0);
    atomic_store(&rb->tail, 0);
    return 0;
}

int rb_put(ringbuffer_t *rb, void *data, size_t size) {
    (void)rb;
    (void)data;
    (void)size;
    return 0;
}

void *rb_read_ptr(ringbuffer_t *rb, size_t size) {
    (void)rb;
    (void)size;
    return NULL;
}

int rb_read_finished(ringbuffer_t *rb, size_t size) {
    (void)rb;
    (void)size;
    return 0;
}

void *rb_write_ptr(ringbuffer_t *rb, size_t size) {
    (void)rb;
    (void)size;
    return NULL;
}

int rb_write_finished(ringbuffer_t *rb, size_t size) {
    (void)rb;
    (void)size;
    return 0;
}

size_t rb_fill_level(const ringbuffer_t *rb) {
    (void)rb;
    return 0;
}

void rb_close(ringbuffer_t *rb) {
    (void)rb;
}

/* ----- event stubs ----- */

int rb_event_init(rb_event_t *event) {
    event->initialized = true;
    return 0;
}

void rb_event_signal(rb_event_t *event) {
    (void)event;
}

void rb_event_wait(rb_event_t *event) {
    (void)event;
}

bool rb_event_wait_timeout(rb_event_t *event, uint32_t timeout_ms) {
    (void)event;
    (void)timeout_ms;
    return false;
}

void rb_event_destroy(rb_event_t *event) {
    event->initialized = false;
}

int main(void) {
    buffer_manager_t mgr;
    buffer_config_t cfg[BUF_COUNT];
    memset(&mgr, 0, sizeof(mgr));
    memset(cfg, 0, sizeof(cfg));

    set_cfg(&cfg[BUF_CAPTURE_RF], "capture_rf", 32 * MIB);
    set_cfg(&cfg[BUF_CAPTURE_AUDIO], "capture_audio", 8 * MIB);
    set_cfg(&cfg[BUF_RECORD_A], "record_a", BUFMGR_SIZE_RECORD);
    set_cfg(&cfg[BUF_RECORD_B], "record_b", BUFMGR_SIZE_RECORD);
    set_cfg(&cfg[BUF_DISPLAY], "display", 8 * MIB);

    if (bufmgr_init_custom(&mgr, cfg) != 0) {
        return expect_true(0, "bufmgr_init_custom failed");
    }

    if (bufmgr_ensure_init(&mgr, BUF_RECORD_A) != 0) {
        return expect_true(0, "bufmgr_ensure_init(record_a) failed");
    }

    {
        const size_t expected_a[] = {
            BUFMGR_SIZE_RECORD,
            (size_t)4 * GIB,
            (size_t)2 * GIB,
        };
        if (expect_trace("record_a", &g_record_a_trace, expected_a, 3) != 0) {
            return 1;
        }
    }

    if (expect_true(mgr.configs[BUF_RECORD_A].size == ((size_t)2 * GIB),
                    "record_a fallback size mismatch") != 0) {
        return 1;
    }
    if (expect_true(mgr.configs[BUF_RECORD_B].size == ((size_t)2 * GIB),
                    "record_b peer fallback pre-adjust mismatch") != 0) {
        return 1;
    }

    if (bufmgr_ensure_init(&mgr, BUF_RECORD_B) != 0) {
        return expect_true(0, "bufmgr_ensure_init(record_b) failed");
    }

    {
        const size_t expected_b[] = {
            (size_t)2 * GIB,
        };
        if (expect_trace("record_b", &g_record_b_trace, expected_b, 1) != 0) {
            return 1;
        }
    }

    bufmgr_cleanup(&mgr);
    puts("PASS: record ringbuffer fallback runtime harness");
    return 0;
}
