#ifndef GUI_RF_PREVIEW_H
#define GUI_RF_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUI_RF_DECODE_RATE_HZ 20000000u
#define GUI_RF_ED_BETA_RATE_HZ 20000000u
#define GUI_RF_FRAME_MAX_WIDTH 768
/* Full-frame mode weaves every field line: PAL needs 2x313 = 626 rows. */
#define GUI_RF_FRAME_MAX_HEIGHT 640

/* VHS/CVBS studio-swing display mapping for the ACTIVE area (Active and Full
 * modes alike). Beta profiles retain their validated full-range active mapping. tbc-tools' makeYuvRangeScale convention — black lifted above the
 * display floor like a production monitor, white with headroom, sync below
 * black clamped at the floor — punched up from the pure 16..235 transfer per
 * user GUI feedback (the picture read as washed out): blacks sit a little
 * deeper and whites a little hotter while ~4% footroom and headroom remain
 * (sync tip -40 IRE still clamps at 0, ~110 IRE overshoots still reach 255).
 * Single source shared by the renderer and the synthetic-test oracle so the
 * two can never drift apart. */
#define RF_DISPLAY_BLACK_255 10.0
#define RF_DISPLAY_WHITE_255 245.0
static inline double rf_display_level(double y01) {
    if (y01 < 0.0) y01 = 0.0;
    double level = (RF_DISPLAY_BLACK_255
                    + (RF_DISPLAY_WHITE_255 - RF_DISPLAY_BLACK_255) * y01) / 255.0;
    return level > 1.0 ? 1.0 : level;
}

/* Hybrid Full-mode background levels, in the IRE domain - shared by the RF
 * preview's profiles and the CVBS full-frame decode so the two decoders can
 * never drift apart. Translated from tbc-tools OutputWriter's fixed 16-bit
 * TBC offsets (commit 063d83e: NTSC 0x0d48, PAL 0x0708) through the canonical
 * VHS-composite IRE calibration in tbc-tools' sample metadata (NTSC 0 IRE
 * = 18048 / 100 IRE = 51200, PAL 0 IRE = 16384 / 100 IRE = 54016), then into
 * each decoder's own level domain. NTSC: (3400-18048)/331.52 = -44.1843 IRE;
 * PAL: (1800-16384)/376.32 = -38.7543 IRE. Like tbc-tools' full-range RGB
 * hybrid path, the span maps the whole demod range onto display 0..1:
 * 65535/331.52 = 197.680 IRE (NTSC), 65535/376.32 = 174.147 IRE (PAL).
 * Result: 0 IRE blanking renders as a ~57/255 dark grey band; NTSC sync
 * tips (-40 IRE) sit ~4 IRE above the background black and stay faintly
 * visible, while PAL's deeper tip clamps at the floor - the
 * CRT/production-panel VBI contrast of tbc-tools' hybrid mode.
 * Full-frame DISPLAY ASPECT now lives in gui_rf_preview_geometry.h
 * (gui_rf_preview_full_frame_aspect), shared with the CVBS full-frame
 * view so the two decoders' Full modes render identical geometry. */
#define RF_HYBRID_BLACK_IRE_PAL  (-38.7543)
#define RF_HYBRID_SPAN_IRE_PAL   (174.147)
#define RF_HYBRID_BLACK_IRE_NTSC (-44.1843)
#define RF_HYBRID_SPAN_IRE_NTSC  (197.680)

typedef enum { GUI_RF_PAL = 0, GUI_RF_NTSC = 1 } gui_rf_format_t;
/* Tape modulation profiles are independent of PAL/NTSC field geometry. */
typedef enum {
    GUI_RF_PROFILE_PAL_VHS = 0,
    GUI_RF_PROFILE_NTSC_VHS,
    GUI_RF_PROFILE_NTSC_BETAMAX,
    GUI_RF_PROFILE_NTSC_BETAMAX_HIFI,
    GUI_RF_PROFILE_NTSC_SUPERBETA,
    GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI,
    GUI_RF_PROFILE_SECAM_VHS,
    GUI_RF_PROFILE_MESECAM_VHS,
    GUI_RF_PROFILE_NTSC_ED_BETA,
    GUI_RF_PROFILE_COUNT
} gui_rf_profile_t;
typedef struct gui_rf_decoder gui_rf_decoder_t;

typedef struct {
    bool h_locked;
    bool v_locked;
    bool color_locked;
    uint64_t hsync_count;
    uint64_t vsync_count;
    uint64_t fields_decoded;
    double line_period_us;
    uint64_t frames_decoded;
    /* Reset-scoped diagnostic events. A V candidate is three consecutive
     * broad pulses at half-line cadence. Accepted fields have a measured line
     * map; rejected counts discarded candidate/reconstruction attempts, with
     * mutually exclusive cadence, parity/reference, and line-map reasons.
     * Color losses count transitions from locked to unlocked, not gray rows.
     * The worker accumulates deltas across resets for session totals. */
    uint64_t vsync_candidates;
    uint64_t fields_accepted;
    uint64_t fields_rejected;
    uint64_t reject_cadence;
    uint64_t reject_parity;
    uint64_t reject_line_map;
    uint64_t color_losses;
    char last_reject_reason[96];
    char last_color_loss_reason[96];
    char reason[96];
} gui_rf_decode_status_t;

/* RGBA is borrowed for the duration of the callback. Rows retain their
 * original field parity: no bob, interpolation, or deinterlacing is applied. */
typedef void (*gui_rf_frame_callback_t)(void *context, const uint8_t *rgba,
                                      int width, int height, uint64_t end_sample,
                                      const gui_rf_decode_status_t *status);

/* Invalid profiles are rejected; reset_profile leaves existing state intact. */
gui_rf_decoder_t *gui_rf_decoder_create_profile(gui_rf_profile_t profile);
void gui_rf_decoder_reset_profile(gui_rf_decoder_t *decoder, gui_rf_profile_t profile);
// RF input rate required by the decoder. All profiles use 20 MSPS; ED uses
// a separate wide-passband converter before its reduced-rate preview filters.
uint32_t gui_rf_profile_video_rate_hz(gui_rf_profile_t profile);
gui_rf_format_t gui_rf_profile_format(gui_rf_profile_t profile);
const char *gui_rf_profile_name(gui_rf_profile_t profile);
/* Only these profiles enable the separate Beta Hi-Fi audio path. */
bool gui_rf_profile_has_hifi(gui_rf_profile_t profile);
/* Compatibility entry points select the original VHS SP profiles. */
gui_rf_decoder_t *gui_rf_decoder_create(gui_rf_format_t format);
void gui_rf_decoder_destroy(gui_rf_decoder_t *decoder);
void gui_rf_decoder_reset(gui_rf_decoder_t *decoder, gui_rf_format_t format);
/* Frame display mode: Active (profile active lines/horizontal window, the
 * default) or Full (whole frame from the field boundary including the VBI,
 * with the horizontal window widened to the full line so the sync tip and
 * the color burst render with their decoded chroma). Full weaves every
 * field line, so the frame height is twice the taller parity's line count
 * (PAL 626, NTSC 526 rows). Unmeasured VBI lines (EQ/broad pulse rows
 * carry no H anchors) use nominal line positions. */
void gui_rf_decoder_set_frame_mode(gui_rf_decoder_t *decoder, bool full_frame);
// first_sample/count use the profile's video input rate. Frame callback
// end_sample always uses the common 20 MSPS media clock, including ED Beta.
void gui_rf_decoder_process(gui_rf_decoder_t *decoder, const float *samples,
                            size_t count, uint64_t first_sample,
                            gui_rf_frame_callback_t callback, void *context);
void gui_rf_decoder_get_status(const gui_rf_decoder_t *decoder,
                               gui_rf_decode_status_t *status);

#endif
