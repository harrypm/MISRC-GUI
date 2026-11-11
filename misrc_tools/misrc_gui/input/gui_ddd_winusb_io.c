/* Native Windows I/O for the shared DdD asynchronous queue. */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winusb.h>

#include <stdlib.h>

#include "../../common/libusb_compat.h"
#include "gui_ddd_winusb_io.h"

#define DDD_WINUSB_CONTROL_SLOT GUI_DDD_ASYNC_TRANSFER_COUNT
#define DDD_WINUSB_SLOT_COUNT (GUI_DDD_ASYNC_TRANSFER_COUNT + 1u)

typedef struct {
    OVERLAPPED overlapped;
    struct libusb_transfer *transfer;
    uint64_t submission_id;
    uint64_t deadline_ms;
    bool active;
    bool completed_inline;
    bool cancel_requested;
    bool timed_out;
} ddd_winusb_request_t;

struct gui_ddd_winusb_io {
    HANDLE file;
    WINUSB_INTERFACE_HANDLE interface_handle;
    ddd_winusb_request_t requests[DDD_WINUSB_SLOT_COUNT];
    uint64_t next_submission_id;
    DWORD first_error;
};

static void ddd_winusb_note_error(gui_ddd_winusb_io_t *io, DWORD error)
{
    if (io->first_error == ERROR_SUCCESS) io->first_error = error;
}

static int ddd_winusb_submit_error(DWORD error)
{
    switch (error) {
        case ERROR_ACCESS_DENIED: return LIBUSB_ERROR_ACCESS;
        case ERROR_INVALID_PARAMETER: return LIBUSB_ERROR_INVALID_PARAM;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: return LIBUSB_ERROR_NO_MEM;
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_NO_SUCH_DEVICE: return LIBUSB_ERROR_NO_DEVICE;
        case ERROR_SEM_TIMEOUT:
        case ERROR_TIMEOUT: return LIBUSB_ERROR_TIMEOUT;
        default: return LIBUSB_ERROR_IO;
    }
}

static enum libusb_transfer_status ddd_winusb_transfer_status(DWORD error,
                                                              bool timed_out)
{
    switch (error) {
        case ERROR_SUCCESS: return LIBUSB_TRANSFER_COMPLETED;
        case ERROR_OPERATION_ABORTED:
            return timed_out ? LIBUSB_TRANSFER_TIMED_OUT : LIBUSB_TRANSFER_CANCELLED;
        case ERROR_SEM_TIMEOUT:
        case ERROR_TIMEOUT: return LIBUSB_TRANSFER_TIMED_OUT;
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_NO_SUCH_DEVICE: return LIBUSB_TRANSFER_NO_DEVICE;
        case ERROR_MORE_DATA:
        case ERROR_INSUFFICIENT_BUFFER: return LIBUSB_TRANSFER_OVERFLOW;
        default: return LIBUSB_TRANSFER_ERROR;
    }
}

static ddd_winusb_request_t *ddd_winusb_find_request(gui_ddd_winusb_io_t *io,
                                                     struct libusb_transfer *transfer,
                                                     bool allocate)
{
    if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        ddd_winusb_request_t *request = &io->requests[DDD_WINUSB_CONTROL_SLOT];
        if (request->transfer == transfer || (allocate && !request->transfer)) {
            return request;
        }
        return NULL;
    }
    if (transfer->type != LIBUSB_TRANSFER_TYPE_BULK) return NULL;

    ddd_winusb_request_t *empty = NULL;
    for (size_t i = 0; i < GUI_DDD_ASYNC_TRANSFER_COUNT; ++i) {
        ddd_winusb_request_t *request = &io->requests[i];
        if (request->transfer == transfer) return request;
        if (!request->transfer && !empty) empty = request;
    }
    return allocate ? empty : NULL;
}

static int ddd_winusb_submit(void *context, struct libusb_transfer *transfer)
{
    gui_ddd_winusb_io_t *io = context;
    if (!io || !transfer || !transfer->buffer || !transfer->callback ||
        transfer->length <= 0) return LIBUSB_ERROR_INVALID_PARAM;

    ddd_winusb_request_t *request = ddd_winusb_find_request(io, transfer, true);
    if (!request) return LIBUSB_ERROR_INVALID_PARAM;
    if (request->active) return LIBUSB_ERROR_BUSY;

    WINUSB_SETUP_PACKET setup = {0};
    const bool control = transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL;
    if (control) {
        if (transfer->length < (int)LIBUSB_CONTROL_SETUP_SIZE) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        const struct libusb_control_setup *packet =
            libusb_control_transfer_get_setup(transfer);
        setup.RequestType = packet->bmRequestType;
        setup.Request = packet->bRequest;
        setup.Value = libusb_le16_to_cpu(packet->wValue);
        setup.Index = libusb_le16_to_cpu(packet->wIndex);
        setup.Length = libusb_le16_to_cpu(packet->wLength);
        if (transfer->length - (int)LIBUSB_CONTROL_SETUP_SIZE != setup.Length) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
    }

    HANDLE event = request->overlapped.hEvent;
    request->overlapped = (OVERLAPPED){.hEvent = event};
    if (!ResetEvent(event)) {
        DWORD error = GetLastError();
        if (!control) ddd_winusb_note_error(io, error);
        return ddd_winusb_submit_error(error);
    }

    BOOL completed;
    if (control) {
        completed = WinUsb_ControlTransfer(io->interface_handle, setup,
            libusb_control_transfer_get_data(transfer), setup.Length, NULL,
            &request->overlapped);
    } else {
        completed = WinUsb_ReadPipe(io->interface_handle, transfer->endpoint,
            transfer->buffer, (ULONG)transfer->length, NULL,
            &request->overlapped);
    }
    if (!completed) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            if (!control) ddd_winusb_note_error(io, error);
            return ddd_winusb_submit_error(error);
        }
    }

    /* Defer even inline completions to the pump. The shared queue does not
     * mark a submission in flight until this function returns successfully. */
    request->transfer = transfer;
    request->active = true;
    request->completed_inline = completed != FALSE;
    request->cancel_requested = false;
    request->timed_out = false;
    request->submission_id = io->next_submission_id++;
    request->deadline_ms = control && transfer->timeout != 0
        ? GetTickCount64() + transfer->timeout : 0;
    return LIBUSB_SUCCESS;
}

static int ddd_winusb_cancel_request(gui_ddd_winusb_io_t *io,
                                    ddd_winusb_request_t *request)
{
    if (request->cancel_requested) return LIBUSB_SUCCESS;
    request->cancel_requested = true;
    if (CancelIoEx(io->file, &request->overlapped)) return LIBUSB_SUCCESS;

    DWORD error = GetLastError();
    if (error == ERROR_NOT_FOUND) return LIBUSB_ERROR_NOT_FOUND;
    if (request->transfer->type != LIBUSB_TRANSFER_TYPE_CONTROL) {
        ddd_winusb_note_error(io, error);
    }
    return ddd_winusb_submit_error(error);
}

static int ddd_winusb_cancel(void *context, struct libusb_transfer *transfer)
{
    gui_ddd_winusb_io_t *io = context;
    if (!io || !transfer) return LIBUSB_ERROR_INVALID_PARAM;
    ddd_winusb_request_t *request = ddd_winusb_find_request(io, transfer, false);
    if (!request || !request->active) return LIBUSB_ERROR_NOT_FOUND;
    return ddd_winusb_cancel_request(io, request);
}

static bool ddd_winusb_dispatch_ready(gui_ddd_winusb_io_t *io)
{
    bool dispatched = false;
    for (size_t i = 0; i < DDD_WINUSB_SLOT_COUNT; ++i) {
        ddd_winusb_request_t *request = &io->requests[i];
        if (!request->active || (!request->completed_inline &&
            !HasOverlappedIoCompleted(&request->overlapped))) {
            continue;
        }

        DWORD actual = 0;
        DWORD error = ERROR_SUCCESS;
        if (!WinUsb_GetOverlappedResult(io->interface_handle,
                                        &request->overlapped, &actual, FALSE)) {
            error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE || error == ERROR_IO_PENDING) {
                request->completed_inline = false;
                continue;
            }
        }
        if (i != DDD_WINUSB_CONTROL_SLOT && error != ERROR_SUCCESS &&
            !(error == ERROR_OPERATION_ABORTED && request->cancel_requested)) {
            ddd_winusb_note_error(io, error);
        }

        struct libusb_transfer *transfer = request->transfer;
        transfer->actual_length = (int)actual;
        transfer->status = ddd_winusb_transfer_status(error, request->timed_out);
        request->active = false;
        request->completed_inline = false;
        dispatched = true;
        transfer->callback(transfer);
    }
    return dispatched;
}

static int ddd_winusb_pump(void *context, long timeout_us)
{
    gui_ddd_winusb_io_t *io = context;
    if (!io) return LIBUSB_ERROR_INVALID_PARAM;
    bool dispatched = ddd_winusb_dispatch_ready(io);

    ddd_winusb_request_t *control = &io->requests[DDD_WINUSB_CONTROL_SLOT];
    if (control->active && !control->cancel_requested &&
        control->deadline_ms != 0 && GetTickCount64() >= control->deadline_ms) {
        control->timed_out = true;
        (void)ddd_winusb_cancel_request(io, control);
    }
    if (dispatched || timeout_us <= 0) return LIBUSB_SUCCESS;

    /* A Win32 wait accepts fewer handles than the RF queue has slots. Wait
     * for the oldest bulk submission and the optional independent FIFO read.
     * The common queue must consume RF blocks in submission order anyway. */
    ddd_winusb_request_t *oldest = NULL;
    for (size_t i = 0; i < GUI_DDD_ASYNC_TRANSFER_COUNT; ++i) {
        ddd_winusb_request_t *request = &io->requests[i];
        if (request->active && (!oldest ||
            request->submission_id < oldest->submission_id)) oldest = request;
    }

    HANDLE events[2];
    DWORD count = 0;
    if (oldest) events[count++] = oldest->overlapped.hEvent;
    if (control->active) events[count++] = control->overlapped.hEvent;
    if (count == 0) return LIBUSB_SUCCESS;

    DWORD timeout_ms = (DWORD)(timeout_us / 1000L + (timeout_us % 1000L != 0));
    if (WaitForMultipleObjects(count, events, FALSE, timeout_ms) == WAIT_FAILED) {
        DWORD error = GetLastError();
        ddd_winusb_note_error(io, error);
        return ddd_winusb_submit_error(error);
    }
    (void)ddd_winusb_dispatch_ready(io);
    return LIBUSB_SUCCESS;
}

gui_ddd_winusb_io_t *gui_ddd_winusb_io_create(void *file_handle,
                                            void *interface_handle)
{
    if (!file_handle || file_handle == INVALID_HANDLE_VALUE || !interface_handle) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    gui_ddd_winusb_io_t *io = calloc(1, sizeof(*io));
    if (!io) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    io->file = file_handle;
    io->interface_handle = interface_handle;
    for (size_t i = 0; i < DDD_WINUSB_SLOT_COUNT; ++i) {
        io->requests[i].overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!io->requests[i].overlapped.hEvent) {
            DWORD error = GetLastError();
            gui_ddd_winusb_io_destroy(io);
            SetLastError(error);
            return NULL;
        }
    }
    return io;
}

void gui_ddd_winusb_io_destroy(gui_ddd_winusb_io_t *io)
{
    if (!io) return;
    for (size_t i = 0; i < DDD_WINUSB_SLOT_COUNT; ++i) {
        if (io->requests[i].active) return;
    }
    for (size_t i = 0; i < DDD_WINUSB_SLOT_COUNT; ++i) {
        HANDLE event = io->requests[i].overlapped.hEvent;
        if (event) CloseHandle(event);
    }
    free(io);
}

void gui_ddd_winusb_io_attach(gui_ddd_winusb_io_t *io,
                             gui_ddd_async_config_t *config)
{
    config->submit_override = ddd_winusb_submit;
    config->submit_context = io;
    config->cancel_override = ddd_winusb_cancel;
    config->cancel_context = io;
    config->event_pump_override = ddd_winusb_pump;
    config->event_pump_context = io;
}

unsigned long gui_ddd_winusb_io_first_error(const gui_ddd_winusb_io_t *io)
{
    return io ? io->first_error : ERROR_SUCCESS;
}

#endif
