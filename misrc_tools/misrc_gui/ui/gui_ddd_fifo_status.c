/* MISRC GUI - DdD FIFO adapter for the generic hardware-buffer view. */

#include "gui_ddd_fifo_status.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

bool gui_ddd_fifo_status_visible(ddd_device_profile_t profile,
                                 bool capture_active,
                                 bool telemetry_present)
{
    return profile == DDD_DEVICE_PROTOCOL_V1 && capture_active &&
           telemetry_present;
}

void gui_ddd_fifo_make_buffer_view(
    const ddd_fifo_telemetry_t *latest,
    const ddd_fifo_telemetry_totals_t *totals,
    gui_device_buffer_layout_t layout,
    gui_device_buffer_view_t *view)
{
    const char *floor_marker;
    int backpressure_percent;

    if (!view) return;
    memset(view, 0, sizeof(*view));
    if (!latest || !totals || !latest->present) return;

    view->visible = true;
    view->meter_percent = ddd_fifo_peak_percent(latest);
    backpressure_percent = ddd_fifo_backpressure_percent(latest);
    floor_marker = !totals->interval_coverage_complete ? "+" : "";

    if (totals->overflow_events > 0 || totals->dropped_words > 0) {
        view->severity = GUI_DEVICE_BUFFER_ERROR;
        if (layout == GUI_DEVICE_BUFFER_LAYOUT_TINY) {
            snprintf(view->caption, sizeof(view->caption),
                     totals->saturated ? "L%" PRIu64 "%s SAT"
                                       : "L%" PRIu64 "%s",
                     totals->dropped_words, floor_marker);
        } else if (layout == GUI_DEVICE_BUFFER_LAYOUT_COMPACT) {
            snprintf(view->caption, sizeof(view->caption),
                     totals->saturated
                         ? "O%" PRIu64 "%s L%" PRIu64 "%s SAT"
                         : "O%" PRIu64 "%s L%" PRIu64 "%s",
                     totals->overflow_events, floor_marker,
                     totals->dropped_words, floor_marker);
        } else {
            snprintf(view->caption, sizeof(view->caption),
                     totals->saturated
                         ? "%" PRIu64 "%s ovf, %" PRIu64 "%s lost (sat)"
                         : "%" PRIu64 "%s ovf, %" PRIu64 "%s lost",
                     totals->overflow_events, floor_marker,
                     totals->dropped_words, floor_marker);
        }
    } else if (latest->peak == 0 && latest->packets_read == 0) {
        snprintf(view->caption, sizeof(view->caption), "Idle");
    } else if (backpressure_percent > 0) {
        view->severity = GUI_DEVICE_BUFFER_WARNING;
        if (layout == GUI_DEVICE_BUFFER_LAYOUT_TINY) {
            snprintf(view->caption, sizeof(view->caption), "B%d%%",
                     backpressure_percent);
        } else if (layout == GUI_DEVICE_BUFFER_LAYOUT_COMPACT) {
            snprintf(view->caption, sizeof(view->caption), "BP %d%%",
                     backpressure_percent);
        } else {
            snprintf(view->caption, sizeof(view->caption), "%d%% pressure",
                     backpressure_percent);
        }
    } else if (layout == GUI_DEVICE_BUFFER_LAYOUT_TINY) {
        snprintf(view->caption, sizeof(view->caption), "P%d%%",
                 view->meter_percent);
    } else if (layout == GUI_DEVICE_BUFFER_LAYOUT_COMPACT) {
        snprintf(view->caption, sizeof(view->caption), "%u/%u peak",
                 (unsigned)latest->peak,
                 (unsigned)latest->depth_words);
    } else {
        snprintf(view->caption, sizeof(view->caption),
                 "now %u, peak %u/%u",
                 (unsigned)latest->used_now,
                 (unsigned)latest->peak,
                 (unsigned)latest->depth_words);
    }
}
