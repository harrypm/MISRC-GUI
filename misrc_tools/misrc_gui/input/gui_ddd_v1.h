/* MISRC GUI - isolated Domesday Duplicator firmware 3.1 backend. */

#ifndef GUI_DDD_V1_H
#define GUI_DDD_V1_H

#ifdef ENABLE_DDD

#include <stdbool.h>
#include <stdint.h>

#include "../../common/device_enum.h"
#include "../../common/ddd_protocol.h"

typedef struct gui_app gui_app_t;

typedef struct gui_ddd_v1_fifo_snapshot {
    ddd_fifo_telemetry_t latest;
    ddd_fifo_telemetry_totals_t totals;
} gui_ddd_v1_fifo_snapshot_t;

int gui_ddd_v1_open(gui_app_t *app, const char *stable_usb_path);
int gui_ddd_v1_start(gui_app_t *app, uint8_t decimation, bool test_mode);
void gui_ddd_v1_stop(gui_app_t *app);
bool gui_ddd_v1_is_active(void);
bool gui_ddd_v1_get_fifo_snapshot(gui_ddd_v1_fifo_snapshot_t *snapshot);

/* A failed/unverified B5 cleanup locks only the exact physical DDD 3.1 path.
 * Two complete enumerations must observe disappearance followed by reappearance
 * before that path is usable again. Other devices and DDD paths are untouched. */
void gui_ddd_v1_observe_enumeration(const misrc_device_list_t *devices);

#endif /* ENABLE_DDD */

#endif /* GUI_DDD_V1_H */
