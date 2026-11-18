/*
 * MISRC GUI - Composite Vectorscope Panel
 *
 * Its own panel view (PANEL_VIEW_VECTORSCOPE, "Vector" in the view picker),
 * like the other monitoring views. The panel runs the CVBS plugin's exact
 * composite decode chain on the channel's raw samples and plots the
 * burst-locked chroma of the active picture (see signal/gui_vectorscope.h).
 *
 * Display and controls follow tbc-tools' tbc-analyse vectorscope dialog:
 * Standard / Advanced mode (Advanced: no V-switch correction, both PAL
 * target sets, line window, field range, burst readouts), 75% / 100% / both
 * / off graticule (target boxes + zones, 2-degree ticks, NTSC I/Q axes),
 * point or density plot, mono / multi / blend colour, beam trace on / off,
 * intensity 1-10 (decode-orc's beam renderer).
 */
#ifndef GUI_VECTORSCOPE_PANEL_H
#define GUI_VECTORSCOPE_PANEL_H

#include <stdbool.h>
#include "panel_interface.h"

// Register the vectorscope panel vtable (call once at startup).
void gui_vectorscope_panel_register(void);

// --vector <key=value,...>: launch state of every vectorscope panel created
// after it (mode, lines, field, graticule, plot, colour, trace, intensity,
// system). Prints the accepted list and returns false on a bad key / value.
bool gui_vectorscope_panel_apply_launch_arg(const char *arg);

#endif // GUI_VECTORSCOPE_PANEL_H
