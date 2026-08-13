/*
 * MISRC FLAC Writer - Implementation
 *
 * Shared FLAC encoding library for CLI and GUI tools.
 */
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "flac_writer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>

#if defined(__linux__)
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#endif

static void flac_writer_set_affinity_error(
    char *error_message,
    size_t error_message_size,
    const char *message
) {
    if (!error_message || error_message_size == 0) return;
    snprintf(error_message, error_message_size, "%s", message ? message : "");
    error_message[error_message_size - 1] = '\0';
}

#if defined(__linux__)
static int flac_writer_cpu_count_set(const cpu_set_t *set) {
    if (!set) return 0;
    int count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, set)) count++;
    }
    return count;
}

static bool flac_writer_parse_affinity_cpu_list_linux(
    const char *cpu_list,
    cpu_set_t *target_set,
    char *error_message,
    size_t error_message_size
) {
    if (!cpu_list || !cpu_list[0]) {
        flac_writer_set_affinity_error(error_message, error_message_size, "FLAC affinity CPU list is empty");
        return false;
    }
    if (!target_set) {
        flac_writer_set_affinity_error(error_message, error_message_size, "Internal error: CPU target set is NULL");
        return false;
    }

    long cpu_conf = sysconf(_SC_NPROCESSORS_CONF);
    long max_cpu_index = (cpu_conf > 0) ? (cpu_conf - 1) : (CPU_SETSIZE - 1);

    CPU_ZERO(target_set);
    const char *p = cpu_list;
    bool parsed_any = false;

    while (1) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;

        if (!isdigit((unsigned char)*p)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Invalid FLAC affinity CPU list near '%c'", *p);
            flac_writer_set_affinity_error(error_message, error_message_size, msg);
            return false;
        }

        errno = 0;
        char *endptr = NULL;
        long start_cpu = strtol(p, &endptr, 10);
        if (errno != 0 || endptr == p || start_cpu < 0 || start_cpu > INT_MAX) {
            flac_writer_set_affinity_error(error_message, error_message_size, "Invalid FLAC affinity CPU index");
            return false;
        }
        p = endptr;

        while (*p && isspace((unsigned char)*p)) p++;

        long end_cpu = start_cpu;
        if (*p == '-') {
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            if (!isdigit((unsigned char)*p)) {
                flac_writer_set_affinity_error(error_message, error_message_size, "Invalid FLAC affinity range end");
                return false;
            }
            errno = 0;
            end_cpu = strtol(p, &endptr, 10);
            if (errno != 0 || endptr == p || end_cpu < 0 || end_cpu > INT_MAX) {
                flac_writer_set_affinity_error(error_message, error_message_size, "Invalid FLAC affinity range end");
                return false;
            }
            p = endptr;
            if (end_cpu < start_cpu) {
                flac_writer_set_affinity_error(error_message, error_message_size, "FLAC affinity range end is smaller than start");
                return false;
            }
        }

        if (start_cpu > max_cpu_index || end_cpu > max_cpu_index) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "FLAC affinity CPU index out of range (max available index: %ld)",
                     max_cpu_index);
            flac_writer_set_affinity_error(error_message, error_message_size, msg);
            return false;
        }
        if (end_cpu >= CPU_SETSIZE) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "FLAC affinity CPU index exceeds CPU_SETSIZE (%d)",
                     CPU_SETSIZE - 1);
            flac_writer_set_affinity_error(error_message, error_message_size, msg);
            return false;
        }

        for (long cpu = start_cpu; cpu <= end_cpu; cpu++) {
            CPU_SET((int)cpu, target_set);
        }
        parsed_any = true;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;
        if (*p != ',') {
            char msg[128];
            snprintf(msg, sizeof(msg), "Invalid FLAC affinity separator near '%c'", *p);
            flac_writer_set_affinity_error(error_message, error_message_size, msg);
            return false;
        }
        p++; // consume comma
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') {
            flac_writer_set_affinity_error(error_message, error_message_size, "FLAC affinity list cannot end with ','");
            return false;
        }
    }

    if (!parsed_any || flac_writer_cpu_count_set(target_set) == 0) {
        flac_writer_set_affinity_error(error_message, error_message_size, "FLAC affinity list produced no CPU targets");
        return false;
    }

    cpu_set_t allowed_set;
    if (sched_getaffinity(0, sizeof(allowed_set), &allowed_set) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (CPU_ISSET(cpu, target_set) && !CPU_ISSET(cpu, &allowed_set)) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "FLAC affinity CPU %d is not available in current process affinity mask",
                         cpu);
                flac_writer_set_affinity_error(error_message, error_message_size, msg);
                return false;
            }
        }
    }

    return true;
}
#endif

bool flac_writer_affinity_supported(void) {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

bool flac_writer_validate_affinity_cpu_list(
    const char *cpu_list,
    char *error_message,
    size_t error_message_size
) {
#if defined(__linux__)
    cpu_set_t parsed_set;
    return flac_writer_parse_affinity_cpu_list_linux(
        cpu_list,
        &parsed_set,
        error_message,
        error_message_size
    );
#else
    (void)cpu_list;
    flac_writer_set_affinity_error(
        error_message,
        error_message_size,
        "FLAC affinity is only supported on Linux"
    );
    return false;
#endif
}

#if LIBFLAC_ENABLED == 1

#include "FLAC/stream_encoder.h"
#include "FLAC/metadata.h"

#if defined(_WIN32) || defined(_WIN64)
typedef __int64 flac_file_off_t;
#define FLAC_STREAM_FSEEK _fseeki64
#define FLAC_STREAM_FTELL _ftelli64
#else
typedef off_t flac_file_off_t;
#define FLAC_STREAM_FSEEK fseeko
#define FLAC_STREAM_FTELL ftello
#endif

static FLAC__uint64 flac_stream_max_offset(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (FLAC__uint64)INT64_MAX;
#else
    if (sizeof(flac_file_off_t) >= sizeof(int64_t)) {
        return (FLAC__uint64)INT64_MAX;
    }
    if (sizeof(flac_file_off_t) >= sizeof(long)) {
        return (FLAC__uint64)LONG_MAX;
    }
    return (FLAC__uint64)INT_MAX;
#endif
}

/* ============================================================================
 * Internal Writer Structure
 * ============================================================================ */
struct flac_writer {
    FLAC__StreamEncoder *encoder;
    FLAC__StreamMetadata *seektable;
    FILE *output_file;
    bool use_stream_callbacks;       // Stream mode vs FILE mode

    // Configuration (copy)
    flac_writer_config_t config;

    // Statistics
    uint64_t samples_written;
    uint64_t bytes_written;

    // Error state
    flac_writer_error_t last_error;
    char error_message[256];

    // Conversion buffer for int16->int32
    int32_t *conv_buffer;
    size_t conv_buffer_size;
};

/* ============================================================================
 * Thread count status strings (for FLAC API v14+)
 * ============================================================================ */
#if defined(HAVE_FLAC_THREADING) && HAVE_FLAC_THREADING && \
    defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
static const char* flac_thread_status_string(uint32_t status) {
    switch (status) {
        case FLAC__STREAM_ENCODER_SET_NUM_THREADS_OK:
            return "OK";
        case FLAC__STREAM_ENCODER_SET_NUM_THREADS_NOT_COMPILED_WITH_MULTITHREADING_ENABLED:
            return "Not compiled with multithreading";
        case FLAC__STREAM_ENCODER_SET_NUM_THREADS_ALREADY_INITIALIZED:
            return "Already initialized";
        case FLAC__STREAM_ENCODER_SET_NUM_THREADS_TOO_MANY_THREADS:
            return "Too many threads";
        default:
            return "Unknown error";
    }
}
#endif

/* ============================================================================
 * Stream Callbacks (for stream mode)
 * These callbacks enable libflac to update metadata (STREAMINFO, seektable)
 * after encoding is complete, which is required for proper file loading.
 * ============================================================================ */

// Write callback - writes encoded data to file and tracks bytes
static FLAC__StreamEncoderWriteStatus stream_write_callback(
    const FLAC__StreamEncoder *encoder,
    const FLAC__byte buffer[],
    size_t bytes,
    uint32_t samples,
    uint32_t current_frame,
    void *client_data)
{
    (void)encoder; (void)samples; (void)current_frame;
    flac_writer_t *writer = (flac_writer_t *)client_data;

    size_t written = fwrite(buffer, 1, bytes, writer->output_file);
    if (written != bytes) {
        return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }

    writer->bytes_written += bytes;

    // Call user callback if provided
    if (writer->config.bytes_cb) {
        writer->config.bytes_cb(writer->config.callback_user_data, bytes);
    }

    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

// Seek callback - allows libflac to seek back and update metadata
static FLAC__StreamEncoderSeekStatus stream_seek_callback(
    const FLAC__StreamEncoder *encoder,
    FLAC__uint64 absolute_byte_offset,
    void *client_data)
{
    (void)encoder;
    flac_writer_t *writer = (flac_writer_t *)client_data;
    if (absolute_byte_offset > flac_stream_max_offset()) {
        return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    }
    if (FLAC_STREAM_FSEEK(writer->output_file, (flac_file_off_t)absolute_byte_offset, SEEK_SET) != 0) {
        return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    }
    return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

// Tell callback - reports current file position to libflac
static FLAC__StreamEncoderTellStatus stream_tell_callback(
    const FLAC__StreamEncoder *encoder,
    FLAC__uint64 *absolute_byte_offset,
    void *client_data)
{
    (void)encoder;
    flac_writer_t *writer = (flac_writer_t *)client_data;
    flac_file_off_t pos = FLAC_STREAM_FTELL(writer->output_file);
    if (pos < 0) {
        return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
    }
    *absolute_byte_offset = (FLAC__uint64)pos;
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

/* ============================================================================
 * Error Reporting Helper
 * ============================================================================ */
static void report_error(flac_writer_t *writer, flac_writer_error_t error, const char *msg) {
    writer->last_error = error;
    strncpy(writer->error_message, msg, sizeof(writer->error_message) - 1);
    writer->error_message[sizeof(writer->error_message) - 1] = '\0';

    if (writer->config.error_cb) {
        writer->config.error_cb(writer->config.callback_user_data, error, msg);
    }
}

/* ============================================================================
 * Default Configuration
 * ============================================================================ */
flac_writer_config_t flac_writer_default_config(void) {
    flac_writer_config_t config = {
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .compression_level = 1,
        .verify = false,
        .num_threads = 0,  // Auto-detect
        .affinity_enabled = false,
        .affinity_cpu_list = "",
        .enable_seektable = true,
        .seektable_spacing = 1 << 18,  // ~6.5 seconds at 40kHz
        .error_cb = NULL,
        .bytes_cb = NULL,
        .callback_user_data = NULL
    };
    return config;
}

/* ============================================================================
 * Internal: Configure Encoder (common setup for both modes)
 * ============================================================================ */
static flac_writer_error_t configure_encoder(flac_writer_t *writer) {
    FLAC__StreamEncoder *enc = writer->encoder;
    FLAC__bool ok = true;

    ok &= FLAC__stream_encoder_set_verify(enc, writer->config.verify);
    ok &= FLAC__stream_encoder_set_compression_level(enc, writer->config.compression_level);
    ok &= FLAC__stream_encoder_set_channels(enc, 1);  // Always mono for MISRC
    ok &= FLAC__stream_encoder_set_bits_per_sample(enc, writer->config.bits_per_sample);
    ok &= FLAC__stream_encoder_set_sample_rate(enc, writer->config.sample_rate);
    ok &= FLAC__stream_encoder_set_total_samples_estimate(enc, 0);  // Unknown length

    if (!ok) {
        report_error(writer, FLAC_WRITER_ERR_CONFIG, "Failed to configure FLAC encoder parameters");
        return FLAC_WRITER_ERR_CONFIG;
    }

    // Multi-threading (FLAC API v14+) - changed from previous version to allow explicit single-threaded mode with num_threads=1
#if defined(HAVE_FLAC_THREADING) && HAVE_FLAC_THREADING
    if (writer->config.num_threads != 1) {  // 1 means explicitly single-threaded
        uint32_t threads = writer->config.num_threads;
        uint32_t status = FLAC__stream_encoder_set_num_threads(enc, threads);
        if (status != FLAC__STREAM_ENCODER_SET_NUM_THREADS_OK) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to set FLAC threads: %s",
                     flac_thread_status_string(status));
            report_error(writer, FLAC_WRITER_ERR_THREADS, msg);
            // Non-fatal - continue with default threading
        }
    }
#endif

    // Seektable
    if (writer->config.enable_seektable) {
        writer->seektable = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
        if (!writer->seektable) {
            report_error(writer, FLAC_WRITER_ERR_SEEKTABLE, "Failed to allocate seektable");
            return FLAC_WRITER_ERR_SEEKTABLE;
        }

        uint32_t spacing = writer->config.seektable_spacing;
        if (spacing == 0) spacing = 1 << 18;

        // Estimate for very long recordings (up to ~1.5 years at 40kHz)
        if (!FLAC__metadata_object_seektable_template_append_spaced_points(
                writer->seektable, spacing, (uint64_t)1 << 41) ||
            !FLAC__stream_encoder_set_metadata(enc, &writer->seektable, 1)) {
            report_error(writer, FLAC_WRITER_ERR_SEEKTABLE, "Failed to configure seektable");
            FLAC__metadata_object_delete(writer->seektable);
            writer->seektable = NULL;
            return FLAC_WRITER_ERR_SEEKTABLE;
        }
    }

    return FLAC_WRITER_OK;
}

flac_writer_error_t flac_writer_apply_thread_affinity(flac_writer_t *writer) {
    if (!writer) return FLAC_WRITER_ERR_ALLOC;

    if (!writer->config.affinity_enabled) {
        return FLAC_WRITER_OK;
    }

#if defined(__linux__) && !defined(__ANDROID__)
    cpu_set_t target_set;
    char parse_err[256];
    if (!flac_writer_parse_affinity_cpu_list_linux(
            writer->config.affinity_cpu_list,
            &target_set,
            parse_err,
            sizeof(parse_err))) {
        report_error(writer, FLAC_WRITER_ERR_AFFINITY, parse_err);
        return FLAC_WRITER_ERR_AFFINITY;
    }

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(target_set), &target_set);
    if (rc != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to apply FLAC thread affinity \"%s\": %s",
                 writer->config.affinity_cpu_list[0] ? writer->config.affinity_cpu_list : "(empty)",
                 strerror(rc));
        report_error(writer, FLAC_WRITER_ERR_AFFINITY, msg);
        return FLAC_WRITER_ERR_AFFINITY;
    }

    return FLAC_WRITER_OK;
#else
    report_error(writer, FLAC_WRITER_ERR_AFFINITY, "FLAC affinity is only supported on Linux");
    return FLAC_WRITER_ERR_AFFINITY;
#endif
}

/* ============================================================================
 * Create Writer (FILE mode)
 * ============================================================================ */
flac_writer_t *flac_writer_create_file(FILE *output_file, const flac_writer_config_t *config) {
    flac_writer_t *writer = calloc(1, sizeof(flac_writer_t));
    if (!writer) return NULL;

    writer->config = *config;
    writer->output_file = output_file;
    writer->use_stream_callbacks = false;

    writer->encoder = FLAC__stream_encoder_new();
    if (!writer->encoder) {
        report_error(writer, FLAC_WRITER_ERR_ALLOC, "Failed to allocate FLAC encoder");
        free(writer);
        return NULL;
    }

    flac_writer_error_t err = configure_encoder(writer);
    if (err != FLAC_WRITER_OK) {
        FLAC__stream_encoder_delete(writer->encoder);
        if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
        free(writer);
        return NULL;
    }

    FLAC__StreamEncoderInitStatus init_status =
        FLAC__stream_encoder_init_FILE(writer->encoder, output_file, NULL, NULL);

    if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg), "FLAC init failed: %s",
                 FLAC__StreamEncoderInitStatusString[init_status]);
        report_error(writer, FLAC_WRITER_ERR_INIT, msg);
        FLAC__stream_encoder_delete(writer->encoder);
        if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
        free(writer);
        return NULL;
    }

    return writer;
}

/* ============================================================================
 * Create Writer (Stream callback mode)
 * ============================================================================ */
flac_writer_t *flac_writer_create_stream(FILE *output_file, const flac_writer_config_t *config) {
    flac_writer_t *writer = calloc(1, sizeof(flac_writer_t));
    if (!writer) return NULL;

    writer->config = *config;
    writer->output_file = output_file;
    writer->use_stream_callbacks = true;

    writer->encoder = FLAC__stream_encoder_new();
    if (!writer->encoder) {
        report_error(writer, FLAC_WRITER_ERR_ALLOC, "Failed to allocate FLAC encoder");
        free(writer);
        return NULL;
    }

    flac_writer_error_t err = configure_encoder(writer);
    if (err != FLAC_WRITER_OK) {
        FLAC__stream_encoder_delete(writer->encoder);
        if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
        free(writer);
        return NULL;
    }

    // Use stream mode with seek/tell callbacks so libflac can update
    // STREAMINFO and seektable metadata after encoding completes.
    // This is critical for proper file loading in players like Audacity.
    FLAC__StreamEncoderInitStatus init_status =
        FLAC__stream_encoder_init_stream(
            writer->encoder,
            stream_write_callback,
            stream_seek_callback,   // enables metadata updates
            stream_tell_callback,   // enables metadata updates
            NULL,                   // metadata callback (not needed with seek/tell)
            writer
        );

    if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg), "FLAC stream init failed: %s",
                 FLAC__StreamEncoderInitStatusString[init_status]);
        report_error(writer, FLAC_WRITER_ERR_INIT, msg);
        FLAC__stream_encoder_delete(writer->encoder);
        if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
        free(writer);
        return NULL;
    }

    return writer;
}

/* ============================================================================
 * Process Samples (int32_t)
 * ============================================================================ */
int flac_writer_process(flac_writer_t *writer, const int32_t *samples, uint32_t num_samples) {
    if (!writer || !samples || num_samples == 0) return -1;

    // FLAC expects pointer to array of pointers (for multi-channel)
    // For mono, we pass address of our single pointer
    const FLAC__int32 *channel_ptrs[1] = { samples };

    FLAC__bool ok = FLAC__stream_encoder_process(writer->encoder, channel_ptrs, num_samples);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "FLAC process error: %s",
                 FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(writer->encoder)]);
        report_error(writer, FLAC_WRITER_ERR_PROCESS, msg);
        return -1;
    }

    writer->samples_written += num_samples;
    return (int)num_samples;
}

/* ============================================================================
 * Process Samples (int16_t convenience)
 * ============================================================================ */
int flac_writer_process_int16(flac_writer_t *writer, const int16_t *samples, uint32_t num_samples) {
    if (!writer || !samples || num_samples == 0) return -1;

    // Ensure conversion buffer is large enough
    if (writer->conv_buffer_size < num_samples) {
        int32_t *new_buf = realloc(writer->conv_buffer, num_samples * sizeof(int32_t));
        if (!new_buf) {
            report_error(writer, FLAC_WRITER_ERR_ALLOC, "Failed to allocate conversion buffer");
            return -1;
        }
        writer->conv_buffer = new_buf;
        writer->conv_buffer_size = num_samples;
    }

    // Convert int16 to int32 (sign-extend)
    for (uint32_t i = 0; i < num_samples; i++) {
        writer->conv_buffer[i] = samples[i];
    }

    return flac_writer_process(writer, writer->conv_buffer, num_samples);
}

/* ============================================================================
 * Finish Encoding
 * ============================================================================ */
flac_writer_error_t flac_writer_finish(flac_writer_t *writer) {
    if (!writer) return FLAC_WRITER_ERR_ALLOC;

    // Sort seektable before finishing
    if (writer->seektable) {
        FLAC__metadata_object_seektable_template_sort(writer->seektable, false);

        // Fix seektable for libflac < 1.5 (set unused points to placeholder)
#if !defined(FLAC_API_VERSION_CURRENT) || FLAC_API_VERSION_CURRENT < 14
        for (int i = (int)writer->seektable->data.seek_table.num_points - 1; i >= 0; i--) {
            if (writer->seektable->data.seek_table.points[i].stream_offset != 0) break;
            writer->seektable->data.seek_table.points[i].sample_number = 0xFFFFFFFFFFFFFFFFULL;
        }
#endif
    }

    FLAC__bool ok = FLAC__stream_encoder_finish(writer->encoder);
    flac_writer_error_t result = FLAC_WRITER_OK;

    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "FLAC finish error: %s",
                 FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(writer->encoder)]);
        report_error(writer, FLAC_WRITER_ERR_FINISH, msg);
        result = FLAC_WRITER_ERR_FINISH;
    }

    // Cleanup
    if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
    FLAC__stream_encoder_delete(writer->encoder);
    if (writer->conv_buffer) free(writer->conv_buffer);
    free(writer);

    return result;
}

/* ============================================================================
 * Abort (cleanup without finalizing)
 * ============================================================================ */
void flac_writer_abort(flac_writer_t *writer) {
    if (!writer) return;

    if (writer->seektable) FLAC__metadata_object_delete(writer->seektable);
    FLAC__stream_encoder_delete(writer->encoder);
    if (writer->conv_buffer) free(writer->conv_buffer);
    free(writer);
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */
const char *flac_writer_get_error_string(flac_writer_t *writer) {
    return writer ? writer->error_message : "NULL writer";
}

uint64_t flac_writer_get_samples_written(flac_writer_t *writer) {
    return writer ? writer->samples_written : 0;
}

uint64_t flac_writer_get_bytes_written(flac_writer_t *writer) {
    return writer ? writer->bytes_written : 0;
}

bool flac_writer_available(void) {
    return true;
}

const char *flac_writer_get_flac_version(void) {
    return FLAC__VERSION_STRING;
}

bool flac_writer_multithreading_available(void) {
#if defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
    return true;
#else
    return false;
#endif
}

#else // LIBFLAC_ENABLED != 1

/* ============================================================================
 * Stub implementations when FLAC is disabled
 * ============================================================================ */

flac_writer_config_t flac_writer_default_config(void) {
    flac_writer_config_t config = {0};
    return config;
}

flac_writer_t *flac_writer_create_file(FILE *f, const flac_writer_config_t *c) {
    (void)f; (void)c;
    return NULL;
}

flac_writer_t *flac_writer_create_stream(FILE *f, const flac_writer_config_t *c) {
    (void)f; (void)c;
    return NULL;
}

int flac_writer_process(flac_writer_t *w, const int32_t *s, uint32_t n) {
    (void)w; (void)s; (void)n;
    return -1;
}

int flac_writer_process_int16(flac_writer_t *w, const int16_t *s, uint32_t n) {
    (void)w; (void)s; (void)n;
    return -1;
}

flac_writer_error_t flac_writer_finish(flac_writer_t *w) {
    (void)w;
    return FLAC_WRITER_ERR_DISABLED;
}

void flac_writer_abort(flac_writer_t *w) { (void)w; }

const char *flac_writer_get_error_string(flac_writer_t *w) {
    (void)w;
    return "FLAC support not compiled in";
}

uint64_t flac_writer_get_samples_written(flac_writer_t *w) { (void)w; return 0; }
uint64_t flac_writer_get_bytes_written(flac_writer_t *w) { (void)w; return 0; }
bool flac_writer_available(void) { return false; }
const char *flac_writer_get_flac_version(void) { return "N/A"; }
bool flac_writer_multithreading_available(void) { return false; }
flac_writer_error_t flac_writer_apply_thread_affinity(flac_writer_t *w) {
    (void)w;
    return FLAC_WRITER_ERR_DISABLED;
}

#endif // LIBFLAC_ENABLED
