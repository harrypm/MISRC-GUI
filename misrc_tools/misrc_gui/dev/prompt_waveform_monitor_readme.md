# Prompt log: composite waveform monitor (tbc-analyse port)

Branch `vectorscope` (worktree `MISRC-GUI-web`), on top of 4c5a0c6.
Date: 2026-11-16. Status: implemented and tested on the Test Gen; awaiting
harrypm's check in the GUI (layout/wording sign-off per
`gui_ux_change_approval_rules.md`). Not committed.

## Input

> I want to also port over tbc-tools Wavefrom monitor

Source: tbc-tools `origin/main` `src/tbc-analyse/waveformmonitorwidget.{h,cpp}`
and `waveformmonitordialog.{h,cpp}` (GPL-3.0-or-later, itself ported from
decode-orc), read with `git show origin/main:...`. The local tbc-tools
checkout was 24 commits behind and was not pulled.

## What was ported

| tbc-analyse | MISRC-GUI WFM panel |
|---|---|
| Count raster: one column per sample of the window, 1 mV bins | One column per decode-rate sample (17.9 MHz PAL / 14.3 MHz NTSC), 512 bins over the same envelope (0.37 IRE / 2.6 mV per bin at PAL Y+C) |
| Accumulated over one frame | Same: published every second field |
| Area-max brightness, `min(1, (count x 5 x gain + 64) / 255)` | Same, via a 1024-entry colour table |
| Phosphor mode: (0,230,0) on black, grid (55,55,55), white axes | Same; off = the channel colour on the meter background |
| Channel Y+C / Y (default Y), Y = `(x[i-2]+x[i-1]+x[i]+x[i+1])/4` | Same filter, plus a C (chroma) channel (harrypm, 2026-11-17; see below). The button cycles Y -> Y+C -> C |
| Range Active video (default) / Whole frame | Active = picture lines, active window; Whole frame = every locked line, 1 us before 0H to 1 us before the next 0H |
| Intensity 0.1-10x, default 3.0x | Button cycles 0.5/1/2/3/5/10x; mouse wheel x1.1 per step |
| IRE / mV axis, major ticks 20 IRE / 100 mV | Same (Units button) |
| Envelope: Y+C = sync tip - 5% .. peak + 5%; Y = -100 mV .. white + 100 mV | Same |
| Level markers Sync tip / Blanking / Black / White / Peak, dash styles + alphas | Same; a label moves under its line when the next level up is too close (NTSC blanking vs 7.5 IRE setup) |
| Time axis: 1-2-5 ticks, ~7 divisions, "Time (us)" | Same ticks; times are us after 0H ("Time (us after 0H)") |

## Design (MISRC-specific)

- `signal/gui_wfm.{c,h}`: the accumulator. `panel_view_type_t` gains
  `PANEL_VIEW_WFM` ("WFM" in the view picker, `wfm` for `--panel-view`).
- `visualization/gui_wfm_panel.{c,h}`: the panel. It runs its own
  `gui_cvbs_feed`, scope-only like the Vector panel, sharing the
  per-frame resample cache.
- Decoder hook (`gui_cvbs.c` `decode_line_to_pixels`): each locked line is
  passed with the raw-sample ring and its 0H position, -8 samples -
  `line_off`, which is the pixel loop's own placement. `NULL` sink = no
  cost.
- **Levels.** tbc reads levels from the TBC metadata. A live capture has no
  calibrated volts, so:
  - each line is clamped at its own back porch, using the decoder's
    DC-restore windows; a broad-pulse line keeps the previous line's
    blanking;
  - the gain comes from the measured sync height. That is the field mean of
    the picture lines' tip-to-porch, with PAL/SECAM = 300 mV and NTSC =
    40 IRE. A source with compressed sync therefore reads high.
- **Column resampling.** An 8-tap Kaiser-windowed sinc (beta 5), 64-phase
  table.
  - Linear interpolation at ~4 samples per subcarrier cycle cut the chroma
    by up to 29% at half-sample phases. The PAL burst read 15.2 of 16.9 IRE.
- **Equivalent-time sampling.** Each line samples its columns at a
  golden-ratio offset within +-1 sample, which is half a subcarrier cycle.
  - NTSC's 227.5 cycles per line otherwise sample every column at two phases
    180 deg apart on every line. The bars drew crossing "rails" and the
    burst read 4.8 of 16 IRE.
  - +-0.5 sample was not enough: 13.9 of 16.
  - +-1 sample is +-56 ns PAL / +-70 ns NTSC, under a pixel on any panel
    narrower than the raster.
- **Raster layout.** Row-major by bin, so a line's increments at one level
  walk contiguous memory. The column-major first version cost ~2x on both
  threads.
- **Image build.** Two sequential passes:
  1. Per bin: each pixel column's max over its raster columns, skipping
     empty bins.
  2. Per image row: the max over its bins, written as one RGBA row.
- **C (chroma) channel.** Requested by harrypm: "Also add a dedicated C /
  Chroma mode to the WFM panel as it has Y / Y+C". tbc has no C mode; this
  follows a hardware monitor's CHROMA filter.
  - Filter: the fs/4 band-pass `[-1 0 2 0 -1]/4` applied twice, i.e.
    `[1 0 -4 0 6 0 -4 0 1]/16`, symmetric and centred on the column grid.
  - Response at the ~4x fsc decode rate is sin^4: unity at fsc, zero at DC
    and fs/2, luma -38 dB at 1 MHz. SECAM's 3.9-4.76 MHz carrier stays
    within 4%.
  - Cost: 5 multiply-adds per column, the same as Y.
  - No clamp offset; displayed around 0 IRE over +-70 IRE.
  - Markers: Blanking at 0, and the +-burst peaks (PAL 21.4, NTSC 20 IRE).
- **Automation (hard rule).** The flag is
  `--wfm channel=y|yc|c,range=active|frame,units=ire|mv,phosphor=on|off,intensity=0.1..10,system=auto|pal|ntsc|secam`.
  - Fails fast with the accepted list.
  - Logs `[AUTO] --wfm` and `[AUTO] WFM panel: ...`.
  - Entered in `gui_automation_flags_rules.md`.
- **Test Gen follow.** A per-view hook table (`gui_cvbs_set_feed_follow`)
  replaces the vectorscope-only pointer.
- **Test signal mode auto-switching (WFM and Vector, 2026-11-17).**
  harrypm: "add in test signal mode WFM and Vectorscope autoswitching ...
  like video decoder modes do (so it switches to PAL NTSC etc)".
  - Each panel's render step (UI thread) follows the generator's system
    (`app->settings.video_system`; MESECAM is SECAM on the composite side)
    while `gui_waveform_test_mode_active`: the Simulated device, not
    recording, no playback. That covers the Gen button, a Video panel's
    System pick (it changes the generator too) and `--gen-system`.
  - Manual System picks still hold until the generator next changes, as
    with the Video panels.
  - After a follow the decode thread holds Auto voting until a measured
    field period matches the followed system, or 250 fields (~5 s) pass.
    Before this the old system's tail in the pipeline voted the panel back:
    NTSC -> SECAM ended on PAL, and every PAL/NTSC change flipped three
    times.
  - Harness (scratchpad `follow_harness.c`, the real panel sources, an
    8-frame old-system tail): all six system pairs now land in one switch,
    on both panels.

## Results

`test/gui_wfm_test.c` (meson `gui_wfm`) drives the Test Gen bars at 40 MSPS
through the panel's own feed:

| Check | PAL | NTSC | SECAM |
|---|---|---|---|
| Y bar levels (worst error) | 0.11 IRE | 0.11 IRE | 0.34 IRE |
| Sync tip / back porch | -42.77 / -0.11 | -39.91 / +0.11 | -42.86 / -0.07 (mean) |
| Sync edge 50% vs 0H | -1 ns | 0 ns | -1 ns |
| Burst peak | 16.87 (want 16.93) | 15.97 (want 16.00) | n/a (FM) |
| Bar chroma peak, yellow / cyan | 26.70 / 37.64 (want 26.55 / 37.46) | n/a | n/a |
| C: bar chroma peak, yellow / cyan | 26.66 / 37.60 | n/a | n/a |
| C: bar mean (luma leak) / white bar | +0.01, -0.01 / +0.14 IRE | n/a | n/a |
| C: burst peak | 16.82 (want 16.93) | 16.00 (want 16.00) | n/a |

The SECAM Y bars are within 0.34 IRE, but the yellow column is wide. tbc's
4-tap average nulls fs/4 = 4.47 MHz, and the yellow bar's ~3.9 MHz carrier
passes at about -16 dB. That is inherited from tbc; only ~75% of the counts
sit within +-2 IRE.

Full suite: 59/60. `update_cli` is the known environmental failure on this
machine.

## CPU (single core, Test Gen at 40 MSPS, scratch `wfm_bench.c`)

| | PAL | NTSC | SECAM |
|---|---|---|---|
| Decode feed alone (resample + decode) | 11.4 ms/frame | 8.5 ms/frame | 11.6 ms/frame |
| `gui_wfm_add_line`, Y / Active | 1.95 us/line | 1.64 us/line | 2.03 us/line |
| `gui_wfm_add_line`, Y+C / Whole frame | 3.15 us/line | 2.64 us/line | 2.86 us/line |
| Image build 600x300 / 900x450 / 1600x800 (render thread, per new frame) | 0.65 / 1.0 / 2.1 ms | 0.65 / 0.97 / 2.0 ms | 0.76 / 1.2 / 2.5 ms |

- The monitor itself costs ~3% of a core in the default Y mode and ~5% in
  Y+C whole frame, only while a WFM panel is open.
- Nothing changes for existing panels: the decoder hook is a NULL check per
  line.

## Open / for harrypm

- Layout and wording need your sign-off (`gui_ux_change_approval_rules.md`):
  - two right-aligned button rows in the panel's top band;
  - the "IRE"/"mV" unit label above the axis instead of tbc's rotated
    title;
  - "Time (us after 0H)";
  - the marker label flip.
- Panel options are not persisted (tbc persists Phosphor via QSettings).
  They are runtime state like the Vector panel's; `--wfm` sets them at
  launch.
- Not verified on a live MS2130 capture yet. The levels come from the sync
  reference, so a capture chain with compressed sync reads high.

## Commands

```
# build (MSYS2 UCRT64 login shell)
PKG_CONFIG_PATH=<scratch>/fakepc:/c/Users/Harry/MISRC-GUI/.deps/install/lib/pkgconfig:/ucrt64/lib/pkgconfig
ninja -C build-ucrt                       # all targets
build-ucrt/gui_wfm_test.exe               # contract test
cd build-ucrt && meson test --no-rebuild  # suite (59/60)

# run
misrc_gui.exe --select-device Simulated --gen-system pal --auto-capture \
  --panel-view a:video,wfm [--wfm channel=yc,range=frame,phosphor=on,units=mv]
```
