/*
 * MISRC GUI - head-switch trigger (Waveform "HS" trigger mode). See
 * gui_hs_trigger.c.
 */
#ifndef GUI_HS_TRIGGER_H
#define GUI_HS_TRIGGER_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Default acceptance: the step must be this many times the frame's median.
#define GUI_HS_TRIGGER_RATIO 8.0f

// Sample index of the head-switch envelope step in buf[min_index, max_index)
// (whole buffer used for the level statistics), or -1 when the frame holds
// none clearly above its typical step.
ssize_t gui_hs_trigger_find(const int16_t *buf, size_t count, size_t min_index,
                            size_t max_index, uint32_t rate_hz, float ratio);
#endif // GUI_HS_TRIGGER_H
