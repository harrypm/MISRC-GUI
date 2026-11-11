# Prompt Log — Player transport icons, Res selector restore/fix/default, per-channel hold gates

Date: 2026-11-03 (continues `2026-11-03_waveform-render-resolution_README.md`)
Repo: `C:\Users\Harry\MISRC-GUI` (branch `main`, base commit `03d3a83`)
Working dir: `C:\Users\Harry\MISRC-GUI`
Build dir: `build-local` (meson/ninja, mingw-w64)

## Inputs (user requests, verbatim order)

1. "per channel dedicated control buttons were not added to the right side
   (aligned with file input name and time counters)"
2. "I wanted icons next to the slider makes senc."
3. "Resoltion control dropdown is broken i.g cant select it as it just closes
   not allowing selection"
4. "▶ is not showing but the button placement I am happy with"
5. "Good now set the deafult to fine and 32768 for player mode and allow for
   zoom on the paused waveform"
6. "also pause and load for spectrograph is broken"
7. "much better, commit and push"

## Work performed

### Per-channel transport buttons -> icons beside the scrub slider

- `gui_ui.c`: added the two-button group to each `render_playback_timeline_row`
  (channel A + B rows), first as text buttons at the row's right end, then
  reworked to compact ICON buttons placed DIRECTLY after the scrub track
  (inside the `PlaybackTimeline` container; stacked narrow rows keep them at
  the row's right end). Layout helper `gui_ui_playback_timeline_layout`
  budgets the group so the scrub track and info tiers adapt.
- Reused the `PlaybackPauseBtn`/`PlaybackLoopBtn` Clay IDs (channel-indexed)
  so the existing click handlers work unchanged; removed the settings-panel
  button rendering (duplicate-ID hazard) and moved the handler loop out of
  the `settings_panel_open` gate so the buttons work with the panel closed.
- `gui_custom_elements.h` + `clay_renderer_raylib.c`: new
  `PLAY_ICON` (filled triangle) and `PAUSE_ICON` (two bars) custom elements;
  loop reuses the existing `LOOP_ICON`. Icon convention: Play glyph when
  paused or not running, Pause only while playing (toolbar RecordButton
  convention).
- WINDING LESSON (one failed round): filled `DrawTriangle` glyphs must use
  the dropdown-arrow winding (negative screen-space cross product). The
  opposite winding rendered as NOTHING — the play glyph was invisible while
  the pause bars (rectangles) rendered fine. Loop arrowhead winding fixed
  the same (risk-free) way.

### Res dropdown "just closes" — out-of-bounds root cause

- `gui_oscilloscope.c`: `res_opts_rect` was declared
  `[WAVEFORM_RES_OPTION_COUNT]` (fine-group size, 10) but the dropdown holds
  Auto + group = 11 rows. While the FINE group's menu was open, the render
  path's `res_opts_rect[10]` write ran 16 bytes out of bounds onto
  `res_dropdown_open` (and `signal_type`/`signal_btn_rect`), so the open flag
  was overwritten with float bytes every frame and the menu closed itself
  before an option could be clicked. Coarse (6 rows) never overflowed.
- Fix: array `+1` slot; the click handler's `res_menu` rect now uses the
  ACTIVE group's row count (the old fine-sized rect counted an 80 px dead
  zone below a Coarse menu as "on menu").

### Player defaults + per-channel hold gates

- Res selector default is now Range Fine / Res 32768 on panel creation
  (player opens at max oversampling; presets clamp to DISPLAY_BUFFER_SIZE
  = 4096, so 8192+ entries are label-equivalent to 4096 — flagged to the
  user as a possible follow-up).
- `gui_oscilloscope.c` (waveform) and `gui_spectrogram.c` (analysis window
  + palette reload): the paused-behavior gates previously required the
  MASTER `PLAYBACK_STATE_PAUSED`. The per-channel transport pauses tracks
  WITHOUT flipping the master state, so per-channel pause left the
  spectrogram rolling (repeating the last frame) with the DAW analysis
  window never loading, and left scroll-zoom dead on a held track's frozen
  waveform. New held predicate: playback device + running + (master PAUSED
  or EOF, or master PLAYING with this channel's paused flag). STARTING/
  STOPPED sessions stay on the normal path.
- `gui_playback.c` (from the earlier uncommitted working tree, included
  here): per-channel transport ungated from Link: Separate — Synced = group
  pause/resume, Separate = per channel, EOF-no-loop pauses only that
  channel, master-pause resume via any track's Play; per-channel loop flags
  in both modes.

## Commands run

- `Get-Process misrc_gui -ErrorAction SilentlyContinue | Stop-Process -Force;
  meson compile -C build-local` (before every build; GUI kill is pre-required)
- `meson test -C build-local gui_spectrogram_playback_lifetime
  gui_waveform_overlay gui_playback_resolve --print-errorlogs`
  → 3/3 OK after adding the `gui_playback_is_running` /
  `gui_playback_is_channel_paused` stubs to
  `test/gui_spectrogram_playback_lifetime_test.c` (first link failed on them).

## Verification

- Builds clean at every round (only pre-existing warnings: vsprintf/vsnprintf
  shadow, unused `channel_value_color` at gui_ui.c:6558, unused
  `gui_ui_cxadc_base_rate_khz` at gui_ui.c:236).
- Targeted suites green: gui_waveform_overlay, gui_spectrogram_playback_lifetime,
  gui_playback_resolve.
- USER real-GUI verified incrementally: placement ("button placement I am
  happy with"), icons after the winding fix, Res dropdown selection after the
  overflow fix, default + paused zoom + spectrograph pause/load ("much
  better").

## Outputs

- Commit on `main` (this change) + push to
  `https://github.com/harrypm/MISRC-GUI.git`
- Dev note: `misrc_tools/misrc_gui/dev/dev_notes_README.md` (top section)
- Restore point: `restore_points/2026-11-03_player-transport-res-hold-gates-good.zip`
- Stray root `prompt_readme_*.md` files moved into `prompt_logs/` per the
  pre-commit tidy rule
- Notes copied to FUTO AI Notebook
