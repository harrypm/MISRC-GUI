#ifndef GUI_DDD_WINUSB_DEVICE_H
#define GUI_DDD_WINUSB_DEVICE_H

#include <stddef.h>
#include <stdint.h>

typedef struct gui_ddd_winusb_device gui_ddd_winusb_device_t;

/* device_instance is the Windows DEVINST of the exact selected DdD. */
gui_ddd_winusb_device_t *gui_ddd_winusb_device_open(unsigned long device_instance,
                                                   char *error, size_t error_size);
/* Only close after every native overlapped request has been reaped. */
void gui_ddd_winusb_device_close(gui_ddd_winusb_device_t *device);
void *gui_ddd_winusb_device_file_handle(gui_ddd_winusb_device_t *device);
void *gui_ddd_winusb_device_interface_handle(gui_ddd_winusb_device_t *device);

/* Synchronous vendor requests, used before or after the capture queue. */
int gui_ddd_winusb_device_control(void *context, uint8_t request_type,
                                  uint8_t request, uint16_t value, uint16_t index,
                                  uint8_t *data, uint16_t length);

#endif
