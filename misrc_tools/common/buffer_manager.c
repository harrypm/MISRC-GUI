/*
 * MISRC Buffer Manager Implementation
 *
 * Copyright (C) 2024-2025 MISRC Authors
 * License: GPL-3.0-or-later
 */

#include "buffer_manager.h"
#include "threading.h"
#include "misrc_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*-----------------------------------------------------------------------------
 * Default Configurations
 *-----------------------------------------------------------------------------*/

static const buffer_config_t s_default_configs[BUF_COUNT] = {
    [BUF_CAPTURE_RF] = {
        .name = "capture_rf",
        .size = BUFMGR_SIZE_CAPTURE_RF,
        .lazy_init = false,
    },
    [BUF_CAPTURE_AUDIO] = {
        .name = "capture_audio",
        .size = BUFMGR_SIZE_CAPTURE_AUDIO,
        .lazy_init = false,
    },
    [BUF_RECORD_A] = {
        .name = "record_a",
        .size = BUFMGR_SIZE_RECORD,
        .lazy_init = true,
    },
    [BUF_RECORD_B] = {
        .name = "record_b",
        .size = BUFMGR_SIZE_RECORD,
        .lazy_init = true,
    },
    [BUF_DISPLAY] = {
        .name = "display",
        .size = BUFMGR_SIZE_DISPLAY,
        .lazy_init = false,
    },
};

static const backpressure_policy_t s_default_policies[BUF_COUNT] = {
    [BUF_CAPTURE_RF] = {
        .max_wait_attempts = 10,
        .wait_timeout_ms = 5,
        .log_first_wait = true,
        .log_drops = true,
    },
    [BUF_CAPTURE_AUDIO] = {
        /* Audio is best-effort: never block the capture callback waiting on audio space.
         * Blocking here increases the chance of missing RF frames and destabilizing sync.
         */
        .max_wait_attempts = 0,
        .wait_timeout_ms = 0,
        .log_first_wait = true,
        .log_drops = true,
    },
    [BUF_RECORD_A] = {
        .max_wait_attempts = 200,
        .wait_timeout_ms = 5,
        .log_first_wait = true,
        .log_drops = true,
    },
    [BUF_RECORD_B] = {
        .max_wait_attempts = 200,
        .wait_timeout_ms = 5,
        .log_first_wait = true,
        .log_drops = true,
    },
    [BUF_DISPLAY] = {
        .max_wait_attempts = 3,    /* Don't really care if this drops */
        .wait_timeout_ms = 1,
        .log_first_wait = true,
        .log_drops = true,
    },
};

/*-----------------------------------------------------------------------------
 * Lifecycle Management
 *-----------------------------------------------------------------------------*/

int bufmgr_init(buffer_manager_t *mgr) {
    return bufmgr_init_custom(mgr, NULL);
}

int bufmgr_init_custom(buffer_manager_t *mgr, const buffer_config_t *configs) {
    if (!mgr) return -1;

    memset(mgr, 0, sizeof(*mgr));

    /* Copy configurations */
    for (int i = 0; i < BUF_COUNT; i++) {
        if (configs && configs[i].name) {
            mgr->configs[i] = configs[i];
        } else {
            mgr->configs[i] = s_default_configs[i];
        }
        mgr->policies[i] = s_default_policies[i];
    }

    /* Initialize non-lazy buffers */
    for (int i = 0; i < BUF_COUNT; i++) {
        if (!mgr->configs[i].lazy_init) {
            int r = bufmgr_ensure_init(mgr, (buffer_id_t)i);
            if (r < 0) {
                fprintf(stderr, "[BUFMGR] Failed to init buffer '%s'\n",
                        mgr->configs[i].name);
                bufmgr_cleanup(mgr);
                return r;
            }
        }
    }

    mgr->manager_initialized = true;
    if (misrc_debug_enabled()) {
        fprintf(stderr, "[BUFMGR] Buffer manager initialized\n");
    }
    return 0;
}

void bufmgr_cleanup(buffer_manager_t *mgr) {
    if (!mgr) return;

    for (int i = 0; i < BUF_COUNT; i++) {
        if (mgr->initialized[i]) {
            rb_close(&mgr->buffers[i]);
            mgr->initialized[i] = false;
        }
        if (mgr->events_initialized[i]) {
            rb_event_destroy(&mgr->data_events[i]);
            rb_event_destroy(&mgr->space_events[i]);
            mgr->events_initialized[i] = false;
        }
    }

    mgr->manager_initialized = false;
    if (misrc_debug_enabled()) {
        fprintf(stderr, "[BUFMGR] Buffer manager cleaned up\n");
    }
}

int bufmgr_ensure_init(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT) return -1;
    if (mgr->initialized[id]) return 0;
    buffer_config_t *cfg = &mgr->configs[id];
    size_t requested_size = cfg->size;
    size_t init_size = cfg->size;
    bool used_fallback = false;

    /* Initialize ringbuffer */
    int r = rb_init(&mgr->buffers[id], (char *)cfg->name, init_size);
    if (r != 0 && (id == BUF_RECORD_A || id == BUF_RECORD_B)) {
        /* Root-cause context:
         * Large record mappings (8GiB default) can fail under virtual-memory
         * commit/pagefile pressure (observed as rb_init code 4 on Windows).
         * Retry with deterministic smaller sizes to preserve recording.
         */
        static const size_t s_record_fallback_sizes[] = {
            (size_t)4 * 1024 * 1024 * 1024ULL,
            (size_t)2 * 1024 * 1024 * 1024ULL,
            (size_t)1 * 1024 * 1024 * 1024ULL,
            (size_t)512 * 1024 * 1024ULL,
        };
        for (size_t i = 0; i < (sizeof(s_record_fallback_sizes) / sizeof(s_record_fallback_sizes[0])); i++) {
            size_t candidate = s_record_fallback_sizes[i];
            if (candidate >= requested_size) continue;
            r = rb_init(&mgr->buffers[id], (char *)cfg->name, candidate);
            if (r == 0) {
                init_size = candidate;
                used_fallback = true;
                break;
            }
        }
    }
    if (r != 0) {
        fprintf(stderr, "[BUFMGR] Failed to init ringbuffer '%s': %d (requested=%zu bytes)\\n",
                cfg->name, r, requested_size);
        return -1;
    }
    cfg->size = init_size;
    if (used_fallback) {
        fprintf(stderr,
                "[BUFMGR] Warning: ringbuffer '%s' fallback size in use (%zu -> %zu bytes)\\n",
                cfg->name, requested_size, init_size);
        /* Harden against repeated failures when the peer record buffer is
         * lazily initialized after this one under the same memory pressure. */
        buffer_id_t peer_id = (id == BUF_RECORD_A) ? BUF_RECORD_B :
                              ((id == BUF_RECORD_B) ? BUF_RECORD_A : BUF_COUNT);
        if (peer_id < BUF_COUNT &&
            !mgr->initialized[peer_id] &&
            mgr->configs[peer_id].size > init_size) {
            size_t peer_requested_size = mgr->configs[peer_id].size;
            mgr->configs[peer_id].size = init_size;
            fprintf(stderr,
                    "[BUFMGR] Warning: preemptively lowering '%s' size (%zu -> %zu bytes)\\n",
                    mgr->configs[peer_id].name, peer_requested_size, init_size);
        }
    }

    /* Initialize events */
    if (rb_event_init(&mgr->data_events[id]) == 0 &&
        rb_event_init(&mgr->space_events[id]) == 0) {
        mgr->events_initialized[id] = true;
    } else {
        fprintf(stderr, "[BUFMGR] Warning: Failed to init events for '%s'\n",
                cfg->name);
    }

    /* Reset statistics */
    memset(&mgr->stats[id], 0, sizeof(buffer_stats_t));

    mgr->initialized[id] = true;
    if (misrc_debug_enabled()) {
        fprintf(stderr, "[BUFMGR] Initialized buffer '%s' (%zu bytes)\n",
                cfg->name, cfg->size);
    }
    return 0;
}

void bufmgr_reset(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT || !mgr->initialized[id]) return;

    /* Reset head/tail pointers atomically */
    atomic_store(&mgr->buffers[id].head, 0);
    atomic_store(&mgr->buffers[id].tail, 0);

    /* Also reset stats for this buffer */
    memset(&mgr->stats[id], 0, sizeof(buffer_stats_t));

    if (misrc_debug_enabled()) {
        fprintf(stderr, "[BUFMGR] Reset buffer '%s'\n", mgr->configs[id].name);
    }
}

void bufmgr_reset_stats(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr) return;

    if (id >= BUF_COUNT) {
        /* Reset all */
        for (int i = 0; i < BUF_COUNT; i++) {
            memset(&mgr->stats[i], 0, sizeof(buffer_stats_t));
        }
    } else {
        memset(&mgr->stats[id], 0, sizeof(buffer_stats_t));
    }
}

/*-----------------------------------------------------------------------------
 * Producer API
 *-----------------------------------------------------------------------------*/

void *bufmgr_write_begin(buffer_manager_t *mgr, buffer_id_t id,
                          size_t bytes, const backpressure_policy_t *policy) {
    if (!mgr || id >= BUF_COUNT) return NULL;

    /* Ensure buffer is initialized */
    if (!mgr->initialized[id]) {
        if (bufmgr_ensure_init(mgr, id) < 0) return NULL;
    }

    const backpressure_policy_t *pol = policy ? policy : &mgr->policies[id];
    ringbuffer_t *rb = &mgr->buffers[id];

    void *ptr = rb_write_ptr(rb, bytes);
    if (ptr) return ptr;

    /* Buffer full - apply backpressure policy */
    if (pol->max_wait_attempts == 0) {
        /* Never wait policy - drop immediately */
        atomic_fetch_add(&mgr->stats[id].write_drops, 1);
        if (pol->log_drops && misrc_debug_enabled()) {
            fprintf(stderr, "[BUFMGR] Buffer '%s' dropping write (%zu bytes)\n",
                    mgr->configs[id].name, bytes);
        }
        return NULL;
    }

    int attempts = 0;
    bool logged_wait = false;

    while (ptr == NULL) {
        attempts++;

        if (pol->log_first_wait && !logged_wait) {
            uint32_t wait_count = atomic_fetch_add(&mgr->stats[id].write_waits, 1) + 1;
            if (misrc_debug_enabled()) {
                fprintf(stderr, "[BUFMGR] Buffer '%s' backpressure - waiting (wait #%u)\n",
                        mgr->configs[id].name, wait_count);
            }
            logged_wait = true;
        } else {
            atomic_fetch_add(&mgr->stats[id].write_waits, 1);
        }

        if (attempts > pol->max_wait_attempts) {
            /* Max attempts exceeded - drop */
            atomic_fetch_add(&mgr->stats[id].write_drops, 1);
            if (pol->log_drops && misrc_debug_enabled()) {
                fprintf(stderr, "[BUFMGR] Buffer '%s' dropping write after %d attempts\n",
                        mgr->configs[id].name, attempts);
            }
            return NULL;
        }

        /* Wait for space */
        if (mgr->events_initialized[id]) {
            rb_event_wait_timeout(&mgr->space_events[id], pol->wait_timeout_ms);
        } else {
            thrd_sleep_ms(1);
        }

        ptr = rb_write_ptr(rb, bytes);
    }

    return ptr;
}

void bufmgr_write_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes) {
    if (!mgr || id >= BUF_COUNT || !mgr->initialized[id]) return;

    rb_write_finished(&mgr->buffers[id], bytes);
    atomic_fetch_add(&mgr->stats[id].bytes_written, bytes);

    /* Signal that data is available */
    if (mgr->events_initialized[id]) {
        rb_event_signal(&mgr->data_events[id]);
    }
}

int bufmgr_write(buffer_manager_t *mgr, buffer_id_t id,
                  const void *data, size_t bytes) {
    void *ptr = bufmgr_write_begin(mgr, id, bytes, NULL);
    if (!ptr) return -1;

    memcpy(ptr, data, bytes);
    bufmgr_write_end(mgr, id, bytes);
    return 0;
}

/*-----------------------------------------------------------------------------
 * Consumer API
 *-----------------------------------------------------------------------------*/

void *bufmgr_read_begin(buffer_manager_t *mgr, buffer_id_t id,
                         size_t bytes, int timeout_ms) {
    if (!mgr || id >= BUF_COUNT || !mgr->initialized[id]) return NULL;

    ringbuffer_t *rb = &mgr->buffers[id];
    void *ptr = rb_read_ptr(rb, bytes);

    if (ptr) return ptr;
    if (timeout_ms == 0) return NULL;

    /* Wait for data */
    int elapsed = 0;
    int wait_chunk = 20;  /* Wait in 20ms chunks */

    if (timeout_ms > 0 && timeout_ms < wait_chunk) {
        wait_chunk = timeout_ms;
    }

    while (ptr == NULL) {
        atomic_fetch_add(&mgr->stats[id].read_waits, 1);

        if (mgr->events_initialized[id]) {
            rb_event_wait_timeout(&mgr->data_events[id], wait_chunk);
        } else {
            thrd_sleep_ms(1);
        }

        ptr = rb_read_ptr(rb, bytes);

        if (timeout_ms > 0) {
            elapsed += wait_chunk;
            if (elapsed >= timeout_ms) {
                atomic_fetch_add(&mgr->stats[id].read_timeouts, 1);
                return NULL;
            }
        }
    }

    return ptr;
}

void bufmgr_read_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes) {
    if (!mgr || id >= BUF_COUNT || !mgr->initialized[id]) return;

    rb_read_finished(&mgr->buffers[id], bytes);
    atomic_fetch_add(&mgr->stats[id].bytes_read, bytes);

    /* Signal that space is available */
    if (mgr->events_initialized[id]) {
        rb_event_signal(&mgr->space_events[id]);
    }
}

/*-----------------------------------------------------------------------------
 * Event API
 *-----------------------------------------------------------------------------*/

bool bufmgr_wait_data(buffer_manager_t *mgr, buffer_id_t id, int timeout_ms) {
    if (!mgr || id >= BUF_COUNT || !mgr->events_initialized[id]) return false;

    if (timeout_ms < 0) {
        rb_event_wait(&mgr->data_events[id]);
        return true;
    } else if (timeout_ms == 0) {
        return false;  /* No wait */
    } else {
        return rb_event_wait_timeout(&mgr->data_events[id], timeout_ms);
    }
}

bool bufmgr_wait_space(buffer_manager_t *mgr, buffer_id_t id, int timeout_ms) {
    if (!mgr || id >= BUF_COUNT || !mgr->events_initialized[id]) return false;

    if (timeout_ms < 0) {
        rb_event_wait(&mgr->space_events[id]);
        return true;
    } else if (timeout_ms == 0) {
        return false;  /* No wait */
    } else {
        return rb_event_wait_timeout(&mgr->space_events[id], timeout_ms);
    }
}

void bufmgr_signal_data(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT || !mgr->events_initialized[id]) return;
    rb_event_signal(&mgr->data_events[id]);
}

void bufmgr_signal_space(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT || !mgr->events_initialized[id]) return;
    rb_event_signal(&mgr->space_events[id]);
}

rb_event_t *bufmgr_get_data_event(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT || !mgr->events_initialized[id]) return NULL;
    return &mgr->data_events[id];
}

rb_event_t *bufmgr_get_space_event(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT || !mgr->events_initialized[id]) return NULL;
    return &mgr->space_events[id];
}

/*-----------------------------------------------------------------------------
 * Statistics API
 *-----------------------------------------------------------------------------*/

size_t bufmgr_fill_level(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT || !mgr->initialized[id]) return 0;

    ringbuffer_t *rb = &mgr->buffers[id];
    size_t head = atomic_load(&rb->head);
    size_t tail = atomic_load(&rb->tail);
    return tail - head;
}

float bufmgr_fill_percent(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT || !mgr->initialized[id]) return 0.0f;

    size_t fill = bufmgr_fill_level(mgr, id);
    size_t size = mgr->configs[id].size;
    if (size == 0) return 0.0f;
    return (float)fill / (float)size;
}

void bufmgr_get_stats(buffer_manager_t *mgr, buffer_id_t id, buffer_stats_t *out) {
    if (!mgr || id >= BUF_COUNT || !out) return;

    buffer_stats_t *src = &mgr->stats[id];
    out->bytes_written = atomic_load(&src->bytes_written);
    out->bytes_read = atomic_load(&src->bytes_read);
    out->write_waits = atomic_load(&src->write_waits);
    out->write_drops = atomic_load(&src->write_drops);
    out->read_waits = atomic_load(&src->read_waits);
    out->read_timeouts = atomic_load(&src->read_timeouts);
}

void bufmgr_get_all_stats(buffer_manager_t *mgr, buffer_manager_stats_t *out) {
    if (!mgr || !out) return;

    memset(out, 0, sizeof(*out));

    for (int i = 0; i < BUF_COUNT; i++) {
        bufmgr_get_stats(mgr, (buffer_id_t)i, &out->per_buffer[i]);

        out->total_bytes_written += atomic_load(&out->per_buffer[i].bytes_written);
        out->total_bytes_read += atomic_load(&out->per_buffer[i].bytes_read);
        out->total_waits += atomic_load(&out->per_buffer[i].write_waits);
        out->total_waits += atomic_load(&out->per_buffer[i].read_waits);
        out->total_drops += atomic_load(&out->per_buffer[i].write_drops);
    }
}

/*-----------------------------------------------------------------------------
 * Debug/Logging
 *-----------------------------------------------------------------------------*/

void bufmgr_dump_status(buffer_manager_t *mgr) {
    if (!mgr) return;

    fprintf(stderr, "\n[BUFMGR] Buffer Status:\n");
    fprintf(stderr, "%-16s  %-10s  %-10s  %-8s  %-8s  %-8s\n",
            "Buffer", "Size", "Fill", "Waits", "Drops", "State");
    fprintf(stderr, "%-16s  %-10s  %-10s  %-8s  %-8s  %-8s\n",
            "------", "----", "----", "-----", "-----", "-----");

    for (int i = 0; i < BUF_COUNT; i++) {
        const char *name = mgr->configs[i].name;
        size_t size = mgr->configs[i].size;
        uint32_t waits = atomic_load(&mgr->stats[i].write_waits);
        uint32_t drops = atomic_load(&mgr->stats[i].write_drops);
        const char *state = mgr->initialized[i] ? "OK" : "UNINIT";

        char size_str[16], fill_str[16];
        if (size >= 1024 * 1024) {
            snprintf(size_str, sizeof(size_str), "%.1fMB", (double)size / (1024 * 1024));
        } else {
            snprintf(size_str, sizeof(size_str), "%.1fKB", (double)size / 1024);
        }

        if (mgr->initialized[i]) {
            snprintf(fill_str, sizeof(fill_str), "%.1f%%", bufmgr_fill_percent(mgr, (buffer_id_t)i) * 100);
        } else {
            snprintf(fill_str, sizeof(fill_str), "-");
        }

        fprintf(stderr, "%-16s  %-10s  %-10s  %-8u  %-8u  %-8s\n",
                name, size_str, fill_str, waits, drops, state);
    }
    fprintf(stderr, "\n");
}

void bufmgr_log_periodic(buffer_manager_t *mgr) {
    if (!mgr) return;

    /* Build compact one-line status showing per-buffer fill/wait/drop */
    /* Format: [BUFMGR] RF:45%/0/0 DISP:12%/0/0 (fill%/waits/drops) */

    char line[512];
    int pos = 0;
    pos += snprintf(line + pos, sizeof(line) - pos, "[BUFMGR] ");

    int active_count = 0;

    for (int i = 0; i < BUF_COUNT; i++) {
        if (!mgr->initialized[i]) continue;

        float fill_pct = bufmgr_fill_percent(mgr, (buffer_id_t)i) * 100.0f;
        uint32_t waits = atomic_load(&mgr->stats[i].write_waits);
        uint32_t drops = atomic_load(&mgr->stats[i].write_drops);

        /* Short name for buffer */
        const char *short_name;
        switch ((buffer_id_t)i) {
            case BUF_CAPTURE_RF:    short_name = "RF"; break;
            case BUF_CAPTURE_AUDIO: short_name = "AUD"; break;
            case BUF_RECORD_A:      short_name = "REC_A"; break;
            case BUF_RECORD_B:      short_name = "REC_B"; break;
            case BUF_DISPLAY:       short_name = "DISP"; break;
            default:                short_name = "?"; break;
        }

        if (active_count > 0) {
            pos += snprintf(line + pos, sizeof(line) - pos, " ");
        }
        pos += snprintf(line + pos, sizeof(line) - pos, "%s:%.0f%%/%u/%u",
                        short_name, fill_pct, waits, drops);
        active_count++;
    }

    if (active_count > 0) {
        fprintf(stderr, "%s\n", line);
    }
}

const char *bufmgr_get_name(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT) return "unknown";
    return mgr->configs[id].name;
}

bool bufmgr_is_initialized(buffer_manager_t *mgr, buffer_id_t id) {
    if (!mgr || id >= BUF_COUNT) return false;
    return mgr->initialized[id];
}
