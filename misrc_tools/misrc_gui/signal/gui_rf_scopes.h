/*
 * MISRC GUI - Scopes on the RF Preview's decoded output (harrypm 2026-11-26:
 * "vectorscope should work on decoded RF Preview feeds when used on the
 * same channel", "same for waveform monitor").
 *
 * A Vectorscope or WFM panel on a channel whose Video panel runs the RF
 * Preview (VHS / S-VHS / Beta / ED Beta / LaserDisc, PAL or NTSC) reads the
 * decoder's own output instead of decoding the channel's raw samples as
 * composite (they are RF there): the RF decoder hands every rendered row -
 * time-base-corrected luma in IRE and burst-locked U / V - to this channel's
 * sink (gui_rf_scope_sink_t), which plots it:
 *   - Vectorscope: U / V of each row per the panel's Lines / Field / trace
 *     settings, in the scope's units (burst-normalised, like its own
 *     decoder), field by field with the ITU field parity.
 *   - WFM: gui_wfm_add_decoded_line (Y / C / Y+C rebuilt at the subcarrier).
 * Near-zero extra CPU: the decoder already computes these values to draw
 * its picture; the scope panels skip their own composite decode meanwhile.
 * Not on an RF feed: SECAM / MESECAM (separate renderer), Advanced-mode
 * burst readouts (the decoder output carries no burst window).
 *
 * Sinks are static per channel (never freed: no lifetime race with the RF
 * worker thread). Main thread: the panels fetch their channel's
 * accumulators each frame (marks them in use); gui_video_update_rf_routes
 * attaches the sink to that channel's RF worker while a scope uses it.
 */
#ifndef GUI_RF_SCOPES_H
#define GUI_RF_SCOPES_H

#include <stdbool.h>
#include "gui_rf_preview.h"
#include "gui_vectorscope.h"
#include "gui_wfm.h"

/* Main thread: this channel's RF-fed accumulators (initialised on first
 * use; marks them in use for ~0.5 s). NULL if allocation failed. */
gui_vectorscope_t *gui_rf_scopes_vectorscope(int channel);
gui_wfm_t *gui_rf_scopes_wfm(int channel);

/* The channel's sink while a scope uses it, else NULL (the provider given
 * to gui_video_set_rf_scope_provider). */
const gui_rf_scope_sink_t *gui_rf_scopes_sink(int channel);

#endif
