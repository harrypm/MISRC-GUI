#ifndef MISRC_ANDROID_RTLSDR_USB_H
#define MISRC_ANDROID_RTLSDR_USB_H

#include <stdint.h>

#define ANDROID_RTL_USB_MAX_DEVICES 8
#define ANDROID_RTL_USB_SERIAL_SIZE 256

typedef struct {
    int id;                       /* Android UsbDevice.getDeviceId() */
    uint16_t vid;
    uint16_t pid;
} android_rtlsdr_usb_device_t;

typedef struct {
    android_rtlsdr_usb_device_t usb;
    char serial[ANDROID_RTL_USB_SERIAL_SIZE];
} android_rtlsdr_usb_identity_t;

unsigned android_rtlsdr_device_revision(void);
int android_rtlsdr_list_usb_devices(android_rtlsdr_usb_device_t *devices, int capacity);
void android_rtlsdr_set_open_identity(const android_rtlsdr_usb_identity_t *identity);
int android_rtlsdr_open_usb_fd(int device_id);
int android_rtlsdr_request_permission(int device_id);
int android_rtlsdr_permission_pending(void);
int android_rtlsdr_poll_permission(android_rtlsdr_usb_identity_t *identity, int *granted);

#endif
