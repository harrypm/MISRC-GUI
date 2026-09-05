/*
 * MISRC GUI - DDD firmware 3.1 asynchronous USB capture queue
 *
 * This interface is intentionally independent from gui_app_t.  It owns all
 * libusb transfers and their backing storage for one capture-thread run, and
 * delivers exact 128 KiB blocks to the caller in submission order.  The
 * smaller completion unit limits the device-FIFO exposure between completion
 * and resubmission while retaining a 12 MiB in-flight queue.
 */

#ifndef GUI_DDD_ASYNC_H
#define GUI_DDD_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

#define GUI_DDD_ASYNC_TRANSFER_BYTES ((size_t)128 * 1024)
#define GUI_DDD_ASYNC_QUEUE_BYTES    ((size_t)12 * 1024 * 1024)
#define GUI_DDD_ASYNC_TRANSFER_COUNT \
    (GUI_DDD_ASYNC_QUEUE_BYTES / GUI_DDD_ASYNC_TRANSFER_BYTES)
#define GUI_DDD_ASYNC_STOP_DRAIN_TIMEOUT_MS UINT64_C(1000)
#define GUI_DDD_ASYNC_CANCEL_REAP_TIMEOUT_MS UINT64_C(1000)
#define GUI_DDD_ASYNC_ABANDONED_CAPACITY 1u
#define GUI_DDD_ASYNC_TELEMETRY_INTERVAL_MS UINT64_C(250)
#define GUI_DDD_ASYNC_TELEMETRY_TIMEOUT_MS 1000u
#define GUI_DDD_ASYNC_TELEMETRY_MAX_FAILURES 2u

typedef struct gui_ddd_async_orphan gui_ddd_async_orphan_t;

typedef enum {
    GUI_DDD_ASYNC_SLOT_UNUSED = 0,
    GUI_DDD_ASYNC_SLOT_SUBMITTED,
    GUI_DDD_ASYNC_SLOT_COMPLETE,
    GUI_DDD_ASYNC_SLOT_CANCELLED,
    GUI_DDD_ASYNC_SLOT_FAILED,
    GUI_DDD_ASYNC_SLOT_RETIRED,
} gui_ddd_async_slot_state_t;

/* Small dependency-free ordering model shared by runtime code and unit tests. */
typedef struct {
    uint64_t submission_id;
    gui_ddd_async_slot_state_t state;
} gui_ddd_async_policy_slot_t;

typedef struct {
    uint64_t next_consume_id;
    size_t slot_count;
} gui_ddd_async_order_policy_t;

typedef enum {
    GUI_DDD_ASYNC_NEXT_WAIT = 0,
    GUI_DDD_ASYNC_NEXT_READY,
    GUI_DDD_ASYNC_NEXT_STALE,
} gui_ddd_async_next_state_t;

static inline void gui_ddd_async_order_policy_init(
    gui_ddd_async_order_policy_t *policy,
    size_t slot_count)
{
    if (!policy) return;
    policy->next_consume_id = 0;
    policy->slot_count = slot_count;
}

static inline gui_ddd_async_next_state_t gui_ddd_async_order_policy_peek(
    const gui_ddd_async_order_policy_t *policy,
    const gui_ddd_async_policy_slot_t *slots,
    size_t *slot_index)
{
    size_t index;
    const gui_ddd_async_policy_slot_t *slot;

    if (!policy || !slots || policy->slot_count == 0) {
        return GUI_DDD_ASYNC_NEXT_STALE;
    }
    index = (size_t)(policy->next_consume_id % policy->slot_count);
    slot = &slots[index];
    if (slot->submission_id != policy->next_consume_id) {
        return GUI_DDD_ASYNC_NEXT_STALE;
    }
    if (slot->state != GUI_DDD_ASYNC_SLOT_COMPLETE) {
        return GUI_DDD_ASYNC_NEXT_WAIT;
    }
    if (slot_index) *slot_index = index;
    return GUI_DDD_ASYNC_NEXT_READY;
}

static inline void gui_ddd_async_order_policy_advance(
    gui_ddd_async_order_policy_t *policy)
{
    if (policy) policy->next_consume_id++;
}

static inline bool gui_ddd_async_policy_initial_queue_ready(
    size_t submitted_count,
    bool failed)
{
    return !failed && submitted_count == GUI_DDD_ASYNC_TRANSFER_COUNT;
}

/* Submitting the USB queue only establishes transport ownership. Recording is
 * safe after the startup discard has finished and one validated RF block has
 * been published, while the queue and capture lifetime are still active. */
static inline bool gui_ddd_async_policy_stream_ready(
    bool queue_ready,
    bool verified_block_published,
    bool capture_running,
    bool startup_failed)
{
    return queue_ready && verified_block_published && capture_running &&
           !startup_failed;
}

static inline bool gui_ddd_async_policy_should_resubmit(
    bool accepting_submissions,
    bool capture_running,
    bool failed)
{
    return accepting_submissions && capture_running && !failed;
}

/* Once capture has stopped, every submitted callback has completed, and every
 * completed block has been consumed, the next ring generation intentionally
 * does not exist. That terminal gap is a successful drain, not stale-order
 * corruption. */
static inline bool gui_ddd_async_policy_stop_drain_complete(
    bool capture_running,
    size_t in_flight,
    uint64_t completed_transfers,
    uint64_t consumed_transfers)
{
    return !capture_running && in_flight == 0 &&
           completed_transfers == consumed_transfers;
}

typedef enum {
    GUI_DDD_ASYNC_REAP_WAIT = 0,
    GUI_DDD_ASYNC_REAP_COMPLETE,
    GUI_DDD_ASYNC_REAP_ORPHAN,
} gui_ddd_async_reap_action_t;

/* Dependency-free cancellation/reap controller. Tests inject a permanently
 * failing event pump by advancing now_ms without decrementing in_flight. */
static inline gui_ddd_async_reap_action_t gui_ddd_async_policy_reap_action(
    bool cancel_started,
    uint64_t now_ms,
    uint64_t cancel_deadline_ms,
    size_t in_flight)
{
    if (in_flight == 0) return GUI_DDD_ASYNC_REAP_COMPLETE;
    if (cancel_started && now_ms >= cancel_deadline_ms) {
        return GUI_DDD_ASYNC_REAP_ORPHAN;
    }
    return GUI_DDD_ASYNC_REAP_WAIT;
}

static inline bool gui_ddd_async_policy_has_pending(
    size_t in_flight,
    size_t callbacks_active)
{
    return in_flight != 0 || callbacks_active != 0;
}

static inline bool gui_ddd_async_policy_telemetry_should_submit(
    bool enabled,
    bool in_flight,
    unsigned failures,
    uint64_t now_ms,
    uint64_t due_ms)
{
    return enabled && !in_flight &&
           failures < GUI_DDD_ASYNC_TELEMETRY_MAX_FAILURES &&
           now_ms >= due_ms;
}

/* The first successful read only establishes the start of this capture's
 * interval. The read itself clears the gateware's interval counters. */
static inline bool gui_ddd_async_policy_telemetry_should_publish(bool *primed)
{
    if (!primed) return false;
    if (!*primed) {
        *primed = true;
        return false;
    }
    return true;
}

/* A synchronous libusb control transfer also depends on the context event
 * pump. Once an async queue has timed out with callbacks still pending, a
 * synchronous B5/rollback may wait forever and must not be attempted. */
static inline bool gui_ddd_async_policy_sync_control_allowed(
    bool orphan_pending)
{
    return !orphan_pending;
}

static inline bool gui_ddd_async_policy_exact_length(size_t actual_length)
{
    return actual_length == GUI_DDD_ASYNC_TRANSFER_BYTES;
}

typedef enum {
    GUI_DDD_ASYNC_CONSUME_CONTINUE = 0,
    GUI_DDD_ASYNC_CONSUME_FAILED,
} gui_ddd_async_consume_result_t;

typedef gui_ddd_async_consume_result_t (*gui_ddd_async_consume_fn)(
    void *context,
    const uint8_t *data,
    size_t size);

typedef void (*gui_ddd_async_telemetry_fn)(
    void *context,
    const uint8_t *data,
    size_t size);

/* Optional fault-injection seams. Production leaves all of them NULL. A test may
 * supply an event pump that returns a permanent error and a deterministic
 * clock to exercise the bounded orphan transition. */
typedef int (*gui_ddd_async_event_pump_fn)(void *context,
                                           long timeout_us);
typedef uint64_t (*gui_ddd_async_now_ms_fn)(void *context);
typedef int (*gui_ddd_async_transfer_action_fn)(
    void *context,
    struct libusb_transfer *transfer);

typedef enum {
    GUI_DDD_ASYNC_RESULT_SUCCESS = 0,
    GUI_DDD_ASYNC_RESULT_INVALID_ARGUMENT,
    GUI_DDD_ASYNC_RESULT_ALLOCATION_FAILURE,
    GUI_DDD_ASYNC_RESULT_SUBMIT_FAILURE,
    GUI_DDD_ASYNC_RESULT_TRANSFER_FAILURE,
    GUI_DDD_ASYNC_RESULT_SHORT_TRANSFER,
    GUI_DDD_ASYNC_RESULT_EVENT_FAILURE,
    GUI_DDD_ASYNC_RESULT_DRAIN_TIMEOUT,
    GUI_DDD_ASYNC_RESULT_ORDER_FAILURE,
    GUI_DDD_ASYNC_RESULT_CONSUMER_FAILURE,
} gui_ddd_async_result_code_t;

typedef struct {
    gui_ddd_async_result_code_t code;
    int libusb_error;
    int transfer_status;
    int actual_length;
    uint64_t submission_id;
    uint64_t completed_transfers;
    uint64_t consumed_transfers;
    uint64_t telemetry_readings;
    unsigned telemetry_failures;
    bool ready_signalled;
    bool transfers_unreaped;
    size_t unreaped_transfers;
    size_t active_callbacks;
    gui_ddd_async_orphan_t *orphan;
} gui_ddd_async_result_t;

typedef struct {
    struct libusb_context *usb_context;
    struct libusb_device_handle *device_handle;
    uint8_t endpoint;
    atomic_bool *capture_running;
    atomic_bool *transfer_ready;
    atomic_bool *startup_failed;
    gui_ddd_async_consume_fn consume;
    void *consume_context;
    /* Optional DdD FIFO instrument. It is driven by the same event thread as
     * the RF queue; no second event handler or synchronous control transfer is
     * introduced while capture is active. */
    gui_ddd_async_telemetry_fn telemetry;
    void *telemetry_context;
    gui_ddd_async_event_pump_fn event_pump_override;
    void *event_pump_context;
    gui_ddd_async_now_ms_fn now_ms_override;
    void *now_ms_context;
    gui_ddd_async_transfer_action_fn submit_override;
    void *submit_context;
    gui_ddd_async_transfer_action_fn cancel_override;
    void *cancel_context;
} gui_ddd_async_config_t;

/* Converts a capture-thread config into callback-safe orphan state. Raw USB
 * identity is retained, while every pointer into caller/thread stack state and
 * every test hook is cleared. */
static inline void gui_ddd_async_detach_external_context(
    gui_ddd_async_config_t *config)
{
    if (!config) return;
    config->capture_running = NULL;
    config->transfer_ready = NULL;
    config->startup_failed = NULL;
    config->consume = NULL;
    config->consume_context = NULL;
    config->telemetry = NULL;
    config->telemetry_context = NULL;
    config->event_pump_override = NULL;
    config->event_pump_context = NULL;
    config->now_ms_override = NULL;
    config->now_ms_context = NULL;
    config->submit_override = NULL;
    config->submit_context = NULL;
    config->cancel_override = NULL;
    config->cancel_context = NULL;
}

static inline bool gui_ddd_async_policy_abandon_slot_available(
    size_t abandoned_count)
{
    return abandoned_count < GUI_DDD_ASYNC_ABANDONED_CAPACITY;
}

/*
 * Run one queue lifetime synchronously on the calling capture thread. Usually
 * returns after every callback is reaped. If cancellation callbacks cannot be
 * reaped within the bounded deadline, result->orphan owns every pending
 * transfer and backing buffer and retains the raw USB identity. The caller
 * must retain the underlying USB interface/handle/context (the engine does
 * not add libusb references) and either reclaim or abandon the engine.
 * result is required so orphan ownership can never be dropped silently.
 *
 * The supplied usb_context is exclusively owned by this DDD capture while
 * run is active: no second event handler and no synchronous libusb operation
 * may use it. Callback state is intentionally non-atomic apart from the
 * lifetime counters and relies on that single event-pump thread. After run
 * returns an orphan, capture must be joined before ownership is inspected.
 * Query/reclaim only after any event-pump call that could dispatch callbacks
 * has returned. In particular, synchronous recovery is forbidden while
 * orphan_has_unreaped is true.
 */
int gui_ddd_async_run(const gui_ddd_async_config_t *config,
                      gui_ddd_async_result_t *result);

bool gui_ddd_async_orphan_has_unreaped(
    const gui_ddd_async_orphan_t *orphan);

/* Frees a formerly orphaned engine only when all callbacks were subsequently
 * delivered by an explicitly serialized event pump. Call only after that
 * event-pump function has returned. Returns true iff ownership was consumed
 * and the pointer must be discarded. */
bool gui_ddd_async_orphan_try_reclaim(gui_ddd_async_orphan_t *orphan);

/* Explicitly retain an unreaped engine until process exit. The caller must
 * likewise transfer USB lifetime by not releasing its handle/interface/context. */
bool gui_ddd_async_orphan_abandon(gui_ddd_async_orphan_t *orphan);

/* The first process-lifetime orphan globally blocks further DDD opens. This
 * bounds retained async queues to GUI_DDD_ASYNC_ABANDONED_CAPACITY. */
bool gui_ddd_async_global_quarantine_active(void);

const char *gui_ddd_async_result_name(gui_ddd_async_result_code_t code);

#endif /* GUI_DDD_ASYNC_H */
