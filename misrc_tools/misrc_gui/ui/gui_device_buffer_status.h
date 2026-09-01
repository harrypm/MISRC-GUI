/* MISRC GUI - device-neutral hardware-buffer status view. */

#ifndef GUI_DEVICE_BUFFER_STATUS_H
#define GUI_DEVICE_BUFFER_STATUS_H

#include <stdbool.h>

typedef enum gui_device_buffer_severity {
    GUI_DEVICE_BUFFER_NORMAL = 0,
    GUI_DEVICE_BUFFER_WARNING,
    GUI_DEVICE_BUFFER_ERROR
} gui_device_buffer_severity_t;

typedef enum gui_device_buffer_layout {
    GUI_DEVICE_BUFFER_LAYOUT_FULL = 0,
    GUI_DEVICE_BUFFER_LAYOUT_COMPACT,
    GUI_DEVICE_BUFFER_LAYOUT_TINY
} gui_device_buffer_layout_t;

/* Backends interpret their own telemetry and provide only presentation
 * semantics. This deliberately does not assume FIFO geometry, packet
 * thresholds, counter units, or any particular transport. */
typedef struct gui_device_buffer_view {
    bool visible;
    int meter_percent;
    gui_device_buffer_severity_t severity;
    char caption[64];
} gui_device_buffer_view_t;

#endif /* GUI_DEVICE_BUFFER_STATUS_H */
