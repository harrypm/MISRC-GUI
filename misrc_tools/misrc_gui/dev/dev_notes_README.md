# MISRC GUI development notes

## 2026-11-03 Waveform: test-gen stock trigger configs no longer leak into record/playback (USER-VERIFIED)

- User request: "remove the stock triggers on config from record/playback
  that was applied over from test signal mode". Root cause:
  `waveform_apply_channel_signal_default` assigned the Test-Gen mapping
  (CH1 -> CVBS with CVBS H-SYNC trigger ENABLED, CH2 -> FM RF with RISING
  trigger ENABLED) on a panel's first render/click UNCONDITIONALLY - device
  irrelevant - so record/playback panels inherited the generator's stock
  trigger configs.
- Fix (gui_oscilloscope.c): the generator mapping now applies ONLY while
  `waveform_test_mode_active(app)`; outside test mode any
  generator-applied (non-user-picked) signal config is cleared back to the
  stock trigger (off / rising / level 0, plain CH A / CH B). A signal type
  the user picked from the dropdown is tracked (`signal_user_picked`) and
  never overwritten. The simulated-start hook
  (`gui_waveform_apply_generator_defaults`) stays test-scoped as before.
- Verified: build + link OK (13:04 exe); 44/46 meson (only the two
  PRE-EXISTING baseline failures); overlay harness 2348/2348; the testgen
  RF suite passes. USER-VERIFIED in the real GUI 2026-11-03 ("fixed").
- HARD rule recorded per user request ("make it a hard defaults rule for
  base dev config"): `dev/gui_signal_default_rules.md` (mode-scoped
  default configs; device defaults never leak across modes; user picks
  never overwritten). Restore point:
  `restore_points/2026-11-03_player-display-trigger-defaults-good.zip`.

## 2026-11-03 Player decode: EVERY convention normalizes into the 12-bit container - in = out at 8/12/16-bit (NOT yet user-verified)

- User rule (final, 2026-11-03): "captures should always look like live ...
  8/12/16-bit in/out should match." The stock design already normalizes
  8-bit (<<4), 12-bit (as-is) and recorder-16-bit (>>4) into the 12-bit
  display container - those always round-tripped level-exact. The ONE
  convention that broke in = out was conv16 PASS (raw 16-bit passthrough
  with no container normalization).
- FINAL fix (after two superseded attempts - a per-channel-4096 display
  scale, then a "13-bit >>1 HALF" class, both WRONG for the real file
  class): the Tape_03 file is an 8-BIT capture externally RESAMPLED to
  20msps (user: "it is 8-bit data") - container-domain values stored raw
  in 16-bit, off the 16-grid (resample broke it) with ~+9% resampler
  ripple past the capture rails. Decode normalizes EVERY 16-bit convention
  into the container via the single shared `gui_playback_apply_conv16`
  (both FLAC write callbacks + the raw s16/WAV16 reader):
  - recorder 16-grid -> >>4 restore; container-domain raw (incl. resampled
    captures) -> pass through with overshoot CLAMPED to the container
    rails, mirroring the recorder's own resample clamp
    (convert_i16_to_flac_i32: "the soxr resampler can overshoot the 12-bit
    range ... Clamp to the signed 12-bit range"); full-range generic
    (peak > 4096) -> >>4 downscale.
  - Display/clip/VU/trigger consumers are on the SINGLE stock 2048
    reference everywhere (both session shapes identical);
    `rf_display_full_scale` stays 2048 for playback.
- New MISRC_DEBUG diagnostics (gated): [WFDISP] per-panel display values,
  [STATS] live peak/clip counters, [CLIPDBG] clip forensics (counted vs
  re-read values), [PACKDBG] packed word vs extracted value, conv16
  decision lines. These enabled fully automated playback verification
  without the GUI.
- Superseded experiments of 2026-11-03 (all REVERTED before this commit):
  waveform Res/Range dropdowns + levels axis + peak-detect (reverted to
  stock after user reports), per-channel 4096 display full scale,
  "13-bit HALF >>1" conv16 class, conv16 promotion-on-hot-block.
- Verified: build OK (13:23+ exe); 44/46 meson (only the two PRE-EXISTING
  baseline failures); gui_playback_resolve 1206/1206 with the new contracts
  (recorder >>4, <=2047 pass-through, 13-bit >>1, full-range >>4, silent-lead
  stickiness, raw/WAV readers). User real-GUI confirmation pending: Tape_03
  chop must show the SAME trace height loaded alone and in a mixed-rate set,
  clip counters at zero on the clean tape.
- TEST-FILE FORENSICS (user's Tape_03 file, 2026-11-03, hard data):
  `Diamler_Chrysler_NTSC_SP_Tape_03_video_rf_20msps.flac` (10.3 GB, mono
  FLAC16, header rate 20000 kHz-domain, NO Vorbis tags, wrapped STREAMINFO
  total): peaks -27 dBFS (max +1258/-1448), ZERO samples at any rail across
  0-390 s, 93.75% non-mod-16 -> 12-bit-domain values stored raw (Tape_03
  class -> decode pass-through = live container scale). Rate resolution
  verified correct: the msps hint parser only accepts a digit run
  immediately followed by "msps" (L:\000 and Tape_03 cannot win) ->
  20 MHz. 30-real-second chop for testing:
  `DC_NTSC_SP_head_20msps.flac` (600M samples; NOTE: ffmpeg -t is in HEADER
  seconds - 30 real s = -t 30000). Production decoder probe
  (gui_rf_preview_file, 20 MHz, unscaled): second 0 = no lock (tape
  head/pre-roll, "vertical broad pulses lack EQ cadence"); 15 s in =
  PERFECT (h/v/color locked, 119/119 fields, "NTSC VHS SP color locked",
  identical with/without x1.41 gain - the demod is amplitude-invariant).
  Instrumented GUI session (MISRC_DEBUG=1, --auto-capture): transport clean
  end-to-end (20 MHz runtime, true values, buffers 0%, clean loop/stop).
  VU meters normalize by 2048 (correct). Decoded picture luma histogram
  healthy (10-255 spread, mean 146) - picture levels fine.
- TEST-FILE FORENSICS (user's Tape_03 file, 2026-11-03, hard data):
  `Diamler_Chrysler_NTSC_SP_Tape_03_video_rf_20msps.flac` (10.3 GB, mono
  FLAC16, header rate 20000 kHz-domain, NO Vorbis tags, wrapped STREAMINFO
  total): peaks -27 dBFS (max +1258/-1448), ZERO samples at any rail across
  0-390 s, 93.75% non-mod-16 -> DIRECT-DOMAIN 16-bit (conv16 PASS). Rate
  resolution verified correct: the msps hint parser only accepts a digit
  run immediately followed by "msps" (L:\000 and Tape_03 cannot win) ->
  20 MHz. 30-real-second chop made for testing:
  `DC_NTSC_SP_head_20msps.flac` (600M samples; NOTE: ffmpeg -t is in HEADER
  seconds - 30 real s = -t 30000).
  Production decoder probe (gui_rf_preview_file, 20 MHz, unscaled):
  second 0 = NO lock ("vertical broad pulses lack EQ cadence" - tape
  head/pre-roll); 15 s in = PERFECT (h/v/color locked, 119/119 fields,
  "NTSC VHS SP color locked", unscaled AND x1.41 gain both fine - amplitude
  is NOT the issue). Paired-path value chain verified in code: decode PASS
  (+-1450) -> encode_raw_sample (no clamp at this amplitude) -> PLAIN
  extraction (get_conv_function(pad=0) = 2047-field, NO <<4) -> display
  70% height, clip counters 0. Verdict: the file and the current pipeline
  are clean; the tape's first seconds genuinely have no stable video
  (expect mush at position 0, color lock after the pre-roll).

## 2026-11-03 Waveform Res/Range + levels axis REVERTED and reimplemented source-scale-aware (NOT yet user-verified)

- User reports after the first implementation round: "completely broke it",
  "clipping is still wrong", and the key fact: "not all samples will be 12-bit
  why is this some baseline assumption?" - correct: the 12-bit assumption WAS
  the bug. The stock display pipeline crushes everything into a 12-bit
  container (paired playback clamps in encode_raw_sample), but the new
  INDEPENDENT mixed-rate pipeline carries native-domain samples, so a
  DIRECT-DOMAIN 16-bit file (conv16 PASS, +-2190-peak tape FLAC) overflowed a
  /2048 display -> pinned trace = the false clipping. The first round also
  changed the trace look drastically (soxr->peak-detect envelope) without
  real-GUI data.
- Action: reverted gui_oscilloscope.c/.h, gui_phosphor_rt.c/.h and the
  overlay harness to stock main, then reimplemented from the source truth:
  - NEW per-channel full scale `app->rf_display_full_scale[2]` (2048 12-bit
    container; 4096 when the playback conv16 detector resolves PASS on that
    channel; published by the producer; capture keeps the
    gui_app_level_autostop_full_scale backend value, e.g. DdD 512).
  - The waveform panel mirrors the per-channel full scale each render and
    uses it EVERYWHERE it used to hardcode 2048: resampler normalization,
    trigger-level/CVBS marker math, grid tick labels, clip rails.
  - Clip counters (independent extraction path) judge clipping against the
    channel's own rails; VU peak atomics stay normalized to the 12-bit
    convention for the existing meters.
  - Res dropdown = decimation scale: `None` (full resolution, one point per
    pixel) + `2x..128x` (Coarse: None/16x/64x; Fine: full ladder); factor D
    shows display_width/D points across the SAME time window (total
    decimation = zoom x D). NO peak-detect this time - the stock soxr
    resampling look is preserved at every setting.
  - Levels axis (Scale: Levels/Expanded/Full) ticks raw container levels
    (full_scale/16 units: 0, +-half, +-full; quarters; sixteenths),
    amplitude-scale aware, with dashed CLIP rails at the true full-scale
    boundary (clamped to the panel edge when the gain pushes them out).
  - pts/div readout next to time/div when a decimation Res is active.
- Verified: build + link OK (12:27 exe); 44/46 meson (only the two
  PRE-EXISTING baseline failures); overlay harness 2348/2348. User real-GUI
  confirmation pending - especially a DIRECT-DOMAIN 16-bit tape in the
  mixed-rate set (trace must sit inside the rails, clip counters must stay
  at zero on a clean tape).

## 2026-11-03 Player: independent per-file playback + per-channel transport (NOT yet user-verified)

- Bug (user report): a MIXED-RATE playback file set (CH A / CH B at different
  rates) stopped the RF Preview decoder from running. Root cause: the paired
  reader zipped both files sample-for-sample into one RF FIFO paced at the
  faster rate, so per-channel time was inconsistent and the session was
  deliberately gated (`rf_preview_input_supported=false`, OSD: "Mixed-rate
  pair: independent input reader required").
- Fix (user decision: "Player should run files independently ... either in
  sync or separate with controls on each panel"): INDEPENDENT mode activates
  at playback commit when both files have resolved, different rates:
  - Buffer manager gains per-channel FIFOs (lazy): `BUF_CAPTURE_RF_A/B`,
    `BUF_DISPLAY_A/B`. Capture devices and same-rate playback keep the paired
    paths unchanged.
  - The playback producer writes each file's display-domain int16 samples to
    its own RF FIFO at its own true clock (batch axis = the faster file;
    slower channel consumes a proportional count with remainder accumulators)
    and stamps per-channel rate/position metadata.
  - Extraction runs `extraction_thread_independent` (no unpacking, per-channel
    stats via `independent_channel_stats`, per-channel preview submit with
    true metadata, record path skipped - recording is impossible during
    playback). The display thread runs `display_thread_independent` and
    dispatches `panel_process_all` per channel with that channel's own rate.
  - Per-channel transport + Link mode: `Link: Synced|Separate` toggle plus
    Play/Pause + Loop buttons NEXT TO EACH playback file input row (settings
    panel, Playback device only). Synced = shared transport (legacy); Separate
    = per-file pause/loop via `playback_channel_paused[2]`/`channel_loop[2]`;
    a channel reaching EOF without loop pauses only itself. Per-channel freeze
    comes free from producer starvation (no worker changes). Scrub stays
    per-timeline in both modes (timeline rows are already per channel).
- Verified: build + link OK (44/46 meson: only the two PRE-EXISTING baseline
  failures `update_cli` + `gui_rf_routes`, reproduced on stashed baseline;
  `gui_rf_playback_start` + `gui_rf_preview` suites pass). User real-GUI
  confirmation pending.
- Prompt log: `prompt_logs/2026-11-03_mixed-rate-playback-rf-preview-bug.md`.

## 2026-11-03 Waveform monitor: render-resolution (Res) + Range presets, playback only (NOT yet user-verified)

- Feature (user request: "samples resolution selection mode ... only to the player
  side for now"): the waveform panel's resample domain is now POINTS ACROSS THE
  PANEL, selected by two overlay dropdowns on a second row (same slot as the
  test-mode Gen row, so the two are mutually exclusive):
  - `Range:` which preset group the Res dropdown lists - Coarse (256/512/1024/
    2048/4096) or Fine (64..32768, 10 presets). Default Coarse.
  - `Res:` how many waveform points the frame holds - Auto (= one point per
    pixel, the stock behaviour and the default) or the group's preset count.
- Playback device ONLY (`waveform_playback_res_visible`: selected device is
  DEVICE_TYPE_PLAYBACK and not test mode). Other sources zero the rects
  (click-inert) and process in Auto; the trace still covers the full panel at
  any Res (points are stretched across the panel width, phosphor via the new
  `phosphor_rt_draw_waveform_xstep`).
- Key mechanic (`waveform_process_display`): zoom_scale stays samples-per-pixel
  (scroll semantics unchanged, grid time/div unchanged); the resampler decimation
  is scaled by display_width/point_count and the trigger x position is computed
  in the point domain, then mapped back to pixels for the grid t=0 and the
  marker (the marker guard is `pos < samples_available`, NOT `< width`).
- State model: per-panel runtime state only (`res_range`, `res_points`), no
  `gui_settings_t` key -> automation rule 6 exemption; recorded in
  `gui_automation_flags_rules.md` Known gaps. Res changes set `config_dirty`
  so DAW-style paused re-processing keeps rendering live.
- Verification status: build + link OK (build-local, 88/88), overlay harness
  test OK; `gui_rf_routes` + `update_cli` meson failures are PRE-EXISTING
  (reproduced on stashed baseline); `gui_rf_playback_start` is flaky (failed
  once, passed twice with the feature in the exe; playback seek paths untouched
  by this change). User real-GUI confirmation still pending.
- Prompt log: `prompt_logs/2026-11-03_waveform-render-resolution_README.md`.

## 2026-10-20 SECAM FM-chroma decoder shipped (colour user-verified; one open follow-up)

- The tbc-tools secamdecoder pipeline ported to the streaming per-line decode
  (backlog item 4): bell-free picture demod (the cloche's phase bias measured
  at -19.5 kHz/+10 kHz on the belled carriers), porch calibration with the
  exact 10*fH pair lock + ident ladder, rails/concealment, and a CAUSAL
  pre-warped de-emphasis IIR carrying state across lines - the reference's
  COMPLEX inverseLf mask, not a magnitude-only FIR (the first port's zero-phase
  FIR bled bars into their neighbours by up to 26%; measured).
- Harness: physically-correct fixtures (pre-corrected + the encoder's deviation
  limiter - without it the pre-correction spikes sweep +-1.9 MHz out of band
  and the colour killer gates whole bar-heads to zero). All bars asserts pass;
  saturated-bar prints are the real limiter clamps.
- UI: SECAM in the System dropdown (+ persisted `video_system` setting), OSD
  SECAM diag line, VHS FM overlay as a vertical stack, and a ONE-SLOT panel
  view-state cache so switching Waveform<->Video no longer destroys the
  decoders (that was the "switching panel modes resets to PAL / loses sync"
  report). The frame-view toggle no longer resets either (render-time crop).
- Verified: 43/43 guards, build + smoke, meson 34/34; user real-GUI: colour
  decodes on a real SECAM capture. OPEN: vertical rolling on that capture
  while the H-PLL holds - needs the OSD VBI counters + a 20 s raw excerpt
  (same ask as backlog item 3). Contracts + rules: `secam_fm_chroma_decoder.md`.

## 2026-10-20 Windows move/resize freeze fixed (user-confirmed)

- Root cause: a title-bar/edge drag enters the MODAL LOOP inside DefWindowProc
  (WM_ENTERSIZEMOVE); raylib drains OS events at the tail of each frame, the
  caption mouse-down is dispatched inside that drain, and the render loop blocks
  for the whole drag (no swaps, frozen panels, monitor stutter). Linux/macOS
  have no modal loop.
- Fix: `gui_win32_modal.c/.h` subclass the GLFW window proc + a 15 ms WM_TIMER
  that calls a render-only frame (`app_modal_frame` in misrc_gui.c: audio pump
  + Clay dims + the SHARED layout build + flush + swap, never event polling)
  inside the loop; KillTimer + original-proc restore on exit/shutdown.
  Windows-only TU (meson), no-op header stubs elsewhere.
- Verified facts behind it (raylib/GLFW source): modal entry always lands after
  the frame flush+swap (empty batch); the GL context stays current on the
  render thread; raylib's WindowSizeCallback already runs SetupViewport inside
  the loop; timeBeginPeriod(1) makes the 15 ms timer real.
- Verified: 42/42 static guards (new `Windows modal move/size render
  contract`), build + smoke OK, meson test 34/34; user real-GUI confirmation.
  Contracts + limits: `windows_modal_resize_render.md`.

## 2026-10-20 Playback drag-and-drop: per-channel zones + audio (user-confirmed)

- Feature: drop files onto the channel panels to load playback - RF files (mono
  FLAC / Ogg-FLAC / raw widths / .lds) land on the channel whose PANEL ZONE the
  drop hit (nearest row vertically outside the canvases); an audio-rate file
  (WAV 1-8 ch incl. the 46.875 kHz clockgen capture, or audio FLAC) becomes the
  monitor source. One drop of a full capture set (RF A + RF B + audio) selects
  the Playback device, stops anything running and starts playback. The audio
  feed is position-synced to the RF playhead (seek/loop/speed/pause follow; a
  jump flushes the queue) via the shared play_q; Audio Mon gates audibility
  (no auto-enable). Speeds fold into pitch (tape semantics); Max is silent.
  The audio file persists (settings.playback_audio_file + the Settings row).
- Contracts + known limitations: `playback_drag_drop_audio.md`; guarded by
  the `playback drag-and-drop contract` in ci_guard_tests.py.
- Verified: 41/41 static guards, LIBFLAC_ENABLED=0 translation proof EXIT 0,
  build-local.ps1 + smoke OK, meson test 34/34; user real-GUI confirmation.

## 2026-10-19 CI guard break on main + local build gate (HARD rules)

- Symptom: a manual workflow_dispatch run launched to test failed in ~40 s at the
  preflight guards (optional-dep guard consistency) and every build job was skipped.
- Root causes: e34dce9 pushed the playback channel-preparation helpers outside the
  `#if LIBFLAC_ENABLED` guard straight to a tags-only-CI main; a second plain-identifier
  break (`PLAYBACK_CONV16_*` inside the guard, unguarded `conv16_state` reset) found only
  by the `LIBFLAC_ENABLED=0` translation proof; the local link then failed on
  `-l:librtlsdr_static.a` (rtl-sdr installed only under mingw64 vs the ucrt64 CI-parity
  toolchain; ucrt64 ld never searches mingw64\lib).
- Fixes: the three helpers guarded as one block; conv16 constants moved outside the
  guard; header-aware optional-dep member checker (no more `duration_seconds` false
  positives); `local CI guard gate contract` + static guards wired into
  `build-local.ps1`/`build-local.sh` before any build work; CI now runs on direct
  pushes to main (`push: branches: [main]`, release job still tag/dispatch-gated);
  rtlsdr static link carries the pc libdir `-L`. Purity note: the clean local fix is
  installing `mingw-w64-ucrt-x86_64-rtl-sdr`.
- Verified: 40/40 static guards, `LIBFLAC_ENABLED=0` translation of gui_playback.c
  EXIT 0, `build-local.ps1` + smoke OK, `meson test` 34/34; the pushed main run must be
  green before this is closed (it now runs CI automatically).
- HARD rules + procedure: `ci_guard_local_build_gate.md`.

## 2026-10-19 CVBS decoder: real-time budget, feed drops, baseline wander (HARD rules)

- Symptom: live AND playback CVBS decode "chokes after 8-10 s" (sync instability,
  horizontally shifted top rows, mid-frame tears; toggling the decoder off/on fixed it
  for another 8-10 s). After that was fixed, the first picture rows still shifted ~13 px
  and crept back over ~20 rows.
- Root causes (all measured, see `cvbs_decoder_realtime_sync_rules.md` and
  `prompt_logs/2026-10-19_cvbs-sync-shift-root-cause_README.md`):
  1. The panel decode ran at 1.08x real time (per-pixel divisions/trig/window loops in
     `decode_line_to_pixels`); the display ring (`BUF_DISPLAY`, no-wait drop policy)
     overran after ~8-10 s and dropped ~26-line chunks; the PLL integrators ingested
     those jumps as period error (period walked 3000 ppm).
  2. The source baseline rises ~+25 IRE after the vertical broad pulses (AC coupling);
     a fixed sync threshold lost ~24 lines of H sync and sliced the recovering edges
     late - each line's origin followed that error.
- Fixes: O(1)-per-pixel prefix-sum chroma/luma decode (56 -> 13 us/line, 1.08x -> 0.43x
  real time, output numerically identical); vectorisable luma FIR; frequency
  integrators gated on good syncs; per-line back-porch DC clamp driving the sync
  threshold and the luma black; field blanking commit from on-baseline lines; RF-style
  EQ+broad cadence gate for VBI candidates; field-buffer clearing + valid-row weave;
  `jumps:` and true per-field "ms" in the OSD; live-diag file dumps off in the GUI.
- Verified: `meson test` 34/34; headless exact-GUI-path decode of the user's 1-minute
  EBU HackDAC capture: 999/0 fields, bars within 0.3 deg, rows aligned top to bottom;
  user GUI confirmation ("dead stable", top shift gone).
- HARD rules + mandatory verification procedure: `cvbs_decoder_realtime_sync_rules.md`.
- Backlog recorded there: player-mode drag-and-drop (DONE 2026-10-20); Windows
  move/resize freeze (DONE 2026-10-20); sync robustness on real media
  (fast-action 4:3 anime, Philips pattern); a separate SECAM CVBS mode from the
  tbc-tools implementation.

Recent capture regressions showed that small callback-gating changes can silently break GUI feeds. Keep the following constraints in mind when touching capture/parser/audio paths:

- Preserve tolerated-frame behavior in MISRC frame mode: only drop frames when `result.error_count > 0 && result.report_errors`.
  Do not reject tolerated CRC-only frames, or GUI RF feed can stall while CLI still works.
- Keep capture heartbeat updates early in the callback (after buffer/null checks), before width/height early returns.
  This prevents false timeout/reconnect loops when callback activity exists.
- After any `capture_handler_init(&s_capture_handler)` during GUI capture start, explicitly restore audio capture state:
  `atomic_store(&s_capture_handler.capture_audio, true);`
  Without this, audio monitor path (`stream1 -> BUF_CAPTURE_AUDIO -> gui_audio`) remains empty.
- Validate RF and monitor audio as separate end-to-end checks after capture-path edits:
  - RF: waveform/scope feed present and stable.
  - Audio monitor: `Audio Mon` audible and `BUF_CAPTURE_AUDIO` no longer pinned at 0%.
- Prefer minimal, isolated fixes in `frame_parser`, `gui_capture`, `gui_extract`, and `gui_audio`; avoid unrelated UI/settings churn during capture debugging.

## 2026-09-02 Status bar dynamic scaling/spacing regression (OPEN)

- Report: bottom status bar dynamic scaling does not correctly preserve variable spacing once UI scale/window constraints tighten.
- Observed breakage:
  - the gap between `status_message` and `free space` readout is not being treated as adjustable free space;
  - the gap between `free space` and `Sync` status indicator is similarly ignored;
  - inter-counter spacing is not constrained before scaling pressure, so counters encroach into each other/available layout space.
- Impact: status row readability degrades under tighter real-world layout conditions (higher UI scale and/or constrained width).
- Status: reported and tracked; fix not implemented yet.

## 2026-08-21 Windows WASAPI clockgen audio — PARKED (firmware-side issue)

- Problem: clockgen audio (PCM1802 2ch + headswitch CH3) captured via WASAPI shared mode is distorted on Windows. Ch3/Ch4 showed red/clipping (now fixed: CH3/Ch4 peaks disabled for MISRC Clockgen, CH4 always zero). Recorded audio is still unusable due to resampling distortion.
- Root cause: the MISRC Clockgen hardware runs at 46875 Hz (stock crystal rate). Windows WASAPI shared mode resamples the 46875 Hz stream to the system mix rate (48000 Hz), corrupting the headswitch signal. Exclusive mode at 46875 Hz fails with `AUDCLNT_E_UNSUPPORTED_FORMAT` (0x8889000e) — the device does not expose a native format Windows will accept exclusively. Exclusive at 48000 Hz also fails (`AUDCLNT_E_UNSUPPORTED_FORMAT`) because the hardware is not at 48000. The endpoint exposes NO `PKEY_AudioEngine_DeviceFormat` property.
- What was tried and reverted:
  - GUI: exclusive mode at 46875 Hz (WAVEFORMATEXTENSIBLE, S24_3LE, 3ch) — failed `AUDCLNT_E_UNSUPPORTED_FORMAT`.
  - GUI: exclusive mode at 48000 Hz — failed `AUDCLNT_E_UNSUPPORTED_FORMAT`.
  - GUI: `PKEY_AudioEngine_DeviceFormat` approach (cxadc-win capture-server pattern) — the endpoint has no such property.
  - Firmware: resample 46875->48000 in `fill_buffer_normal()` with a 128/125 phase accumulator (zero-order hold, per-buffer) — produced a variable/wobbly rate because the USB endpoint pulls at a fixed 48000 Hz and the buffer fill rate doesn't match exactly. Reverted.
  - Firmware: present 48000 to USB (descriptor + `CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE`) — Windows still reports 48000 shared-mode resampling; the device format isn't exposed so Windows resamples regardless.
- Conclusion: this is a firmware-side issue that needs a proper approach not yet found. The GUI-side WASAPI path is reverted to the original working shared-mode capture (distorted but functional). The firmware repo is reverted to the working `5f98d2f` baseline. Both repos are clean.
- Open question for future work: the firmware needs to present the clockgen as a real 48000 Hz device to USB with the PCM1802 data accurately resampled in firmware before USB transmit. The per-buffer 128/125 phase accumulator approach was time-base accurate in theory but the USB endpoint's fixed 64-sample buffer size makes the output rate wobble. A variable-output-rate approach (not fixed 64) or a proper fractional resampler (linear interpolation / windowed sinc) may be needed. This is parked until a correct firmware resample design is worked out.
- GUI-side fixes that ARE committed and working: CH3/Ch4 peaks disabled for MISRC Clockgen (no red), CH4 always zero (no mirror), audio monitor forced to CH1/2 for MISRC Clockgen. Audio monitoring works (CH1/2, distorted by Windows resampling but functional).

## 2026-08-21 Vendored deps caching + CI parity (any terminal just works)

- Problem: building the Windows exe locally snagged because `.deps/install` (compiled hsdaoh + libuvc + raylib) never existed locally. CI rebuilt these from scratch every run with no cache, and the local scripts bailed at the deps gate pointing at a Linux-only script. The dev notes only covered the Meson PATH snag, not the deps-build step.
- Root cause: hsdaoh/libuvc/raylib are not system packages; they must be compiled from `third_party/hsdaoh` + upstream clones into `.deps/install`. This is what CI does every run (`windows-exe` job, `.github/workflows/build.yml`), but it was never scripted or documented for local Windows use.
- Fix: `scripts/build-deps-windows.sh` mirrors the CI `windows-exe` deps block (static libuvc v0.0.7 + vendored hsdaoh + vendored raylib with internal GLFW) into `.deps/install`. It is stamp-gated: a content-addressed hash of the hsdaoh source + raylib tag + libuvc ref + pacman dep versions skips the rebuild when unchanged. `scripts/build-local.ps1` auto-invokes it via MSYS2 MINGW64 on first run or when inputs change — one command builds everything, no manual deps step, no shell restart.
- Cross-platform parity: `scripts/build-deps-unix.sh` mirrors the `linux-appimage`/`macos` CI deps blocks with the same stamp gate. `scripts/build-local.sh` auto-invokes it.
- CI caching: `actions/cache@v4` on `.deps/install` (keyed on `hashFiles(third_party/hsdaoh/**)` + raylib/libuvc versions) added to all four build jobs, with a cache-hit guard that skips the rebuild when the install is already present.
- Prebuilt publishing: `scripts/publish-deps-cache.sh <platform> <arch>` packages `.deps/install` into a tar.xz + sha256 for upload to `harrypm/MISRC-ci-cache` (same flow as libFLAC 1.5.0). Not auto-uploaded; prints the `gh release upload` command for manual review.
- Local CI/dev guard: `python misrc_tools/test/ci_guard_tests.py --static-only` now checks the deps scripts exist, the stamp gate is present, `build-local.ps1` invokes the deps script (not bails), and docs snippets exist — so the local==CI path can't be silently removed.
- Validated locally: `misrc_gui.exe` (10.7 MB) builds clean, `--smoke-test` exits 0, deps stamp cache skips on second run.

## 2026-08-21 Windows local build bootstrap note (Meson PATH snag)

- Symptom:
  - `meson: The term 'meson' is not recognized...` in PowerShell.
- Root cause:
  - Local shell does not have Meson/Ninja on `PATH`, even when Python is installed.
- Standard recovery (now the expected flow):
  - Bootstrap and verify build-tool connection:
    - `pwsh -File scripts/build-local.ps1 -BootstrapOnly`
  - Build locally:
    - `pwsh -File scripts/build-local.ps1`
- Why this avoids repeat breakage:
  - `scripts/build-local.ps1` auto-installs user-level `meson` + `ninja` when missing and adds the Python user `Scripts` directory to session `PATH` before invoking Meson.
- Local CI/dev guard:
  - `python misrc_tools/test/ci_guard_tests.py --static-only` now checks this contract (script + note snippets), so future edits don’t accidentally remove the bootstrap path.

## 2026-04-16 capture/runtime snapshot

- Timestamp (UTC): `2026-04-16T03:38:18Z`
- OS: `Linux Mint 21.3`
- System: `Linux 5.15.0-173-generic x86_64 GNU/Linux`
- Branch: `heads/misrc_gui_dev`
- Commit: `48054ea`
- Stability note: current version is running stable for 8+ hours.

## 2026-04-19 local AppImage build note

- Local AppImage builds are now reproducible and passing smoke tests using:
  - `./scripts/build-appimage-local.sh`
- Default mode runs in an `ubuntu:22.04` container (`docker`/`podman`) to keep a portable glibc baseline.
- Script output location:
  - `.ci-artifacts/linux-appimage/`
- Verified locally:
  - AppImage artifact builds successfully.
  - `APPIMAGE_EXTRACT_AND_RUN=1 <AppImage> --smoke-test` passes.
  - Direct run `<AppImage> --smoke-test` passes on host.
## 2026-04-22 macOS Apple Silicon capture scheduling fix

- Symptom: on M-series Macs, capture/recording workloads could remain on efficiency cores, causing immediate drops/errors under GUI load.
- Root cause pattern: process-priority/QoS promotion was happening too late (after stream startup), so transport/ingest workers did not reliably inherit elevated scheduling class.
- Changes made:
  - `misrc_tools/common/threading.h`
    - Apple Silicon maps `THRD_PRIORITY_ABOVE+` to `QOS_CLASS_USER_INTERACTIVE`.
    - Added macOS QoS hinting in `proc_set_priority(...)`.
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - Apply `proc_set_priority(PROC_PRIORITY_ABOVE)` before `sc_start_capture(...)` / `hsdaoh_start_stream(...)`.
    - Roll back to `PROC_PRIORITY_NORMAL` on startup failure and on capture stop.
  - `misrc_tools/misrc_gui/output/gui_record.c`
    - Move FLAC-recording priority promotion to before encoder/worker creation.
    - Restore normal priority on FLAC init failure paths.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - Apply `proc_set_priority(PROC_PRIORITY_ABOVE)` before stream startup.
    - Restore normal priority on shutdown/startup-failure exit paths.
- Validation:
  - GUI soak run (`--debug-view`) for ~331.9s showed:
    - `waits=0`, `drops=0`
    - record buffers: A waits/drops `0/0`, B waits/drops `0/0`
    - no capture/dropout instability lines during soak window.

## 2026-04-22 macOS scheduling regression repair (post-rebase)

- Issue: after rebasing to `main`, a conflict-resolution mistake in `misrc_tools/common/threading.h` weakened macOS QoS escalation for capture-critical threads and reduced the effectiveness of Apple Silicon core placement.
- Corrective changes:
  - `misrc_tools/common/threading.h`
    - restored clean separation between `thrd_set_priority(...)` and `proc_set_priority(...)` QoS logic.
    - strengthened macOS QoS calls by adding non-zero relative priority for `ABOVE/HIGH/CRITICAL` levels.
    - added Mach thread precedence (`THREAD_PRECEDENCE_POLICY`) alongside QoS for capture-critical caller threads, avoiding blanket process-wide escalation of unrelated threads.
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - move `proc_set_priority(PROC_PRIORITY_ABOVE)` earlier in HSDAOH startup (before `hsdaoh_open`/`hsdaoh_alloc`/`hsdaoh_open2`) so early transport/open threads inherit elevated scheduling.
    - rollback to `PROC_PRIORITY_NORMAL` on all HSDAOH open/alloc failure exits.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - mirror earlier process-priority elevation for CLI HSDAOH path before `hsdaoh_alloc/open2`, with rollback on failure exits.
    - removed accidental early option-parse priority side effect so elevation only happens at real capture startup intent.

## 2026-04-22 macOS callback-thread scheduling follow-up

- Issue: callback-priority promotion was previously guarded by a single process-wide one-shot flag, so only the first callback thread was guaranteed to be elevated.
- Corrective changes:
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - changed callback promotion to thread-local one-shot (`MISRC_THREAD_LOCAL`) so each callback worker thread self-promotes once to `THRD_PRIORITY_CRITICAL`.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - mirrored the same thread-local callback-promotion behavior in CLI capture callback path.
  - `misrc_tools/common/threading.h`
    - in macOS `proc_set_priority(...)`, return early when `pthread_set_qos_class_self_np(...)` succeeds (after setting caller-thread precedence), avoiding unnecessary fallthrough into process `nice` fallback that can fail with EPERM on non-root runs.
- Runtime check (privileged CLI path):
  - successful `hsdaoh` capture runs with `waits=0`, `rf_drops=0`, `audio_drops=0`.
  - sampled `powermetrics --samplers cpu_power` during capture showed sustained higher P-cluster activity than E-cluster activity.
## 2026-05-03 parser CRC mismatch root-cause note
- Symptom: persistent parser error bursts with stable low counts (`13/14/15`) even when capture backpressure remained clean.
- Debug evidence: per-frame parser diagnostics showed `idle=0` and `total==crc` for all observed bursts, confirming CRC-only mismatches.
- Root cause: MISRC shared parser (`misrc_tools/common/frame_parser.c`) diverged from upstream hsdaoh CRC behavior by masking trailer/stream-id high nibbles before `crc16_ccitt(...)`.
- Upstream reference (`.deps-gha-local/hsdaoh/src/libhsdaoh.c`) computes CRC over raw line bytes without this masking.
- Corrective direction: compute CRC on raw line bytes in shared parser to align verifier input with upstream transport format, then revalidate mismatch rate with `MISRC_DEBUG=1` capture runs.

## 2026-08-13 Android arm64-v8a APK support (android-support branch)

- Target: basic, launchable Android 11+ (API 30) `arm64-v8a` APK via NativeActivity + raylib `rcore_android.c`, cross-compiled with NDK r25c on an x86_64 Linux host. Full plan in `android/ANDROID_SUPPORT_PLAN.md`.
- Toolchain: Android SDK cmdline-tools + build-tools;34.0.0 + platforms;android-30/34 + NDK 25.2.9519653 installed to `~/Android/Sdk`; `ANDROID_HOME`/`ANDROID_NDK_HOME`/`NDK_HOME` persisted to `~/.bashrc`.
- Build flow (all reproducible via scripts under `android/`):
  1. `android/build-deps-android.sh` cross-builds 7 static deps into `.deps/install-android-arm64` (libFLAC 1.5.0, FFTW 3.3.10 fftw3f, libsoxr 0.1.3, libusb 1.0.27, libuvc, vendored hsdaoh, raylib 5.5 PLATFORM=Android GLES 3.0). Versions + SHAs recorded in `android/deps-versions.txt`.
  2. `android/gen-cross-file.sh` emits the Meson cross-file `android/aarch64-linux-android.ini` from `$ANDROID_NDK_HOME` + `$DEPS_PREFIX`; `android/android-pkg-config` is a self-contained cross pkg-config wrapper (DEPS_PREFIX-only, no host .pc leak).
  3. `PKG_CONFIG=android/android-pkg-config meson setup --cross-file android/aarch64-linux-android.ini -Dbuildtype=release build-android misrc_tools` then `meson compile -C build-android misrc_gui` produces `build-android/libmisrc_gui.so`.
  4. `android/build-apk.sh` packages `libmisrc_gui.so` + `libhsdaoh.so` + manifest + icon into a debug-signed, zipaligned APK at `.ci-artifacts/android-apk/`.
- Code changes (see `git show` on the android-support commit for the full diff):
  - `misrc_tools/meson.build`: `android` host branch — GUI built as `shared_library('misrc_gui')` (NativeActivity loads .so), CLI executables gated off, link `-lEGL -lGLESv2 -lGLESv3 -landroid -llog -lOpenSLES -lm -ldl` (no -lpthread/-lX11/-lGL), `-DGRAPHICS_API_OPENGL_ES3` added to cflags.
  - `misrc_tools/misrc_capture/simple_capture/simple_capture.h` + new `simple_capture_android.c`: `__ANDROID__` branch checked BEFORE `__linux__` (NDK clang defines both) + no-device `sc_*` stub (CXADC/V4L2 out of scope for basic release).
  - `misrc_tools/misrc_gui/core/gui_settings.c`: `__ANDROID__` storage paths -> `/sdcard/Android/data/dev.misrc.gui/files/` (no HOME/Desktop on Android).
  - `misrc_tools/common/shm_anon.h`: `__ANDROID__` branch uses bionic's native `memfd_create` (API 30), not the glibc syscall shim.
  - `misrc_tools/common/flac_writer.c`: FLAC thread-affinity block gated to `__linux__ && !__ANDROID__` (bionic lacks `pthread_setaffinity_np`).
  - `.github/workflows/build.yml` + `misrc_tools/test/ci_guard_tests.py`: `android-apk` CI job + Android packaging-mirror guard.
- Entry-point contract (risk #2 resolved by reading `rcore_android.c:269-279`): raylib 5.5 defines `android_main()` which calls the app's standard `main()` — the existing `misrc_gui.c` main() is reused UNCHANGED. Do NOT add an `android_main` shim (would collide with raylib's).
- Bugs found & fixed during the cross-build (all verified against hard data, not assumed):
  - raylib 5.5 `rlgl.h:1908-1909` has inverted ES3/non-ES3 branches in `rlActiveDrawBuffers()`: under `GRAPHICS_API_OPENGL_ES3` it called `glDrawBuffersEXT` (an ES2 extension symbol absent on GLES3) instead of `glDrawBuffers` (GLES3 core). Patched via sed in `build-deps-android.sh` to use `glDrawBuffers`. Upstream raylib bug.
  - raylib CMake install emits a Desktop-style `raylib.pc` with `Requires.private: glfw3` (wrong for PLATFORM=Android — rcore_android.c uses EGL/ANativeWindow, no glfw). With the cross pkg-config wrapper forcing `--static`, this made `pkg-config --static raylib` fail and Meson fell back to CMake, picking up a HOST x86_64 `/usr/local/lib/libraylib.a` (arch-mismatch leak). Fixed by dropping the glfw3 require + setting Android `Libs.private` in the deps script.
  - hsdaoh hard-requires libuvc (`CMakeLists.txt:132` FATAL_ERROR) — libuvc was not in the original plan's 6 deps. Added a libuvc cross-build block.
  - hsdaoh `src/CMakeLists.txt:143` links `hsdaoh_test` with `-lrt`, which does not exist on Android (clock_gettime is in libc, like macOS). sed-patched the build copy to add `ANDROID` to the no-rt branch.
  - Meson 0.61.2 reads machine info from `[host_machine]`, NOT `[properties]` — the cross-file must set `system = 'android'` under `[host_machine]` or `host_system` is misreported as `linux` and the linux branch adds `simple_capture_v4l2.c` (no V4L2 headers on Android -> compile cascade failure).
  - Cross pkg-config wrapper must be self-contained (derive `DEPS_PREFIX` from its own location): Meson invokes the binary without the shell env, so requiring `DEPS_PREFIX` as an env var broke dep resolution.
- Verification (hard data):
  - `build-android/libmisrc_gui.so`: 16.2 MB, ELF 64-bit ARM aarch64 shared object.
  - `llvm-nm -D`: `ANativeActivity_onCreate` exported (T) — NativeActivity entry; `android_main` + `main` both exported.
  - `llvm-readelf -d` NEEDED: libEGL.so, libGLESv2.so, libGLESv3.so, libandroid.so, liblog.so, libm.so, libdl.so, libhsdaoh.so, libc.so — all Android sysroot. Zero host x86_64 lib leaks (`/usr/lib/x86_64`/`/usr/local/lib` grep count = 0).
  - APK `misrc_gui-v1.0.7-3-gaf76387-dirty-android-arm64.apk` (17 MB): `lib/arm64-v8a/libmisrc_gui.so` + `libhsdaoh.so` at correct path, both ARM aarch64; `aapt2 dump badging` -> `sdkVersion:'30'`, `targetSdkVersion:'34'`, `package: name='dev.misrc.gui'`; `apksigner verify` v3 scheme TRUE.
  - All 7 cross deps verified ARM aarch64 via `file` on extracted `.o` objects from each static archive.
  - `python3 misrc_tools/test/ci_guard_tests.py --static-only`: 22/22 PASS (including new Android packaging assertions).
- NOT yet validated (separate milestones, per plan out-of-scope):
  - Real-device install + GUI render + simulated-device selection (needs a physical Android 11+ arm64 device; cannot verify on this host).
  - USB host capture (hsdaoh/FX3/DdD) on Android — risk #1; libusb Android backend needs root or a JNI USB-Host bridge. Connect path is runtime-gated off in the basic release.
- Key constraints to preserve when touching Android build paths:
  - Keep `[host_machine] system = 'android'` in the cross-file; do not move it to `[properties]` (Meson 0.61.x ignores it there and misreports host_system as linux).
  - Keep the `__ANDROID__` branch in `simple_capture.h` BEFORE `__linux__` (NDK defines both).
  - Keep `-DGRAPHICS_API_OPENGL_ES3` in the android cflags — it must match the raylib cross-build's GRAPHICS or `rlActiveDrawBuffers` re-introduces the `glDrawBuffersEXT` link error.
  - Do not add `-lpthread` to android link flags (bionic pthread is in libc; -lpthread fails to link).
  - Keep the raylib.pc + rlgl.h sed-patches in `build-deps-android.sh` (run unconditionally so reruns stay correct).

## 2026-08-18 FLAC encode failure on clipped/resampled signal (issue #1)

- Issue: https://github.com/harrypm/MISRC-GUI/issues/1 ("Clipped, resampled signal causes FLAC encode to fail")
- Symptom: when capturing and downsampling on the fly (e.g. 40 MHz -> 20 MHz), clipped input lets the soxr resampler overshoot the source 12-bit range, and the FLAC encoder aborts with:
  - `FLAC process error: FLAC__STREAM_ENCODER_CLIENT_ERROR`
  - `FLAC encoder error on channel A` (or B)
- Root cause: `convert_i16_to_flac_i32()` in `misrc_tools/misrc_gui/output/gui_record.c` (at the function referenced in the issue) clamped the 8-bit path via `gui_record_sample_12bit_to_i8()`, but the 12-bit and 16-bit paths did not clamp:
  - 12-bit path: `dst[i] = (int32_t)src[i];` could write values outside the signed 12-bit range `[-2048, 2047]` declared to the FLAC encoder.
  - 16-bit path: `dst[i] = (int32_t)src[i] << 4;` could shift an overshooting 12-bit value outside the signed 16-bit range `[-32768, 32767]` declared to the FLAC encoder.
  - Out-of-range samples violate the declared bits-per-sample and trigger `FLAC__STREAM_ENCODER_CLIENT_ERROR`.
- Fix: clamp both unclamped paths to their declared bit-depth range, mirroring the existing 8-bit clamp:
  - 12-bit: clamp to `[-2048, 2047]`.
  - 16-bit: clamp the post-`<<4` value to `[-32768, 32767]`.
  - RAW 16-bit path was already safe (writes `int16_t` directly, inherently bounded).
- Files changed:
  - `misrc_tools/misrc_gui/output/gui_record.c` (`convert_i16_to_flac_i32`).
- Base commit: `1f6c54c` (prior to this fix).
- Validation (local, Linux Mint):
  - `meson compile -C build-local2 misrc_gui` -> builds clean (only pre-existing warnings).
  - `build-local2/misrc_gui --smoke-test` -> passes.
- Not yet validated: real on-the-fly resample capture (40 MHz -> 20 MHz) with clipped input against live hardware; confirm no `FLAC__STREAM_ENCODER_CLIENT_ERROR` under sustained clipped+resampled capture before shipping a release.
- Restore point: zip of the patched `gui_record.c` + this note preserved on the host at `~/MISRC-GUI-restore-points/fix-issue-1-flac-resample-clamp/`.

## 2026-09-03 Toolbar + status bar dynamic scaling rework

- Symptoms (user-reported across sessions):
  - Bottom status bar: right-side readouts clipped off the window edge at constrained widths; tiered short forms flickered frame-to-frame while counters ticked at boundary widths; compact labels/spacing appeared while dead space was still available; Sync/Samples looked fine but other readouts "kicked down" too fast.
  - Toolbar: a ~1020-1233px window where no breakpoint profile fit made right-side controls vanish/bounce on resize; "Audio Mon" and "Disconnect" buttons kept full-size text inside shrunken buttons (Clay does not clip text -> overflow / double-stack wrap).
- Root causes:
  - Status bar budget assumed small fixed container widths while drawn text was wider (Clay FIT/FIXED containers do not clip text children).
  - Tier/label state was recomputed every frame from live ticking data with no hysteresis, and hard width breakpoints pre-seeded compact forms regardless of actual space.
  - Toolbar tier flags were incomplete (ultra-narrow profile shrank buttons without tripping the label-shortening flags).
- Fixes (all in `misrc_tools/misrc_gui/ui/gui_ui.c` unless noted):
  - Status bar: budget right-side readouts by real measured text widths; value containers FIT(min); tiered shortening (sample rate / samples / free space) in lockstep via a persisted level with hysteresis (immediate step-up under pressure, one-step relax only on first-pass fit with slack beyond the measured tier step).
  - Status bar: the status message claims its natural rendered width (capped at the readable minimum) instead of a fixed 96px budget; compaction is earned by measured pressure — compact-seed/narrow breakpoint pre-seeding removed; compact-label flip persisted with the same hysteresis (`s_status_readout_level`, `s_status_labels_compact`).
  - Toolbar: layout tiers selected by measured fit (largest profile whose requirement fits), device dropdown compresses to its floor before a tier steps down and grows back into leftover slack first; two-row only when even tiny cannot fit.
  - Toolbar: Audio Mon and Connect/Disconnect labels pick the longest form that measurably fits the button (measured-fit fallback instead of tier flags).
  - Counter group spacing tightened at full quality (right gap 16->10, inner gap 4->3).
- Contract notes:
  - `gui_ui_toolbar_uses_two_rows()` signature/semantics unchanged (CI guard "UI scale integration contract").
  - `gui_ui_get_status_layout_mode()` / `gui_ui_status_*` pure functions in `gui_ui_scale.h` unchanged; harness (`misrc_tools/test/gui_ui_scale_harness.c`) still passes.
- Validation (local, Linux Mint): `scripts/build-local.sh` build + smoke OK (pre-existing warnings only); `misrc_tools/test/ci_guard_tests.py` 34/34 PASS; `meson test -C build-local` 3/3 OK; user real-world GUI confirmation of the right-side clip fix and general behavior.
- Commits: `a552e7e` (status bar clipping + toolbar dead-space-first + Audio Mon measured fit), `42b845c` (status bar measured-fit compaction + hysteresis) on `capture-server-client-modes`.
- Session log: `PROMPT_STATUSBAR_README.md` (repo root); restore-point zip `statusbar-rightside-fix-2026-09-02.zip` on the host (untracked, predates later follow-ups).

## 2026-09-06 FLAC finalize metadata rewrite elimination

- Symptom (user-reported): a massive duplicate `flac.metadata` temp file (~ the full FLAC size) was created at recording stop, wasting disk space and adding large amounts of extra working time to finalize.
- Root cause: at recording stop, `gui_record_finalize_stop_sync()` called two post-encode metadata functions in `gui_record.c`:
  - `gui_record_update_flac_streaminfo_duration()` used `FLAC__metadata_chain_read`/`chain_write`. libFLAC's chain API always rewrites the ENTIRE FLAC file to a temp copy and renames it over the original - even for a 34-byte STREAMINFO block change.
  - `gui_record_embed_flac_duration_metadata()` used `FLAC__metadata_simple_iterator_insert_block_after()`. With no PADDING block in the encoder output, any new block insertion also forces a full-file rewrite via the padding/temp path.
  - The encoder (`flac_writer.c` `configure_encoder`) created only a SEEKTABLE metadata block - no VORBIS_COMMENT, no PADDING - so neither update had room to land in place.
- Fix (in-place finalize, no temp file):
  - `misrc_tools/common/flac_writer.c`: the encoder now emits metadata in the order STREAMINFO -> VORBIS_COMMENT -> PADDING(4096 bytes) -> SEEKTABLE -> audio. The VORBIS_COMMENT block is provided explicitly so the encoder does not auto-append one after the seektable (which would put it out of padding range). The 4096-byte PADDING block sits adjacent to the VC block so the post-encode duration tags can grow the VC block in place by consuming the padding. Cleanup paths (`flac_writer_finish`, `flac_writer_abort`, both create error paths) free the new `vorbis_comment` and `padding` blocks.
  - `misrc_tools/misrc_gui/output/gui_record.c`: replaced both old functions with a single `gui_record_finalize_flac_metadata()` that opens the file once with `FLAC__Metadata_SimpleIterator`:
    1. STREAMINFO update (first block, fixed 34-byte size) via `set_block(use_padding=false)` - pure in-place write.
    2. Walk to the existing VORBIS_COMMENT block, append DURATION_SECONDS/LENGTH/RF_TOTAL_SAMPLES/RF_SAMPLE_RATE/RF_SAMPLE_RATE_KHZ, `set_block(use_padding=true)` - grows into the adjacent PADDING in place.
  - Call site in `gui_record_finalize_stop_sync()` now calls the single combined function per channel instead of the two chain/insert functions.
- Effect: finalize is now a few bytes of in-place I/O per FLAC file instead of copying the entire multi-GB file to a temp copy. Eliminates the duplicate `flac.metadata` temp file and the extra working time at recording stop.
- Compatibility note: FLAC files recorded BEFORE this change do not contain the PADDING block, so their VC tag insertion would still need a one-time rewrite; the STREAMINFO in-place update still works. The combined function handles a missing VC block gracefully (logs a WARN, leaves STREAMINFO updated). New recordings get the full in-place benefit.
- Files changed:
  - `misrc_tools/common/flac_writer.c` (struct, `configure_encoder`, both create functions, `flac_writer_finish`, `flac_writer_abort`).
  - `misrc_tools/misrc_gui/output/gui_record.c` (replaced `gui_record_embed_flac_duration_metadata` + `gui_record_update_flac_streaminfo_duration` with `gui_record_finalize_flac_metadata`; updated call site).
- Validation (local, Linux Mint): `scripts/build-local.sh` build + smoke OK (libFLAC 1.5.0 multithreading; pre-existing warnings only); `misrc_tools/test/ci_guard_tests.py` 34/34 PASS; `meson test -C build-local` 3/3 OK.
- Not yet validated: real multi-GB FLAC recording + finalize against live hardware - confirm (a) no large `flac.metadata`/temp file appears in the output directory during finalize, (b) finalize completes in seconds not minutes, (c) the written FLAC still reports correct duration in a player/Audacity and the DURATION_SECONDS/LENGTH/etc. Vorbis comment tags are present.

## 2026-09-11 RP2350 dual_external_adc black frames on Windows (HARD note)

- `rp2350_firmware_timing_fix.md`: every Windows software layer was instrumented and
  proven working (libusb transfers, libuvc frame assembly, hsdaoh HID, UVC
  negotiation) — the failure is the RP2350 firmware's HDMI timing (45-px hsync vs the
  FPGA's 4-px), outside the MS2130's TMDS data-recovery lock range. Fix-to-make
  (MODE_H_SYNC_WIDTH 45→4, MODE_V_SYNC_WIDTH 2→1, MODE_V_FRONT_PORCH 1→0) plus the
  what-NOT-to-touch-again rules (no Windows driver / MS2130 / libuvc-libusb-hsdaoh
  changes — all proven working) are recorded there.
