/* Native Windows I/O for the shared DdD asynchronous queue. */
#ifndef GUI_DDD_WINUSB_IO_H
#define GUI_DDD_WINUSB_IO_H

#ifdef _WIN32

#include "gui_ddd_async.h"

typedef struct gui_ddd_winusb_io gui_ddd_winusb_io_t;

/* Borrows the overlapped file and WinUSB interface handles. Keep all three
 * alive until every request is reaped, including when the queue is orphaned. */
gui_ddd_winusb_io_t *gui_ddd_winusb_io_create(void *file_handle,
                                            void *interface_handle);
void gui_ddd_winusb_io_destroy(gui_ddd_winusb_io_t *io);
void gui_ddd_winusb_io_attach(gui_ddd_winusb_io_t *io,
                             gui_ddd_async_config_t *config);

/* First Win32 transport error, excluding normal cancellation and FIFO-only
 * failures. The common queue result separately describes the failed phase. */
unsigned long gui_ddd_winusb_io_first_error(const gui_ddd_winusb_io_t *io);

#endif
#endif
