# Prompt log: Spectro player-mode quick wins

Branch `spectro-player-quickwins` (worktree `MISRC-GUI-web`), on top of
c8f0268. Date: 2026-11-19. Status: committed and pushed to main 2026-11-19 at harrypm's request
("much better commit and push"). The running list is
`AI Notebook/MISRC_GUI_Improvements.md` (harrypm's FUTO notes).

## Input

> I like 4 but it should be Frames / Fields as each frame is 2 fields
> combined, 1/2/3 I like but also thinking about it making markers to mark
> cut points for FLAC-Chop use is also useful
>
> Make a MISRC_GUI_Improvements.md in my FUTO notes and let's start with the
> easyist/quickest to impliment features and work on the major ones later

## Done in this pass

- **Frames / fields span (paused window).**
  - The wheel steps run 1 field, then 1, 2, 3, 5, 10 and 25 frames
    (`s_span_fields`); above that, x1.25 seconds steps up to 60 s.
  - The old 0.1 s floor is gone.
  - The label reads `PAUSED  1 field` / `PAUSED  3 frames` /
    `PAUSED  3.00s window`.
  - The field length comes from `settings.video_system` (the Video panel's
    System dropdown writes it): 625 lines 20 ms, 525 lines 1001/60000 s.
    A field-unit span follows a system change.
  - Timecodes switch to milliseconds (m:ss.sss) for windows under 1 s.
- **Hover readout.**
  - The frequency and time under the mouse, on the axes' own scales:
    rolling shows seconds back from now; paused shows the file's absolute
    timecode.
  - Hidden with the chrome, while a dropdown is open, and over the
    dropdown buttons.
  - Level is not shown: the history stores LUT colours, and a float copy
    would add per-frame CPU / memory.

No new controls, so no new launch flags: the wheel span and the hover are
existing interactions. CPU: one text box per frame while hovered.

## Follow-up (same day)

> it would be nice to have frame/field locked with the decoder mode that is
> active, I dont see any 1 or 3 faems lable stuff
>
> no the labe does show just the pos cotext is not there

harrypm's answers: span control **both** (a dropdown, and pausing opens at
1 frame); the lock is **centred on the playhead**.

- **Span dropdown.** Left of the direction dropdown, shown only while paused.
  - Options: 1 field, 1 / 2 / 3 / 5 / 10 / 25 frames, 1 / 3 / 10 / 30 / 60 s.
  - The wheel steps the same list.
  - Pausing opens at 1 frame.
  - Flag: `--spectro span=1field|1frame|...|60s`.
- **Position context.** For frame / field spans, the axis labels read
  frame.field (`2103.1` = frame 2103, field 1, counted from the file start)
  and the hover reads `frame 2103 f1`. These are nominal until the field
  lock aligns the window to real syncs.
- **Next: the field lock** (the major item 6). The decoders do not track
  file positions, and the live feed drops blocks under load. So the analysis
  worker will find the fields in the paused window itself, the way the
  channel's active Video decoder mode would:
  - CVBS: the broad-pulse vertical-sync slicer;
  - RF Preview: FM demod for its format / system first.
  
  The window is then snapped centred on the playhead.

## Boundary markers, line scale, snapping, Heat default

> I need field and frame boundry markers
> also scale locking at frame / field accurate to lines
> so it clicks on these points when in frame/field level view
> set the deafult to heat map also

- **Boundary markers.** On frame / field spans: a thin line at each field
  start, a brighter, thicker one at each frame start (field 1), and f1 / f2
  tags where a field is at least 22 px.
- **Line scale.** Line ticks on the time edge (bottom; left when Vertical)
  every 1 / 5 / 10 / 25 / 50 / 100 lines, using the finest step at least
  6 px apart, every 5th tick longer. They count frame lines 1..625 / 525
  from each frame start. The hover reads `frame N fK Lnnn`.
- **Snapping.** On frame / field spans the window start snaps to the
  nearest field boundary, so the markers stay put and the window moves a
  field at a time. A field is a whole number of lines (312.5 / 262.5), so
  the scale is exact to lines. The request is resent only when the snapped
  window changes. The axis, markers and hover all use the snapped start.
  Read as "the view clicks onto field boundaries"; click-to-seek snapping
  is not added, pending harrypm.
- **Default palette** is Heat (`--spectro` usage updated).
- Boundaries count from the file start: nominal until the field lock keys
  them to real syncs.
- `test/gui_spectrogram_playback_lifetime_test.c` gains a
  `gui_playback_get_selected_file_info` stub (the paused window now reads
  the file rate).

## Field lock (from the active Video decoder mode)

> Not accurate positioning detection (screenshot: the nominal markers off
> the real field boundaries)

- **Two causes:**
  - The markers were nominal (counted from the file start).
  - The analysis worker clamped every window under 0.1 s up to 0.1 s
    (`PLAYBACK_ANALYSIS_SPAN_MIN_SECONDS`), so a 1-field / 1-frame request
    rendered 5 fields while the panel marked the short span. The floor is
    now 5 ms.
- **`signal/gui_field_locate.{c,h}`** (new, pure C, unit-tested):
  - **Input.** CVBS uses the samples. FM (RF Preview / Tape Decode) uses the
    instantaneous carrier frequency from interpolated zero crossings: every
    FM video format puts the sync tip at the lowest frequency.
  - **Slicer.** 0.5 / 99.5 % levels, slice 22 % up.
  - **Detection.** Broad-pulse trains (low runs of 0.25..0.62 line, at
    least 3 per train, a whole train only). The field start is the first
    broad pulse: 625 lines, line 1 / 313 by ITU numbering; 525 lines, 3 lines
    earlier (line 1 = the first pre-equalising pulse).
  - **Parity.** The train's phase against the last H sync before it: on the
    line grid = field 1, half a line off = field 2.
- **Wiring.**
  - The analysis worker feeds a locator in decode order for frame / field
    spans (it forces a sequential decode) and publishes the marks with the
    window: `gui_playback_request_analysis_window_fields`,
    `gui_playback_analysis_field_marks`.
  - The Spectro panel takes the mode and 625 / 525 from the channel's Video
    panel (`gui_video_panel_sync_source`); without one, CVBS on the Video
    System setting. Field / line lengths follow it.
  - The field lattice is anchored on a detected field 1 (`lattice_t0`).
    Snapping, frame.field and line numbers, the line ticks and the nominal
    fallback all use it. A window more than half a line off is requested
    again.
  - The markers draw the detected starts with their parity.
  - The label reads `PAUSED  1 frame  locked CVBS` / `locked FM` /
    `nominal`.
- **Tests.**
  - `gui_field_locate` (Test Gen, PAL / NTSC, CVBS / VHS RF, 20 / 40 MS/s,
    run started 0.3 field in): every field start found, 0.00 lines off,
    parity right.
  - On real captures (raw via `ffmpeg.exe`, 0.5 s): EBU PAL CVBS and the
    Hellsing PAL stress test give 25 starts each, every gap 312.50 lines,
    parity alternating.
  - FM is checked on the Test Gen only; no real VHS RF capture was found
    on disk.

## Arrow-key field step

> add arrow keys to move by field on a tap and slow frame advance on hold

- **Where.** `core/misrc_gui.c` (`gui_field_step_keys`, called beside the
  Space play / pause key).
- **Behaviour.**
  - Left / Right move every playing channel's playhead one field per tap
    (per-channel seek, each in its file's own samples).
  - Held for 0.4 s, they advance one frame every 150 ms.
  - Playing playback pauses on the first press.
- **Field length.** Channel A's Video panel system (625 / 525) through
  `gui_video_panel_sync_source`, else the Video System setting.
- **Editors.** The keys stand down while an editor owns the keyboard: the
  new `gui_ui_keyboard_captured()` covers text fields, the record-limit
  timecode, and the metadata / record-limit / version windows.
- **Flags.** A key binding, not a GUI control, so no launch flag.

## Verification

- Build clean; suite 58 / 59 (`update_cli` environmental;
  `gui_capture_watchdog` excluded, an upstream link break).
- GUI, player mode, `PAL/10sec/...Video_RF_8-bit_20msps.flac`: rolling
  Spectro with the hover readout showing `2.730 MHz  -0.371s` against axis
  labels to 10.0M.
- The paused window (field / frame steps, ms timecodes) needs a pause
  click: not yet checked.

## Next (quick items, from the list)

- Frequency range / zoom.
- Tape-format markers.
- dB range.
