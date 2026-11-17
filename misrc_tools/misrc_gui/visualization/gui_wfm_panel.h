/*
 * MISRC GUI - Composite Waveform Monitor Panel
 *
 * Its own panel view (PANEL_VIEW_WFM, "WFM" in the view picker): the
 * tbc-analyse waveform monitor (signal/gui_wfm.h) driven by its own
 * composite decode chain (gui_cvbs_feed, scope-only like the Vector panel).
 */
#ifndef GUI_WFM_PANEL_H
#define GUI_WFM_PANEL_H

#include <stdbool.h>

void gui_wfm_panel_register(void);

// --wfm <key=value,...>: launch defaults for WFM panels created after the
// call (channel=y|yc|c, range=active|frame, units=ire|mv, phosphor=on|off,
// intensity=0.1..10, system=auto|pal|ntsc|secam). Logs "[AUTO] --wfm" and
// each panel's applied state; an unknown key/value prints the accepted list
// and returns false (the caller exits 1).
bool gui_wfm_panel_apply_launch_arg(const char *arg);

#endif // GUI_WFM_PANEL_H
