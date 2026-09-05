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
    floor_marker = totals->saturated || !totals->interval_coverage_complete
        ? ">=" : "";

    snprintf(view->details, sizeof(view->details),
             "Current: %u/%u words\n"
             "Peak (interval): %u/%u words (%d%%)\n"
             "Peak (observed): %u words\n"
             "BP: %d%% (observed max %d%%)\n"
             "Normal packet threshold: %u words\n"
             "BP = peak above this threshold,\n"
             "scaled to remaining capacity; not time or loss rate.\n"
             "Overflow total: %s%" PRIu64 "\n"
             "Lost words total: %s%" PRIu64 "\n"
             "%s%s",
             (unsigned)latest->used_now,
             (unsigned)latest->depth_words,
             (unsigned)latest->peak,
             (unsigned)latest->depth_words,
             view->meter_percent,
             (unsigned)totals->peak_words,
             backpressure_percent,
             totals->peak_backpressure_percent,
             (unsigned)latest->packet_words,
             floor_marker, totals->overflow_events,
             floor_marker, totals->dropped_words,
             totals->saturated
                 ? "SAT: counters saturated; totals are lower bounds.\n" : "",
             totals->interval_coverage_complete
                 ? "Coverage complete."
                 : "Coverage incomplete: missed intervals; totals are lower bounds.");

    if (totals->overflow_events > 0 || totals->dropped_words > 0) {
        view->severity = GUI_DEVICE_BUFFER_ERROR;
        if (totals->dropped_words > 0) {
            snprintf(view->caption, sizeof(view->caption), "%s",
                     layout == GUI_DEVICE_BUFFER_LAYOUT_TINY ? "LOSS" : "Loss");
        } else {
            snprintf(view->caption, sizeof(view->caption), "%s",
                     layout == GUI_DEVICE_BUFFER_LAYOUT_TINY
                         ? "OVF" : "Overflow");
        }
    } else if (latest->peak == 0 && latest->packets_read == 0) {
        snprintf(view->caption, sizeof(view->caption), "Idle");
    } else if (backpressure_percent > 0) {
        view->severity = GUI_DEVICE_BUFFER_WARNING;
        if (layout == GUI_DEVICE_BUFFER_LAYOUT_TINY) {
            snprintf(view->caption, sizeof(view->caption), "BP");
        } else if (layout == GUI_DEVICE_BUFFER_LAYOUT_COMPACT) {
            snprintf(view->caption, sizeof(view->caption), "P%d%% BP",
                     view->meter_percent);
        } else {
            snprintf(view->caption, sizeof(view->caption), "Peak %d%% BP",
                     view->meter_percent);
        }
    } else if (layout != GUI_DEVICE_BUFFER_LAYOUT_FULL) {
        snprintf(view->caption, sizeof(view->caption), "P%d%%",
                 view->meter_percent);
    } else {
        snprintf(view->caption, sizeof(view->caption), "Peak %d%%",
                 view->meter_percent);
    }
}
