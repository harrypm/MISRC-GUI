/*
 * MISRC GUI - Composite Vectorscope Panel
 *
 * Its own panel view (PANEL_VIEW_VECTORSCOPE, "Vector" in the view picker),
 * like the other monitoring views. The panel runs the CVBS plugin's exact
 * composite decode chain on the channel's raw samples and plots the
 * burst-locked chroma of the active picture (see signal/gui_vectorscope.h).
 *
 * Display and controls follow tbc-tools' tbc-analyse vectorscope dialog:
 * 75% / 100% / off graticule (target boxes + zones, 2-degree ticks, NTSC
 * I/Q axes), point or density plot, mono or multi-colour points.
 */
#ifndef GUI_VECTORSCOPE_PANEL_H
#define GUI_VECTORSCOPE_PANEL_H

#include "panel_interface.h"

// Register the vectorscope panel vtable (call once at startup).
void gui_vectorscope_panel_register(void);

#endif // GUI_VECTORSCOPE_PANEL_H
