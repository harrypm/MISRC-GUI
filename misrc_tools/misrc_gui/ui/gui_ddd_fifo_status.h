/* MISRC GUI - DdD FIFO adapter for the generic hardware-buffer view. */

#ifndef GUI_DDD_FIFO_STATUS_H
#define GUI_DDD_FIFO_STATUS_H

#include <stdbool.h>

#include "../../common/ddd_protocol.h"
#include "gui_device_buffer_status.h"

bool gui_ddd_fifo_status_visible(ddd_device_profile_t profile,
                                 bool capture_active,
                                 bool telemetry_present);
void gui_ddd_fifo_make_buffer_view(
    const ddd_fifo_telemetry_t *latest,
    const ddd_fifo_telemetry_totals_t *totals,
    gui_device_buffer_layout_t layout,
    gui_device_buffer_view_t *view);

#endif /* GUI_DDD_FIFO_STATUS_H */
