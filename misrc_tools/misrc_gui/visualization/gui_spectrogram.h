/*
 * MISRC GUI - Real-time Spectrogram panel module ("Spectro")
 *
 * One FFT-derived scrolling 2D colour view with a readout-direction
 * dropdown (top-right, left of the palette dropdown, no label):
 *
 *   Horizontal - time on X scrolling left, frequency on Y with low freq
 *                at the bottom and high freq at the top (audio / speech
 *                spectrogram convention). The default.
 *   Vertical   - frequency on X, time scrolling down, newest at top
 *                (SDR / radio waterfall convention; the former Waterfall
 *                view, merged 2026-11-18).
 *
 * Both reuse the existing FFT engine (gui_fft_process_raw / fft_state_t) for
 * the per-frame magnitude spectrum, then map magnitude to the same heatmap
 * ramp used by the GPU phosphor compositor (black->blue->cyan->green->
 * yellow->red) so the colors match the rest of the application.
 *
 * Threading:
 *   - process() runs on the display thread (no OpenGL) and only computes the
 *     FFT into the embedded fft_state_t back buffer.
 *   - render() runs on the render/main thread (OpenGL available); it consumes
 *     the latest magnitude frame, appends it to a CPU history buffer, uploads
 *     to a Texture2D and draws it.
 *
 * Requires FFTW3 single-precision (fftw3f), same as the FFT panel.
 */

#ifndef GUI_SPECTROGRAM_H
#define GUI_SPECTROGRAM_H

#include <stdbool.h>
#include "../core/gui_app.h"
#include "panel_interface.h"

//-----------------------------------------------------------------------------
// Panel Interface Registration
//-----------------------------------------------------------------------------

// Register the Spectro panel vtable with the panel registry. Call once at startup.
void gui_spectrograph_panel_register(void);

// --spectro <key=value,...>: launch state of every Spectro panel created after
// it (dir=horizontal|vertical, palette=phosphor|heat; the palette is shared by
// all Spectro panels). Prints the accepted list and returns false on a bad
// key / value.
bool gui_spectrogram_apply_launch_arg(const char *arg);

// Set a Spectro panel's readout direction (state = the panel's view state;
// the legacy --panel-view name "waterfall" opens Spectro vertical).
void gui_spectrogram_set_vertical(void *state, bool vertical);

//-----------------------------------------------------------------------------
// Availability
//-----------------------------------------------------------------------------

// Returns true if the spectrogram panels are available (requires FFTW, like the FFT panel).
bool gui_spectrogram_available(void);

// The Spectro panels' current palette (Heat / Phosphor) as a 256-entry
// RGBA LUT, index = intensity 0..255 (the Radio panel's waterfall).
void gui_spectrogram_palette_lut(unsigned char lut[256][4]);

#endif // GUI_SPECTROGRAM_H
