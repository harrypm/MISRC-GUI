/* MISRC GUI VHS RF preview wrapper. GPL-3.0-or-later.
 * DSP and resampling run exclusively in the bounded preview worker.
 * Raylib resources and the front RGBA copy belong exclusively to the UI.
 */
#include "gui_vhs_fm.h"
#include "gui_rf_preview_geometry.h"
#include "../visualization/gui_text.h"
#include "../ui/gui_ui.h"
#include "../output/gui_audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../../common/threading.h"
#define RF_RGBA_BYTES ((size_t)GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4)

bool gui_vhs_fm_init(vhs_fm_decoder_t *decoder) {
    if (!decoder) return false;
    memset(decoder, 0, sizeof(*decoder));
    atomic_init(&decoder->input_supported, true);
    decoder->format = VHS_FM_FORMAT_PAL;
    decoder->bound_channel = -1;
    return true; // No RF allocation or worker until this panel selects VHS FM.
}
bool gui_vhs_fm_bind_worker(vhs_fm_decoder_t *decoder, gui_rf_preview_worker_t *worker) {
    if (!decoder) return false;
    if (worker && !decoder->rgba) {
        decoder->rgba = calloc(1, RF_RGBA_BYTES);
        if (!decoder->rgba) return false;
    }
    if (!worker) {
        if (decoder->texture_valid) UnloadTexture(decoder->texture);
        decoder->texture_valid = false;
        free(decoder->rgba); decoder->rgba = NULL;
    }
    decoder->worker = worker;
    // Re-apply the stored frame display mode to each newly bound worker
    // (the DSP decoder preserves it across its own resets, but a fresh
    // worker starts in Active mode).
    if (worker && decoder->overlay.frame_mode == 1) {
        gui_rf_preview_worker_set_frame_mode(worker, true);
    }
    decoder->generation = 0;
    decoder->width = decoder->height = 0;
    decoder->texture_dirty = false;
    decoder->ld_tail_latched = false;
    decoder->displayed_frames = decoder->display_window_frames = decoder->display_window_ms = 0;
    decoder->displayed_generation = 0; decoder->display_fps = 0;
    memset(&decoder->stats,0,sizeof(decoder->stats));
    return true;
}
void gui_vhs_fm_cleanup(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;
    // Pool teardown releases the borrowed worker before this UI-only cleanup.
    if (decoder->texture_valid) UnloadTexture(decoder->texture);
    free(decoder->rgba);
    decoder->worker = NULL;
    decoder->rgba = NULL;
    decoder->texture_valid = false;
}
void gui_vhs_fm_reset(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;
    decoder->ld_tail_latched = false;
    gui_rf_preview_worker_reset(decoder->worker);
}
void gui_vhs_fm_set_format(vhs_fm_decoder_t *decoder, int format_select) {
    if (!decoder) return;
    // 0 = PAL, 1 = NTSC, 2 = native SECAM, 3 = MESECAM (a first-class TV
    // system for VHS RF: VHS "SECAM" tapes are MESECAM color-under; native
    // SECAM FM is the 2" quad / 1" Type-C domain).
    int system = format_select >= 0 && format_select <= 3 ? format_select : 0;
    gui_rf_family_t family = decoder->overlay.selected_tape;
    if (family == GUI_RF_FAMILY_BETAMAX) system = 1;
    if (family == GUI_RF_FAMILY_LASERDISC && system >= 2) system = 0;
    decoder->overlay.selected_system = system;
    // SECAM/MESECAM recordings currently use ordinary VHS modulation only.
    if (family == GUI_RF_FAMILY_VHS && system >= 2 &&
        decoder->overlay.vhs_preset == GUI_VHS_PRESET_SVHS)
        decoder->overlay.vhs_preset = GUI_VHS_PRESET_VHS;
    decoder->format = gui_vhs_fm_get_format(decoder);
    // The UI route update rebinds this subscriber; other subscribers keep their system.
}
void gui_vhs_fm_cycle_system(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;
    gui_rf_profile_t profile = gui_vhs_fm_get_profile(decoder);
    if (decoder->overlay.selected_tape == GUI_RF_FAMILY_LASERDISC ||
        gui_rf_profile_is_audio_only(profile) ||
        profile == GUI_RF_PROFILE_PAL_SVHS || profile == GUI_RF_PROFILE_NTSC_SVHS) {
        gui_vhs_fm_set_format(decoder, decoder->overlay.selected_system == 1 ? 0 : 1);
    } else {
        gui_vhs_fm_set_format(decoder, (decoder->overlay.selected_system + 1) % 4);
    }
}

void gui_vhs_fm_set_mesecam(vhs_fm_decoder_t *decoder, bool enabled) {
    if (!decoder) return;
    // Legacy API kept for callers that predate the 4-system selector:
    // maps onto the system value that now drives the profile.
    decoder->overlay.secam_mesecam = enabled;
    if (decoder->overlay.selected_system == 2 || decoder->overlay.selected_system == 3) {
        decoder->overlay.selected_system = enabled ? 3 : 2;
    }
    decoder->format = gui_vhs_fm_get_format(decoder);
}
void gui_vhs_fm_set_beta_preset(vhs_fm_decoder_t *decoder, int preset) {
    if (!decoder) return;
    decoder->overlay.beta_preset = preset >= 0 && preset < GUI_BETA_PRESET_COUNT
        ? (gui_beta_preset_t)preset : GUI_BETA_PRESET_BETAMAX;
    // Route updates rebind video only when the recording preset changes.
}
void gui_vhs_fm_set_vhs_preset(vhs_fm_decoder_t *decoder, int preset) {
    if (!decoder) return;
    decoder->overlay.vhs_preset = preset >= 0 && preset < GUI_VHS_PRESET_COUNT
        ? (gui_vhs_preset_t)preset : GUI_VHS_PRESET_VHS;
    // Selecting S-VHS from SECAM or MESECAM moves to PAL.
    if (decoder->overlay.selected_tape == GUI_RF_FAMILY_VHS &&
        decoder->overlay.vhs_preset == GUI_VHS_PRESET_SVHS &&
        decoder->overlay.selected_system >= 2)
        decoder->overlay.selected_system = 0;
    decoder->format = gui_vhs_fm_get_format(decoder);
}
const char *gui_vhs_fm_get_vhs_preset_name(const vhs_fm_decoder_t *decoder) {
    if (decoder && decoder->overlay.vhs_preset == GUI_VHS_PRESET_HIFI) return "Hi-Fi";
    return decoder && decoder->overlay.vhs_preset == GUI_VHS_PRESET_SVHS ? "S-VHS" : "VHS";
}
const char *gui_vhs_fm_get_beta_preset_name(const vhs_fm_decoder_t *decoder) {
    if (decoder && decoder->overlay.beta_preset == GUI_BETA_PRESET_ED_BETA) return "ED Beta";
    if (decoder && decoder->overlay.beta_preset == GUI_BETA_PRESET_SUPERBETA) return "SuperBeta";
    if (decoder && decoder->overlay.beta_preset == GUI_BETA_PRESET_BETAMAX_HIFI) return "Betamax Hi-Fi";
    return "Betamax";
}
gui_rf_profile_t gui_vhs_fm_get_profile(const vhs_fm_decoder_t *decoder) {
    if (decoder && decoder->overlay.selected_tape == GUI_RF_FAMILY_BETAMAX) {
        if (decoder->overlay.beta_preset == GUI_BETA_PRESET_ED_BETA)
            return GUI_RF_PROFILE_NTSC_ED_BETA;
        if (decoder->overlay.beta_preset == GUI_BETA_PRESET_SUPERBETA)
            // Keep Hi-Fi available without changing/relocking video when Audio Mon is toggled.
            return GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI;
        return decoder->overlay.beta_preset == GUI_BETA_PRESET_BETAMAX_HIFI
            ? GUI_RF_PROFILE_NTSC_BETAMAX_HIFI : GUI_RF_PROFILE_NTSC_BETAMAX;
    }
    if (decoder && decoder->overlay.selected_tape == GUI_RF_FAMILY_LASERDISC)
        return decoder->overlay.selected_system == 1
            ? GUI_RF_PROFILE_NTSC_LASERDISC : GUI_RF_PROFILE_PAL_LASERDISC;
    if (decoder && decoder->overlay.vhs_preset == GUI_VHS_PRESET_HIFI)
        return decoder->overlay.selected_system == 1
            ? GUI_RF_PROFILE_NTSC_VHS_HIFI : GUI_RF_PROFILE_PAL_VHS_HIFI;
    // TV system: 3 = MESECAM (VHS SECAM tapes), 2 = native SECAM FM
    // (2" quad / 1" Type-C domain) - the dedicated MESECAM toggle was
    // removed; the system selector owns the distinction (user 2026-11-03).
    if (decoder && decoder->overlay.selected_system == 3)
        return GUI_RF_PROFILE_MESECAM_VHS;
    if (decoder && decoder->overlay.selected_system == 2)
        return GUI_RF_PROFILE_SECAM_VHS;
    if (decoder && decoder->overlay.vhs_preset == GUI_VHS_PRESET_SVHS)
        return decoder->overlay.selected_system == 1 ? GUI_RF_PROFILE_NTSC_SVHS : GUI_RF_PROFILE_PAL_SVHS;
    return decoder && decoder->overlay.selected_system == 1 ? GUI_RF_PROFILE_NTSC_VHS : GUI_RF_PROFILE_PAL_VHS;
}
void gui_vhs_fm_set_frame_mode(vhs_fm_decoder_t *decoder, int mode_select) {
    if (!decoder) return;
    decoder->overlay.frame_mode = (mode_select == 1) ? 1 : 0;
    if (decoder->worker) {
        gui_rf_preview_worker_set_frame_mode(decoder->worker,
                                            decoder->overlay.frame_mode == 1);
    }
}
vhs_fm_format_t gui_vhs_fm_get_format(vhs_fm_decoder_t *decoder) {
    if (!decoder) return VHS_FM_FORMAT_UNKNOWN;
    gui_rf_profile_t profile = gui_vhs_fm_get_profile(decoder);
    return profile == GUI_RF_PROFILE_MESECAM_VHS ? VHS_FM_FORMAT_MESECAM
        : profile == GUI_RF_PROFILE_SECAM_VHS ? VHS_FM_FORMAT_SECAM
        : gui_rf_profile_format(profile) == GUI_RF_PAL ? VHS_FM_FORMAT_PAL : VHS_FM_FORMAT_NTSC;
}
const char *gui_vhs_fm_get_format_name(vhs_fm_decoder_t *decoder) {
    vhs_fm_format_t format = gui_vhs_fm_get_format(decoder);
    return format == VHS_FM_FORMAT_MESECAM ? "MESECAM"
        : format == VHS_FM_FORMAT_SECAM ? "SECAM"
        : format == VHS_FM_FORMAT_NTSC ? "NTSC" : "PAL";
}
void gui_vhs_fm_latch_ld_tail(vhs_fm_decoder_t *decoder) {
    // Copy the final picture while playback still holds this RF generation.
    // A new lap can start before render, so keep the copy until texture upload.
    if (gui_rf_preview_worker_take_frame(decoder->worker, decoder->rgba, RF_RGBA_BYTES,
            &decoder->generation, &decoder->width, &decoder->height, &decoder->stats)) {
        decoder->texture_dirty = true;
        decoder->ld_tail_latched = true;
    }
}
void gui_vhs_fm_swap_buffers(vhs_fm_decoder_t *decoder) {
    if (!decoder || !decoder->worker || decoder->ld_tail_latched) return;
    bool copied;
    if (gui_rf_profile_is_laserdisc(gui_vhs_fm_get_profile(decoder))) {
        uint64_t audio_sample = 0;
        bool clock_valid = gui_audio_get_ld_position(decoder->worker, &audio_sample);
        copied = gui_rf_preview_worker_take_ld_frame(decoder->worker, decoder->rgba, RF_RGBA_BYTES,
                    &decoder->generation, &decoder->width, &decoder->height, &decoder->stats,
                    clock_valid, audio_sample);
    } else {
        copied = gui_rf_preview_worker_take_frame(decoder->worker, decoder->rgba, RF_RGBA_BYTES,
                    &decoder->generation, &decoder->width, &decoder->height, &decoder->stats);
    }
    if (copied) decoder->texture_dirty = true;
}
// Draw one stats line, truncating with "..." when it is wider than the
// panel so long counters stay readable on narrow or split panels.
static void vhs_draw_stat_fitted(const char *text, float dx, float dy, int font_size,
                                 float max_width, Color color) {
    if (!*text || max_width <= 0.0f) return;
    char fit[224];
    size_t len = strlen(text);
    if (len >= sizeof(fit)) len = sizeof(fit) - 1;
    memcpy(fit, text, len);
    fit[len] = '\0';
    while ((float)gui_text_measure_mono(fit, font_size) > max_width) {
        if (len <= 3) return;
        --len;
        memcpy(fit + len - 3, "...", 4);
    }
    gui_text_draw_mono(fit, dx, dy, font_size, color);
}

static void vhs_draw_osd_backing(Rectangle panel, float pad, float first_line) {
    float inset = pad - 4.0f;
    float top = fmaxf(first_line, panel.y + pad) - 2.0f;
    float bottom = panel.y + panel.height - pad + 2.0f;
    if (panel.width > 2.0f * inset && bottom > top) {
        DrawRectangleRec((Rectangle){panel.x + inset, top, panel.width - 2.0f * inset, bottom - top},
                         (Color){0, 0, 0, 110});
    }
}

void gui_vhs_fm_render_frame(vhs_fm_decoder_t *decoder, float x, float y,
                             float width, float height, bool controls_hidden) {
    // Letterbox: the standard panel background (COLOR_PANEL_BG) so the
    // area outside the picture matches the rest of the GUI while still
    // reading clearly against the picture's own black/VBI — the previous
    // near-black fill made the sides indistinguishable from the picture
    // (user feedback 2026-11-03).
    Rectangle panel = {x, y, width, height};
    DrawRectangleRec(panel, COLOR_PANEL_BG);
    if (!decoder) return;
    const gui_rf_decode_status_t *status = &decoder->stats.decode;
    bool supported = atomic_load(&decoder->input_supported);
    gui_rf_profile_t profile = gui_vhs_fm_get_profile(decoder);
    // The renderer scales the panel and its glyphs together.
    const int font_size = FONT_SIZE_DROPDOWN_OPT;
    const float line_h = font_size + 6.0f;
    const float pad = 8.0f;
    const float max_width = width - pad * 2.0f;
    const float reason_y = y + height - pad - line_h;
    if (gui_rf_profile_is_audio_only(profile)) {
        if (controls_hidden || decoder->overlay.stats_view == 2 ||
            reason_y < y + pad || max_width <= 0.0f) return;
        char format[96];
        uint32_t input_rate = decoder->stats.source_rate_hz;
        uint32_t audio_rate = decoder->stats.vhs_hifi_rf_rate_hz;
        if (audio_rate && audio_rate != input_rate) {
            snprintf(format, sizeof(format), "%.3f -> 20 MSPS | 48 kHz stereo",
                     input_rate / 1000000.0);
        } else if (audio_rate) {
            snprintf(format, sizeof(format), "%.3f MSPS RF | 48 kHz stereo",
                     audio_rate / 1000000.0);
        } else {
            snprintf(format, sizeof(format), "%s | 48 kHz stereo", gui_rf_profile_name(profile));
        }
        const char *reason = !supported ? "Input rate unavailable" :
            decoder->stats.paused ? "Hi-Fi preview paused" :
            input_rate && !audio_rate ? status->reason :
            !decoder->stats.audio_enabled ? "Audio monitoring inactive" :
            decoder->stats.hifi.reason[0] ? decoder->stats.hifi.reason : "Waiting for Hi-Fi RF";
        float format_y = reason_y - line_h;
        bool show_format = format_y >= y + pad;
        vhs_draw_osd_backing(panel, pad, show_format ? format_y : reason_y);
        if (show_format) vhs_draw_stat_fitted(format, x + pad, format_y, font_size, max_width, COLOR_TEXT);
        vhs_draw_stat_fitted(reason, x + pad, reason_y, font_size, max_width,
                             decoder->stats.hifi.locked ? (Color){220,240,220,255}
                                                        : (Color){255,220,120,255});
        return;
    }
    if (decoder->texture_dirty && decoder->width > 0 && decoder->height > 0) {
        if (!decoder->texture_valid || decoder->texture.width != decoder->width ||
            decoder->texture.height != decoder->height) {
            if (decoder->texture_valid) UnloadTexture(decoder->texture);
            Image image = {.data = decoder->rgba, .width = decoder->width, .height = decoder->height,
                           .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            decoder->texture = LoadTextureFromImage(image);
            decoder->texture_valid = decoder->texture.id != 0;
            if (decoder->texture_valid) SetTextureFilter(decoder->texture, TEXTURE_FILTER_BILINEAR);
        } else {
            UpdateTexture(decoder->texture, decoder->rgba);
        }
        decoder->texture_dirty = false;
        decoder->ld_tail_latched = false;
    }
    // Fit rect (also drawn as the picture outline while acquiring, so the
    // future frame area is delineated even before the first lock).
    gui_rf_preview_rect_t frame = gui_rf_preview_fit_frame(
        decoder->width, decoder->height, x, y, width, height);
    if (decoder->width > 0 && decoder->height > 0 && frame.width > 0) {
        DrawRectangleLinesEx(
            (Rectangle){frame.x, frame.y, frame.width, frame.height},
            1.0f, COLOR_GRID_MAJOR);
    }

    // Display both original fields, in their original alternating rows.
    // Draw whenever a decoded frame EXISTS - including while sync is being
    // acquired or when the decoder system does not match the signal: hiding
    // the picture until lock would blank the wrong-decoder decode attempt,
    // which is a useful visualization (user report 2026-11-03). The reason
    // line below communicates the lock state.
    if (decoder->texture_valid && supported) {
        if (decoder->generation != decoder->displayed_generation) {
            decoder->displayed_generation = decoder->generation;
            decoder->displayed_frames++;
        }
        /* Full widens the sampled horizontal window as well as adding VBI.
         * Correct its display pixels so the active picture keeps its shape. */
        DrawTexturePro(decoder->texture,
                       (Rectangle){0, 0, (float)decoder->width, (float)decoder->height},
                       (Rectangle){frame.x, frame.y, frame.width, frame.height},
                       (Vector2){0, 0}, 0, WHITE);
    }
    uint64_t now = get_time_ms();
    if (!decoder->display_window_ms) decoder->display_window_ms = now;
    if (now < decoder->display_window_ms || now-decoder->display_window_ms >= 1000) {
        uint64_t elapsed = now > decoder->display_window_ms ? now-decoder->display_window_ms : 0;
        decoder->display_fps = elapsed ? (decoder->displayed_frames-decoder->display_window_frames)*1000.0/elapsed : 0;
        decoder->display_window_ms = now;
        decoder->display_window_frames = decoder->displayed_frames;
    }
    char line[224];
    int stats_view = decoder->overlay.stats_view;
    if (stats_view < 0 || stats_view > 2) stats_view = 0;
    // Stats: Off hides the WHOLE readout — the stats lines AND the reason
    // line below them (user feedback: the colour-lock reason line previously
    // lingered with all stats disabled). The reason line is part of the
    // same bottom-stacked block, so both share one gate.
    bool show_stats = !controls_hidden && stats_view != 2;
    bool show_basic_only = stats_view == 1;
    const bool show_hifi=gui_rf_profile_has_hifi(profile);
    const bool show_ld_audio=gui_rf_profile_is_laserdisc(profile);
    const int stat_lines = show_stats ? (show_basic_only ? 2 : 5) + (show_hifi || show_ld_audio ? 1 : 0) : 0;

    if (show_stats) {
        // Slight translucent backing behind the whole text block (stats
        // lines + reason line) so the yellow readout stays readable over
        // bright video — like an OSD backing on a production monitor. The
        // top follows the first drawn line: on very short panels the highest
        // lines are skipped, so clamp to the panel's pad edge.
        float ly = reason_y - line_h * (float)stat_lines;
        vhs_draw_osd_backing(panel, pad, ly);
        bool beta = decoder->overlay.selected_tape == GUI_RF_FAMILY_BETAMAX;
        snprintf(line, sizeof(line), "%s%s | %.3f -> %.0f MHz | H:%s V:%s Color:%s",
                 beta ? "NTSC " : "",
                 beta ? gui_vhs_fm_get_beta_preset_name(decoder)
                     : gui_rf_profile_name(gui_vhs_fm_get_profile(decoder)),
                 decoder->stats.source_rate_hz / 1000000.0,
                 gui_rf_profile_video_rate_hz(gui_vhs_fm_get_profile(decoder)) / 1000000.0,
                 status->h_locked ? "LOCK" : "SCAN", status->v_locked ? "LOCK" : "SCAN",
                 status->color_locked ? "LOCK" : "OFF");
        // Yellow OSD-style text (matching the reason line) over the backing
        // above: readable over both bright and dark video (user feedback).
        if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_size, max_width, (Color){255,220,120,255});
        ly += line_h;
        snprintf(line,sizeof(line),"Decode %.1f fps | Display %.1f fps | DSP %.1f ms | Age %.1f ms",
                 decoder->stats.fps,decoder->display_fps,decoder->stats.decode_ms,decoder->stats.latency_ms);
        if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_size, max_width, (Color){255,220,120,255});
        if (show_hifi || show_ld_audio) {
            ly += line_h;
            if (show_ld_audio) {
                snprintf(line,sizeof(line),"LD audio: %s | queue %llu",
                    !decoder->stats.audio_enabled ? "monitor off" :
                    decoder->stats.ld_audio_locked ? "LOCK" : "acquiring / muted",
                    (unsigned long long)decoder->stats.audio_blocks_dropped);
            } else {
                snprintf(line,sizeof(line),"Beta Hi-Fi: %s | switches %llu | dropouts %llu | queue %llu",
                     !decoder->stats.audio_enabled ? "monitor off" :
                     decoder->stats.hifi.locked ? "LOCK" : "calibrating / muted",
                     (unsigned long long)decoder->stats.hifi.head_switches,
                     (unsigned long long)decoder->stats.hifi.dropouts,
                     (unsigned long long)decoder->stats.audio_blocks_dropped);
            }
            if (ly >= y + pad) vhs_draw_stat_fitted(line,x+pad,ly,font_size,max_width,(Color){220,240,220,255});
        }
        if (!show_basic_only) {
            ly += line_h;
            snprintf(line,sizeof(line),"V candidates %llu | Fields %llu | Reject %llu (cad %llu/par %llu/map %llu)",
                     (unsigned long long)decoder->stats.total_vsync_candidates,
                     (unsigned long long)decoder->stats.total_fields_accepted,
                     (unsigned long long)decoder->stats.total_fields_rejected,
                     (unsigned long long)decoder->stats.total_reject_cadence,
                     (unsigned long long)decoder->stats.total_reject_parity,
                     (unsigned long long)decoder->stats.total_reject_line_map);
            if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_size, max_width, (Color){255,220,120,255});
            ly += line_h;
            snprintf(line,sizeof(line),"Queue drops %llu | Pressure skips %llu | Color loss %llu | CPU %.0f%% core",
                     (unsigned long long)decoder->stats.blocks_dropped,
                     (unsigned long long)decoder->stats.pause_drops,
                     (unsigned long long)decoder->stats.total_color_losses,decoder->stats.cpu_percent);
            if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_size, max_width, (Color){255,220,120,255});
            ly += line_h;
            snprintf(line,sizeof(line),"Last field rejection: %s",
                     status->last_reject_reason[0] ? status->last_reject_reason : "none");
            if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_size, max_width, (Color){255,220,120,255});
        }
    }
    // The reason/sync line is part of the overlay chrome: double-click hides
    // it, and Stats: Off hides it with the rest of the readout (user
    // feedback: it lingered showing "colour locked" with all stats off).
    if (show_stats) {
        const char *reason = !supported ? "Mixed-rate pair: independent input reader required" :
                         decoder->stats.paused ? "Preview paused: capture/recording pressure (frozen frame)" :
                         decoder->stats.budget_waiting ? "Preview yielding to the shared processing budget" : status->reason;
        if (reason_y >= y + pad)
            vhs_draw_stat_fitted(reason, x + pad, reason_y, font_size, max_width, (Color){255,220,120,255});
    }
}
void gui_vhs_fm_panel_register(void) {
    // Video panels subscribe to pooled RF workers fed continuously by extraction.
}
