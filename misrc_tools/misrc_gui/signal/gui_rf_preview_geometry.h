/* RF preview display geometry. GPL-3.0-or-later. */
#ifndef GUI_RF_PREVIEW_GEOMETRY_H
#define GUI_RF_PREVIEW_GEOMETRY_H

/* Shared with the RF sampling profiles. Full mode samples the entire line
 * into the same number of columns as Active mode; its pixels therefore have
 * to be wider on screen to retain the active picture's 4:3 geometry. */
#define GUI_RF_PAL_ACTIVE_WIDTH 768
#define GUI_RF_PAL_ACTIVE_HEIGHT 576
#define GUI_RF_PAL_FULL_HEIGHT 626
#define GUI_RF_PAL_LINE_US 64.0
#define GUI_RF_PAL_ACTIVE_START_US 10.5
#define GUI_RF_PAL_ACTIVE_END_US 62.5
#define GUI_RF_NTSC_ACTIVE_WIDTH 640
#define GUI_RF_NTSC_ACTIVE_HEIGHT 480
#define GUI_RF_NTSC_FULL_HEIGHT 526
#define GUI_RF_NTSC_LINE_US 63.55555555555556
#define GUI_RF_NTSC_ACTIVE_START_US 9.45
#define GUI_RF_NTSC_ACTIVE_END_US 62.55555555555556

typedef struct {
    float x, y, width, height;
} gui_rf_preview_rect_t;

/* Full-frame display aspect for a given frame height (625/626 -> PAL
 * family, 525/526 -> NTSC family): the full line period spans more time
 * than the active window, so full-frame columns are wider in time than
 * active columns; the correction keeps the active picture 4:3 inside the
 * full frame. Shared by the RF preview's Full mode AND the CVBS composite
 * decoder's Frame: Full view so the two decoders' full-frame pictures get
 * the SAME on-screen geometry (user request 2026-11-03: "make the framing
 * more aligned with each other on the full-frame modes"). */
static inline double gui_rf_preview_full_frame_aspect(int height) {
    if (height == 625 || height == 626) {
        return (GUI_RF_PAL_LINE_US
                    / (GUI_RF_PAL_ACTIVE_END_US - GUI_RF_PAL_ACTIVE_START_US))
               * ((double)GUI_RF_PAL_ACTIVE_WIDTH / (double)GUI_RF_PAL_FULL_HEIGHT);
    }
    if (height == 525 || height == 526) {
        return (GUI_RF_NTSC_LINE_US
                    / (GUI_RF_NTSC_ACTIVE_END_US - GUI_RF_NTSC_ACTIVE_START_US))
               * ((double)GUI_RF_NTSC_ACTIVE_WIDTH / (double)GUI_RF_NTSC_FULL_HEIGHT);
    }
    return 4.0 / 3.0;
}

/* Use the delivered frame dimensions, not the requested frame mode/system:
 * the previous frame can remain visible while a worker applies a change.
 * PAL, native SECAM and MESECAM share geometry; all NTSC tape profiles share
 * the NTSC window. Active frames already contain square display pixels. */
static inline double gui_rf_preview_display_aspect(int width, int height) {
    if (width <= 0 || height <= 0) return 4.0 / 3.0;
    if (width == GUI_RF_PAL_ACTIVE_WIDTH && height == GUI_RF_PAL_FULL_HEIGHT) {
        return gui_rf_preview_full_frame_aspect(height);
    }
    if (width == GUI_RF_NTSC_ACTIVE_WIDTH && height == GUI_RF_NTSC_FULL_HEIGHT) {
        return gui_rf_preview_full_frame_aspect(height);
    }
    return (double)width / (double)height;
}

static inline gui_rf_preview_rect_t gui_rf_preview_fit_frame(
    int frame_width, int frame_height, float x, float y, float width, float height) {
    gui_rf_preview_rect_t rect = {x, y, 0.0f, 0.0f};
    if (width <= 0.0f || height <= 0.0f) return rect;
    double aspect = gui_rf_preview_display_aspect(frame_width, frame_height);
    rect.width = width;
    rect.height = (float)((double)width / aspect);
    if (rect.height > height) {
        rect.height = height;
        rect.width = (float)((double)height * aspect);
    }
    rect.x += (width - rect.width) * 0.5f;
    rect.y += (height - rect.height) * 0.5f;
    return rect;
}

#endif
