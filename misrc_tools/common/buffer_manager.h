/*
 * MISRC Buffer Manager
 *
 * Centralized management of all ringbuffers with:
 * - Unified lifecycle (init/cleanup)
 * - Consistent backpressure handling
 * - Integrated event signaling
 * - Centralized statistics
 *
 * Copyright (C) 2024-2025 MISRC Authors
 * License: GPL-3.0-or-later
 */

#ifndef MISRC_BUFFER_MANAGER_H
#define MISRC_BUFFER_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "ringbuffer.h"
#include "rb_event.h"

/*
 * Buffer identifiers - extensible enum for all buffer types
 */
typedef enum {
    BUF_CAPTURE_RF = 0,    /* Hardware -> Extraction (raw RF data) */
    BUF_CAPTURE_AUDIO,     /* Hardware -> Audio writer (raw audio) */
    BUF_RECORD_A,          /* Extraction -> File writer A (extracted samples) */
    BUF_RECORD_B,          /* Extraction -> File writer B (extracted samples) */
    BUF_DISPLAY,           /* Extraction -> Display thread (for CVBS/oscilloscope) */
    BUF_COUNT              /* Must be last */
} buffer_id_t;

/*
 * Buffer sizes (must be page-aligned)
 */
#define BUFMGR_SIZE_CAPTURE_RF    ((size_t)1024 * 1024 * 1024)           /* 1GB */
#define BUFMGR_SIZE_CAPTURE_AUDIO ((size_t)256 * 1024 * 1024)            /* 256MB */
#define BUFMGR_SIZE_RECORD        ((size_t)8 * 1024 * 1024 * 1024ULL)    /* 8GB */
#define BUFMGR_SIZE_DISPLAY       ((size_t)256 * 1024 * 1024)            /* 256MB */

/*
 * Per-buffer configuration
 */
typedef struct {
    const char *name;      /* Human-readable name for logging */
    size_t size;           /* Buffer size in bytes (must be page-aligned) */
    bool lazy_init;        /* If true, init on first use instead of at startup */
} buffer_config_t;

/*
 * Per-buffer statistics (all atomic for thread-safe access)
 */
typedef struct {
    atomic_uint_fast64_t bytes_written;    /* Total bytes written */
    atomic_uint_fast64_t bytes_read;       /* Total bytes read */
    atomic_uint_fast32_t write_waits;      /* Times producer waited for space */
    atomic_uint_fast32_t write_drops;      /* Writes dropped due to full buffer */
    atomic_uint_fast32_t read_waits;       /* Times consumer waited for data */
    atomic_uint_fast32_t read_timeouts;    /* Reads that timed out */
} buffer_stats_t;

/*
 * Backpressure policy configuration
 */
typedef struct {
    int max_wait_attempts;     /* Max retries before dropping (0 = never wait, drop immediately) */
    int wait_timeout_ms;       /* Timeout per wait attempt */
    bool log_first_wait;       /* Log on first wait occurrence */
    bool log_drops;            /* Log when data is dropped */
} backpressure_policy_t;

/*
 * Buffer manager instance
 */
typedef struct buffer_manager {
    /* Buffers */
    ringbuffer_t buffers[BUF_COUNT];
    bool initialized[BUF_COUNT];

    /* Events (each buffer has data + space events) */
    rb_event_t data_events[BUF_COUNT];      /* Signaled when data available */
    rb_event_t space_events[BUF_COUNT];     /* Signaled when space available */
    bool events_initialized[BUF_COUNT];

    /* Configuration */
    buffer_config_t configs[BUF_COUNT];
    backpressure_policy_t policies[BUF_COUNT];

    /* Statistics */
    buffer_stats_t stats[BUF_COUNT];

    /* Lifecycle */
    bool manager_initialized;
} buffer_manager_t;

/*-----------------------------------------------------------------------------
 * Lifecycle Management
 *-----------------------------------------------------------------------------*/

/*
 * Initialize the buffer manager with default configuration
 *
 * @param mgr       Buffer manager instance
 * @return 0 on success, negative on error
 */
int bufmgr_init(buffer_manager_t *mgr);

/*
 * Initialize with custom configuration
 *
 * @param mgr       Buffer manager instance
 * @param configs   Array of BUF_COUNT configurations (NULL entries use defaults)
 * @return 0 on success, negative on error
 */
int bufmgr_init_custom(buffer_manager_t *mgr, const buffer_config_t *configs);

/*
 * Cleanup all buffers and events
 *
 * @param mgr       Buffer manager instance
 */
void bufmgr_cleanup(buffer_manager_t *mgr);

/*
 * Ensure a specific buffer is initialized (for lazy-init buffers)
 *
 * @param mgr       Buffer manager instance
 * @param id        Buffer to initialize
 * @return 0 on success, negative on error
 */
int bufmgr_ensure_init(buffer_manager_t *mgr, buffer_id_t id);

/*
 * Reset a buffer (clear contents, reset head/tail)
 *
 * @param mgr       Buffer manager instance
 * @param id        Buffer to reset
 */
void bufmgr_reset(buffer_manager_t *mgr, buffer_id_t id);

/*
 * Reset statistics for a buffer (or all if id == BUF_COUNT)
 *
 * @param mgr       Buffer manager instance
 * @param id        Buffer ID or BUF_COUNT for all
 */
void bufmgr_reset_stats(buffer_manager_t *mgr, buffer_id_t id);

/*-----------------------------------------------------------------------------
 * Producer API (writing to buffers)
 *-----------------------------------------------------------------------------*/

/*
 * Begin a write operation with backpressure handling
 *
 * @param mgr           Buffer manager instance
 * @param id            Buffer to write to
 * @param bytes         Number of bytes to write
 * @param policy        Backpressure policy (NULL for buffer's default)
 * @return Pointer to write location, or NULL if dropped/unavailable
 *
 * This function:
 * 1. Attempts to get write pointer
 * 2. If buffer full, waits according to policy
 * 3. Updates statistics (waits, drops)
 * 4. Returns pointer or NULL if policy allows dropping
 */
void *bufmgr_write_begin(buffer_manager_t *mgr, buffer_id_t id,
                          size_t bytes, const backpressure_policy_t *policy);

/*
 * Complete a write operation
 *
 * @param mgr       Buffer manager instance
 * @param id        Buffer that was written to
 * @param bytes     Number of bytes written (must match write_begin)
 *
 * This function:
 * 1. Advances the write pointer
 * 2. Signals the data_event for this buffer
 * 3. Updates bytes_written statistic
 */
void bufmgr_write_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes);

/*
 * Convenience: write data with default policy
 *
 * @param mgr       Buffer manager instance
 * @param id        Buffer to write to
 * @param data      Data to write
 * @param bytes     Number of bytes to write
 * @return 0 on success, -1 if dropped
 */
int bufmgr_write(buffer_manager_t *mgr, buffer_id_t id,
                  const void *data, size_t bytes);

/*-----------------------------------------------------------------------------
 * Consumer API (reading from buffers)
 *-----------------------------------------------------------------------------*/

/*
 * Begin a read operation with optional waiting
 *
 * @param mgr           Buffer manager instance
 * @param id            Buffer to read from
 * @param bytes         Number of bytes to read
 * @param timeout_ms    Max time to wait (-1 = infinite, 0 = no wait)
 * @return Pointer to read location, or NULL if timeout/no data
 */
void *bufmgr_read_begin(buffer_manager_t *mgr, buffer_id_t id,
                         size_t bytes, int timeout_ms);

/*
 * Complete a read operation
 *
 * @param mgr       Buffer manager instance
 * @param id        Buffer that was read from
 * @param bytes     Number of bytes consumed (must match read_begin)
 *
 * This function:
 * 1. Advances the read pointer
 * 2. Signals the space_event for this buffer
 * 3. Updates bytes_read statistic
 */
void bufmgr_read_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes);

/*-----------------------------------------------------------------------------
 * Event API (for advanced use cases)
 *-----------------------------------------------------------------------------*/

/*
 * Wait for data to become available
 *
 * @param mgr           Buffer manager instance
 * @param id            Buffer to wait on
 * @param timeout_ms    Max time to wait (-1 = infinite, 0 = no wait)
 * @return true if data available, false if timeout
 */
bool bufmgr_wait_data(buffer_manager_t *mgr, buffer_id_t id, int timeout_ms);

/*
 * Wait for space to become available
 *
 * @param mgr           Buffer manager instance
 * @param id            Buffer to wait on
 * @param timeout_ms    Max time to wait (-1 = infinite, 0 = no wait)
 * @return true if space available, false if timeout
 */
bool bufmgr_wait_space(buffer_manager_t *mgr, buffer_id_t id, int timeout_ms);

/*
 * Signal that data is available (normally done by write_end)
 */
void bufmgr_signal_data(buffer_manager_t *mgr, buffer_id_t id);

/*
 * Signal that space is available (normally done by read_end)
 */
void bufmgr_signal_space(buffer_manager_t *mgr, buffer_id_t id);

/*
 * Get raw event handles (for external waiting)
 */
rb_event_t *bufmgr_get_data_event(buffer_manager_t *mgr, buffer_id_t id);
rb_event_t *bufmgr_get_space_event(buffer_manager_t *mgr, buffer_id_t id);

/*-----------------------------------------------------------------------------
 * Statistics API
 *-----------------------------------------------------------------------------*/

/*
 * Get current fill level of a buffer
 *
 * @param mgr       Buffer manager instance
 * @param id        Buffer to check
 * @return Current bytes in buffer
 */
size_t bufmgr_fill_level(buffer_manager_t *mgr, buffer_id_t id);

/*
 * Get fill percentage (0.0 - 1.0)
 */
float bufmgr_fill_percent(buffer_manager_t *mgr, buffer_id_t id);

/*
 * Get a copy of statistics for a buffer
 *
 * @param mgr       Buffer manager instance
 * @param id        Buffer to get stats for
 * @param out       Output structure (values are copied atomically)
 */
void bufmgr_get_stats(buffer_manager_t *mgr, buffer_id_t id, buffer_stats_t *out);

/*
 * Aggregate statistics for all buffers
 */
typedef struct {
    buffer_stats_t per_buffer[BUF_COUNT];
    uint64_t total_bytes_written;
    uint64_t total_bytes_read;
    uint32_t total_waits;
    uint32_t total_drops;
} buffer_manager_stats_t;

void bufmgr_get_all_stats(buffer_manager_t *mgr, buffer_manager_stats_t *out);

/*-----------------------------------------------------------------------------
 * Debug/Logging
 *-----------------------------------------------------------------------------*/

/*
 * Print buffer status to stderr (detailed multi-line)
 */
void bufmgr_dump_status(buffer_manager_t *mgr);

/*
 * Print compact one-line status (for periodic logging)
 * Example: [BUFMGR] RF:45% DISP:12% (waits:5 drops:0)
 */
void bufmgr_log_periodic(buffer_manager_t *mgr);

/*
 * Get buffer name for logging
 */
const char *bufmgr_get_name(buffer_manager_t *mgr, buffer_id_t id);

/*
 * Check if buffer is initialized
 */
bool bufmgr_is_initialized(buffer_manager_t *mgr, buffer_id_t id);

#endif /* MISRC_BUFFER_MANAGER_H */
