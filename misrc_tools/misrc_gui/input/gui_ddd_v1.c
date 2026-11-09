/* MISRC GUI - isolated Domesday Duplicator firmware 3.1 backend. */

#ifdef ENABLE_DDD

#include <inttypes.h>
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
#ifdef ENABLE_DDD_WINUSB
#include "gui_ddd_winusb_device.h"
#include "gui_ddd_winusb_io.h"
#endif
#include "../core/gui_app.h"
#include "../output/gui_record.h"
#include "../processing/gui_display_thread.h"
#include "../processing/gui_extract.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#define DDD_V1_CONTROL_TIMEOUT_MS 1000
#define DDD_V1_STARTUP_DISCARD_BYTES (UINT64_C(8) * 1024u * 1024u)
#ifndef MISRC_DDD_DIAGNOSTIC_FIFO_INTERVAL_MS
#define MISRC_DDD_DIAGNOSTIC_FIFO_INTERVAL_MS GUI_DDD_ASYNC_TELEMETRY_INTERVAL_MS
#endif
#ifndef MISRC_DDD_DIAGNOSTIC_FAILURE_TAIL_WAIT_MS
#define MISRC_DDD_DIAGNOSTIC_FAILURE_TAIL_WAIT_MS 0u
#endif

typedef enum {
    DDD_V1_RESULT_RUNNING = 0,
    DDD_V1_RESULT_SUCCESS,
    DDD_V1_RESULT_USB_FAILURE,
    DDD_V1_RESULT_RAW_IO_UNAVAILABLE,
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
    uint64_t failure_ms;
    size_t failure_block_offset;
    unsigned inspected_tail_blocks;
} ddd_v1_consumer_t;

typedef struct {
    ddd_fifo_telemetry_t latest;
    uint64_t received_ms;
} ddd_v1_fifo_observation_t;

static libusb_context *s_context;
static libusb_device_handle *s_handle;
#ifdef ENABLE_DDD_WINUSB
static gui_ddd_winusb_device_t *s_winusb_device;
static gui_ddd_winusb_io_t *s_winusb_io;
#endif
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

/* Firmware 3.2 marker boundaries move through USB transfers. The validator
 * carries their position across callbacks; only USB packets must align. */
_Static_assert(GUI_DDD_ASYNC_TRANSFER_BYTES % DDD_STREAM_MAX_PACKET_SIZE == 0,
               "DdD transfers must contain whole USB packets");
_Static_assert(DDD_V1_STARTUP_DISCARD_BYTES %
                   GUI_DDD_ASYNC_TRANSFER_BYTES == 0,
               "DDD 3.1 startup discard must contain whole transfers");

static const char *ddd_v1_result_name(ddd_v1_capture_result_t result)
{
    switch (result) {
        case DDD_V1_RESULT_RUNNING: return "Running";
        case DDD_V1_RESULT_SUCCESS: return "Success";
        case DDD_V1_RESULT_USB_FAILURE: return "UsbFailure";
        case DDD_V1_RESULT_RAW_IO_UNAVAILABLE: return "RawIoUnavailable";
        case DDD_V1_RESULT_SEQUENCE_FAILURE: return "SequenceFailure";
        case DDD_V1_RESULT_TEST_FAILURE: return "TestFailure";
        case DDD_V1_RESULT_BACKPRESSURE: return "Backpressure";
    }
    return "Unknown";
}

static bool ddd_v1_uses_winusb(void)
{
#ifdef ENABLE_DDD_WINUSB
    return s_winusb_device != NULL;
#else
    return false;
#endif
}

static const char *ddd_v1_backend_name(void)
{
    return ddd_v1_uses_winusb() ? "WinUSB" : "libusb";
}

static bool ddd_v1_has_device(void)
{
    return s_handle != NULL || ddd_v1_uses_winusb();
}

static bool ddd_v1_libusb_raw_io_requested(const gui_app_t *app)
{
#ifdef ENABLE_DDD_WINUSB
    return s_handle != NULL && app->settings.ddd_libusb_raw_io;
#else
    (void)app;
    return false;
#endif
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
    ddd_v1_fifo_observation_t *observation = context;
    ddd_fifo_telemetry_t latest;

    if (ddd_fifo_telemetry_parse(data, size, &latest)) {
        if (observation) {
            observation->latest = latest;
            observation->received_ms = get_time_us() / UINT64_C(1000);
        }
        (void)ddd_fifo_telemetry_totals_add(&s_fifo_totals, &latest);
    }
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
#ifdef ENABLE_DDD_WINUSB
    if (s_winusb_device) {
        return (ddd_control_ops_t){
            .transfer = gui_ddd_winusb_device_control,
            .context = s_winusb_device,
        };
    }
#endif
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
#ifdef ENABLE_DDD_WINUSB
    if (!retain_usb) {
        gui_ddd_winusb_io_destroy(s_winusb_io);
        gui_ddd_winusb_device_close(s_winusb_device);
    }
    s_winusb_io = NULL;
    s_winusb_device = NULL;
#endif
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
#ifdef ENABLE_DDD_WINUSB
    if (app->settings.ddd_use_winusb) {
        const unsigned long instance = libusb_get_session_data(selected);
        char message[256] = {0};
        libusb_free_device_list(devices, 1);
        libusb_exit(s_context);
        s_context = NULL;
        if (!instance) {
            gui_app_set_status(app, "Native WinUSB could not identify the selected DdD");
            return -1;
        }
        s_winusb_device = gui_ddd_winusb_device_open(instance, message, sizeof(message));
        if (!s_winusb_device) {
            fprintf(stderr, "[DdD 3.1] WinUSB open failed for %s: %s\n",
                    stable_usb_path, message);
            gui_app_set_status(app, message[0] ? message : "Failed to open DdD with WinUSB");
            return -1;
        }
        s_winusb_io = gui_ddd_winusb_io_create(
            gui_ddd_winusb_device_file_handle(s_winusb_device),
            gui_ddd_winusb_device_interface_handle(s_winusb_device));
        if (!s_winusb_io) {
            gui_app_set_status(app, "Failed to prepare native WinUSB transfers");
            ddd_v1_close();
            return -1;
        }
        snprintf(s_usb_path, sizeof(s_usb_path), "%s", stable_usb_path);
        fprintf(stderr, "[DdD 3.1] Opened exact device path %s (backend=WinUSB)\n", s_usb_path);
        return 0;
    }
#endif
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
    fprintf(stderr, "[DdD 3.1] Opened exact device path %s (backend=libusb)\n", s_usb_path);
    return 0;
}

static gui_ddd_async_consume_result_t ddd_v1_fail(
    gui_app_t *app,
    ddd_v1_capture_result_t result,
    gui_dropout_reason_t reason,
    const char *message)
{
    if (app) {
        gui_record_mark_source_loss();
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

static gui_ddd_async_consume_result_t ddd_v1_consumer_fail(
    ddd_v1_consumer_t *consumer, ddd_v1_capture_result_t result,
    gui_dropout_reason_t reason, const char *message)
{
    /* Record detection before the existing error log can block on I/O. */
    if (consumer) consumer->failure_ms = get_time_us() / UINT64_C(1000);
    return ddd_v1_fail(consumer ? consumer->app : NULL, result, reason, message);
}

static void ddd_v1_log_diagnostic(gui_app_t *app, const char *message)
{
    char line[1024];
    int length = snprintf(line, sizeof(line), "[DdD diagnostic] %s\n", message);

    if (length < 0 || (size_t)length >= sizeof(line)) return;
    (void)fwrite(line, 1, (size_t)length, stderr);
    gui_record_log_capture_event(app, "INFO", line, GUI_ERROR_CLASS_NONE, 0);
}

static void ddd_v1_log_sequence_runs(gui_app_t *app, const char *side,
                                     const ddd_sequence_run_t *runs,
                                     size_t count)
{
    char message[256];

    for (size_t i = 0; i < count; ++i) {
        const ddd_sequence_run_t *run = &runs[i];
        snprintf(message, sizeof(message),
                 "sequence side=%s index=%zu marker=%u start_sample=%" PRIu64
                 " observed_samples=%" PRIu32 " start_observed=%u end_observed=%u",
                 side, i, (unsigned)run->marker, run->start_sample_index,
                 run->sample_count, (unsigned)run->start_observed,
                 (unsigned)run->end_observed);
        ddd_v1_log_diagnostic(app, message);
    }
}

static void ddd_v1_log_sequence_diagnostic(const ddd_v1_consumer_t *consumer)
{
    char message[320];

    snprintf(message, sizeof(message),
             "sequence fault_sample=%" PRIu64 " offset_in_usb_block_samples=%zu"
             " learned_samples_per_marker=%" PRIu32 " phase=%s expected=%u observed=%u"
             " before_runs=%zu after_runs=%zu inspected_following_usb_blocks=%u"
             " following_usb_wait_budget_ms=%" PRIu64,
             s_sequence.error_sample_index, consumer->failure_block_offset,
             s_sequence.samples_per_marker,
             s_sequence.failure_phase == DDD_SEQUENCE_RUNNING ? "running" : "synchronizing",
             (unsigned)s_sequence.expected_marker, (unsigned)s_sequence.actual_marker,
             (size_t)s_sequence.before_count, (size_t)s_sequence.after_count,
             consumer->inspected_tail_blocks,
             (uint64_t)MISRC_DDD_DIAGNOSTIC_FAILURE_TAIL_WAIT_MS);
    ddd_v1_log_diagnostic(consumer->app, message);
    ddd_v1_log_sequence_runs(consumer->app, "before", s_sequence.before,
                             s_sequence.before_count);
    ddd_v1_log_sequence_runs(consumer->app, "after", s_sequence.after,
                             s_sequence.after_count);
}

static void ddd_v1_log_fifo_diagnostic(
    gui_app_t *app, const char *source, uint64_t fault_ms,
    const ddd_v1_fifo_observation_t *observation,
    const ddd_fifo_telemetry_totals_t *totals)
{
    const ddd_fifo_telemetry_t *fifo = &observation->latest;
    char message[768];

    if (!fifo->present) {
        snprintf(message, sizeof(message), "FIFO source=%s snapshot=unavailable", source);
    } else {
        int64_t offset_ms = (int64_t)observation->received_ms - (int64_t)fault_ms;
        snprintf(message, sizeof(message),
                 "FIFO source=%s fault_offset_ms=%" PRId64
                 " latch=%u used_words=%u depth_words=%u peak_words=%u open_peak_words=%u"
                 " interval_overflows=%u interval_dropped_words=%u interval_packets=%u"
                 " interval_near_full_units=%u near_full_words=%u packet_words=%u"
                 " overflow_seen_since_fpga_reset=%u saturated=%u",
                 source, offset_ms, (unsigned)fifo->latch_count,
                 (unsigned)fifo->used_now, (unsigned)fifo->depth_words,
                 (unsigned)fifo->peak, (unsigned)fifo->peak_since_open,
                 (unsigned)fifo->overflow_events, (unsigned)fifo->dropped_words,
                 (unsigned)fifo->packets_read, (unsigned)fifo->near_full_units,
                 (unsigned)fifo->near_full_words, (unsigned)fifo->packet_words,
                 (unsigned)fifo->overflow_seen, (unsigned)fifo->saturated);
    }
    ddd_v1_log_diagnostic(app, message);

    if (totals && totals->latch_seen) {
        snprintf(message, sizeof(message),
                 "FIFO source=%s total_overflows=%" PRIu64
                 " total_dropped_words=%" PRIu64 " total_near_full_units=%" PRIu64
                 " run_peak_words=%u interval_coverage_complete=%u totals_saturated=%u",
                 source, totals->overflow_events, totals->dropped_words,
                 totals->near_full_units, (unsigned)totals->peak_words,
                 (unsigned)totals->interval_coverage_complete,
                 (unsigned)totals->saturated);
        ddd_v1_log_diagnostic(app, message);
    }
}

static void ddd_v1_log_usb_diagnostic(gui_app_t *app, uint64_t fault_ms,
                                      bool libusb_raw_io_requested,
                                      const gui_ddd_async_result_t *result)
{
    const gui_ddd_async_failure_snapshot_t *fault = &result->failure;
    const struct libusb_version *usb_version = libusb_get_version();
    unsigned long win32_error = 0;
    bool native_raw_io = ddd_v1_uses_winusb();
    char message[976];

#ifdef ENABLE_DDD_WINUSB
    if (s_winusb_io) win32_error = gui_ddd_winusb_io_first_error(s_winusb_io);
#endif

    snprintf(message, sizeof(message),
             "USB path=%s backend=%s capture_result=%s queue_result=%s sample_rate_hz=%" PRIu32
             " raw_io_requested=%s raw_io_active=%s raw_io_stage=%s"
             " raw_io_packet_bytes=%d raw_io_max_transfer_bytes=%d"
             " libusb_version=%u.%u.%u.%u"
             " fifo_poll_interval_ms=%" PRIu64
             " fault_snapshot=%u fault_tick_ms=%" PRIu64 " queue_tick_ms=%" PRIu64
             " libusb_error=%d win32_error=%lu"
             " transfer_status=%s(%d) actual_bytes=%d expected_bytes=%zu"
             " submission_id=%" PRIu64 " next_consume_id=%" PRIu64
             " next_submit_id=%" PRIu64 " in_flight=%zu ready_transfers=%zu"
             " completed=%" PRIu64 " consumed=%" PRIu64
             " fifo_reads=%" PRIu64 " fifo_read_failures=%u fifo_in_flight=%u"
             " unreaped_after_cleanup=%zu active_callbacks_after_cleanup=%zu",
             s_usb_path, ddd_v1_backend_name(), ddd_v1_result_name(s_result),
             gui_ddd_async_result_name(result->code), s_sample_rate_hz,
             (native_raw_io || libusb_raw_io_requested) ? "on" : "off",
             (native_raw_io || result->libusb_raw_io_enabled) ? "on" : "off",
             result->libusb_raw_io_stage ? result->libusb_raw_io_stage : "none",
             result->libusb_raw_io_packet_size, result->libusb_raw_io_max_transfer_size,
             (unsigned)usb_version->major, (unsigned)usb_version->minor,
             (unsigned)usb_version->micro, (unsigned)usb_version->nano,
             (uint64_t)MISRC_DDD_DIAGNOSTIC_FIFO_INTERVAL_MS,
             (unsigned)fault->valid, fault_ms, fault->time_ms,
             result->libusb_error, win32_error,
             gui_ddd_async_transfer_status_name(result->transfer_status),
             result->transfer_status, result->actual_length,
             GUI_DDD_ASYNC_TRANSFER_BYTES, result->submission_id,
             fault->next_consume_id, fault->next_submit_id, fault->in_flight,
             fault->ready_transfers, fault->completed_transfers,
             fault->consumed_transfers, fault->telemetry_readings,
             fault->telemetry_failures, (unsigned)fault->telemetry_in_flight,
             result->unreaped_transfers, result->active_callbacks);
    ddd_v1_log_diagnostic(app, message);
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
        return ddd_v1_consumer_fail(consumer, DDD_V1_RESULT_USB_FAILURE,
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
    uint64_t block_start = s_sequence.samples_seen;
    if (ddd_sequence_validator_feed(&s_sequence, input, sample_count) !=
        DDD_VALIDATION_OK) {
        char message[192];
        consumer->failure_block_offset = (size_t)(s_sequence.error_sample_index - block_start);
        snprintf(message, sizeof(message),
                 "Sequence mismatch at sample %llu (expected %u, got %u)",
                 (unsigned long long)s_sequence.error_sample_index,
                 (unsigned)s_sequence.expected_marker,
                 (unsigned)s_sequence.actual_marker);
        atomic_fetch_add(&app->missed_frame_count, 1);
        return ddd_v1_consumer_fail(consumer, DDD_V1_RESULT_SEQUENCE_FAILURE,
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
        return ddd_v1_consumer_fail(consumer, DDD_V1_RESULT_TEST_FAILURE,
                                    GUI_DROPOUT_FRAME_ERROR, message);
    }
    output_size = sample_count * sizeof(*output);
    output = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                            output_size, NULL);
    if (!output) {
        atomic_fetch_add(&app->rb_drop_count, 1);
        return ddd_v1_consumer_fail(consumer, DDD_V1_RESULT_BACKPRESSURE,
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

static bool ddd_v1_inspect_failed_tail(void *context, const uint8_t *data, size_t size)
{
    ddd_v1_consumer_t *consumer = context;

    if (s_result != DDD_V1_RESULT_SEQUENCE_FAILURE ||
        !ddd_sequence_validator_extend_failure_history(&s_sequence, NULL, 0)) return false;
    if (size == 0) return true;
    ++consumer->inspected_tail_blocks;
    return ddd_sequence_validator_extend_failure_history(
        &s_sequence, (const uint16_t *)data, size / sizeof(uint16_t));
}

static int ddd_v1_capture_thread(void *context)
{
    gui_app_t *app = (gui_app_t *)context;
    ddd_v1_consumer_t consumer = {.app = app};
    ddd_v1_fifo_observation_t fifo_before_fault = {0};
    gui_ddd_async_config_t config = {
        .usb_context = s_context,
        .device_handle = s_handle,
        .endpoint = DDD_STREAM_ENDPOINT_ADDRESS,
        .libusb_raw_io = ddd_v1_libusb_raw_io_requested(app),
        .capture_running = &app->ddd_running,
        .transfer_ready = &s_queue_ready,
        .startup_failed = &s_startup_failed,
        .consume = ddd_v1_consume,
        .consume_context = &consumer,
        .inspect_failed_tail = ddd_v1_inspect_failed_tail,
        .failure_tail_wait_ms = MISRC_DDD_DIAGNOSTIC_FAILURE_TAIL_WAIT_MS,
        .telemetry = ddd_v1_fifo_receive,
        .telemetry_context = &fifo_before_fault,
        .telemetry_interval_ms = MISRC_DDD_DIAGNOSTIC_FIFO_INTERVAL_MS,
    };
    gui_ddd_async_result_t async_result = {0};
    int result;

#ifdef ENABLE_DDD_WINUSB
    if (s_winusb_io) gui_ddd_winusb_io_attach(s_winusb_io, &config);
#endif
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
            s_result = async_result.code == GUI_DDD_ASYNC_RESULT_RAW_IO_UNAVAILABLE
                ? DDD_V1_RESULT_RAW_IO_UNAVAILABLE : DDD_V1_RESULT_USB_FAILURE;
            fprintf(stderr, "[DdD 3.1] %s\n", message);
        }
    }
    if (result < 0) {
        uint64_t fault_ms = consumer.failure_ms ? consumer.failure_ms :
            async_result.failure.time_ms;
        ddd_v1_log_usb_diagnostic(app, fault_ms, config.libusb_raw_io, &async_result);
        if (s_result == DDD_V1_RESULT_SEQUENCE_FAILURE) {
            ddd_v1_log_sequence_diagnostic(&consumer);
        }
        ddd_v1_log_fifo_diagnostic(app, "last_poll", fault_ms,
                                   &fifo_before_fault, &s_fifo_totals);
        if (async_result.fifo_completion_after_fault.valid) {
            const ddd_v1_fifo_observation_t crossing_fault = {
                .latest = async_result.fifo_completion_after_fault.telemetry,
                .received_ms = async_result.fifo_completion_after_fault.completed_ms,
            };
            char message[160];
            snprintf(message, sizeof(message),
                     "FIFO source=inflight_at_fault submit_offset_ms=%" PRId64
                     " timing=host_completion_not_device_latch",
                     (int64_t)async_result.fifo_completion_after_fault.submitted_ms -
                     (int64_t)fault_ms);
            ddd_v1_log_diagnostic(app, message);
            ddd_v1_log_fifo_diagnostic(app, "inflight_at_fault", fault_ms,
                                       &crossing_fault, NULL);
        } else if (async_result.failure.telemetry_in_flight) {
            ddd_v1_log_diagnostic(app,
                "FIFO source=inflight_at_fault snapshot=unavailable");
        }
        fflush(stderr);
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
    if (!ddd_v1_has_device()) {
        gui_app_set_status(app, "DdD device is not open");
        return -1;
    }
    if (!ddd_decimation_is_supported(decimation)) {
        gui_app_set_status(app, "Invalid DdD hardware rate");
        return -1;
    }
#ifdef MISRC_DDD_AB_DIAGNOSTIC
    if (decimation != DDD_DECIMATION_HALF_RATE) {
        ddd_v1_close();
        gui_app_set_status(app, "DdD A/B diagnostic requires RF ChA at 20 MSPS");
        return -1;
    }
#endif
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
            char message[512];
            char commit[DDD_COMMIT_LENGTH + 7] = {0};
            const struct libusb_version *usb_version = libusb_get_version();
            /* Both backends apply a requested RAW_IO policy before the queue
             * can signal ready. An unsupported request fails startup. */
            const char *raw_io = ddd_v1_uses_winusb() || ddd_v1_libusb_raw_io_requested(app)
                ? "on" : "off";
            (void)ddd_format_gateware_commit(
                s_collection.identity, sizeof(s_collection.identity),
                commit, sizeof(commit));
            snprintf(message, sizeof(message),
                     "DdD protocol v1 capture started path=%s backend=%s sample_rate_hz=%" PRIu32
                     " usb_transfers=%zu transfer_bytes=%zu queue_bytes=%zu fifo_poll_interval_ms=%" PRIu64
                     " raw_io_requested=%s raw_io_active=%s libusb_version=%u.%u.%u.%u test=%s gateware=%s",
                     s_usb_path, ddd_v1_backend_name(), s_sample_rate_hz,
                     GUI_DDD_ASYNC_TRANSFER_COUNT, GUI_DDD_ASYNC_TRANSFER_BYTES,
                     GUI_DDD_ASYNC_QUEUE_BYTES, (uint64_t)MISRC_DDD_DIAGNOSTIC_FIFO_INTERVAL_MS,
                     raw_io, raw_io, (unsigned)usb_version->major, (unsigned)usb_version->minor,
                     (unsigned)usb_version->micro, (unsigned)usb_version->nano,
                     test_mode ? "on" : "off", commit[0] ? commit : "n/a");
            ddd_v1_log_diagnostic(app, message);
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
    } else if (!replug_required && s_result == DDD_V1_RESULT_RAW_IO_UNAVAILABLE) {
        gui_app_set_status(app, "DdD libusb RAW_IO unavailable; turn it off and reconnect");
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

    if (!app || (!ddd_v1_has_device() && !atomic_load(&app->ddd_running))) return;
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
    return ddd_v1_has_device() || s_usb_path[0] != '\0';
}

#endif /* ENABLE_DDD */
