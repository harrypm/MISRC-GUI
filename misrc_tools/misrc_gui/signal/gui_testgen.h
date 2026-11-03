/*
 * MISRC GUI - Test Signal Generator (Test Gen)
 *
 * Generates standards-accurate composite CVBS test signals (75% colour bars
 * with the hacktv test-card lower rows) for PAL, NTSC and SECAM, ON DEMAND.
 *
 * All timing/level/burst/subcarrier values are copied from hacktv
 * (https://codeberg.org/fsphil/hacktv, GPL-3.0-or-later, (c)2017-2026
 * Philip Heron) - the vid_config_pal / vid_config_ntsc / SECAM constants in
 * its src/video.c. Credit and thanks to Philip Heron; see
 * third_party/hacktv/COPYING. This project is GPL-3.0-or-later too, so the
 * values and formulas may be shared freely.
 *
 * The generator synthesises one FULL interlaced frame (both fields, correct
 * half-line offset) as a function of ABSOLUTE TIME, so no integer-samples-
 * per-line assumption exists (NTSC at 40 MSPS is 2542.22 samples/line - a
 * line-buffered generator would drift; this one cannot).
 */
#ifndef GUI_TESTGEN_H
#define GUI_TESTGEN_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    GUI_TESTGEN_PAL = 0,
    GUI_TESTGEN_NTSC = 1,
    GUI_TESTGEN_SECAM = 2,
    GUI_TESTGEN_SYSTEM_COUNT
} gui_testgen_system_t;

/* "PAL" / "NTSC" / "SECAM" */
const char *gui_testgen_system_name(gui_testgen_system_t system);

/* Samples in one full interlaced frame at the given rate (exact alloc size). */
size_t gui_testgen_frame_samples(gui_testgen_system_t system, uint32_t sample_rate);

/* Generate one full interlaced frame of composite CVBS into out[cap].
 * frame_index selects the frame in the stream (SECAM uses it for the
 * D'r/D'b line cadence, which alternates every frame, and for the per-line
 * 3-line carrier phase pattern; PAL/NTSC ignore it).
 * Returns the number of int16 samples written, 0 if cap is too small.
 * Output is a full-scale int16 composite signal (sync tip ~ -0.30, white
 * ~ +0.70 of full scale for PAL/SECAM; NTSC setup per hacktv). */
size_t gui_testgen_frame(gui_testgen_system_t system, int16_t *out, size_t cap,
                         uint32_t sample_rate, uint32_t frame_index);

/* Generate one full interlaced frame of VHS-family tape RF into out[cap].
 * Same slot-grid timing as gui_testgen_frame; PAL/NTSC carry FM luma plus
 * color-under QAM, native SECAM carries FM luma plus the divided-by-four
 * SECAM chroma FM. Physical parameters match the RF preview decoder
 * profiles, so the VHS FM panel decodes this signal. frame_index drives the
 * SECAM D'r/D'b cadence identically to the composite generator.
 * Returns the number of int16 samples written (same count as the composite
 * frame), 0 if cap is too small. Output amplitude ~0.72-0.87 of full scale. */
size_t gui_testgen_rf_frame(gui_testgen_system_t system, int16_t *out, size_t cap,
                             uint32_t sample_rate, uint32_t frame_index);

#endif /* GUI_TESTGEN_H */
