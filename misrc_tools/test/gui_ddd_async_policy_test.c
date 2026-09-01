#include "../misrc_gui/input/gui_ddd_async.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    gui_ddd_async_order_policy_t policy;
    bool telemetry_primed = false;
    gui_ddd_async_policy_slot_t slots[2] = {
        {.submission_id = 0, .state = GUI_DDD_ASYNC_SLOT_COMPLETE},
        {.submission_id = 1, .state = GUI_DDD_ASYNC_SLOT_SUBMITTED}
    };
    size_t index = 99;

    gui_ddd_async_order_policy_init(&policy, 2);
    CHECK(gui_ddd_async_order_policy_peek(&policy, slots, &index) ==
          GUI_DDD_ASYNC_NEXT_READY);
    CHECK(index == 0);
    gui_ddd_async_order_policy_advance(&policy);
    CHECK(gui_ddd_async_order_policy_peek(&policy, slots, &index) ==
          GUI_DDD_ASYNC_NEXT_WAIT);
    CHECK(gui_ddd_async_policy_initial_queue_ready(
        GUI_DDD_ASYNC_TRANSFER_COUNT, false));
    CHECK(!gui_ddd_async_policy_stream_ready(true, false, true, false));
    CHECK(gui_ddd_async_policy_stream_ready(true, true, true, false));
    CHECK(!gui_ddd_async_policy_should_resubmit(true, false, false));
    CHECK(gui_ddd_async_policy_stop_drain_complete(false, 0, 10, 10));
    CHECK(gui_ddd_async_policy_reap_action(true, 100, 100, 1) ==
          GUI_DDD_ASYNC_REAP_ORPHAN);
    CHECK(!gui_ddd_async_policy_sync_control_allowed(true));
    CHECK(gui_ddd_async_policy_exact_length(
        GUI_DDD_ASYNC_TRANSFER_BYTES));
    CHECK(!gui_ddd_async_policy_exact_length(
        GUI_DDD_ASYNC_TRANSFER_BYTES - 1));
    CHECK(gui_ddd_async_policy_telemetry_should_submit(
        true, false, 0, 1000, 1000));
    CHECK(!gui_ddd_async_policy_telemetry_should_submit(
        true, true, 0, 1000, 1000));
    CHECK(!gui_ddd_async_policy_telemetry_should_submit(
        true, false, 0, 999, 1000));
    CHECK(!gui_ddd_async_policy_telemetry_should_submit(
        true, false, GUI_DDD_ASYNC_TELEMETRY_MAX_FAILURES,
        1000, 1000));
    CHECK(!gui_ddd_async_policy_telemetry_should_publish(
        &telemetry_primed));
    CHECK(telemetry_primed);
    CHECK(gui_ddd_async_policy_telemetry_should_publish(
        &telemetry_primed));
    CHECK(!gui_ddd_async_policy_telemetry_should_publish(NULL));
    CHECK(gui_ddd_async_policy_abandon_slot_available(0));
    CHECK(!gui_ddd_async_policy_abandon_slot_available(
        GUI_DDD_ASYNC_ABANDONED_CAPACITY));
    puts("DDD async policy tests passed");
    return 0;
}
