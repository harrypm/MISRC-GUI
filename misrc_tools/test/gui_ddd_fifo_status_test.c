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

static int check_layouts(const ddd_fifo_telemetry_t *latest,
                         const ddd_fifo_telemetry_totals_t *totals,
                         gui_device_buffer_severity_t severity,
                         const char *full, const char *compact,
                         const char *tiny)
{
    const char *captions[] = {full, compact, tiny};
    gui_device_buffer_view_t view;
    char details[sizeof(view.details)];

    for (int i = GUI_DEVICE_BUFFER_LAYOUT_FULL;
         i <= GUI_DEVICE_BUFFER_LAYOUT_TINY; ++i) {
        gui_ddd_fifo_make_buffer_view(
            latest, totals, (gui_device_buffer_layout_t)i, &view);
        CHECK(view.visible);
        CHECK(view.severity == severity);
        CHECK(view.meter_percent == ddd_fifo_peak_percent(latest));
        CHECK(strcmp(view.caption, captions[i]) == 0);
        CHECK(strlen(view.caption) <= 12);
        CHECK(view.details[0] != '\0');
        if (i == GUI_DEVICE_BUFFER_LAYOUT_FULL) {
            memcpy(details, view.details, sizeof(details));
        } else {
            CHECK(strcmp(view.details, details) == 0);
        }
    }
    return 0;
}

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
        DDD_DEVICE_UNSUPPORTED, true, true));
    CHECK(!gui_ddd_fifo_status_visible(
        DDD_DEVICE_NOT_DDD, true, true));
    CHECK(!gui_ddd_fifo_status_visible(
        DDD_DEVICE_PROTOCOL_V1, false, true));
    CHECK(!gui_ddd_fifo_status_visible(
        DDD_DEVICE_PROTOCOL_V1, true, false));

    ddd_fifo_telemetry_init(&latest);
    ddd_fifo_telemetry_totals_init(&totals);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_FULL, &view);
    CHECK(!view.visible);
    CHECK(view.caption[0] == '\0');
    CHECK(view.details[0] == '\0');

    latest.present = true;
    latest.depth_words = 16384;
    latest.packet_words = 8192;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_NORMAL,
                        "Idle", "Idle", "Idle") == 0);

    latest.used_now = 4000;
    latest.peak = 8192;
    latest.packets_read = 100;
    totals.peak_words = 8192;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_NORMAL,
                        "Peak 50%", "P50%", "P50%") == 0);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_FULL, &view);
    CHECK(view.meter_percent == 50);
    CHECK(strstr(view.details, "Current: 4000/16384 words\n"));
    CHECK(strstr(view.details, "Peak (interval): 8192/16384 words (50%)\n"));
    CHECK(strstr(view.details, "Peak (observed): 8192 words\n"));
    CHECK(strstr(view.details, "Normal packet threshold: 8192 words\n"));
    CHECK(strstr(view.details, "BP: 0% (observed max 0%)\n"));
    CHECK(strstr(view.details, "not time or loss rate."));
    CHECK(strstr(view.details, "Overflow total: 0\nLost words total: 0\n"));
    CHECK(strstr(view.details, "Coverage complete."));

    latest.peak = 12288;
    totals.peak_words = 12288;
    totals.peak_backpressure_percent = 50;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_WARNING,
                        "Peak 75% BP", "P75% BP", "BP") == 0);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(view.meter_percent == 75);
    CHECK(strstr(view.details, "BP: 50% (observed max 50%)\n"));
    CHECK(strstr(view.details, "Peak (interval): 12288/16384 words (75%)\n"));

    latest.peak = 16384;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_WARNING,
                        "Peak 100% BP", "P100% BP", "BP") == 0);

    totals.overflow_events = 2;
    totals.dropped_words = 128;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_ERROR,
                        "Loss", "Loss", "LOSS") == 0);

    /* Historic losses remain visible after the latest interval is healthy. */
    latest.peak = 8192;
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(view.severity == GUI_DEVICE_BUFFER_ERROR);
    CHECK(strcmp(view.caption, "Loss") == 0);
    CHECK(strstr(view.details, "Overflow total: 2\nLost words total: 128\n"));

    totals.saturated = true;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_ERROR,
                        "Loss", "Loss", "LOSS") == 0);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(strstr(view.details, "Overflow total: >=2\nLost words total: >=128\n"));
    CHECK(strstr(view.details, "SAT: counters saturated; totals are lower bounds."));
    totals.saturated = false;
    totals.interval_coverage_complete = false;
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(strstr(view.details, "Overflow total: >=2\nLost words total: >=128\n"));
    CHECK(strstr(view.details,
                 "Coverage incomplete: missed intervals; totals are lower bounds."));
    CHECK(!strstr(view.details, "SAT:"));

    /* An overflow without a reported word count must not read as zero loss. */
    totals.dropped_words = 0;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_ERROR,
                        "Overflow", "Overflow", "OVF") == 0);

    totals.overflow_events = 0;
    totals.dropped_words = 1;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_ERROR,
                        "Loss", "Loss", "LOSS") == 0);

    /* The longest hardware values and counters must retain both limitations. */
    latest.used_now = UINT16_MAX;
    latest.peak = UINT16_MAX;
    latest.depth_words = UINT16_MAX;
    latest.packet_words = UINT16_MAX - 1;
    totals.peak_words = UINT16_MAX;
    totals.peak_backpressure_percent = 100;
    totals.overflow_events = UINT64_MAX;
    totals.dropped_words = UINT64_MAX;
    totals.saturated = true;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_ERROR,
                        "Loss", "Loss", "LOSS") == 0);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_FULL, &view);
    CHECK(view.meter_percent == 100);
    CHECK(strcmp(view.details,
        "Current: 65535/65535 words\n"
        "Peak (interval): 65535/65535 words (100%)\n"
        "Peak (observed): 65535 words\n"
        "BP: 100% (observed max 100%)\n"
        "Normal packet threshold: 65534 words\n"
        "BP = peak above this threshold,\n"
        "scaled to remaining capacity; not time or loss rate.\n"
        "Overflow total: >=18446744073709551615\n"
        "Lost words total: >=18446744073709551615\n"
        "SAT: counters saturated; totals are lower bounds.\n"
        "Coverage incomplete: missed intervals; totals are lower bounds.") == 0);
    CHECK(strlen(view.details) < sizeof(view.details));

    totals.dropped_words = 0;
    CHECK(check_layouts(&latest, &totals, GUI_DEVICE_BUFFER_ERROR,
                        "Overflow", "Overflow", "OVF") == 0);

    gui_ddd_fifo_make_buffer_view(
        NULL, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(!view.visible);
    CHECK(view.details[0] == '\0');
    gui_ddd_fifo_make_buffer_view(
        &latest, NULL, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, &view);
    CHECK(!view.visible);
    CHECK(view.caption[0] == '\0');
    CHECK(view.details[0] == '\0');
    CHECK(view.meter_percent == 0);
    CHECK(view.severity == GUI_DEVICE_BUFFER_NORMAL);
    gui_ddd_fifo_make_buffer_view(
        &latest, &totals, GUI_DEVICE_BUFFER_LAYOUT_COMPACT, NULL);

    puts("DDD FIFO status presentation tests passed");
    return 0;
}
