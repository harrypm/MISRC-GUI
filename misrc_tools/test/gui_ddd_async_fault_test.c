#include "../misrc_gui/input/gui_ddd_async.h"
#include "../common/ddd_protocol.h"

#include <assert.h>
#include <libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_EVENT_ERROR (-99)

typedef struct {
    uint64_t now_ms;
    size_t submit_calls;
    size_t cancel_calls;
    size_t event_pump_calls;
    struct libusb_transfer *bulk_transfers[GUI_DDD_ASYNC_TRANSFER_COUNT];
    size_t bulk_transfer_count;
    struct libusb_transfer *telemetry_transfer;
    atomic_bool *capture_running;
    size_t telemetry_failure_callbacks;
    size_t telemetry_success_callbacks;
    size_t telemetry_disable_notices;
    size_t telemetry_publish_calls;
    size_t telemetry_last_size;
    uint8_t telemetry_last_first_byte;
    size_t consume_calls;
} async_fault_state_t;

static uint64_t advancing_now_ms(void *context)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    state->now_ms += 400;
    return state->now_ms;
}

static int permanent_event_error(void *context, long timeout_us)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    (void)timeout_us;
    state->event_pump_calls++;
    return TEST_EVENT_ERROR;
}

static int accept_fake_submit(
    void *context,
    struct libusb_transfer *transfer)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    assert(transfer != NULL);
    state->submit_calls++;
    if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        state->telemetry_transfer = transfer;
    } else if (state->bulk_transfer_count < GUI_DDD_ASYNC_TRANSFER_COUNT) {
        state->bulk_transfers[state->bulk_transfer_count++] = transfer;
    }
    return 0;
}

static int accept_fake_cancel(
    void *context,
    struct libusb_transfer *transfer)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    assert(transfer != NULL);
    state->cancel_calls++;
    return 0;
}

static gui_ddd_async_consume_result_t reject_unexpected_consume(
    void *context,
    const uint8_t *data,
    size_t size)
{
    (void)context;
    (void)data;
    (void)size;
    assert(!"permanent event failure must not consume a block");
    return GUI_DDD_ASYNC_CONSUME_FAILED;
}

static void reject_unexpected_telemetry(
    void *context,
    const uint8_t *data,
    size_t size)
{
    (void)context;
    (void)data;
    (void)size;
    assert(!"permanent event failure must not publish telemetry");
}

static gui_ddd_async_consume_result_t accept_expected_consume(
    void *context,
    const uint8_t *data,
    size_t size)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    assert(data != NULL);
    assert(size == GUI_DDD_ASYNC_TRANSFER_BYTES);
    state->consume_calls++;
    return GUI_DDD_ASYNC_CONSUME_CONTINUE;
}

static void record_telemetry_notice(
    void *context,
    const uint8_t *data,
    size_t size)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    if (!data && size == 0) {
        state->telemetry_disable_notices++;
    } else {
        assert(data != NULL);
        state->telemetry_publish_calls++;
        state->telemetry_last_size = size;
        state->telemetry_last_first_byte = data[0];
    }
}

static void complete_all_bulk_transfers(async_fault_state_t *state)
{
    assert(state != NULL);
    assert(state->bulk_transfer_count == GUI_DDD_ASYNC_TRANSFER_COUNT);
    for (size_t i = 0; i < state->bulk_transfer_count; ++i) {
        struct libusb_transfer *bulk = state->bulk_transfers[i];
        assert(bulk != NULL);
        assert(bulk->callback != NULL);
        bulk->status = LIBUSB_TRANSFER_COMPLETED;
        bulk->actual_length = (int)GUI_DDD_ASYNC_TRANSFER_BYTES;
        bulk->callback(bulk);
    }
}

static void write_valid_telemetry_block(uint8_t *data)
{
    assert(data != NULL);
    memset(data, 0, DDD_FIFO_TELEMETRY_LENGTH);
    data[0] = DDD_FIFO_TELEMETRY_ID;
    data[DDD_FIFO_OFFSET_STATUS] = DDD_FIFO_TELEMETRY_FORMAT;
    data[DDD_FIFO_OFFSET_DEPTH] = 0x00;
    data[DDD_FIFO_OFFSET_DEPTH + 1u] = 0x40;
    data[DDD_FIFO_OFFSET_PACKET_WORDS] = 0x00;
    data[DDD_FIFO_OFFSET_PACKET_WORDS + 1u] = 0x20;
}

static int drive_two_telemetry_successes(void *context, long timeout_us)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    struct libusb_transfer *telemetry = state->telemetry_transfer;
    struct libusb_control_setup *setup;
    uint8_t *data;

    (void)timeout_us;
    state->event_pump_calls++;
    assert(telemetry != NULL);
    assert(telemetry->callback != NULL);
    assert(state->telemetry_success_callbacks < 2);

    setup = libusb_control_transfer_get_setup(telemetry);
    assert(setup != NULL);
    assert(setup->bmRequestType == DDD_USB_REQUEST_VENDOR_IN);
    assert(setup->bRequest == DDD_REQUEST_REGISTER_READ);
    assert(libusb_le16_to_cpu(setup->wValue) ==
           DDD_REGISTER_FIFO_TELEMETRY);
    assert(libusb_le16_to_cpu(setup->wIndex) == 0);
    assert(libusb_le16_to_cpu(setup->wLength) ==
           DDD_FIFO_TELEMETRY_LENGTH);

    data = libusb_control_transfer_get_data(telemetry);
    assert(data != NULL);
    write_valid_telemetry_block(data);
    telemetry->status = LIBUSB_TRANSFER_COMPLETED;
    telemetry->actual_length = (int)DDD_FIFO_TELEMETRY_LENGTH;
    telemetry->callback(telemetry);
    state->telemetry_success_callbacks++;

    if (state->telemetry_success_callbacks == 2) {
        assert(state->capture_running != NULL);
        atomic_store(state->capture_running, false);
        complete_all_bulk_transfers(state);
    }
    return 0;
}

static int drive_two_invalid_telemetry_blocks(void *context, long timeout_us)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    struct libusb_transfer *telemetry = state->telemetry_transfer;
    uint8_t *data;

    (void)timeout_us;
    state->event_pump_calls++;
    assert(telemetry != NULL);
    assert(telemetry->callback != NULL);

    if (state->telemetry_failure_callbacks == 2) {
        /* Leave capture active for one extra loop iteration. A correct retry
         * policy must not submit a third telemetry read after two invalid
         * full-length responses. */
        assert(state->submit_calls ==
               GUI_DDD_ASYNC_TRANSFER_COUNT + 2u);
        assert(state->capture_running != NULL);
        atomic_store(state->capture_running, false);
        complete_all_bulk_transfers(state);
        return 0;
    }

    data = libusb_control_transfer_get_data(telemetry);
    assert(data != NULL);
    memset(data, 0, DDD_FIFO_TELEMETRY_LENGTH);
    if (state->telemetry_failure_callbacks == 1) {
        write_valid_telemetry_block(data);
        data[DDD_FIFO_OFFSET_STATUS] = DDD_FIFO_TELEMETRY_FORMAT + 1u;
    }
    telemetry->status = LIBUSB_TRANSFER_COMPLETED;
    telemetry->actual_length = (int)DDD_FIFO_TELEMETRY_LENGTH;
    telemetry->callback(telemetry);
    state->telemetry_failure_callbacks++;
    return 0;
}

static int drive_two_telemetry_timeouts(void *context, long timeout_us)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    struct libusb_transfer *telemetry = state->telemetry_transfer;

    (void)timeout_us;
    state->event_pump_calls++;
    assert(telemetry != NULL);
    assert(telemetry->callback != NULL);
    assert(state->telemetry_failure_callbacks < 2);

    telemetry->status = LIBUSB_TRANSFER_TIMED_OUT;
    telemetry->actual_length = 0;
    telemetry->callback(telemetry);
    state->telemetry_failure_callbacks++;

    if (state->telemetry_failure_callbacks == 2) {
        assert(state->capture_running != NULL);
        atomic_store(state->capture_running, false);
        complete_all_bulk_transfers(state);
    }
    return 0;
}

static gui_ddd_async_config_t make_fault_config(
    async_fault_state_t *state,
    atomic_bool *capture_running,
    atomic_bool *transfer_ready,
    atomic_bool *startup_failed,
    int *usb_context_marker,
    int *device_handle_marker)
{
    gui_ddd_async_config_t config;

    memset(&config, 0, sizeof(config));
    config.usb_context =
        (struct libusb_context *)(void *)usb_context_marker;
    config.device_handle =
        (struct libusb_device_handle *)(void *)device_handle_marker;
    config.endpoint = 0x82;
    config.capture_running = capture_running;
    config.transfer_ready = transfer_ready;
    config.startup_failed = startup_failed;
    config.consume = reject_unexpected_consume;
    config.consume_context = state;
    config.telemetry = reject_unexpected_telemetry;
    config.telemetry_context = state;
    config.event_pump_override = permanent_event_error;
    config.event_pump_context = state;
    config.now_ms_override = advancing_now_ms;
    config.now_ms_context = state;
    config.submit_override = accept_fake_submit;
    config.submit_context = state;
    config.cancel_override = accept_fake_cancel;
    config.cancel_context = state;
    return config;
}

static void test_telemetry_success_primes_then_publishes(void)
{
    async_fault_state_t state = {0};
    atomic_bool capture_running = true;
    atomic_bool transfer_ready = false;
    atomic_bool startup_failed = false;
    int usb_context_marker = 1;
    int device_handle_marker = 2;
    gui_ddd_async_config_t config = make_fault_config(
        &state, &capture_running, &transfer_ready, &startup_failed,
        &usb_context_marker, &device_handle_marker);
    gui_ddd_async_result_t result;

    state.capture_running = &capture_running;
    config.consume = accept_expected_consume;
    config.telemetry = record_telemetry_notice;
    config.event_pump_override = drive_two_telemetry_successes;
    memset(&result, 0, sizeof(result));

    assert(gui_ddd_async_run(&config, &result) == 0);
    assert(result.code == GUI_DDD_ASYNC_RESULT_SUCCESS);
    assert(result.ready_signalled);
    assert(!result.transfers_unreaped);
    assert(result.completed_transfers == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(result.consumed_transfers == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(result.telemetry_readings == 2);
    assert(result.telemetry_failures == 0);
    assert(state.submit_calls == GUI_DDD_ASYNC_TRANSFER_COUNT + 2u);
    assert(state.cancel_calls == 0);
    assert(state.telemetry_success_callbacks == 2);
    assert(state.telemetry_disable_notices == 0);
    assert(state.telemetry_publish_calls == 1);
    assert(state.telemetry_last_size == DDD_FIFO_TELEMETRY_LENGTH);
    assert(state.telemetry_last_first_byte == DDD_FIFO_TELEMETRY_ID);
    assert(state.consume_calls == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(!atomic_load(&capture_running));
    assert(!atomic_load(&transfer_ready));
    assert(!atomic_load(&startup_failed));
}

static void test_telemetry_failures_do_not_fail_rf_capture(void)
{
    async_fault_state_t state = {0};
    atomic_bool capture_running = true;
    atomic_bool transfer_ready = false;
    atomic_bool startup_failed = false;
    int usb_context_marker = 1;
    int device_handle_marker = 2;
    gui_ddd_async_config_t config = make_fault_config(
        &state, &capture_running, &transfer_ready, &startup_failed,
        &usb_context_marker, &device_handle_marker);
    gui_ddd_async_result_t result;

    state.capture_running = &capture_running;
    config.consume = accept_expected_consume;
    config.telemetry = record_telemetry_notice;
    config.event_pump_override = drive_two_telemetry_timeouts;
    memset(&result, 0, sizeof(result));

    assert(gui_ddd_async_run(&config, &result) == 0);
    assert(result.code == GUI_DDD_ASYNC_RESULT_SUCCESS);
    assert(result.ready_signalled);
    assert(!result.transfers_unreaped);
    assert(result.completed_transfers == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(result.consumed_transfers == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(result.telemetry_readings == 0);
    assert(result.telemetry_failures == 2);
    assert(state.submit_calls == GUI_DDD_ASYNC_TRANSFER_COUNT + 2u);
    assert(state.cancel_calls == 0);
    assert(state.telemetry_failure_callbacks == 2);
    assert(state.telemetry_disable_notices == 1);
    assert(state.telemetry_publish_calls == 0);
    assert(state.consume_calls == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(!atomic_load(&capture_running));
    assert(!atomic_load(&transfer_ready));
    assert(!atomic_load(&startup_failed));
}

static void test_invalid_telemetry_blocks_disable_instrument(void)
{
    async_fault_state_t state = {0};
    atomic_bool capture_running = true;
    atomic_bool transfer_ready = false;
    atomic_bool startup_failed = false;
    int usb_context_marker = 1;
    int device_handle_marker = 2;
    gui_ddd_async_config_t config = make_fault_config(
        &state, &capture_running, &transfer_ready, &startup_failed,
        &usb_context_marker, &device_handle_marker);
    gui_ddd_async_result_t result;

    state.capture_running = &capture_running;
    config.consume = accept_expected_consume;
    config.telemetry = record_telemetry_notice;
    config.event_pump_override = drive_two_invalid_telemetry_blocks;
    memset(&result, 0, sizeof(result));

    assert(gui_ddd_async_run(&config, &result) == 0);
    assert(result.code == GUI_DDD_ASYNC_RESULT_SUCCESS);
    assert(result.ready_signalled);
    assert(!result.transfers_unreaped);
    assert(result.completed_transfers == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(result.consumed_transfers == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(result.telemetry_readings == 0);
    assert(result.telemetry_failures == 2);
    assert(state.submit_calls == GUI_DDD_ASYNC_TRANSFER_COUNT + 2u);
    assert(state.cancel_calls == 0);
    assert(state.telemetry_failure_callbacks == 2);
    assert(state.telemetry_disable_notices == 1);
    assert(state.telemetry_publish_calls == 0);
    assert(state.consume_calls == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(!atomic_load(&capture_running));
    assert(!atomic_load(&transfer_ready));
    assert(!atomic_load(&startup_failed));
}

static void test_result_is_required_for_orphan_ownership(void)
{
    async_fault_state_t state = {0};
    atomic_bool capture_running = true;
    atomic_bool transfer_ready = true;
    atomic_bool startup_failed = false;
    int usb_context_marker = 1;
    int device_handle_marker = 2;
    gui_ddd_async_config_t config = make_fault_config(
        &state, &capture_running, &transfer_ready, &startup_failed,
        &usb_context_marker, &device_handle_marker);

    assert(gui_ddd_async_run(&config, NULL) == -1);
    assert(!atomic_load(&transfer_ready));
    assert(atomic_load(&startup_failed));
    assert(state.submit_calls == 0);
}

static void test_permanent_event_error_returns_bounded_orphan(void)
{
    async_fault_state_t state = {0};
    atomic_bool capture_running = true;
    atomic_bool transfer_ready = false;
    atomic_bool startup_failed = false;
    int usb_context_marker = 1;
    int device_handle_marker = 2;
    gui_ddd_async_config_t config = make_fault_config(
        &state, &capture_running, &transfer_ready, &startup_failed,
        &usb_context_marker, &device_handle_marker);
    gui_ddd_async_result_t result;

    memset(&result, 0, sizeof(result));
    assert(!gui_ddd_async_global_quarantine_active());
    assert(gui_ddd_async_run(&config, &result) == -1);

    assert(result.code == GUI_DDD_ASYNC_RESULT_EVENT_FAILURE);
    assert(result.libusb_error == TEST_EVENT_ERROR);
    assert(result.ready_signalled);
    assert(result.transfers_unreaped);
    assert(result.unreaped_transfers ==
           GUI_DDD_ASYNC_TRANSFER_COUNT + 1u);
    assert(result.active_callbacks == 0);
    assert(result.orphan != NULL);
    assert(state.submit_calls == GUI_DDD_ASYNC_TRANSFER_COUNT + 1u);
    assert(state.cancel_calls == GUI_DDD_ASYNC_TRANSFER_COUNT + 1u);
    assert(state.event_pump_calls > 0);
    assert(state.event_pump_calls <= 4);
    assert(state.now_ms <=
           GUI_DDD_ASYNC_CANCEL_REAP_TIMEOUT_MS + 1200);
    assert(!atomic_load(&transfer_ready));
    assert(atomic_load(&startup_failed));

    assert(gui_ddd_async_orphan_has_unreaped(result.orphan));
    assert(!gui_ddd_async_orphan_try_reclaim(result.orphan));
    assert(!gui_ddd_async_policy_sync_control_allowed(true));
    assert(gui_ddd_async_orphan_abandon(result.orphan));
    assert(gui_ddd_async_global_quarantine_active());
    assert(gui_ddd_async_orphan_abandon(result.orphan));
}

int main(void)
{
    test_result_is_required_for_orphan_ownership();
    test_telemetry_success_primes_then_publishes();
    test_telemetry_failures_do_not_fail_rf_capture();
    test_invalid_telemetry_blocks_disable_instrument();
    test_permanent_event_error_returns_bounded_orphan();
    puts("gui_ddd_async_fault_test: OK");
    return 0;
}
