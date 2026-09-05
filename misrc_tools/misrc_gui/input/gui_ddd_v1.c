/* MISRC GUI - isolated Domesday Duplicator firmware 3.1 backend. */

#ifdef ENABLE_DDD

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include "../../common/libusb_compat.h"
#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#undef NOUSER
#else
#include "../../common/libusb_compat.h"
#endif

#include "gui_ddd_v1.h"
#include "gui_ddd.h"
#include "gui_ddd_async.h"
#include "../core/gui_app.h"
#include "../output/gui_record.h"
#include "../processing/gui_display_thread.h"
#include "../processing/gui_extract.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#define DDD_V1_CONTROL_TIMEOUT_MS 1000
#define DDD_V1_STARTUP_DISCARD_BYTES (UINT64_C(8) * 1024u * 1024u)

typedef enum {
    DDD_V1_RESULT_RUNNING = 0,
    DDD_V1_RESULT_SUCCESS,
    DDD_V1_RESULT_USB_FAILURE,
    DDD_V1_RESULT_SEQUENCE_FAILURE,
    DDD_V1_RESULT_TEST_FAILURE,
    DDD_V1_RESULT_BACKPRESSURE
} ddd_v1_capture_result_t;

typedef enum {
    DDD_V1_LOCK_NONE = 0,
    DDD_V1_LOCK_WAIT_DISAPPEARANCE,
    DDD_V1_LOCK_WAIT_REAPPEARANCE
} ddd_v1_lock_phase_t;

typedef struct {
    gui_app_t *app;
    uint64_t discarded_bytes;
    uint64_t published_blocks;
} ddd_v1_consumer_t;

static libusb_context *s_context;
static libusb_device_handle *s_handle;
static bool s_interface_claimed;
static atomic_bool s_queue_ready = ATOMIC_VAR_INIT(false);
static atomic_bool s_startup_failed = ATOMIC_VAR_INIT(false);
static gui_ddd_async_orphan_t *s_orphan;
static ddd_collection_state_t s_collection;
static ddd_sequence_validator_t s_sequence;
static ddd_test_ramp_validator_t s_test_ramp;
static bool s_test_mode;
static uint8_t s_decimation = DDD_DECIMATION_FULL_RATE;
static uint32_t s_sample_rate_hz = DDD_CONVERTER_SAMPLE_RATE_HZ;
static char s_usb_path[DDD_STABLE_ID_MAX];
static ddd_v1_capture_result_t s_result = DDD_V1_RESULT_SUCCESS;
static ddd_v1_lock_phase_t s_lock_phase;
static char s_locked_path[DDD_STABLE_ID_MAX];

typedef struct {
    atomic_uint revision;
    atomic_bool present;
    atomic_uint format;
    atomic_bool overflow_seen;
    atomic_bool saturated;
    atomic_uint latch_count;
    atomic_uint used_now;
    atomic_uint peak;
    atomic_uint peak_since_open;
    atomic_uint overflow_events;
    atomic_uint dropped_words;
    atomic_uint packets_read;
    atomic_uint near_full_units;
    atomic_uint depth_words;
    atomic_uint packet_words;
    atomic_uint near_full_words;
    atomic_bool totals_latch_seen;
    atomic_uint totals_last_latch_count;
    atomic_bool interval_coverage_complete;
    atomic_bool totals_saturated;
    atomic_ullong total_overflow_events;
    atomic_ullong total_dropped_words;
    atomic_ullong total_near_full_units;
    atomic_uint run_peak_words;
    atomic_int peak_backpressure_percent;
} ddd_v1_fifo_store_t;

static ddd_v1_fifo_store_t s_fifo_store;
static ddd_fifo_telemetry_totals_t s_fifo_totals;

_Static_assert(GUI_DDD_ASYNC_TRANSFER_BYTES ==
                   (size_t)DDD_SEQUENCE_SAMPLES_PER_MARKER * sizeof(uint16_t),
               "DDD 3.1 transfer must contain one sequence-marker block");
_Static_assert(DDD_V1_STARTUP_DISCARD_BYTES %
                   GUI_DDD_ASYNC_TRANSFER_BYTES == 0,
               "DDD 3.1 startup discard must contain whole transfers");

static const char *ddd_v1_result_name(ddd_v1_capture_result_t result)
{
    switch (result) {
        case DDD_V1_RESULT_RUNNING: return "Running";
        case DDD_V1_RESULT_SUCCESS: return "Success";
        case DDD_V1_RESULT_USB_FAILURE: return "UsbFailure";
        case DDD_V1_RESULT_SEQUENCE_FAILURE: return "SequenceFailure";
        case DDD_V1_RESULT_TEST_FAILURE: return "TestFailure";
        case DDD_V1_RESULT_BACKPRESSURE: return "Backpressure";
    }
    return "Unknown";
}

static const char *ddd_v1_protocol_result_name(ddd_protocol_result_t result)
{
    switch (result) {
        case DDD_PROTOCOL_OK: return "OK";
        case DDD_PROTOCOL_INVALID_ARGUMENT: return "InvalidArgument";
        case DDD_PROTOCOL_UNSUPPORTED_PROFILE: return "UnsupportedProfile";
        case DDD_PROTOCOL_UNSUPPORTED_DECIMATION: return "UnsupportedDecimation";
        case DDD_PROTOCOL_CONTROL_FAILURE: return "ControlFailure";
        case DDD_PROTOCOL_IDENTITY_MISMATCH: return "IdentityMismatch";
        case DDD_PROTOCOL_READBACK_MISMATCH: return "ReadbackMismatch";
    }
    return "Unknown";
}

static void ddd_v1_fifo_publish(
    const ddd_fifo_telemetry_t *latest,
    const ddd_fifo_telemetry_totals_t *totals)
{
    if (!latest || !totals) return;

    /* Sequential consistency keeps a validated revision from spanning two
     * telemetry generations while remaining non-blocking in the USB loop. */
    atomic_fetch_add_explicit(&s_fifo_store.revision, 1,
                              memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.present, latest->present,
                          memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.format, latest->format,
                          memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.overflow_seen,
                          latest->overflow_seen, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.saturated, latest->saturated,
                          memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.latch_count, latest->latch_count,
                          memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.used_now, latest->used_now,
                          memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.peak, latest->peak,
                          memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.peak_since_open,
                          latest->peak_since_open, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.overflow_events,
                          latest->overflow_events, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.dropped_words,
                          latest->dropped_words, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.packets_read,
                          latest->packets_read, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.near_full_units,
                          latest->near_full_units, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.depth_words, latest->depth_words,
                          memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.packet_words, latest->packet_words,
                          memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.near_full_words,
                          latest->near_full_words, memory_order_seq_cst);

    atomic_store_explicit(&s_fifo_store.totals_latch_seen,
                          totals->latch_seen, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.totals_last_latch_count,
                          totals->last_latch_count, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.interval_coverage_complete,
                          totals->interval_coverage_complete,
                          memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.totals_saturated,
                          totals->saturated, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.total_overflow_events,
                          totals->overflow_events, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.total_dropped_words,
                          totals->dropped_words, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.total_near_full_units,
                          totals->near_full_units, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.run_peak_words,
                          totals->peak_words, memory_order_seq_cst);
    atomic_store_explicit(&s_fifo_store.peak_backpressure_percent,
                          totals->peak_backpressure_percent,
                          memory_order_seq_cst);
    atomic_fetch_add_explicit(&s_fifo_store.revision, 1,
                              memory_order_seq_cst);
}

static void ddd_v1_fifo_reset(void)
{
    ddd_fifo_telemetry_t latest;

    ddd_fifo_telemetry_init(&latest);
    ddd_fifo_telemetry_totals_init(&s_fifo_totals);
    ddd_v1_fifo_publish(&latest, &s_fifo_totals);
}

static void ddd_v1_fifo_receive(void *context,
                                const uint8_t *data,
                                size_t size)
{
    ddd_fifo_telemetry_t latest;

    (void)context;

    (void)ddd_fifo_telemetry_parse(data, size, &latest);
    (void)ddd_fifo_telemetry_totals_add(&s_fifo_totals, &latest);
    ddd_v1_fifo_publish(&latest, &s_fifo_totals);
}

bool gui_ddd_v1_get_fifo_snapshot(gui_ddd_v1_fifo_snapshot_t *snapshot)
{
    unsigned before;
    unsigned after;

    if (!snapshot) return false;
    do {
        before = atomic_load_explicit(&s_fifo_store.revision,
                                      memory_order_seq_cst);
        if ((before & 1u) != 0) continue;

        snapshot->latest.present = atomic_load_explicit(
            &s_fifo_store.present, memory_order_seq_cst);
        snapshot->latest.format = (uint8_t)atomic_load_explicit(
            &s_fifo_store.format, memory_order_seq_cst);
        snapshot->latest.overflow_seen = atomic_load_explicit(
            &s_fifo_store.overflow_seen, memory_order_seq_cst);
        snapshot->latest.saturated = atomic_load_explicit(
            &s_fifo_store.saturated, memory_order_seq_cst);
        snapshot->latest.latch_count = (uint8_t)atomic_load_explicit(
            &s_fifo_store.latch_count, memory_order_seq_cst);
        snapshot->latest.used_now = (uint16_t)atomic_load_explicit(
            &s_fifo_store.used_now, memory_order_seq_cst);
        snapshot->latest.peak = (uint16_t)atomic_load_explicit(
            &s_fifo_store.peak, memory_order_seq_cst);
        snapshot->latest.peak_since_open = (uint16_t)atomic_load_explicit(
            &s_fifo_store.peak_since_open, memory_order_seq_cst);
        snapshot->latest.overflow_events = (uint16_t)atomic_load_explicit(
            &s_fifo_store.overflow_events, memory_order_seq_cst);
        snapshot->latest.dropped_words = (uint16_t)atomic_load_explicit(
            &s_fifo_store.dropped_words, memory_order_seq_cst);
        snapshot->latest.packets_read = (uint16_t)atomic_load_explicit(
            &s_fifo_store.packets_read, memory_order_seq_cst);
        snapshot->latest.near_full_units = (uint16_t)atomic_load_explicit(
            &s_fifo_store.near_full_units, memory_order_seq_cst);
        snapshot->latest.depth_words = (uint16_t)atomic_load_explicit(
            &s_fifo_store.depth_words, memory_order_seq_cst);
        snapshot->latest.packet_words = (uint16_t)atomic_load_explicit(
            &s_fifo_store.packet_words, memory_order_seq_cst);
        snapshot->latest.near_full_words = (uint16_t)atomic_load_explicit(
            &s_fifo_store.near_full_words, memory_order_seq_cst);

        snapshot->totals.latch_seen = atomic_load_explicit(
            &s_fifo_store.totals_latch_seen, memory_order_seq_cst);
        snapshot->totals.last_latch_count = (uint8_t)atomic_load_explicit(
            &s_fifo_store.totals_last_latch_count, memory_order_seq_cst);
        snapshot->totals.interval_coverage_complete = atomic_load_explicit(
            &s_fifo_store.interval_coverage_complete,
            memory_order_seq_cst);
        snapshot->totals.saturated = atomic_load_explicit(
            &s_fifo_store.totals_saturated, memory_order_seq_cst);
        snapshot->totals.overflow_events = atomic_load_explicit(
            &s_fifo_store.total_overflow_events, memory_order_seq_cst);
        snapshot->totals.dropped_words = atomic_load_explicit(
            &s_fifo_store.total_dropped_words, memory_order_seq_cst);
        snapshot->totals.near_full_units = atomic_load_explicit(
            &s_fifo_store.total_near_full_units, memory_order_seq_cst);
        snapshot->totals.peak_words = (uint16_t)atomic_load_explicit(
            &s_fifo_store.run_peak_words, memory_order_seq_cst);
        snapshot->totals.peak_backpressure_percent = atomic_load_explicit(
            &s_fifo_store.peak_backpressure_percent,
            memory_order_seq_cst);

        after = atomic_load_explicit(&s_fifo_store.revision,
                                     memory_order_seq_cst);
    } while (before != after || (after & 1u) != 0);
    return snapshot->latest.present;
}

static bool ddd_v1_format_usb_path(libusb_device *device,
                                   char *path,
                                   size_t path_size)
{
    uint8_t ports[8];
    int port_count;

    if (!device || !path || path_size == 0) return false;
    port_count = libusb_get_port_numbers(device, ports, (int)sizeof(ports));
    return ddd_format_usb_topology_path(libusb_get_bus_number(device), ports,
                                        port_count, path, path_size);
}

static bool ddd_v1_find_exact_endpoint(libusb_device *device)
{
    struct libusb_config_descriptor *config = NULL;
    ddd_stream_selector_t selector;
    ddd_stream_path_t selected;
    int result = libusb_get_active_config_descriptor(device, &config);

    if (result != 0 || !config) {
        result = libusb_get_config_descriptor(device, 0, &config);
    }
    if (result != 0 || !config) return false;
    ddd_stream_selector_init(&selector, DDD_DEVICE_PROTOCOL_V1);
    for (int i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *interface = &config->interface[i];
        for (int j = 0; j < interface->num_altsetting; ++j) {
            const struct libusb_interface_descriptor *alternate =
                &interface->altsetting[j];
            for (int k = 0; k < alternate->bNumEndpoints; ++k) {
                const struct libusb_endpoint_descriptor *endpoint =
                    &alternate->endpoint[k];
                ddd_stream_endpoint_candidate_t candidate = {
                    .interface_number = alternate->bInterfaceNumber,
                    .alternate_setting = alternate->bAlternateSetting,
                    .endpoint_address = endpoint->bEndpointAddress,
                    .max_packet_size = endpoint->wMaxPacketSize,
                    .is_bulk = (endpoint->bmAttributes & 0x03) ==
                        LIBUSB_TRANSFER_TYPE_BULK,
                    .is_in = (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0
                };
                ddd_stream_selector_consider(&selector, &candidate);
            }
        }
    }
    libusb_free_config_descriptor(config);
    return ddd_stream_selector_get(&selector, &selected);
}

static int ddd_v1_control_transfer(void *context,
                                   uint8_t request_type,
                                   uint8_t request,
                                   uint16_t value,
                                   uint16_t index,
                                   uint8_t *data,
                                   uint16_t length)
{
    return libusb_control_transfer((libusb_device_handle *)context,
                                   request_type, request, value, index,
                                   data, length, DDD_V1_CONTROL_TIMEOUT_MS);
}

static ddd_control_ops_t ddd_v1_control_ops(void)
{
    ddd_control_ops_t ops = {
        .transfer = ddd_v1_control_transfer,
        .context = s_handle
    };
    return ops;
}

static void ddd_v1_lock_active_path(const char *reason)
{
    if (s_usb_path[0]) {
        snprintf(s_locked_path, sizeof(s_locked_path), "%s", s_usb_path);
        s_lock_phase = DDD_V1_LOCK_WAIT_DISAPPEARANCE;
    }
    fprintf(stderr, "[DdD 3.1] Safety lock for %s: %s\n",
            s_locked_path[0] ? s_locked_path : "unknown path",
            reason ? reason : "unverified device state");
}

void gui_ddd_v1_observe_enumeration(const misrc_device_list_t *devices)
{
    bool present = false;

    if (!devices || !devices->ddd_enumeration_complete ||
        s_lock_phase == DDD_V1_LOCK_NONE || !s_locked_path[0]) {
        return;
    }
    for (size_t i = 0; i < devices->count; ++i) {
        const misrc_device_info_t *device = &devices->devices[i];
        if (device->type == MISRC_DEVICE_TYPE_DDD &&
            device->ddd_profile == DDD_DEVICE_PROTOCOL_V1 &&
            strcmp(device->ddd_usb_path, s_locked_path) == 0) {
            present = true;
            break;
        }
    }
    if (s_lock_phase == DDD_V1_LOCK_WAIT_DISAPPEARANCE && !present) {
        s_lock_phase = DDD_V1_LOCK_WAIT_REAPPEARANCE;
        fprintf(stderr, "[DdD 3.1] Safety lock observed unplug for %s\n",
                s_locked_path);
    } else if (s_lock_phase == DDD_V1_LOCK_WAIT_REAPPEARANCE && present) {
        fprintf(stderr, "[DdD 3.1] Safety lock cleared after replug for %s\n",
                s_locked_path);
        s_lock_phase = DDD_V1_LOCK_NONE;
        s_locked_path[0] = '\0';
    }
}

static bool ddd_v1_path_locked(const char *path)
{
    return path && s_lock_phase != DDD_V1_LOCK_NONE &&
           strcmp(path, s_locked_path) == 0;
}

static bool ddd_v1_sync_control_allowed(void)
{
    if (s_orphan && gui_ddd_async_orphan_has_unreaped(s_orphan)) return false;
    if (s_orphan) {
        if (!gui_ddd_async_orphan_try_reclaim(s_orphan)) return false;
        s_orphan = NULL;
    }
    return true;
}

static void ddd_v1_close(void)
{
    bool retain_usb = false;

    if (s_orphan) {
        if (gui_ddd_async_orphan_try_reclaim(s_orphan)) {
            s_orphan = NULL;
        } else if (gui_ddd_async_orphan_abandon(s_orphan)) {
            s_orphan = NULL;
            retain_usb = true;
        } else {
            return;
        }
    }
    if (!retain_usb && s_handle) {
        if (s_interface_claimed) {
            libusb_release_interface(s_handle, DDD_STREAM_INTERFACE_NUMBER);
        }
        libusb_close(s_handle);
    }
    if (!retain_usb && s_context) libusb_exit(s_context);
    s_handle = NULL;
    s_context = NULL;
    s_interface_claimed = false;
    s_usb_path[0] = '\0';
    ddd_collection_state_init(&s_collection);
    ddd_v1_fifo_reset();
}

int gui_ddd_v1_open(gui_app_t *app, const char *stable_usb_path)
{
    libusb_device **devices = NULL;
    libusb_device *selected = NULL;
    ssize_t device_count;
    size_t match_count = 0;
    int result;

    if (!app) return -1;
    if (!stable_usb_path || !stable_usb_path[0]) {
        gui_app_set_status(app, "Selected DdD USB path is unavailable");
        return -1;
    }
    if (gui_ddd_async_global_quarantine_active()) {
        gui_app_set_status(app, "DdD USB safety lock active; restart MISRC");
        return -1;
    }
    if (ddd_v1_path_locked(stable_usb_path)) {
        gui_app_set_status(app, "DdD safety lock active; unplug and reconnect it");
        return -1;
    }
#if LIBUSB_API_VERSION >= 0x0100010A
    result = libusb_init_context(&s_context, NULL, 0);
#else
    result = libusb_init(&s_context);
#endif
    if (result != 0) {
        gui_app_set_status(app, "Failed to initialize DdD USB access");
        return -1;
    }
    device_count = libusb_get_device_list(s_context, &devices);
    if (device_count < 0) {
        gui_app_set_status(app, "Failed to enumerate DdD USB devices");
        ddd_v1_close();
        return -1;
    }
    for (ssize_t i = 0; i < device_count; ++i) {
        struct libusb_device_descriptor descriptor;
        char candidate_path[DDD_STABLE_ID_MAX];
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0 ||
            ddd_classify_device(descriptor.idVendor, descriptor.idProduct,
                                descriptor.bcdDevice) !=
                DDD_DEVICE_PROTOCOL_V1 ||
            !ddd_v1_format_usb_path(devices[i], candidate_path,
                                    sizeof(candidate_path)) ||
            strcmp(candidate_path, stable_usb_path) != 0) {
            continue;
        }
        selected = devices[i];
        ++match_count;
    }
    if (match_count != 1 || !selected) {
        fprintf(stderr, "[DdD 3.1] Exact USB path match count was %zu\n",
                match_count);
        gui_app_set_status(app,
                           "Selected DdD device is no longer at its USB path");
        libusb_free_device_list(devices, 1);
        ddd_v1_close();
        return -1;
    }
    {
        enum libusb_speed speed = libusb_get_device_speed(selected);
        if (!ddd_v1_link_speed_allowed(speed != LIBUSB_SPEED_UNKNOWN,
                                       speed >= LIBUSB_SPEED_SUPER)) {
            gui_app_set_status(app, "DdD requires USB 3 SuperSpeed");
            libusb_free_device_list(devices, 1);
            ddd_v1_close();
            return -1;
        }
    }
    if (!ddd_v1_find_exact_endpoint(selected)) {
        gui_app_set_status(app, "DdD USB stream descriptor mismatch");
        libusb_free_device_list(devices, 1);
        ddd_v1_close();
        return -1;
    }
    result = libusb_open(selected, &s_handle);
    libusb_free_device_list(devices, 1);
    if (result != 0 || !s_handle) {
        gui_app_set_status(app, "Failed to open DdD device");
        ddd_v1_close();
        return -1;
    }
#if LIBUSB_API_VERSION >= 0x01000106
    (void)libusb_set_auto_detach_kernel_driver(s_handle, 1);
#endif
    result = libusb_claim_interface(s_handle, DDD_STREAM_INTERFACE_NUMBER);
    if (result != 0) {
        gui_app_set_status(app, "Failed to claim DdD USB interface");
        ddd_v1_close();
        return -1;
    }
    s_interface_claimed = true;
    snprintf(s_usb_path, sizeof(s_usb_path), "%s", stable_usb_path);
    fprintf(stderr, "[DdD 3.1] Opened exact device path %s\n", s_usb_path);
    return 0;
}

static gui_ddd_async_consume_result_t ddd_v1_fail(
    gui_app_t *app,
    ddd_v1_capture_result_t result,
    gui_dropout_reason_t reason,
    const char *message)
{
    if (app) {
        fprintf(stderr, "[DdD 3.1] %s\n", message);
        gui_record_log_capture_event(app, "ERROR", message,
                                     GUI_ERROR_CLASS_SYSTEM, 1);
        atomic_store(&app->dropout_stop_reason, reason);
        atomic_store(&app->dropout_stop_requested, true);
        atomic_store(&app->ddd_running, false);
        atomic_store(&app->stream_synced, false);
    }
    s_result = result;
    return GUI_DDD_ASYNC_CONSUME_FAILED;
}

static gui_ddd_async_consume_result_t ddd_v1_consume(
    void *context, const uint8_t *data, size_t size)
{
    ddd_v1_consumer_t *consumer = (ddd_v1_consumer_t *)context;
    gui_app_t *app = consumer ? consumer->app : NULL;
    const uint16_t *input;
    uint32_t *output;
    size_t sample_count;
    size_t output_size;

    if (!app || !data || !gui_ddd_async_policy_exact_length(size)) {
        return ddd_v1_fail(app, DDD_V1_RESULT_USB_FAILURE,
                           GUI_DROPOUT_FRAME_ERROR,
                           "Async USB transfer had an invalid length");
    }
    if (consumer->discarded_bytes < DDD_V1_STARTUP_DISCARD_BYTES) {
        consumer->discarded_bytes += size;
        atomic_store(&app->last_callback_time_ms, get_time_ms());
        return GUI_DDD_ASYNC_CONSUME_CONTINUE;
    }
    input = (const uint16_t *)data;
    sample_count = size / sizeof(*input);
    if (ddd_sequence_validator_feed(&s_sequence, input, sample_count) !=
        DDD_VALIDATION_OK) {
        char message[192];
        snprintf(message, sizeof(message),
                 "Sequence mismatch at sample %llu (expected %u, got %u)",
                 (unsigned long long)s_sequence.error_sample_index,
                 (unsigned)s_sequence.expected_marker,
                 (unsigned)s_sequence.actual_marker);
        atomic_fetch_add(&app->missed_frame_count, 1);
        return ddd_v1_fail(app, DDD_V1_RESULT_SEQUENCE_FAILURE,
                           GUI_DROPOUT_MISSED_FRAME, message);
    }
    if (s_test_mode &&
        ddd_test_ramp_validator_feed(&s_test_ramp, input, sample_count) !=
            DDD_VALIDATION_OK) {
        char message[192];
        snprintf(message, sizeof(message),
                 "Test ramp mismatch at sample %llu (expected %u, got %u)",
                 (unsigned long long)s_test_ramp.error_sample_index,
                 (unsigned)s_test_ramp.expected_value,
                 (unsigned)s_test_ramp.actual_value);
        return ddd_v1_fail(app, DDD_V1_RESULT_TEST_FAILURE,
                           GUI_DROPOUT_FRAME_ERROR, message);
    }
    output_size = sample_count * sizeof(*output);
    output = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                            output_size, NULL);
    if (!output) {
        atomic_fetch_add(&app->rb_drop_count, 1);
        return ddd_v1_fail(app, DDD_V1_RESULT_BACKPRESSURE,
                           GUI_DROPOUT_BACKPRESSURE,
                           "RF capture buffer backpressure");
    }
    for (size_t i = 0; i < sample_count; ++i) {
        uint32_t sample = input[i] & DDD_SAMPLE_MASK;
        int32_t signed_sample = (int32_t)sample - 512;
        if (sample == 0) atomic_fetch_add(&app->clip_count_a_neg, 1);
        if (sample == DDD_SAMPLE_MASK) atomic_fetch_add(&app->clip_count_a_pos, 1);
        if (signed_sample >= 0) {
            uint16_t peak = atomic_load(&app->peak_a_pos);
            if ((uint16_t)signed_sample > peak) {
                atomic_store(&app->peak_a_pos, (uint16_t)signed_sample);
            }
        } else {
            uint16_t magnitude = (uint16_t)(-signed_sample);
            uint16_t peak = atomic_load(&app->peak_a_neg);
            if (magnitude > peak) atomic_store(&app->peak_a_neg, magnitude);
        }
        output[i] = DDD_PACK_12BIT(sample);
    }
    bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, output_size);
    bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);
    atomic_fetch_add(&app->total_samples, sample_count);
    atomic_fetch_add(&app->samples_a, sample_count);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    ++consumer->published_blocks;
    if (consumer->published_blocks == 1) atomic_store(&app->stream_synced, true);
    return GUI_DDD_ASYNC_CONSUME_CONTINUE;
}

static int ddd_v1_capture_thread(void *context)
{
    gui_app_t *app = (gui_app_t *)context;
    ddd_v1_consumer_t consumer = {.app = app};
    gui_ddd_async_config_t config = {
        .usb_context = s_context,
        .device_handle = s_handle,
        .endpoint = DDD_STREAM_ENDPOINT_ADDRESS,
        .capture_running = &app->ddd_running,
        .transfer_ready = &s_queue_ready,
        .startup_failed = &s_startup_failed,
        .consume = ddd_v1_consume,
        .consume_context = &consumer,
        .telemetry = ddd_v1_fifo_receive,
        .telemetry_context = NULL
    };
    gui_ddd_async_result_t async_result = {0};
    int result;

    thrd_set_priority(THRD_PRIORITY_CRITICAL);
    result = gui_ddd_async_run(&config, &async_result);
    if (async_result.transfers_unreaped && async_result.orphan) {
        s_orphan = async_result.orphan;
    }
    if (result < 0 &&
        async_result.code != GUI_DDD_ASYNC_RESULT_CONSUMER_FAILURE) {
        char message[224];
        snprintf(message, sizeof(message),
                 "Async USB queue failed: %s (usb=%d status=%d length=%d)",
                 gui_ddd_async_result_name(async_result.code),
                 async_result.libusb_error, async_result.transfer_status,
                 async_result.actual_length);
        if (async_result.ready_signalled) {
            (void)ddd_v1_fail(app, DDD_V1_RESULT_USB_FAILURE,
                              GUI_DROPOUT_DEVICE_ERROR, message);
        } else {
            s_result = DDD_V1_RESULT_USB_FAILURE;
            fprintf(stderr, "[DdD 3.1] %s\n", message);
        }
    }
    if (s_result == DDD_V1_RESULT_RUNNING) s_result = DDD_V1_RESULT_SUCCESS;
    return result;
}

static void ddd_v1_stop_workers(gui_app_t *app, bool display_started)
{
    if (display_started && app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }
    gui_extract_stop();
}

int gui_ddd_v1_start(gui_app_t *app, uint8_t decimation, bool test_mode)
{
    ddd_control_ops_t ops;
    ddd_protocol_result_t protocol_result;
    bool display_started = false;
    thrd_t thread;

    if (!app) return -1;
    ddd_v1_fifo_reset();
    if (!s_handle) {
        gui_app_set_status(app, "DdD device is not open");
        return -1;
    }
    if (!ddd_decimation_is_supported(decimation)) {
        gui_app_set_status(app, "Invalid DdD hardware rate");
        return -1;
    }
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    atomic_store(&s_queue_ready, false);
    atomic_store(&s_startup_failed, false);
    s_test_mode = test_mode;
    s_decimation = decimation;
    s_sample_rate_hz = ddd_sample_rate_hz(decimation);
    atomic_store(&app->sample_rate, s_sample_rate_hz);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        gui_app_set_status(app, "Failed to initialize DdD capture buffer");
        ddd_v1_close();
        return -1;
    }
    bufmgr_reset(&app->buffers, BUF_CAPTURE_RF);
    app->is_capturing = true;
    if (gui_extract_start(app) < 0) {
        app->is_capturing = false;
        gui_app_set_status(app, "Failed to start DdD extraction");
        ddd_v1_close();
        return -1;
    }
    if (app->display_thread &&
        gui_display_thread_start(app->display_thread, app, &app->buffers) == 0) {
        display_started = true;
    }
    ops = ddd_v1_control_ops();
    protocol_result = ddd_collection_start_v1(&ops, test_mode, decimation,
                                              &s_collection);
    if (protocol_result != DDD_PROTOCOL_OK) {
        bool unsafe = s_collection.rollback_attempted &&
                      !s_collection.rollback_succeeded;
        fprintf(stderr, "[DdD 3.1] Configuration failed: %s\n",
                ddd_v1_protocol_result_name(protocol_result));
        if (unsafe) ddd_v1_lock_active_path("startup rollback failed");
        app->is_capturing = false;
        ddd_v1_stop_workers(app, display_started);
        ddd_v1_close();
        gui_app_set_status(app, unsafe
            ? "DdD rollback failed; unplug and reconnect it"
            : "DdD configuration/readback failed");
        return -1;
    }
    ddd_sequence_validator_init(&s_sequence);
    ddd_test_ramp_validator_init(&s_test_ramp);
    s_result = DDD_V1_RESULT_RUNNING;
    atomic_store(&app->ddd_running, true);
    if (thrd_create_with_priority(&thread, ddd_v1_capture_thread, app,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        bool rollback_failed;
        atomic_store(&app->ddd_running, false);
        app->is_capturing = false;
        ddd_v1_stop_workers(app, display_started);
        rollback_failed = ddd_collection_rollback_v1(
            &ops, &s_collection) != DDD_PROTOCOL_OK;
        if (rollback_failed) {
            ddd_v1_lock_active_path("thread-start rollback failed");
        }
        ddd_v1_close();
        gui_app_set_status(app, rollback_failed
            ? "DdD thread-start rollback failed; unplug and reconnect it"
            : "Failed to start DdD capture thread");
        return -1;
    }
    app->ddd_thread = (void *)(uintptr_t)thread;
    for (int i = 0; i < 100; ++i) {
        if (gui_ddd_async_policy_stream_ready(
                atomic_load(&s_queue_ready),
                atomic_load(&app->stream_synced),
                atomic_load(&app->ddd_running),
                atomic_load(&s_startup_failed))) {
            char message[224];
            char commit[DDD_COMMIT_LENGTH + 7] = {0};
            (void)ddd_format_gateware_commit(
                s_collection.identity, sizeof(s_collection.identity),
                commit, sizeof(commit));
            snprintf(message, sizeof(message),
                     "DdD protocol v1 capture started (path=%s, %u MSPS, test=%s, gateware=%s)",
                     s_usb_path, (unsigned)(s_sample_rate_hz / 1000000u),
                     test_mode ? "on" : "off", commit[0] ? commit : "n/a");
            gui_record_log_capture_event(app, "INFO", message,
                                         GUI_ERROR_CLASS_NONE, 0);
            gui_app_set_status(app, "DdD capture running");
            return 0;
        }
        if (atomic_load(&s_startup_failed) ||
            !atomic_load(&app->ddd_running)) break;
        thrd_sleep_ms(10);
    }
    atomic_store(&app->ddd_running, false);
    app->is_capturing = false;
    thrd_join(thread, NULL);
    app->ddd_thread = NULL;
    ddd_v1_stop_workers(app, display_started);
    bool restart_required = false;
    bool replug_required = false;
    if (!ddd_v1_sync_control_allowed()) {
        restart_required = true;
        ddd_v1_lock_active_path("startup callbacks remained unreaped");
    } else if (ddd_collection_rollback_v1(&ops, &s_collection) !=
               DDD_PROTOCOL_OK) {
        replug_required = true;
        ddd_v1_lock_active_path("readiness rollback failed");
    }
    ddd_v1_close();
    if (restart_required || gui_ddd_async_global_quarantine_active()) {
        gui_app_set_status(app, "DdD USB callbacks unverified; restart MISRC");
    } else {
        gui_app_set_status(app, replug_required
            ? "DdD stream cleanup failed; unplug and reconnect it"
            : "DdD stream did not become ready");
    }
    return -1;
}

void gui_ddd_v1_stop(gui_app_t *app)
{
    ddd_control_ops_t ops;
    bool unsafe = false;

    if (!app || (!s_handle && !atomic_load(&app->ddd_running))) return;
    atomic_store(&s_queue_ready, false);
    atomic_store(&app->ddd_running, false);
    if (app->ddd_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)app->ddd_thread;
        thrd_join(thread, NULL);
        app->ddd_thread = NULL;
    }
    app->is_capturing = false;
    if (app->display_thread) gui_display_thread_stop(app->display_thread);
    gui_extract_stop();
    if (!ddd_v1_sync_control_allowed()) {
        unsafe = true;
        ddd_v1_lock_active_path("callbacks remained unreaped at stop");
    } else {
        ops = ddd_v1_control_ops();
        if (ddd_collection_stop_v1(&ops, &s_collection) != DDD_PROTOCOL_OK &&
            ddd_collection_rollback_v1(&ops, &s_collection) !=
                DDD_PROTOCOL_OK) {
            unsafe = true;
            ddd_v1_lock_active_path("B5 stop and rollback failed");
        }
    }
    fprintf(stderr, "[DdD 3.1] Capture stopped: %s\n",
            ddd_v1_result_name(s_result));
    ddd_v1_close();
    atomic_store(&app->stream_synced, false);
    if (gui_ddd_async_global_quarantine_active()) {
        gui_app_set_status(app, "DdD USB callbacks unverified; restart MISRC");
    } else {
        gui_app_set_status(app, unsafe
            ? "DdD stop unverified; unplug and reconnect it"
            : "DdD capture stopped");
    }
}

bool gui_ddd_v1_is_active(void)
{
    return s_handle != NULL || s_usb_path[0] != '\0';
}

#endif /* ENABLE_DDD */
