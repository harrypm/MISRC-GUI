# Prompt Log — Mixed-rate playback file set kills RF Preview decoder (bug)

Start: 2026-11-03
Working dir: `C:\Users\Harry\MISRC-GUI`
Branch: `main` (b75f325 + uncommitted waveform Res/Range work in gui_oscilloscope.c / gui_phosphor_rt.*)

## User input
- "bug mixed rate file set inside of player mode stopes decoder from running (RF Preview)"

## Repro conditions (from user report, to confirm)
- Player (playback device) mode.
- A file set where CH A and CH B have DIFFERENT sample rates (mixed rate).
- Symptom: the RF Preview decoder does not run.

## Investigation (hard data)
- Gate chain: `gui_playback_commit` set `rf_preview_input_supported =
  !file_a || !file_b || rate_a == rate_b` (gui_playback.c:5714) ->
  `gui_video_submit_rf` resets workers and never submits samples when
  unsupported (gui_cvbs.c:5444) -> decoder starves; OSD shows the explicit
  reason (gui_vhs_fm.c:324). The restriction was a documented scope decision
  of commit 9842782 ("The paired reader currently requires equal physical
  clocks for both channels", rf_preview_README.md:78-80).
- Why the pairing breaks: the producer zips `encode_raw_sample(buf_a[i],
  buf_b[i])` - one A sample + one B sample per 32-bit word - and paces at
  `realtime_sample_rate_hz` = MAX(rate_a, rate_b). Mixed rates make the
  slower channel consume at the wrong clock (fast-forward) and the shared
  axis dishonest.
- Recording during playback is impossible (Record button = Pause/Resume
  there, gui_ui.c:10442), so a fix cannot touch archival paths.
- User decision (asked 2026-11-03): "Full independent reader: decouple the
  paired FIFO into per-channel streams" + "playing both either in sync or
  separate with controls on each panel" + "(next to file input play/pause &
  loop)".

## Fix (implemented)
- `common/buffer_manager.{h,c}`: new lazy per-channel FIFOs
  `BUF_CAPTURE_RF_A/B` + `BUF_DISPLAY_A/B` (policies mirror the paired
  buffers; budget builder caps them at RF/4 + display/4).
- `core/gui_app.h`: `playback_independent_mode`, `playback_channel_paused[2]`,
  `playback_link_separate` atomics.
- `input/gui_playback.c`: producer independent branch - per-channel writes
  (display-domain int16, no pairing), per-channel stamps
  (`gui_extract_stamp_playback_channel`), batch axis = faster file with
  remainder accumulators, per-channel EOF-without-loop pauses only that
  channel; commit activates the mode for mixed-rate sets (supported stays
  true), stop clears it; per-channel transport APIs + link mode +
  `channel_loop[2]`.
- `processing/gui_extract.c`: `extraction_thread_independent` - reads
  RF_A/RF_B, per-channel stats helper, per-channel preview submit with true
  rate/positions, forwards to DISPLAY_A/B, skips record; prepare_stream
  resets the new rings + flags; start dispatches on the mode.
- `processing/gui_display_thread.{c,h}`: `display_thread_independent` -
  reads DISPLAY_A/B, dispatches `panel_process_all` per channel with that
  channel's rate, publishes per-side counts (`sample_count_b` added).
- `ui/gui_ui.c`: `Link: Synced|Separate` toggle + Play/Pause and Loop
  buttons next to each playback file input row (player device only, dimmed
  without a running session), click handlers with status messages.

## Verification
- `meson compile -C build-local`: OK after fixing a thread-signature
  mismatch (`void*` ctx), a brace imbalance (paired display loop restored),
  and a CLAY_STRING-on-runtime-expression error (`make_string`).
- `meson test`: 44/46 OK. The 2 failures (`MISRC:update_cli`,
  `MISRC:gui_rf_routes`) are PRE-EXISTING - both fail on the stashed clean
  baseline (verified earlier today); `gui_rf_playback_start` and the full
  `gui_rf_preview` suite PASS with this change.
- NOT yet validated: user real-GUI run of a mixed-rate file set (decoders
  run on both channels; Synced vs Separate; per-row buttons; EOF pause).
  Per project rules: restore-point zip + log note only after user
  confirmation.
