#include "../misrc_gui/ui/gui_ddd_fifo_status.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    ddd_fifo_telemetry_t latest;
    ddd_fifo_telemetry_totals_t totals;
    gui_device_buffer_view_t view;

    CHECK(gui_ddd_fifo_status_visible(
        DDD_DEVICE_PROTOCOL_V1, true, true));
    CHECK(!gui_ddd_fifo_status_visible(
        DDD_DEVICE_LEGACY, true, true));
    CHECK(!gui_ddd_fifo_status_visible(
        DDD_DEVICE_PROTOCOL_V1, false, true));
    CHECK(!gui_ddd_fifo_status_visible(
        DDD_DEVICE_PROTOCOL_V1, true, false));

    ddd_fifo_telemetry_init(&latest);
    ddd_fifo_telemetry_totals_init(&totals);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_FULL, &view);
    CHECK(!view.visible);

    latest.present = true;
    latest.used_now = 4000;
    latest.peak = 8192;
    latest.depth_words = 16384;
    latest.packet_words = 8192;
    latest.packets_read = 100;
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_FULL, &view);
    CHECK(view.visible);
    CHECK(view.meter_percent == 50);
    CHECK(view.severity == GUI_DEVICE_BUFFER_NORMAL);
    CHECK(strcmp(view.caption, "now 4000, peak 8192/16384") == 0);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_TINY, &view);
    CHECK(strcmp(view.caption, "P50%") == 0);

    latest.peak = 12288;
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(view.meter_percent == 75);
    CHECK(view.severity == GUI_DEVICE_BUFFER_WARNING);
    CHECK(strcmp(view.caption, "BP 50%") == 0);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_TINY, &view);
    CHECK(strcmp(view.caption, "B50%") == 0);

    totals.overflow_events = 2;
    totals.dropped_words = 128;
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(view.severity == GUI_DEVICE_BUFFER_ERROR);
    CHECK(strcmp(view.caption, "O2 L128") == 0);

    totals.saturated = true;
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(strcmp(view.caption, "O2 L128 SAT") == 0);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_TINY, &view);
    CHECK(strcmp(view.caption, "L128 SAT") == 0);
    totals.saturated = false;
    totals.interval_coverage_complete = false;
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(strcmp(view.caption, "O2+ L128+") == 0);
    gui_ddd_fifo_make_buffer_view(
        NULL, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(!view.visible);
    gui_ddd_fifo_make_buffer_view(
        &latest, NULL, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(!view.visible);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, NULL);

    puts("DDD FIFO status presentation tests passed");
    return 0;
}
