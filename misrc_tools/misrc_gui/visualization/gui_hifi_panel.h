/*
 * MISRC GUI - Hi-Fi Audio panel (PANEL_VIEW_HIFI, "HiFi" in the view picker).
 *
 * Realtime readout of the Hi-Fi audio decoded on CH3/4 (processing/
 * gui_hifi34.c): stereo L / R waveform lanes (min / max envelope per pixel
 * column, a line trace at short spans), peak dBFS per lane, the decoder's
 * source channel / format / lock and the carrier probe's scores.
 * harrypm 2026-11-25: "Add a realtime waveform readout to the decoded
 * hifi-audio so it visually shows something then just levels."
 *
 * Showing the panel is a use of the decoder: it runs while the panel is
 * up (and a source runs), even with CH1/2 monitored.
 *
 * Controls (right stack): Source (Auto / RF A / RF B), Format (Auto /
 * VHS PAL / VHS NTSC / Beta), Span (10 ms - 2 s; also the mouse wheel).
 * Flag: --hifi src=auto|a|b,fmt=auto|pal|ntsc|beta,span=<ms>.
 */
#ifndef GUI_HIFI_PANEL_H
#define GUI_HIFI_PANEL_H

#include <stdbool.h>
#include "panel_interface.h"

void gui_hifi_panel_register(void);

/* --hifi <key=value,...>. Span applies to panels created after it; src / fmt
 * are returned (-1 = not given) for the caller to put in the settings after
 * the settings load (flag > --config > default). False + usage on error. */
bool gui_hifi_panel_apply_launch_arg(const char *arg, int *source, int *format);

#endif
