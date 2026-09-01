/*
 * MISRC GUI - DDD firmware 3.1 asynchronous USB capture queue
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "../../common/libusb_compat.h"
#include "../../common/ddd_protocol.h"
#include "../../common/threading.h"
#include "gui_ddd_async.h"

#define GUI_DDD_ASYNC_EVENT_POLL_US 10000L

_Static_assert(GUI_DDD_ASYNC_TRANSFER_COUNT == 96,
               "DDD 3.1 queue geometry must remain 96 x 128 KiB");
_Static_assert(GUI_DDD_ASYNC_QUEUE_BYTES ==
                   GUI_DDD_ASYNC_TRANSFER_COUNT *
                       GUI_DDD_ASYNC_TRANSFER_BYTES,
               "DDD 3.1 queue geometry must divide exactly");
_Static_assert(GUI_DDD_ASYNC_ABANDONED_CAPACITY == 1,
               "A process may retain only one unreaped DDD queue");

typedef struct gui_ddd_async_orphan gui_ddd_async_engine_t;

typedef struct {
    gui_ddd_async_engine_t *owner;
    size_t slot_index;
    struct libusb_transfer *transfer;
    uint8_t *buffer;
    int actual_length;
    bool cancel_requested;
} gui_ddd_async_slot_t;

struct gui_ddd_async_orphan {
    gui_ddd_async_config_t config_storage;
    const gui_ddd_async_config_t *config;
    gui_ddd_async_slot_t *slots;
    gui_ddd_async_policy_slot_t policy_slots[GUI_DDD_ASYNC_TRANSFER_COUNT];
    uint8_t *buffer_pool;
    gui_ddd_async_order_policy_t order;
    gui_ddd_async_result_t result;
    uint64_t next_submit_id;
    size_t submitted_count;
    atomic_size_t in_flight;
    atomic_size_t callbacks_active;
    struct libusb_transfer *telemetry_transfer;
    uint8_t *telemetry_buffer;
    atomic_bool telemetry_in_flight;
    uint64_t telemetry_due_ms;
    unsigned telemetry_failures;
    bool telemetry_primed;
    bool telemetry_cancel_requested;
    uint64_t cancel_deadline_ms;
    bool accepting_submissions;
    bool stopping;
    bool cancel_issued;
    bool failed;
    bool abandoned;
};

/* Deliberate process-lifetime ownership for a backend that never returns its
 * cancellation callbacks. This is bounded (at most one entry per quarantined
 * capture attempt) and prevents UAF of libusb transfer/user-data storage. */
static gui_ddd_async_engine_t *s_ddd_async_abandoned = NULL;

static uint64_t gui_ddd_async_now_ms(gui_ddd_async_engine_t *engine)
{
    if (engine && engine->config && engine->config->now_ms_override) {
        return engine->config->now_ms_override(
            engine->config->now_ms_context);
    }
    /* get_time_ms() uses 32-bit GetTickCount on Windows and wraps after
     * 49.7 days. get_time_us() uses GetTickCount64 there and is monotonic on
     * every supported platform, so the reap deadline remains truly bounded. */
    return get_time_us() / UINT64_C(1000);
}

static bool gui_ddd_async_engine_has_pending(
    const gui_ddd_async_engine_t *engine)
{
    if (!engine) return false;
    return gui_ddd_async_policy_has_pending(
        atomic_load(&engine->in_flight) +
            (atomic_load(&engine->telemetry_in_flight) ? 1u : 0u),
        atomic_load(&engine->callbacks_active));
}

static void gui_ddd_async_signal_failure(gui_ddd_async_engine_t *engine)
{
    if (!engine || !engine->config) return;
    if (engine->config->transfer_ready) {
        atomic_store(engine->config->transfer_ready, false);
    }
    if (engine->config->startup_failed) {
        atomic_store(engine->config->startup_failed, true);
    }
}

static void gui_ddd_async_latch_failure(gui_ddd_async_engine_t *engine,
                                        gui_ddd_async_result_code_t code,
                                        int libusb_error,
                                        int transfer_status,
                                        int actual_length,
                                        uint64_t submission_id)
{
    if (!engine || engine->failed) return;
    engine->failed = true;
    engine->accepting_submissions = false;
    engine->result.code = code;
    engine->result.libusb_error = libusb_error;
    engine->result.transfer_status = transfer_status;
    engine->result.actual_length = actual_length;
    engine->result.submission_id = submission_id;
    gui_ddd_async_signal_failure(engine);
}

static void LIBUSB_CALL gui_ddd_async_transfer_callback(
    struct libusb_transfer *transfer)
{
    gui_ddd_async_slot_t *slot;
    gui_ddd_async_engine_t *engine;
    gui_ddd_async_policy_slot_t *policy_slot;

    if (!transfer || !transfer->user_data) return;
    slot = (gui_ddd_async_slot_t *)transfer->user_data;
    engine = slot->owner;
    if (!engine) return;
    atomic_fetch_add(&engine->callbacks_active, 1);
    policy_slot = &engine->policy_slots[slot->slot_index];

    if (policy_slot->state != GUI_DDD_ASYNC_SLOT_SUBMITTED ||
        atomic_load(&engine->in_flight) == 0) {
        gui_ddd_async_latch_failure(
            engine, GUI_DDD_ASYNC_RESULT_ORDER_FAILURE, 0,
            (int)transfer->status, transfer->actual_length,
            policy_slot->submission_id);
        policy_slot->state = GUI_DDD_ASYNC_SLOT_FAILED;
        atomic_fetch_sub(&engine->callbacks_active, 1);
        return;
    }

    engine->result.completed_transfers++;
    slot->actual_length = transfer->actual_length;

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (!gui_ddd_async_policy_exact_length(
                (size_t)transfer->actual_length)) {
            policy_slot->state = GUI_DDD_ASYNC_SLOT_FAILED;
            gui_ddd_async_latch_failure(
                engine, GUI_DDD_ASYNC_RESULT_SHORT_TRANSFER, 0,
                (int)transfer->status, transfer->actual_length,
                policy_slot->submission_id);
            atomic_fetch_sub(&engine->in_flight, 1);
            atomic_fetch_sub(&engine->callbacks_active, 1);
            return;
        }
        policy_slot->state = GUI_DDD_ASYNC_SLOT_COMPLETE;
        atomic_fetch_sub(&engine->in_flight, 1);
        atomic_fetch_sub(&engine->callbacks_active, 1);
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_CANCELLED &&
        (slot->cancel_requested || engine->failed)) {
        policy_slot->state = GUI_DDD_ASYNC_SLOT_CANCELLED;
        atomic_fetch_sub(&engine->in_flight, 1);
        atomic_fetch_sub(&engine->callbacks_active, 1);
        return;
    }

    policy_slot->state = GUI_DDD_ASYNC_SLOT_FAILED;
    gui_ddd_async_latch_failure(
        engine, GUI_DDD_ASYNC_RESULT_TRANSFER_FAILURE, 0,
        (int)transfer->status, transfer->actual_length,
        policy_slot->submission_id);
    atomic_fetch_sub(&engine->in_flight, 1);
    atomic_fetch_sub(&engine->callbacks_active, 1);
}

static int gui_ddd_async_submit_slot(gui_ddd_async_engine_t *engine,
                                     gui_ddd_async_slot_t *slot)
{
    gui_ddd_async_policy_slot_t *policy_slot =
        &engine->policy_slots[slot->slot_index];
    int rc;

    policy_slot->submission_id = engine->next_submit_id++;
    policy_slot->state = GUI_DDD_ASYNC_SLOT_SUBMITTED;
    slot->actual_length = 0;
    slot->cancel_requested = false;
    libusb_fill_bulk_transfer(slot->transfer,
                             engine->config->device_handle,
                             engine->config->endpoint,
                             slot->buffer,
                             (int)GUI_DDD_ASYNC_TRANSFER_BYTES,
                             gui_ddd_async_transfer_callback,
                             slot,
                             0);
    slot->transfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK;

    if (engine->config->submit_override) {
        rc = engine->config->submit_override(
            engine->config->submit_context, slot->transfer);
    } else {
        rc = libusb_submit_transfer(slot->transfer);
    }
    if (rc < 0) {
        policy_slot->state = GUI_DDD_ASYNC_SLOT_FAILED;
        gui_ddd_async_latch_failure(
            engine, GUI_DDD_ASYNC_RESULT_SUBMIT_FAILURE, rc, 0, 0,
            policy_slot->submission_id);
        return -1;
    }

    engine->submitted_count++;
    atomic_fetch_add(&engine->in_flight, 1);
    return 0;
}

static void gui_ddd_async_note_telemetry_failure(
    gui_ddd_async_engine_t *engine)
{
    if (!engine) return;
    if (engine->telemetry_failures < UINT_MAX) {
        engine->telemetry_failures++;
    }
    if (engine->result.telemetry_failures < UINT_MAX) {
        engine->result.telemetry_failures++;
    }
    if (engine->telemetry_failures ==
            GUI_DDD_ASYNC_TELEMETRY_MAX_FAILURES &&
        !engine->stopping && engine->config->telemetry) {
        /* Remove a stale instrument after the bounded retry budget. The
         * backend callback is deliberately parse/publish-only, so this cannot
         * stall the USB event pump. */
        engine->config->telemetry(engine->config->telemetry_context, NULL, 0);
    }
}

static void LIBUSB_CALL gui_ddd_async_telemetry_callback(
    struct libusb_transfer *transfer)
{
    gui_ddd_async_engine_t *engine;
    ddd_fifo_telemetry_t validated;
    const uint8_t *data;

    if (!transfer || !transfer->user_data) return;
    engine = (gui_ddd_async_engine_t *)transfer->user_data;
    atomic_fetch_add(&engine->callbacks_active, 1);
    atomic_store(&engine->telemetry_in_flight, false);

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->actual_length < (int)DDD_FIFO_TELEMETRY_LENGTH) {
            gui_ddd_async_note_telemetry_failure(engine);
        } else {
            data = libusb_control_transfer_get_data(transfer);
            if (!ddd_fifo_telemetry_parse(
                    data, (size_t)transfer->actual_length, &validated)) {
                /* Older or incompatible protocol-v1 firmware may return a
                 * full-length placeholder for an unknown register. Count it
                 * against the same bounded retry budget as a refused read. */
                gui_ddd_async_note_telemetry_failure(engine);
            } else {
                engine->result.telemetry_readings++;
                if (!engine->stopping && engine->config->telemetry &&
                    gui_ddd_async_policy_telemetry_should_publish(
                        &engine->telemetry_primed)) {
                    engine->config->telemetry(
                        engine->config->telemetry_context,
                        data, DDD_FIFO_TELEMETRY_LENGTH);
                }
            }
        }
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        /* Telemetry is diagnostic. A refusal disables only the instrument
         * after the bounded retry count; it never fails the RF queue. */
        gui_ddd_async_note_telemetry_failure(engine);
    }

    atomic_fetch_sub(&engine->callbacks_active, 1);
}

static void gui_ddd_async_maybe_submit_telemetry(
    gui_ddd_async_engine_t *engine,
    uint64_t now_ms)
{
    int rc;
    bool enabled;

    if (!engine) return;
    enabled = engine->config->telemetry != NULL &&
              engine->telemetry_transfer != NULL &&
              engine->telemetry_buffer != NULL &&
              !engine->stopping && !engine->failed;
    if (!gui_ddd_async_policy_telemetry_should_submit(
            enabled,
            atomic_load(&engine->telemetry_in_flight),
            engine->telemetry_failures,
            now_ms,
            engine->telemetry_due_ms)) {
        return;
    }
    engine->telemetry_due_ms =
        now_ms + GUI_DDD_ASYNC_TELEMETRY_INTERVAL_MS;
    engine->telemetry_cancel_requested = false;

    libusb_fill_control_setup(
        engine->telemetry_buffer,
        DDD_USB_REQUEST_VENDOR_IN,
        DDD_REQUEST_REGISTER_READ,
        DDD_REGISTER_FIFO_TELEMETRY,
        0,
        DDD_FIFO_TELEMETRY_LENGTH);
    libusb_fill_control_transfer(
        engine->telemetry_transfer,
        engine->config->device_handle,
        engine->telemetry_buffer,
        gui_ddd_async_telemetry_callback,
        engine,
        GUI_DDD_ASYNC_TELEMETRY_TIMEOUT_MS);

    if (engine->config->submit_override) {
        rc = engine->config->submit_override(
            engine->config->submit_context,
            engine->telemetry_transfer);
    } else {
        rc = libusb_submit_transfer(engine->telemetry_transfer);
    }
    if (rc < 0) {
        gui_ddd_async_note_telemetry_failure(engine);
        return;
    }
    atomic_store(&engine->telemetry_in_flight, true);
}

static void gui_ddd_async_cancel_telemetry(
    gui_ddd_async_engine_t *engine)
{
    int rc;

    if (!engine || !atomic_load(&engine->telemetry_in_flight) ||
        engine->telemetry_cancel_requested) {
        return;
    }
    engine->telemetry_cancel_requested = true;
    if (engine->config->cancel_override) {
        rc = engine->config->cancel_override(
            engine->config->cancel_context,
            engine->telemetry_transfer);
    } else {
        rc = libusb_cancel_transfer(engine->telemetry_transfer);
    }
    /* NOT_FOUND means completion won the race. Other errors remain bounded by
     * the same orphan deadline as the RF transfers. */
    (void)rc;
}

static void gui_ddd_async_cancel_in_flight(gui_ddd_async_engine_t *engine)
{
    size_t i;

    if (!engine || engine->cancel_issued) return;
    engine->cancel_issued = true;
    engine->cancel_deadline_ms = gui_ddd_async_now_ms(engine) +
        GUI_DDD_ASYNC_CANCEL_REAP_TIMEOUT_MS;
    engine->accepting_submissions = false;
    gui_ddd_async_cancel_telemetry(engine);
    for (i = 0; i < GUI_DDD_ASYNC_TRANSFER_COUNT; i++) {
        gui_ddd_async_slot_t *slot = &engine->slots[i];
        gui_ddd_async_policy_slot_t *policy_slot =
            &engine->policy_slots[i];
        int rc;
        if (policy_slot->state != GUI_DDD_ASYNC_SLOT_SUBMITTED) continue;
        slot->cancel_requested = true;
        if (engine->config->cancel_override) {
            rc = engine->config->cancel_override(
                engine->config->cancel_context, slot->transfer);
        } else {
            rc = libusb_cancel_transfer(slot->transfer);
        }
        if (rc < 0 && rc != LIBUSB_ERROR_NOT_FOUND) {
            /* Preserve the original failure, but keep pumping callbacks. */
            gui_ddd_async_latch_failure(
                engine, GUI_DDD_ASYNC_RESULT_EVENT_FAILURE, rc, 0, 0,
                policy_slot->submission_id);
        }
    }
}

static int gui_ddd_async_consume_ready(gui_ddd_async_engine_t *engine)
{
    for (;;) {
        gui_ddd_async_next_state_t next_state;
        gui_ddd_async_slot_t *slot;
        size_t slot_index = 0;
        bool capture_running =
            atomic_load(engine->config->capture_running);

        if (gui_ddd_async_policy_stop_drain_complete(
                capture_running,
                atomic_load(&engine->in_flight),
                engine->result.completed_transfers,
                engine->result.consumed_transfers)) {
            return 0;
        }

        next_state = gui_ddd_async_order_policy_peek(
            &engine->order,
            engine->policy_slots,
            &slot_index);
        if (next_state == GUI_DDD_ASYNC_NEXT_WAIT) return 0;
        if (next_state == GUI_DDD_ASYNC_NEXT_STALE) {
            gui_ddd_async_latch_failure(
                engine, GUI_DDD_ASYNC_RESULT_ORDER_FAILURE, 0, 0, 0,
                engine->order.next_consume_id);
            return -1;
        }

        slot = &engine->slots[slot_index];
        if (engine->config->consume(
                engine->config->consume_context,
                slot->buffer,
                (size_t)slot->actual_length) !=
            GUI_DDD_ASYNC_CONSUME_CONTINUE) {
            engine->policy_slots[slot_index].state =
                GUI_DDD_ASYNC_SLOT_FAILED;
            gui_ddd_async_latch_failure(
                engine, GUI_DDD_ASYNC_RESULT_CONSUMER_FAILURE, 0, 0,
                slot->actual_length,
                engine->policy_slots[slot_index].submission_id);
            return -1;
        }

        engine->result.consumed_transfers++;
        engine->policy_slots[slot_index].state =
            GUI_DDD_ASYNC_SLOT_RETIRED;
        gui_ddd_async_order_policy_advance(&engine->order);
        capture_running = atomic_load(engine->config->capture_running);
        if (gui_ddd_async_policy_should_resubmit(
                engine->accepting_submissions,
                capture_running,
                engine->failed)) {
            if (gui_ddd_async_submit_slot(engine, slot) < 0) return -1;
        }
    }
}

static int gui_ddd_async_pump_events(gui_ddd_async_engine_t *engine)
{
    struct timeval timeout;
    int rc;

    if (engine->config->event_pump_override) {
        rc = engine->config->event_pump_override(
            engine->config->event_pump_context,
            GUI_DDD_ASYNC_EVENT_POLL_US);
    } else {
        timeout.tv_sec = 0;
        timeout.tv_usec = GUI_DDD_ASYNC_EVENT_POLL_US;
        rc = libusb_handle_events_timeout_completed(
            engine->config->usb_context, &timeout, NULL);
    }
    if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
        gui_ddd_async_latch_failure(
            engine, GUI_DDD_ASYNC_RESULT_EVENT_FAILURE, rc, 0, 0,
            engine->order.next_consume_id);
        return -1;
    }
    return 0;
}

static int gui_ddd_async_allocate(gui_ddd_async_engine_t *engine)
{
    size_t i;

    engine->slots = (gui_ddd_async_slot_t *)calloc(
        GUI_DDD_ASYNC_TRANSFER_COUNT, sizeof(*engine->slots));
    engine->buffer_pool = (uint8_t *)malloc(GUI_DDD_ASYNC_QUEUE_BYTES);
    if (!engine->slots || !engine->buffer_pool) {
        gui_ddd_async_latch_failure(
            engine, GUI_DDD_ASYNC_RESULT_ALLOCATION_FAILURE,
            LIBUSB_ERROR_NO_MEM, 0, 0, 0);
        return -1;
    }

    for (i = 0; i < GUI_DDD_ASYNC_TRANSFER_COUNT; i++) {
        gui_ddd_async_slot_t *slot = &engine->slots[i];
        slot->owner = engine;
        slot->slot_index = i;
        slot->buffer = engine->buffer_pool +
            i * GUI_DDD_ASYNC_TRANSFER_BYTES;
        slot->transfer = libusb_alloc_transfer(0);
        if (!slot->transfer) {
            gui_ddd_async_latch_failure(
                engine, GUI_DDD_ASYNC_RESULT_ALLOCATION_FAILURE,
                LIBUSB_ERROR_NO_MEM, 0, 0, i);
            return -1;
        }
    }

    if (engine->config->telemetry) {
        engine->telemetry_transfer = libusb_alloc_transfer(0);
        engine->telemetry_buffer = (uint8_t *)calloc(
            1, LIBUSB_CONTROL_SETUP_SIZE + DDD_FIFO_TELEMETRY_LENGTH);
        if (!engine->telemetry_transfer || !engine->telemetry_buffer) {
            if (engine->telemetry_transfer) {
                libusb_free_transfer(engine->telemetry_transfer);
            }
            free(engine->telemetry_buffer);
            engine->telemetry_transfer = NULL;
            engine->telemetry_buffer = NULL;
            engine->telemetry_failures =
                GUI_DDD_ASYNC_TELEMETRY_MAX_FAILURES;
            engine->result.telemetry_failures =
                GUI_DDD_ASYNC_TELEMETRY_MAX_FAILURES;
        }
    }
    return 0;
}

static void gui_ddd_async_destroy(gui_ddd_async_engine_t *engine)
{
    size_t i;

    if (!engine) return;
    /* Never release transfer/user-data storage while a callback is pending. */
    if (gui_ddd_async_engine_has_pending(engine)) return;
    if (engine->slots) {
        for (i = 0; i < GUI_DDD_ASYNC_TRANSFER_COUNT; i++) {
            if (engine->slots[i].transfer) {
                libusb_free_transfer(engine->slots[i].transfer);
            }
        }
    }
    free(engine->buffer_pool);
    free(engine->slots);
    if (engine->telemetry_transfer) {
        libusb_free_transfer(engine->telemetry_transfer);
    }
    free(engine->telemetry_buffer);
    engine->buffer_pool = NULL;
    engine->slots = NULL;
    engine->telemetry_transfer = NULL;
    engine->telemetry_buffer = NULL;
    free(engine);
}

int gui_ddd_async_run(const gui_ddd_async_config_t *config,
                      gui_ddd_async_result_t *result)
{
    gui_ddd_async_engine_t *engine;
    gui_ddd_async_result_t local_result;
    uint64_t stop_deadline_ms = 0;
    size_t i;

    memset(&local_result, 0, sizeof(local_result));
    local_result.code = GUI_DDD_ASYNC_RESULT_INVALID_ARGUMENT;

    if (!result || !config || !config->usb_context || !config->device_handle ||
        !config->capture_running || !config->transfer_ready ||
        !config->startup_failed || !config->consume) {
        if (config && config->transfer_ready) {
            atomic_store(config->transfer_ready, false);
        }
        if (config && config->startup_failed) {
            atomic_store(config->startup_failed, true);
        }
        if (result) *result = local_result;
        return -1;
    }

    atomic_store(config->transfer_ready, false);
    atomic_store(config->startup_failed, false);

    engine = (gui_ddd_async_engine_t *)calloc(1, sizeof(*engine));
    if (!engine) {
        local_result.code = GUI_DDD_ASYNC_RESULT_ALLOCATION_FAILURE;
        local_result.libusb_error = LIBUSB_ERROR_NO_MEM;
        atomic_store(config->startup_failed, true);
        *result = local_result;
        return -1;
    }
    engine->config_storage = *config;
    engine->config = &engine->config_storage;
    engine->result.code = GUI_DDD_ASYNC_RESULT_SUCCESS;
    engine->accepting_submissions = true;
    atomic_init(&engine->in_flight, 0);
    atomic_init(&engine->callbacks_active, 0);
    atomic_init(&engine->telemetry_in_flight, false);
    gui_ddd_async_order_policy_init(&engine->order,
                                    GUI_DDD_ASYNC_TRANSFER_COUNT);

    if (gui_ddd_async_allocate(engine) < 0) goto finished;

    for (i = 0; i < GUI_DDD_ASYNC_TRANSFER_COUNT; i++) {
        if (gui_ddd_async_submit_slot(engine, &engine->slots[i]) < 0) {
            break;
        }
    }

    if (gui_ddd_async_policy_initial_queue_ready(engine->submitted_count,
                                                  engine->failed)) {
        engine->result.ready_signalled = true;
        atomic_store(config->transfer_ready, true);
    } else if (!engine->failed) {
        gui_ddd_async_latch_failure(
            engine, GUI_DDD_ASYNC_RESULT_SUBMIT_FAILURE,
            LIBUSB_ERROR_OTHER, 0, 0, engine->next_submit_id);
    }

    if (engine->failed) gui_ddd_async_cancel_in_flight(engine);

    for (;;) {
        bool capture_running = atomic_load(config->capture_running);
        uint64_t now_ms = gui_ddd_async_now_ms(engine);
        size_t in_flight;
        size_t total_in_flight;
        bool telemetry_in_flight;

        if (!capture_running && !engine->stopping) {
            engine->stopping = true;
            engine->accepting_submissions = false;
            stop_deadline_ms = now_ms +
                GUI_DDD_ASYNC_STOP_DRAIN_TIMEOUT_MS;
            gui_ddd_async_cancel_telemetry(engine);
        }

        if (capture_running) {
            gui_ddd_async_maybe_submit_telemetry(engine, now_ms);
        }

        if (!engine->failed &&
            gui_ddd_async_consume_ready(engine) < 0) {
            gui_ddd_async_cancel_in_flight(engine);
        }

        in_flight = atomic_load(&engine->in_flight);
        telemetry_in_flight = atomic_load(&engine->telemetry_in_flight);
        total_in_flight = in_flight + (telemetry_in_flight ? 1u : 0u);
        if (engine->failed) {
            gui_ddd_async_cancel_in_flight(engine);
        } else if (engine->stopping && total_in_flight > 0 &&
                   now_ms >= stop_deadline_ms) {
            gui_ddd_async_latch_failure(
                engine, GUI_DDD_ASYNC_RESULT_DRAIN_TIMEOUT, 0, 0, 0,
                engine->order.next_consume_id);
            gui_ddd_async_cancel_in_flight(engine);
        }

        in_flight = atomic_load(&engine->in_flight);
        telemetry_in_flight = atomic_load(&engine->telemetry_in_flight);
        total_in_flight = in_flight + (telemetry_in_flight ? 1u : 0u);
        if (engine->failed && total_in_flight == 0) break;
        if (!engine->failed &&
            gui_ddd_async_policy_stop_drain_complete(
                capture_running, in_flight,
                engine->result.completed_transfers,
                engine->result.consumed_transfers) &&
            !telemetry_in_flight) {
            break;
        }
        if (gui_ddd_async_policy_reap_action(
                engine->cancel_issued,
                now_ms,
                engine->cancel_deadline_ms,
                total_in_flight) == GUI_DDD_ASYNC_REAP_ORPHAN) {
            break;
        }
        if (!engine->failed && capture_running && in_flight == 0) {
            gui_ddd_async_latch_failure(
                engine, GUI_DDD_ASYNC_RESULT_ORDER_FAILURE, 0, 0, 0,
                engine->order.next_consume_id);
            gui_ddd_async_cancel_in_flight(engine);
            continue;
        }

        if (gui_ddd_async_pump_events(engine) < 0) {
            gui_ddd_async_cancel_in_flight(engine);
            thrd_sleep_ms(1);
        }
    }

finished:
    atomic_store(config->transfer_ready, false);
    if (gui_ddd_async_engine_has_pending(engine)) {
        engine->result.transfers_unreaped = true;
        engine->result.unreaped_transfers =
            atomic_load(&engine->in_flight) +
            (atomic_load(&engine->telemetry_in_flight) ? 1u : 0u);
        engine->result.active_callbacks =
            atomic_load(&engine->callbacks_active);
        engine->result.orphan = engine;
        /* The capture-thread stack and gui_app may go away after return.
         * Future callbacks may only touch heap-owned engine/slot state. */
        gui_ddd_async_detach_external_context(&engine->config_storage);
        *result = engine->result;
        return -1;
    }

    local_result = engine->result;
    gui_ddd_async_destroy(engine);
    *result = local_result;
    return local_result.code == GUI_DDD_ASYNC_RESULT_SUCCESS ? 0 : -1;
}

bool gui_ddd_async_orphan_has_unreaped(
    const gui_ddd_async_orphan_t *orphan)
{
    return gui_ddd_async_engine_has_pending(orphan);
}

bool gui_ddd_async_orphan_try_reclaim(gui_ddd_async_orphan_t *orphan)
{
    if (!orphan || gui_ddd_async_engine_has_pending(orphan)) return false;
    gui_ddd_async_destroy(orphan);
    return true;
}

bool gui_ddd_async_orphan_abandon(gui_ddd_async_orphan_t *orphan)
{
    if (!orphan) return false;
    if (!gui_ddd_async_engine_has_pending(orphan)) {
        gui_ddd_async_destroy(orphan);
        return true;
    }
    /* One process-global slot is intentional: after the first unreaped queue,
     * gui_ddd refuses every further open so retained USB ownership is bounded. */
    if (gui_ddd_async_policy_abandon_slot_available(
            s_ddd_async_abandoned ? 1u : 0u)) {
        s_ddd_async_abandoned = orphan;
        orphan->abandoned = true;
        return true;
    }
    return s_ddd_async_abandoned == orphan;
}

bool gui_ddd_async_global_quarantine_active(void)
{
    return s_ddd_async_abandoned != NULL;
}

const char *gui_ddd_async_result_name(gui_ddd_async_result_code_t code)
{
    switch (code) {
        case GUI_DDD_ASYNC_RESULT_SUCCESS: return "Success";
        case GUI_DDD_ASYNC_RESULT_INVALID_ARGUMENT: return "InvalidArgument";
        case GUI_DDD_ASYNC_RESULT_ALLOCATION_FAILURE: return "AllocationFailure";
        case GUI_DDD_ASYNC_RESULT_SUBMIT_FAILURE: return "SubmitFailure";
        case GUI_DDD_ASYNC_RESULT_TRANSFER_FAILURE: return "TransferFailure";
        case GUI_DDD_ASYNC_RESULT_SHORT_TRANSFER: return "ShortTransfer";
        case GUI_DDD_ASYNC_RESULT_EVENT_FAILURE: return "EventFailure";
        case GUI_DDD_ASYNC_RESULT_DRAIN_TIMEOUT: return "DrainTimeout";
        case GUI_DDD_ASYNC_RESULT_ORDER_FAILURE: return "OrderFailure";
        case GUI_DDD_ASYNC_RESULT_CONSUMER_FAILURE: return "ConsumerFailure";
        default: return "Unknown";
    }
}
