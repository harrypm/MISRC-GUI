/* MISRC GUI - exact Windows device binding for the native DdD v1 transport. */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <winusbio.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "gui_ddd_winusb_device.h"
#include "gui_ddd_async.h"
#include "../../common/ddd_protocol.h"

struct gui_ddd_winusb_device {
    HANDLE file;
    WINUSB_INTERFACE_HANDLE interface;
};

/* GUID_DEVINTERFACE_USB_DEVICE, defined here for SDK/import-library portability. */
static GUID ddd_usb_device_interface = {
    0xa5dcbf10, 0x6530, 0x11d2, {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}
};

static wchar_t *ddd_winusb_interface_path(DEVINST instance, char *error, size_t error_size)
{
    wchar_t device_id[MAX_DEVICE_ID_LEN];
    CONFIGRET result = CM_Get_Device_IDW(instance, device_id, MAX_DEVICE_ID_LEN, 0);
    if (result != CR_SUCCESS || !device_id[0]) {
        snprintf(error, error_size, "Resolve selected DdD device instance (CM=%lu)",
                 (unsigned long)result);
        return NULL;
    }

    /* The device may disappear between the size and list calls. Never search
     * other devices if the selected instance no longer has a present path. */
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        ULONG length = 0;
        result = CM_Get_Device_Interface_List_SizeW(&length, &ddd_usb_device_interface,
                                                     device_id, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result != CR_SUCCESS || length < 2) break;
        wchar_t *paths = calloc((size_t)length, sizeof(*paths));
        if (!paths) {
            snprintf(error, error_size, "Allocate DdD WinUSB interface path");
            return NULL;
        }
        result = CM_Get_Device_Interface_ListW(&ddd_usb_device_interface, device_id, paths,
                                                length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result == CR_SUCCESS && paths[0]) {
            const wchar_t *end = wmemchr(paths, L'\0', length);
            if (end && end + 1 < paths + length && end[1] == L'\0') return paths;
        }
        free(paths);
        if (result != CR_BUFFER_SMALL) break;
    }
    snprintf(error, error_size, "Selected DdD has no unique present USB interface (CM=%lu)",
             (unsigned long)result);
    return NULL;
}

static bool ddd_winusb_configure(WINUSB_INTERFACE_HANDLE interface,
                                const char **stage, DWORD *error)
{
    ULONG timeout_ms = GUI_DDD_ASYNC_TELEMETRY_TIMEOUT_MS;
    *stage = "Set WinUSB control timeout";
    if (!WinUsb_SetPipePolicy(interface, 0, PIPE_TRANSFER_TIMEOUT,
                             sizeof(timeout_ms), &timeout_ms)) return false;

    USB_DEVICE_DESCRIPTOR device = {0};
    ULONG length = 0;
    *stage = "Read USB device descriptor";
    if (!WinUsb_GetDescriptor(interface, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                              (PUCHAR)&device, sizeof(device), &length)) return false;
    if (length != sizeof(device) || device.bLength != sizeof(device) ||
        device.bDescriptorType != USB_DEVICE_DESCRIPTOR_TYPE ||
        ddd_classify_device(device.idVendor, device.idProduct, device.bcdDevice) !=
            DDD_DEVICE_PROTOCOL_V1) {
        *error = ERROR_BAD_DEVICE;
        return false;
    }

    USB_INTERFACE_DESCRIPTOR descriptor = {0};
    UCHAR alternate = 0;
    *stage = "Read DdD USB interface settings";
    if (!WinUsb_QueryInterfaceSettings(interface, DDD_STREAM_ALTERNATE_SETTING, &descriptor) ||
        !WinUsb_GetCurrentAlternateSetting(interface, &alternate)) return false;
    if (descriptor.bInterfaceNumber != DDD_STREAM_INTERFACE_NUMBER ||
        descriptor.bAlternateSetting != DDD_STREAM_ALTERNATE_SETTING ||
        alternate != DDD_STREAM_ALTERNATE_SETTING) {
        *error = ERROR_BAD_DEVICE;
        return false;
    }

    bool found = false;
    *stage = "Find DdD 0x81 bulk endpoint with 1024-byte packets";
    for (UCHAR i = 0; i < descriptor.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe = {0};
        if (!WinUsb_QueryPipe(interface, DDD_STREAM_ALTERNATE_SETTING, i, &pipe)) return false;
        if (pipe.PipeId == DDD_STREAM_ENDPOINT_ADDRESS && pipe.PipeType == UsbdPipeTypeBulk &&
            pipe.MaximumPacketSize == DDD_STREAM_MAX_PACKET_SIZE) {
            found = true;
            break;
        }
    }
    if (!found) {
        *error = ERROR_BAD_DEVICE;
        return false;
    }

    ULONG maximum = 0;
    length = sizeof(maximum);
    *stage = "Query WinUSB bulk maximum transfer size";
    if (!WinUsb_GetPipePolicy(interface, DDD_STREAM_ENDPOINT_ADDRESS,
                             MAXIMUM_TRANSFER_SIZE, &length, &maximum)) return false;
    if (length != sizeof(maximum) || maximum < GUI_DDD_ASYNC_TRANSFER_BYTES) {
        *stage = "WinUSB maximum transfer size is below 128 KiB";
        *error = ERROR_BAD_LENGTH;
        return false;
    }

    UCHAR raw_io = TRUE;
    *stage = "Enable WinUSB RAW_IO";
    return WinUsb_SetPipePolicy(interface, DDD_STREAM_ENDPOINT_ADDRESS,
                                RAW_IO, sizeof(raw_io), &raw_io) != FALSE;
}

gui_ddd_winusb_device_t *gui_ddd_winusb_device_open(unsigned long device_instance,
                                                   char *error, size_t error_size)
{
    wchar_t *path = ddd_winusb_interface_path((DEVINST)device_instance, error, error_size);
    if (!path) return NULL;

    gui_ddd_winusb_device_t *device = calloc(1, sizeof(*device));
    if (!device) {
        snprintf(error, error_size, "Allocate DdD WinUSB device");
        free(path);
        return NULL;
    }

    const char *stage = "Open selected DdD WinUSB path";
    DWORD win32_error = ERROR_SUCCESS;
    device->file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (device->file == INVALID_HANDLE_VALUE) {
        win32_error = GetLastError();
        free(path);
        goto fail;
    }
    free(path);
    stage = "Initialize WinUSB interface";
    if (!WinUsb_Initialize(device->file, &device->interface)) goto fail;
    if (!ddd_winusb_configure(device->interface, &stage, &win32_error)) goto fail;
    return device;

fail:
    if (win32_error == ERROR_SUCCESS) win32_error = GetLastError();
    snprintf(error, error_size, "%s (Win32=%lu)", stage, (unsigned long)win32_error);
    gui_ddd_winusb_device_close(device);
    return NULL;
}

void gui_ddd_winusb_device_close(gui_ddd_winusb_device_t *device)
{
    if (!device) return;
    if (device->interface) WinUsb_Free(device->interface);
    if (device->file != INVALID_HANDLE_VALUE) CloseHandle(device->file);
    free(device);
}

void *gui_ddd_winusb_device_file_handle(gui_ddd_winusb_device_t *device)
{
    return device ? device->file : NULL;
}

void *gui_ddd_winusb_device_interface_handle(gui_ddd_winusb_device_t *device)
{
    return device ? device->interface : NULL;
}

int gui_ddd_winusb_device_control(void *context, uint8_t request_type,
                                  uint8_t request, uint16_t value, uint16_t index,
                                  uint8_t *data, uint16_t length)
{
    gui_ddd_winusb_device_t *device = context;
    WINUSB_SETUP_PACKET setup = {request_type, request, value, index, length};
    ULONG transferred = 0;

    if (!device) return -1;
    if (!WinUsb_ControlTransfer(device->interface, setup, data, length, &transferred, NULL)) {
        DWORD error = GetLastError();
        fprintf(stderr, "[DdD WinUSB] Control request=0x%02x value=0x%04x failed (Win32=%lu)\n",
                (unsigned)request, (unsigned)value, (unsigned long)error);
        return -1;
    }
    return (int)transferred;
}
