/* MISRC GUI VHS RF preview wrapper. GPL-3.0-or-later. */
#ifndef GUI_VHS_FM_H
#define GUI_VHS_FM_H
#include "../core/gui_app.h"
#include "../processing/gui_rf_preview_worker.h"
#include "raylib.h"
#include <stdatomic.h>

typedef enum { VHS_FM_FORMAT_UNKNOWN, VHS_FM_FORMAT_PAL, VHS_FM_FORMAT_NTSC,
               VHS_FM_FORMAT_SECAM, VHS_FM_FORMAT_MESECAM } vhs_fm_format_t;
typedef enum {
    GUI_VHS_PRESET_VHS = 0,
    GUI_VHS_PRESET_SVHS,
    GUI_VHS_PRESET_COUNT
} gui_vhs_preset_t;
// Video recording preset; Audio Mon controls Hi-Fi listening independently.
typedef enum {
    GUI_BETA_PRESET_BETAMAX = 0,
    GUI_BETA_PRESET_BETAMAX_HIFI,
    GUI_BETA_PRESET_SUPERBETA,
    GUI_BETA_PRESET_ED_BETA,
    GUI_BETA_PRESET_COUNT
} gui_beta_preset_t;
typedef struct vhs_fm_decoder {
    gui_rf_preview_worker_t *worker; // Borrowed from the channel/profile pool.
    int bound_channel, bound_system, bound_profile;
    uint64_t displayed_frames, display_window_frames, display_window_ms, displayed_generation;
    double display_fps;
    uint8_t *rgba;
    Texture2D texture;
    int width, height;
    bool texture_valid, texture_dirty;
    uint64_t generation;
    gui_rf_preview_stats_t stats;
    atomic_bool input_supported;
    vhs_fm_format_t format;
    struct {
        Rectangle button_rect, format_rect, budget_rect, tape_rect, beta_preset_rect;
        Rectangle tape_opts_rect[2];   // Format dropdown options: VHS / Betamax
        Rectangle vhs_preset_rect;
        Rectangle stats_btn_rect, stats_opts_rect[3];
        Rectangle frame_rect;
        bool is_visible, dropdown_open, stats_dropdown_open;
        bool tape_dropdown_open;      // Format (tape family) dropdown open
        // TV system: 0 = PAL, 1 = NTSC, 2 = native SECAM, 3 = MESECAM.
        // MESECAM is a first-class system here (VHS SECAM tapes are
        // heterodyne MESECAM); native SECAM stays for quad/1" sources.
        int selected_system;
        int selected_tape;            // 0 = VHS, 1 = Betamax (NTSC only)
        bool secam_mesecam;           // Legacy flag kept for API compat; the profile now keys off selected_system == 3
        gui_vhs_preset_t vhs_preset;   // VHS / S-VHS (PAL and NTSC)
        gui_beta_preset_t beta_preset; // Betamax / Betamax Hi-Fi / SuperBeta / ED Beta
        int stats_view;               // 0 = All, 1 = Basic, 2 = Off (OSD view)
        int frame_mode;               // 0 = Active, 1 = Full (VBI + burst view)
    } overlay;
    // controls_hidden + the double-click tracker live on video_panel_state_t
    // (gui_cvbs.c) so the hidden state is shared by all three decoder modes
    // and survives decoder-mode cycling; render_frame receives the flag.
} vhs_fm_decoder_t;
bool gui_vhs_fm_init(vhs_fm_decoder_t *decoder);
void gui_vhs_fm_cleanup(vhs_fm_decoder_t *decoder);
bool gui_vhs_fm_bind_worker(vhs_fm_decoder_t *decoder, gui_rf_preview_worker_t *worker);
void gui_vhs_fm_reset(vhs_fm_decoder_t *decoder);
void gui_vhs_fm_swap_buffers(vhs_fm_decoder_t *decoder);
// controls_hidden: the panel-wide chrome-hide flag (video_panel_state_t.chrome
// in gui_cvbs.c) — it hides the stats readout and the reason line too.
void gui_vhs_fm_render_frame(vhs_fm_decoder_t *decoder, float x, float y,
                             float width, float height, bool controls_hidden);
void gui_vhs_fm_set_format(vhs_fm_decoder_t *decoder, int format_select);
void gui_vhs_fm_set_mesecam(vhs_fm_decoder_t *decoder, bool enabled);
void gui_vhs_fm_set_vhs_preset(vhs_fm_decoder_t *decoder, int preset);
const char *gui_vhs_fm_get_vhs_preset_name(const vhs_fm_decoder_t *decoder);
void gui_vhs_fm_set_beta_preset(vhs_fm_decoder_t *decoder, int preset);
const char *gui_vhs_fm_get_beta_preset_name(const vhs_fm_decoder_t *decoder);
gui_rf_profile_t gui_vhs_fm_get_profile(const vhs_fm_decoder_t *decoder);
// Frame display mode: 0 = Active (default), 1 = Full (VBI + burst with
// decoded chroma). Persists across worker rebinds.
void gui_vhs_fm_set_frame_mode(vhs_fm_decoder_t *decoder, int mode_select);
vhs_fm_format_t gui_vhs_fm_get_format(vhs_fm_decoder_t *decoder);
const char *gui_vhs_fm_get_format_name(vhs_fm_decoder_t *decoder);
void gui_vhs_fm_panel_register(void);
#endif
