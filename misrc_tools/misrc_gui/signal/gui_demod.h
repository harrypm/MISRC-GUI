/*
 * MISRC GUI - Demodulator signal module + Demod panel
 *
 * Device-agnostic demodulation view. The panel reads live complex I/Q from the
 * display thread's latest frame (channel A = I, channel B = Q) and produces
 * demodulated audio for WFM / NFM / AM / USB / LSB. Demod audio is resampled
 * to a fixed 48 k (via libsoxr) and pushed to the audio monitor path so the
 * "Audio Mon" button plays it. Any I/Q-providing source (RTL-SDR live capture,
 * or I/Q FLAC playback) can drive it.
 *
 * The panel renders a scrolling waveform of the demodulated audio plus mode/
 * volume/squelch controls via the panel menu system.
 */

#ifndef GUI_DEMOD_H
#define GUI_DEMOD_H

#include <stdbool.h>
#include "../core/gui_app.h"
#include "../visualization/panel_interface.h"

//-----------------------------------------------------------------------------
// Demod modes (mirrors gui_settings_t.demod_mode)
//-----------------------------------------------------------------------------

typedef enum {
    DEMOD_MODE_WFM = 0,   // Wideband FM (broadcast FM): discriminate + 75us de-emphasis
    DEMOD_MODE_NFM = 1,   // Narrowband FM: discriminate, no de-emphasis
    DEMOD_MODE_AM  = 2,   // AM: magnitude + DC block + AGC
    DEMOD_MODE_USB = 3,   // Upper sideband
    DEMOD_MODE_LSB = 4,   // Lower sideband
    DEMOD_MODE_COUNT
} demod_mode_t;

//-----------------------------------------------------------------------------
// Panel Interface Registration
//-----------------------------------------------------------------------------

// Register the Demod panel vtable with the panel registry. Call once at startup.
void gui_demod_panel_register(void);

// True if the Demod panel is usable (always; DSP is self-contained).
bool gui_demod_available(void);

// --radio <key=value,...>: launch state of new Radio panels:
// tune=<Hz> (signed offset of the tuning line from the I/Q centre; k / M
// suffixes allowed). Prints the accepted list and returns false on error.
bool gui_demod_apply_launch_arg(const char *arg);
// The --radio keys that are settings (mode, bw, sql, vol, out, region, and
// the SDR's freq / band / gain / agc / rate): applied once after the
// settings load, before any capture opens the dongle.
void gui_demod_apply_launch_settings(gui_app_t *app);

#endif // GUI_DEMOD_H
