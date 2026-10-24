/*
 * MISRC GUI - Waveform Panel
 *
 * Waveform panel vtable registration, grid drawing, and trigger detection.
 * Uses libsoxr for high-quality waveform resampling.
 */

#ifndef GUI_OSCILLOSCOPE_H
#define GUI_OSCILLOSCOPE_H

#include "../core/gui_app.h"
#include "raylib.h"
#include <sys/types.h>
#include "panel_interface.h"

//-----------------------------------------------------------------------------
// Panel Registration
//-----------------------------------------------------------------------------

void gui_waveform_panel_register(void);
/* Remote Monitor: populate the original waveform panel's state with reduced
 * on-wire ADC samples. The native renderer, overlay, zoom and phosphor remain
 * exactly the same implementation as in Local mode. */
struct gui_rm_scope_packet;
void gui_waveform_remote_apply(void *state, const struct gui_rm_scope_packet *packet,
                               int channel);


//-----------------------------------------------------------------------------
// Phosphor Parameters
//-----------------------------------------------------------------------------

#define SCOPE_DECAY_RATE        0.75f   // Per-frame decay (higher = slower fade)
#define SCOPE_HIT_INCREMENT     0.25f   // Intensity added per waveform hit
#define SCOPE_BLOOM             2.0f    // Bloom factor

//-----------------------------------------------------------------------------
// Cleanup
//-----------------------------------------------------------------------------

void gui_oscilloscope_cleanup(void);

//-----------------------------------------------------------------------------
// Grid Drawing
//-----------------------------------------------------------------------------

// Draw grid with amplitude scale ticks and time labels. The optional output
// reports the time/div label for overlay placement, or an empty rectangle.
// scale_mode selects the amplitude tick label format + density:
//   0 = Level (0.X, thinned 0.1/0.3/0.5/0.8)
//   1 = Basic (0.X, majors only +-0.5/0)
//   2 = Full  (0.X, all 0.1-0.8)
//   3 = mV w/2Vpp (mV labels, thinned ticks, Vpp=2.0)
//   4 = mV w/1Vpp (mV labels, thinned ticks, Vpp=1.0)
// channel (0=A, 1=B) is for bounds caching (unused now but kept for ABI).
void draw_channel_grid(float x, float y, float width, float height,
                       const char *label, Color channel_color, bool show_grid,
                       float zoom_scale, uint32_t sample_rate,
                       bool trigger_enabled, int trigger_display_pos,
                       Rectangle *time_div_rect,
                       int scale_mode, int channel);

//-----------------------------------------------------------------------------
// Trigger Detection
//-----------------------------------------------------------------------------

// Find first trigger point in sample buffer
// Returns sample index of trigger point, or -1 if no trigger found
ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig);

// Register the latest CH1/CH2 RF buffers for source-selectable trigger evaluation.
void gui_waveform_set_trigger_source_buffers(const int16_t *samples_ch1,
                                             const int16_t *samples_ch2,
                                             size_t count);

/* v5 shared ORIGINAL resampler+trigger output (CPU only; safe in headless). */
#include "../net/gui_remote_display.h"
size_t gui_waveform_server_display_points(const int16_t *src,const int16_t *a,
    const int16_t *b,size_t count,uint32_t sample_rate,
    const gui_rd_config_t *cfg,float out[GUI_RD_WIDTH],int32_t *trigger_pos);
void gui_waveform_remote_apply_points(void *state,const gui_rd_channel_t *data,
                                    uint32_t sequence);
void gui_waveform_remote_publish_config(void *state,int channel);

#endif // GUI_OSCILLOSCOPE_H
