/*
 * MISRC Common - Device Enumeration
 *
 * Shared device discovery for CLI and GUI.
 * Supports both hsdaoh (libusb) and simple_capture (OS-native) devices.
 */

#ifndef MISRC_DEVICE_ENUM_H
#define MISRC_DEVICE_ENUM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*-----------------------------------------------------------------------------
 * Device Information
 *-----------------------------------------------------------------------------*/

#define DEVICE_NAME_MAX 256
#define DEVICE_ID_MAX 128

typedef enum {
    MISRC_DEVICE_TYPE_HSDAOH,         /* hsdaoh/libusb device */
    MISRC_DEVICE_TYPE_SIMPLE_CAPTURE, /* OS-native capture (V4L2, MediaFoundation, AVFoundation) */
#ifdef ENABLE_FX3
    MISRC_DEVICE_TYPE_FX3             /* Cypress FX3 USB device */
#endif
} misrc_device_type_t;

typedef struct {
    misrc_device_type_t type;
    int index;                          /* Index for hsdaoh, unused for simple_capture */
    char name[DEVICE_NAME_MAX];         /* Human-readable device name */
    char device_id[DEVICE_ID_MAX];      /* Device ID (for simple_capture) */
    bool supports_1080p60;              /* True if device supports 1920x1080 @ 60fps YUYV */
} misrc_device_info_t;

/*-----------------------------------------------------------------------------
 * Device List
 *-----------------------------------------------------------------------------*/

typedef struct {
    misrc_device_info_t *devices;     /* Array of device info */
    size_t count;                     /* Number of devices */
    size_t capacity;                  /* Allocated capacity */
} misrc_device_list_t;

/* Initialize device list
 *
 * @param list          List to initialize
 */
void misrc_device_list_init(misrc_device_list_t *list);

/* Free device list
 *
 * @param list          List to free
 */
void misrc_device_list_free(misrc_device_list_t *list);

/*-----------------------------------------------------------------------------
 * Device Enumeration
 *-----------------------------------------------------------------------------*/

/* Enumerate all available capture devices
 *
 * @param list          Device list to populate (will be cleared first)
 * @param include_hsdaoh        Include hsdaoh devices
 * @param include_simple_capture Include simple_capture devices
 * @return Number of devices found, or negative on error
 *
 * For simple_capture devices, only those supporting 1920x1080 YUYV at >=40fps
 * are included (matching CLI behavior).
 */
int misrc_device_enumerate(misrc_device_list_t *list, bool include_hsdaoh, bool include_simple_capture);

#ifdef ENABLE_FX3
/* Enumerate all available capture devices including FX3
 *
 * @param list          Device list to populate (will be cleared first)
 * @param include_hsdaoh        Include hsdaoh devices
 * @param include_simple_capture Include simple_capture devices
 * @param include_fx3           Include FX3 devices
 * @return Number of devices found, or negative on error
 */
int misrc_device_enumerate_fx3(misrc_device_list_t *list, bool include_hsdaoh,
                                bool include_simple_capture, bool include_fx3);
#endif

/* Get implementation name for simple_capture
 *
 * @return "MediaFoundation", "V4L2", or "AVFoundation" depending on platform
 */
const char *device_get_simple_capture_name(void);

/* Get short implementation name for simple_capture
 *
 * @return "mf", "v4l2", or "avf" depending on platform
 */
const char *device_get_simple_capture_short_name(void);

/*-----------------------------------------------------------------------------
 * Device List Output (for CLI)
 *-----------------------------------------------------------------------------*/

/* Print device list to stderr (CLI format)
 *
 * @param list          Device list to print
 */
void misrc_device_list_print(const misrc_device_list_t *list);

#endif /* MISRC_DEVICE_ENUM_H */
