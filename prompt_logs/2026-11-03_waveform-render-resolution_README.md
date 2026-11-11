# Prompt Log — Waveform monitor render-resolution (Res) + range presets, playback only

Start: 2026-11-03
Working dir: `C:\Users\Harry\MISRC-GUI`
Branch: `main` (up to date with origin at b75f325)

## User input
- "I want to add a samples resolution selection mode for the resolution of the waveform, add this only to the player side for now"
- Clarifying answers (asked 2026-11-03):
  - Q: What should the resolution dropdown select? -> **"Number of waveform points drawn across the panel width (render resolution)"**
  - Q: What should the range-selection control select? -> **"Which preset resolutions the dropdown offers (a coarse/fine grouping of values)"**
  - Q: Checkbox or dropdown for the range control? -> **"Dropdown box"** ("Or dropdown box")
- "update internal dev docs for the waveform monitor feature"
- "This feature is just a resolution selection one" / "One more checkbox for range selection" / "Or dropdown box"
- "lets pick this back up" / "continue"

## Design (from user answers)
- Per-waveform-panel overlay dropdowns, playback device ONLY (DEVICE_TYPE_PLAYBACK selected + no test mode):
  - `Range:` coarse/fine preset group -> offers which Res presets are listed:
    - Coarse: 256 / 512 / 1024 / 2048 / 4096 points
    - Fine: 64 / 128 / 256 / 512 / 1024 / 2048 / 4096 / 8192 / 16384 / 32768 points
    - Default group: Coarse; default value: 4096 (matches today's behaviour: display buffer is 4096 points across the panel)
  - `Res:` number of waveform points drawn across the panel width (render resolution), independent of pixel width.
- Non-playback sources: the pair is not rendered and processing stays on today's exact path (display_width points across the panel = Auto behaviour).
- Automation rule check: per-panel runtime state only (no settings key) -> rule-6 exemption; the element still must be recorded in gui_automation_flags_rules.md inventory/gaps in the same change.

## Research (files inspected)
- `misrc_tools/misrc_gui/visualization/gui_oscilloscope.c` — waveform panel vtable: `waveform_panel_state_t`, `waveform_process_display` (decimation from zoom_scale, display_width = pixel width), `waveform_resample_to_buffer` (soxr decimation -> `display_samples[DISPLAY_BUFFER_SIZE=4096]`), render functions `render_waveform_line_internal` / `render_waveform_phosphor_internal` (1 sample per pixel), overlay dropdown machinery (Scale/Mode/Trig), test-mode-only Gen row pattern (`waveform_test_mode_active`, sim_sys_btn_rect click gating), `waveform_panel_handle_click` dispatch order (menu rects first), chrome-hide integration, DAW-style paused re-process (`config_dirty` + `s_trigger_src_*` last frame).
- `misrc_tools/misrc_gui/visualization/gui_phosphor_rt.c` / `.h` — `phosphor_rt_draw_waveform` draws sample i at x=i (1 px per point); phosphor RT is sized to panel (logical size), so a point count != pixel width needs an x-step variant.
- `misrc_tools/misrc_gui/core/gui_app.h` — `DISPLAY_BUFFER_SIZE 4096`, `waveform_sample_t` (float value), zoom limits 1..128 (max view = 128 * panel px; a 1920 px panel tops out at ~245k source samples -> 32768-point Res needs a >4x zoom-out beyond current max to be fully sourced).
- `misrc_tools/misrc_gui/processing/gui_display_thread.c` — display frames are 65536 samples/channel; `panel_process_all` dispatch.
- `misrc_tools/misrc_gui/input/gui_playback.c` — playback device type + DAW-style pause re-process path.
- `misrc_tools/misrc_gui/dev/gui_automation_flags_rules.md` — HARD rule: every GUI element needs a CLI flag/config key OR (rule 6) stays exempt as per-panel runtime state until persisted.
- `misrc_tools/misrc_gui/dev/README.md` — dev-doc conventions (this folder is the single home for dev notes; prompt logs in repo root `prompt_logs/`).

## Implementation (what was changed)
- `gui_oscilloscope.c`:
  - New per-panel state: `res_points` (0 = Auto -> display_width points across panel; else the selected preset), `res_range` (0 = Coarse, 1 = Fine), `res_dropdown_open`, `range_dropdown_open`, `res_opts_rect[]`, `range_btn_rect`, `range_opts_rect[]`.
  - New runtime cfg field `res_points` (captured under the state lock in `waveform_capture_runtime_cfg`), so the display-thread process path uses the same value the UI drew.
  - `waveform_process_display` reworked to compute `point_count = res_points ? min(res_points, DISPLAY_BUFFER_SIZE) : display_width` and treat the resample domain as **points across the panel**: decimation is scaled by `display_width/point_count`, trigger-display mapping (pre-trigger 10%, `trigger_display_pos`) uses point-domain x, and the raw-sample window spans the full panel width.
  - Line/phosphor renderers: points are drawn stretched across the panel width (`x = px * w / points_to_draw`) so the trace covers the full panel at any Res (phosphor via the new `x_step` API).
  - Trigger vertical marker: drawn at `x + pos * w / max(points-1, 1)` (point-domain x) in `draw_panel_trigger_markers`.
  - Overlay: a second row under the Scale/Mode/Trig group renders `[Range: Coarse|Fine] [Res: Auto|256|512|...]` — playback device ONLY (`gui_ui_selected_device_is_playback(app)` && !`waveform_test_mode_active(app)`); when not playback, rects are zeroed (click-inert) and the dropdown state is force-closed.
  - Click handling in `waveform_handle_control_click`: open/close toggles + option picks (Range sets group + clamps an incompatible Res pick; Res sets the point count, 0 = Auto) + click-outside closes; both close the other dropdowns; chrome-hide resets on consume, matching Scale/Mode/Trig.
  - Leaving playback (device switch, test mode): UI stops rendering the pair and processing returns to Auto (`res_points` stays but is only honoured when the pair is visible).
- `gui_phosphor_rt.c` / `.h`: new `phosphor_rt_draw_waveform_xstep` (per-point x step = w/(n-1)); `phosphor_rt_draw_waveform` kept as a 1.0-step wrapper (existing callers unchanged).

## Automation flags rule compliance
- Range/Res are per-panel runtime state (no `gui_settings_t` key added): rule-6 exemption applies. The element is recorded in `gui_automation_flags_rules.md` "Known gaps" in the same change (headless runs cannot set it today; a config key + flag would be added the moment it becomes persisted state).

## Build / verification
- `meson compile -C build-local` -> first run failed (typo `waveform_res_range_labels`
  vs the actual `s_res_range_labels`); fixed -> build reached [88/88],
  `misrc_gui.exe` linked (10:19 build contains the feature - string-verified:
  "Coarse" / "Range: %s" present in the exe bytes).
- `meson test -C build-local` (full suite): 43/46 pass. Failure triage with
  hard data:
  - `MISRC:update_cli` - PRE-EXISTING: fails on stashed baseline too
    (version-compare test: v9.9.9 vs dev-2026-11-03-b75f325-dirty; unrelated
    to this feature).
  - `MISRC:gui_rf_routes` - PRE-EXISTING: fails on stashed baseline too
    (MESECAM VHS profile assert; unrelated).
  - `MISRC:gui_rf_playback_start` - FLAKY: failed once in the full run,
    then PASSED twice with the feature build in place (seek-position assert;
    playback seek paths untouched by this change).
- Overlay harness test (`gui_waveform_overlay_test`) builds + passes with the
  reworked gui_oscilloscope.c.

## Output / results
### Files changed
- `misrc_tools/misrc_gui/visualization/gui_oscilloscope.c` - Res/Range state +
  presets + point-domain processing + playback-only overlay row + click paths.
- `misrc_tools/misrc_gui/visualization/gui_phosphor_rt.c` / `.h` -
  `phosphor_rt_draw_waveform_xstep` (stretched phosphor drawing); old
  `phosphor_rt_draw_waveform` kept as a 1 px-step wrapper.
- `misrc_tools/misrc_gui/dev/dev_notes_README.md` - new waveform-monitor
  feature section (2026-11-03, NOT-yet-user-verified status).
- `misrc_tools/misrc_gui/dev/gui_automation_flags_rules.md` - Known gaps
  entry for the Res/Range dropdowns (rule 6 exemption record).
- This prompt log.

## 2026-11-03 follow-up: scope-style resolution levels + clip scale
- User: "make the resolution info more oscilloscope like with resolution
  level instead of the current level" + "the clipping scale needs to be
  corrected for the new information level when higher resolution modes are
  used". Clarified: BOTH levels-based vertical axis AND per-division Res
  labels; the clip boundary must track the real rendering.
- `draw_channel_grid(amplitude_scale, pts_per_div)`: level modes 0-2 now
  tick raw 12-bit resolution LEVELS (256-level steps: Levels 0/+-1024/+-2048,
  Expanded binary 512-step, Full every 256); ticks are amplitude_scale-aware
  (fixes pre-existing label/trace misalignment when amplitude gain != 1);
  dashed CLIP rails mark the true +-2048 boundary whenever the gain puts it
  inside the panel; the time/div readout gains "NNNpts/div" when a fixed Res
  is active. Scale dropdown mode 0 renamed Basic -> Levels. Harness call
  sites updated; overlay harness 2348/2348 OK.

## 2026-11-03 follow-up 2: decimation ladder + peak-detect (user GUI feedback)
- User: "clipping is still triggering, and the res still shows 64-32768
  instead of just scale of decimation with full resolution being non[e]".
- Res ladder rework: dropdown = `None` (full resolution) + `2x..128x`
  decimation factors (Coarse: None/16x/64x; Fine: full ladder); factor D =
  display_width/D points across the same time window; the 32768->4096
  display-buffer clamp is gone.
- Clipping fix: decimation > 1 now uses PEAK-DETECT acquisition (per-point
  extremum of real samples) instead of soxr QQ lowpass - the filter's
  ringing was overshooting the rails and pinning the trace (false
  clipping). Trace rails only where the file genuinely clips; soxr kept
  for 1:1/upsample. Trade-off: honest envelope look at high decimation.
- Build OK (12:11 exe); 44/46 meson (only the 2 pre-existing baseline
  failures); overlay harness 2348/2348.

## NOT yet verified (user real-GUI test pending)
- Select the Playback device, load files, start playback:
  - Res/Range row appears on waveform panels (and does NOT appear on
    capture/test-gen sources).
  - Res = None looks identical to before; higher factors show a denser
    real envelope; the trace still spans the full panel and the SAME time
    window at every factor.
  - Range = Fine exposes None/2x..128x; Range = Coarse clamps a Fine-only
    pick to the largest coarse factor.
  - Trigger marker stays at 10% across at every Res; trigger + zoom + pause
    re-process still behave; chrome double-click hides the new row too.
  - Scale = Levels shows +-1024/+-2048-style labels; with amplitude gain < 1
    the dashed CLIP rails sit exactly where the waveform rails - and the
    trace no longer pins at the rails unless the file truly clips; the
    time/div readout shows NNNpts/div with a decimation Res.
