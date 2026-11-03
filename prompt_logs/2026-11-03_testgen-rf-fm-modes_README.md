# Prompt Log — Test Gen FM RF mode fixes + waveform CH1/CH2 signal sub-selector

Repo: `C:\Users\Harry\MISRC-GUI` (harrypm fork)
Session start: 2026-11-03

## User request (verbatim)

> I want to fix the FM Genarator modes, make a sub slector on the wavefrom
> playback monitor so ch1 is cvbs and ch2 is FM RF for tape Genarator /
> System boxes are needed for this basically

## Repo correction (important)

The first implementation attempt landed in the STALE `C:\Users\Harry\misrc_gui_dev`
clone (pre-hacktv-testgen codebase). The user corrected this: the current repo is
`C:\Users\Harry\MISRC-GUI` (v1.2.7+ with the hacktv Test Gen port). Per the new
personal rule, the repo was verified current before work: local `main` was 11
commits behind `origin/main` (Android monitor client, macOS 15 CI migration);
pulled fast-forward to `103efbb` before any edits. The stale-repo edits remain
uncommitted in `misrc_gui_dev` and are NOT part of this work.

## Clarified intent (ask_user_question answers)

1. "FM Generator" = the Simulated device's FM RF generation (Channel B).
2. Fix the defects AND add proper mode following.
3. Waveform sub-selector = per-channel dropdown (CVBS / FM RF / Audio / Raw)
   driving trigger defaults and labels.

## Verified defects (hard data from code inspection)

Channel B (FM RF) of `gui_simulated.c` is the legacy generator:

- FM level-domain mismatch: it FM-modulates the hacktv-domain CVBS
  (sync -0.30 / white +0.70 of full scale) through the OLD -0.4/1.0
  normalization -> sync tip maps to ~3.47 MHz instead of 3.4, white to
  ~4.19 MHz instead of 4.4. Wrong deviation in every mode.
- The System selection (PAL/NTSC/SECAM via `settings.video_system`) never
  reaches Channel B: its grid/bar/chroma path is PAL 625-line hardcoded with
  NTSC VHS endpoints (3.4/4.4 MHz) and a 629 kHz color-under for all modes.
- Legacy color-under defects: carrier phase resets per line; 0/90 rotation
  alternation by line parity instead of per-field track rotation
  (PAL 0/-90, NTSC -90/+90 per the repo's own RF decoder).
- Dead/broken remnants: the colour-carrier LUT machinery is still allocated
  in `gui_simulated_start` though unused by the Test Gen path.

## Fix design (sources of truth)

- Channel B becomes a full-frame tape-RF generator `gui_testgen_rf_frame()`
  in `gui_testgen.{c,h}`, sibling of the composite generator, sharing the
  slot-grid geometry (extracted into a static helper; composite output
  proven byte-identical before/after via a temp harness).
- Physical RF model follows the repo's OWN validated oracles:
  `test/gui_rf_preview_test.c` `make_rf_signal_profile()` (PAL/NTSC VHS:
  FM endpoints, record pre-emphasis 100/400 ns PAL, 120/320 ns NTSC;
  color-under carrier + per-field rotation; burst u/v; 0.72 FM + 0.20
  chroma amplitude) and `test/gui_rf_secam_test.c` `make_source()` (native
  SECAM VHS: luma -300k sync / +700k·y pre-emphasized 400/100 ns;
  D'R/D'B LF pre-correction 85/255 kHz; ident bottles; chroma FM divided
  by 4; 0.15 chroma amplitude).
- `gui_simulated.c` pre-generates 2 RF frames per system (alternating like
  the CVBS frames; SECAM cadence needs both) and streams Channel B from
  them; legacy per-sample RF code deleted.
- `gui_oscilloscope.c` gains a per-panel Signal dropdown (Raw/CVBS/FM RF/
  Audio) in the right-anchored overlay group: "CH1 CVBS"/"CH2 FM RF" labels
  + trigger defaults (CVBS -> CVBS HSYNC; FM RF/Audio -> rising edge); auto
  -assign CH1=CVBS / CH2=FM RF when the Simulated device starts.
- New meson test `gui_testgen_rf`: generated RF decoded through the GUI's
  own `gui_rf_decoder` (PAL/NTSC/SECAM VHS profiles) with lock + bar-hue
  assertions — the same panel-path A/B philosophy as the SECAM testgen
  verification in TECHNICAL.md.

## Commands run (session)

- `git fetch origin` / `git pull --ff-only` (5cb3691 -> 103efbb, 11 commits)
- Read: gui_testgen.{c,h}, gui_simulated.{c,h}, gui_rf_preview.{c,h},
  gui_rf_preview_worker.h, gui_rf_preview_geometry.h, gui_vhs_fm.h,
  gui_oscilloscope.c, gui_capture.c, gui_app.h, TECHNICAL.md Test Gen
  section, test/gui_rf_preview_test.c, test/gui_rf_secam_test.c, meson.build
- Build dir: `build-local` (meson, misrc_gui.exe present)

## Output log

### Implementation (all in this repo, working tree, NOT committed)

Files changed:

- `misrc_tools/misrc_gui/signal/gui_testgen.c` — slot-grid setup extracted
  into a shared static `ht_grid_t`/`ht_grid_build()` (composite + RF
  generators can no longer drift); new `gui_testgen_rf_frame()` tape-RF
  generator per the oracle model (details in TECHNICAL.md).
- `misrc_tools/misrc_gui/signal/gui_testgen.h` — `gui_testgen_rf_frame`
  declaration + docs.
- `misrc_tools/misrc_gui/input/gui_simulated.c` — rewritten: both channels
  stream pre-generated frame pairs; Channel B from the RF generator; legacy
  per-sample RF code + colour LUT deleted.
- `misrc_tools/misrc_gui/input/gui_simulated.h` — updated docs.
- `misrc_tools/misrc_gui/visualization/gui_oscilloscope.c` — Signal dropdown
  (Raw/CVBS/FM RF/Audio) in the overlay group; CH1 CVBS / CH2 FM RF / CHn
  Audio labels; trigger defaults under the data lock with config_dirty;
  `gui_waveform_apply_generator_defaults()` auto-assign on sim start.
- `misrc_tools/misrc_gui/visualization/gui_oscilloscope.h` — declaration.
- `misrc_tools/misrc_gui/input/gui_capture.c` — sim start calls the
  generator defaults helper.
- `misrc_tools/test/gui_testgen_rf_test.c` — new headless decode test.
- `misrc_tools/meson.build` — registers `gui_testgen_rf` (timeout 120).
- `TECHNICAL.md` — "Test Gen — Channel B tape RF" subsection.

### Bugs found and fixed during the port (hard data)

1. **H-line/blank-slot double write**: the RF slot pass writes every sample
   (continuous FM) so the blank slot following each H line overwrote the
   second half of every H line with blank-level RF — the decoded picture
   truncated at bar 4 (bars 4-7 read display black 10/255). Fix: skip the
   SL_BLANK slot that follows an SL_H slot (standalone blanks still write).
2. **SECAM D'r/D'b cadence**: the composite CH1 cadence (line parity flips
   every field) does not match the RF decoder's identification flywheel,
   which expects the real-transmitter convention (same parity for both
   fields of a frame, flipping with the odd 625-line frame stride). With
   the composite cadence, `secam_ident_streak` never exceeded 2, so colour
   never locked ("acquiring SECAM line identification"; bottles measured
   perfectly — instrumented decoder copy printed plateau 1188926 vs rest
   1189062, ramps ~60 kHz, every bottle line ok=1). Fix: the RF generator
   uses `(hidx+field)` as the decoder-line parity basis with the per-frame
   flip.

### Verification (hard data)

- `meson test gui_testgen_rf`: OK (PAL/NTSC/SECAM all H/V/C lock + bar hue
  order at -O3 -ffast-math).
- Composite byte-identity: PAL/NTSC/SECAM frames (0/1 at 40 MSPS)
  hash-identical before the grid extraction -> after it -> after all RF
  work.
- Full `meson test`: 44/46 OK. `update_cli` FAIL = documented pre-existing.
  `gui_rf_playback_start` FAILed once under full-suite parallel load but
  passes in isolation (timing-sensitive; its binary links none of the
  changed files — not a regression).
- `meson compile misrc_gui`: clean (one pre-existing shadow warning in the
  composite SECAM pass, untouched by this work).

### Round 2 — user GUI feedback (2026-11-03 03:26)

User reports from the click-through:
1. "Test gen modes should only show up in test mode not playback or
   recording modes."
2. "the signals are mixed up — Ch1 has fm instead of cvbs"
3. "also switch the decoder plugins preview to the mode of the signal gen
   when tape/video signal changes but allow for user overide once flipped"

Root causes found (hard data):

1. **Channel swap**: the extraction thread swaps raw A/B for MISRC-mode
   captures (gui_extract.c: `mapped_a = s_buf_b` when
   `user_capture_mode_misrc`, the default) — the sim wrote CVBS to raw A /
   RF to raw B, so the display's CH1 showed the RF and CH2 the CVBS.
   Fix (gui_simulated.c): the capture thread mirrors the extraction swap
   (runtime latch while recording + the v1.5/v2.5 wiring override) per
   batch and writes CVBS/RF into the raw slots that land CH1/CH2 correctly
   AFTER the swap: CH1 = CVBS, CH2 = tape RF.
2. **Gen box visibility** (gui_oscilloscope.c): now test-mode only —
   Simulated device selected AND no recording AND no active playback
   session (drag-drop FLAC playback can start while [Simulated] stays
   selected; `atomic_load(&app->playback_running)` guards that case).
3. **Decoder follow with override latch** (gui_cvbs.c/.h + gui_oscilloscope.c):
   new `gui_video_follow_generator_system(app, system)` switches every
   still-following Video panel's previews (CVBS composite decoder, Tape
   Decode plugin, VHS FM RF profile + worker rebinding) to the Gen box's
   system; each panel has a `gen_follow` latch (default on) that is
   cleared by any manual system/tape/MESECAM selection on that panel, so
   the user's pick wins from then on. New panels start in the followed
   system. The Gen box calls it after updating settings.video_system.

Validation:

- `meson compile misrc_gui`: clean (the one transient link failure was a
  locked exe during the user's test session; retry linked fine).
- `meson test gui_testgen_rf gui_cvbs_baseband gui_waveform_overlay`: all
  OK (the overlay harness needed a stub for the new follow call — it
  includes gui_oscilloscope.c without linking gui_cvbs.c).

### Round 3 — user GUI feedback (2026-11-03 03:39)

User reports:
1. "the modes are still presenting on Recording devices, and on files"
2. "Layout is fixed, but we need to put the gen options on a second row
   like 'Gen:XXXX' is for system"
3. "RF Preview decoder does not auto flip system with the generator mode
   change; cvbs does just fine however"

Fixes:

1. **Signal options are test-mode-only now**: the Signal dropdown moved OFF
   the always-visible row-1 overlay group onto the generator row, and the
   CH1 CVBS / CH2 FM RF / CHn Audio labels fall back to plain CH A / CH B
   on every non-test source (real capture devices, FLAC playback —
   `waveform_test_mode_active()` gates both rendering and labels).
2. **Generator row** (gui_oscilloscope.c waveform_render): second row below
   the Scale/Mode/Trig group, right-aligned, test mode only:
   `[Signal: Raw/CVBS/FM RF/Audio] [Gen: PAL/NTSC/SECAM]`. Row 1 keeps the
   original three-button group; the Signal options popup opens below the
   button and closes when the row hides.
3. **RF preview follow made unconditional**: the per-panel `gen_follow`
   latch was almost certainly the cause — a manual System click on the RF
   panel permanently stopped that panel following (the untouched CVBS panel
   kept flipping). Removed the latch entirely: a Gen change now flips EVERY
   Video panel (CVBS + Tape Decode + VHS FM profile with worker rebinding);
   manual panel picks keep working between generator changes and the next
   Gen change flips the panels again ("override once flipped"). Added a
   `GEN FOLLOW` TraceLog per panel (ch/slot/system/rf_profile before->after)
   so a persistent non-flip is diagnosable from the debug view.

Validation: `meson compile misrc_gui` clean; `gui_testgen_rf` +
`gui_cvbs_baseband` + `gui_waveform_overlay` all OK.

### Round 4 — RF Preview letterbox contrast (2026-11-03 03:48)

User: "fix the panel contrast (outside area of picture rendered) for the
RF Preview decoder as right now it shows black on the sides".

Fix (gui_vhs_fm.c gui_vhs_fm_render_frame): the letterbox fill was
near-black {20,20,20} — indistinguishable from the decoded picture's own
black/VBI. First attempt used a bezel grey {46,46,52}; user follow-up
"almost but not matching standard background" -> now the standard
COLOR_PANEL_BG {40,40,48} (gui_ui.h) plus a 1px COLOR_GRID_MAJOR outline
around the fitted frame rect (drawn even while acquiring sync, so the
future picture area is delineated). CVBS composite panel still uses
{20,20,30} — left as-is this round; can match later if wanted.

Validation: build clean; gui_testgen_rf / gui_cvbs_baseband /
gui_waveform_overlay all OK.

### Round 5 — full-frame framing alignment (2026-11-03 05:07)

User: "make the framing more aligned with each other on the full-frame
modes for the decoders".

Root cause: the CVBS composite decoder's Frame: Full view fitted its full
frame (1135x625 PAL / 910x525 NTSC at 4fsc) into a FIXED 4:3 rect, while
the RF preview's Full mode fits the same physical span (full line period
x full field) at the aspect-correct ~1.510 PAL / ~1.456 NTSC — so the
two decoders' full-frame pictures had mismatched geometry (CVBS squeezed
~12% horizontally).

Fix:
- New shared helper `gui_rf_preview_full_frame_aspect(height)` in
  `gui_rf_preview_geometry.h` (the header both decoders already share for
  geometry): (LINE_US / active window) * (active width / full height),
  keyed by 625/626 -> PAL, 525/526 -> NTSC.
- `gui_rf_preview_display_aspect` refactored to delegate to it (identical
  values; the existing gui_rf_preview_geometry test passes unchanged).
- `gui_cvbs_render_frame_impl`: Full view now fits with the shared
  full-frame aspect; Active view keeps 4:3 (picture crop).

Validation: `meson compile misrc_gui` clean; `gui_rf_preview_geometry` +
`gui_cvbs_baseband` + `gui_testgen_rf` all OK. Fresh exe 05:09.

### Committed

- `67b5935` "Test Gen: fix FM RF modes, tape-RF CH2, waveform signal
  sub-selector" pushed to harrypm/MISRC-GUI main (rounds 1-4 + restore
  point `restore_points/2026-11-03_testgen-rf-fm-modes-good.zip`).

### Round 6 — remove Raw, default CH1=CVBS / CH2=FM RF (2026-11-03 05:18)

User: "remove raw mode form signal gen set cvbs on ch1 and set fm rf on
ch2 on defulat opneing".

Fix (gui_oscilloscope.c):
- The Signal dropdown now lists only CVBS / FM RF / Audio. WAVE_SIGNAL_RAW
  stays as the internal pre-default state of a freshly created panel but
  is not selectable (WAVE_SIGNAL_OPTION_COUNT / WAVE_SIGNAL_OPTION_VALUE
  mapping for the 3-option popup and click handling).
- New `waveform_apply_channel_signal_default(state, channel)` (same
  once-only pattern as the trigger-source channel default): CH1 -> CVBS,
  CH2 -> FM RF with the matching trigger defaults, applied at the panel's
  first render/click — so the waveform monitors open with the generator
  mapping already assigned. `gui_waveform_apply_generator_defaults`
  (sim-start hook) remains as belt-and-braces for never-rendered panels.

Validation: build clean; gui_waveform_overlay / gui_testgen_rf /
gui_cvbs_baseband all OK. Fresh exe 05:21.

### Round 7 — UX batch (2026-11-03 05:39)

User requests + implementation:

1. **Middle-button double-click resets the waveform view**
   (gui_oscilloscope.c): 350 ms / 6 px double-click tracker (same
   thresholds as chrome-hide), polled per frame in waveform_render with a
   bounds hit test (the left-click dispatch never sees the middle button).
   Reset = ZOOM_SCALE_DEFAULT zoom, trigger level 0, trigger position -1,
   phosphor cleared; trigger mode/enabled left as-is.
2. **Fullscreen**: F11 (cross-platform) and Alt+Enter (fallback where F11 is
   system-reserved, e.g. macOS) toggle raylib ToggleFullscreen in the main
   loop. Windows throw-at-top Aero Snap: the win32 modal subclass now flags
   WM_SIZE/SIZE_MAXIMIZED (gui_win32_modal_take_maximize_request); the main
   loop converts it into the same clean borderless fullscreen (with a 1 s
   suppression window after manual toggles so the fullscreen-exit restore
   cannot bounce straight back in).
3. **CVBS decoder right/top padding vs RF preview**: NOT yet diagnosed —
   both decoders now share the full-frame aspect; the residual padding is
   inside the CVBS decoded texture. Needs a screenshot/next round to
   identify the exact blank region (decode fills the full frame_width per
   line; suspected field-boundary row phase).
4. **Stats -> OSD** on the RF preview decoder: the selector button now
   reads "OSD: All/Basic/Off" (options unchanged), matching the CVBS
   decoder's OSD naming.
5. **MESECAM restructure**: the RF preview System cycle is now
   PAL -> NTSC -> SECAM -> MESECAM (4 states); the dedicated MESECAM
   toggle button is removed. gui_vhs_fm_set_format accepts 0-3;
   gui_vhs_fm_get_profile keys off selected_system==3 -> MESECAM_VHS,
   ==2 -> native SECAM_VHS (the 2"-quad / 1"-Type-C domain). The tape
   family cycle button became a real "Format:" dropdown listing
   VHS / Betamax (open popup owns clicks; Betamax presets unchanged).
   gui_vhs_fm_set_mesecam kept as legacy API, now mapping onto the system
   value.

Validation: build clean (05:48); gui_waveform_overlay, gui_rf_preview_
geometry, gui_testgen_rf, gui_cvbs_baseband, gui_rf_secam all OK.

### Round 8 — RF Preview Format dropdown + generator Fmt + MESECAM (2026-11-03 05:56)

User reports:
1. "dropdown is broken (not overlaying) for format selection for RF
   Preview" — the Format options were drawn inline at their button's row;
   immediate-mode drawing let the LATER System/Preset/Frame/Budget/OSD
   rows paint over them. Fixed: the options are now drawn at the very END
   of `render_vhs_decoder_overlay`.
2. "format selection is missing from waveform generation mode" — new Fmt
   button on the gen row: [Signal] [Fmt] [Gen]. The tape format is
   persisted in settings.gen_tape_format and hot-swapped by the simulated
   capture thread on the next batch (same regenerate pattern as the system).
3. "MESECAM mode adding also needed" — GUI_TESTGEN_MESECAM (RF-only: the
   composite channel generates SECAM, the RF channel mixes the studio
   SECAM chroma down through the PAL converter LO 5060571.875 Hz to the
   MESECAM rests 654321.875 / 810571.875 Hz). gui_testgen_rf_frame now
   takes the tape format and a per-(family, format) parameter table
   (blank/sync/hz-ire/color-under carrier/track rotation) from the
   tape-decode-rs profiles; SECAM/MESECAM force VHS. Gen box cycles
   PAL->NTSC->SECAM->MESECAM; the follow maps MESECAM composite to SECAM
   on the composite side and MESECAM_VHS on the RF side.
4. "mirror like cvbs does the RF Preview system change with signal
   change, as there is now a new bug added when wrong decoder is selected
   to the system it blanks instead of trying to decode it" — the VHS FM
   render gate `status->v_locked || paused` hid the picture until lock,
   so a wrong decoder showed nothing. Fixed: the decoded frame now draws
   whenever one exists (wrong-decoder attempts are visible; the reason
   line communicates the lock state).

Validation: build clean (06:06); gui_testgen_rf (now including the
MESECAM->MESECAM_VHS decode + bar hues), gui_cvbs_baseband,
gui_waveform_overlay all OK (harness stubbed the new testgen helpers).

## Round 9 — format selection removed from CVBS mode; format cycle decoupled from CH1/CH2

User request: "Remove format selection from cvbs mode decouple that cycle
from Ch1/2".

Changes:

1. `gui_oscilloscope.c` — the generator row's `Fmt` button now renders on
   FM RF signal panels ONLY. A tape format parameterizes the RF
   modulation, so it has no meaning in a CVBS or Audio signal context;
   the button follows the panel's Signal pick rather than the CH1/CH2
   channel mapping. The default CVBS panel (CH1) shows just
   `[Signal][Gen]`; the FM RF panel keeps `[Signal][Fmt][Gen]`. The click
   rect is zeroed when hidden, so the handler (already width-gated) stays
   inert.
2. `gui_simulated.c` — per-channel stream positions (`cvbs_stream_pos`,
   `rf_stream_pos`) replace the single shared `stream_pos`. A system
   change still regenerates both frame pairs and resets both positions;
   a FORMAT-only change now regenerates just the RF pair and restarts
   only the RF stream, so CH1's composite signal keeps flowing without a
   discontinuity (no decoder glitch on the CVBS panel when the format
   cycles). Log line distinguishes the two paths
   ("CH1 CVBS + CH2 RF" vs "CH2 RF only, CH1 CVBS continuous").

Validation: build clean; gui_testgen_rf, gui_cvbs_baseband,
gui_waveform_overlay all OK (the overlay harness exercises
waveform_render_overlay only, so the gen-row gating has no harness
coverage — needs GUI click-through).

## Round 10 — MESECAM generator follow fixed (stale 0-2 guards)

User report: "MESECAM auto cycle when I switch to that FM mode it should
change to it on the fm decoder" — the Gen box cycling PAL -> NTSC ->
SECAM -> MESECAM never flipped the RF Preview at the MESECAM step.

Root cause: `gui_video_follow_generator_system`'s entry guard was still
`system > 2` (a round-4 relic; the MESECAM-aware body from round 8 never
ran for system 3). Two more 0-2 guards blocked the same value space:
`gui_cvbs_set_default_system` (startup push of the persisted pick) and
the RF plugin init hardcoding `gui_vhs_fm_set_format(..., 0)` (new RF
panels always started PAL).

Changes (all `gui_cvbs.c` + doc comments in `gui_cvbs.h` / `misrc_gui.c`):

1. Follow guard now accepts 0-3: MESECAM flips the RF Preview to the
   MESECAM_VHS profile (worker rebinding via gui_video_update_rf_routes)
   while the composite/tape side maps 3 -> 2 (SECAM).
2. `gui_cvbs_set_default_system` accepts 0-3, so a persisted MESECAM pick
   survives relaunch.
3. `video_panel_init_decoder_common` maps 3 -> 2 for the composite/tape
   decoders (selected_system/selected_tape_format/set_format).
4. `video_vhs_fm_plugin_init` starts new RF preview panels in the
   generator's default system instead of hardcoded PAL.

Validation: build clean; gui_testgen_rf, gui_cvbs_baseband,
gui_waveform_overlay all OK. Follow behavior needs GUI click-through
(overlay harness stubs the follow function).

## Round 11 — MESECAM removed from the CVBS-mode Gen cycle

User request: "Remove MESECAM from CVBS mode" — the Gen system button
offered PAL -> NTSC -> SECAM -> MESECAM on EVERY waveform panel, but
MESECAM is RF-only (a MESECAM generator's composite output is plain
SECAM), so the step is meaningless in a CVBS/Audio signal context.

Changes:

1. `gui_oscilloscope.c` Gen button follows the panel's Signal pick (same
   rule as the round-9 Fmt gating): FM RF panels cycle the full
   PAL -> NTSC -> SECAM -> MESECAM; CVBS/Audio panels cycle
   PAL -> NTSC -> SECAM only. A MESECAM generator state (set from an FM RF
   panel) displays as its composite equivalent, "Gen: SECAM", on
   CVBS/Audio panels and cycles on from there (SECAM -> PAL).
2. `gui_waveform_overlay_harness.c`: stub `gui_testgen_clamp_system`
   (next to the clamp_format stub) — the new click-handler call survived
   LTO stripping and broke the harness link.

Validation: build clean; gui_testgen_rf, gui_cvbs_baseband,
gui_waveform_overlay all OK.

### Committed (rounds 5-11)

- `f5fc496` "Test Gen: MESECAM + tape-format UX, fullscreen, signal-mode
  gating (rounds 5-11)" pushed to harrypm/MISRC-GUI main (18 files;
  restore point `restore_points/2026-11-03_testgen-rf-ux-rounds5-11-good.zip`).
  User confirmed rounds 5-11 in the GUI before committing.

## Round 12 — borderless fullscreen + UI-scale fixes for the new controls

User report: "The F11 mode has introduced a graphical bug instead of doing
a normal borderless full-screen when enabled/cyceled this tripped up my
driver on a system level, I beleve this is a scaling bug... Also the
scaling modes, the new GUI elements for the signal genarators does not
scale properly.."

Root causes + fixes:

1. `ToggleFullscreen()` (raylib) switches the MONITOR'S VIDEO MODE to the
   window's size — a non-native resolution the GPU driver rescales (the
   system-level driver glitch). Replaced with `ToggleBorderlessWindowed()`
   (misrc_gui.c, both the F11/Alt+Enter path and the Windows snap-maximize
   conversion): keeps the desktop's native video mode, makes the window
   undecorated at monitor size — a true borderless fullscreen with no mode
   change. State checks now use `IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)`
   (raylib 4.5+; the MSYS2 toolchain has 5.x).
2. The generator row (Signal/Fmt/Gen) and the RF Preview overlay measured
   button widths with raw `gui_text_measure` values (physical pixels) used
   directly as LOGICAL layout units — the row-1 overlay group's documented
   contract (commit 8b19ddf) converts them via `1/gui_ui_get_scale_factor()`.
   At zoom < 100% the labels overflowed the too-narrow buttons; at zoom >
   100% the buttons bloated ~scale× too wide. Both `waveform_render`'s gen
   row (gui_oscilloscope.c) and `render_vhs_decoder_overlay` (gui_cvbs.c:
   Decoder/Format/System/Preset/Frame/Budget/OSD buttons + both popups)
   now convert measurements AND fixed pixel floors to logical units.

Validation: build clean; gui_testgen_rf, gui_cvbs_baseband,
gui_waveform_overlay all OK (the cvbs test LTO-strips the render path, so
the new scale call adds no link dependency). Needs GUI click-through:
F11/Alt+Enter/snap at native resolution, and the gen row + RF Preview
controls at several UI zoom levels (Ctrl+wheel / Ctrl+0 / Ctrl+/-).

NOTE: the older composite CVBS overlay (render_cvbs_system_overlay) still
uses raw physical mixing throughout — same latent zoom issue, not touched
this round (pre-existing code, not reported). Convert it the same way if
the user sees it mis-scaling.

## Round 13 — drag gestures: throw-at-top = maximize, hold-at-top = top half

User request: "I want throw up to be full-screen window not borderless,
f11 while windowed to be borderless and hold at top to be half size and
throw at top to be full screen".

Three distinct behaviors now (misrc_gui.c + gui_win32_modal.c/h):

1. **F11 / Alt+Enter while windowed** -> borderless fullscreen (round 12's
   ToggleBorderlessWindowed kept; toggling again returns to windowed).
2. **Quick THROW at the top edge** -> MaximizeWindow(): a normal maximized
   window (title bar/taskbar intact, NOT borderless). This also overrides
   any Win11 snap-layout arrangement the fast gesture landed in - the
   round-7 "goes half mode" symptom.
3. **HOLD at the top edge (>= 400 ms)** -> the window snaps to the TOP HALF
   of the current monitor's work area (taskbar excluded).

Mechanics: the win32 modal subclass now tracks the drag (WM_SYSCOMMAND
SC_MOVE gates it to caption moves only - resizes/buttons unaffected),
polls the cursor vs the monitor top edge (4 px zone) on WM_MOUSEMOVE + the
15 ms modal timer, and classifies the release at WM_EXITSIZEMOVE by edge
dwell time. The old WM_SIZE/SIZE_MAXIMIZED -> borderless conversion and
its 1 s suppression bookkeeping are REMOVED. Requests are queued in the
wndproc and consumed in the main loop (MaximizeWindow via raylib;
top-half via SW_RESTORE-if-maximized + SetWindowPos to the work-area top
half - GLFW's wndproc stays chained and observes the resulting
notifications). Leaving the edge zone resets the dwell, so hovering the
Win11 snap-layouts flyout never counts as a hold. Non-Windows platforms
stub everything to no-ops (drag gestures there are native anyway).

User follow-up: "throws somtimes trigger half blank mode still" — the
dwell-only classifier misread real throws: a flick that lingered at the
edge >= 400 ms before release landed as a HOLD (top half), and a fast
flick's last WM_MOUSEMOVE could lag the true release position out of the
4 px edge zone (no request queued, so the native Win11 half/quarter snap
stood). Fix (same session):

- Release VELOCITY now decides: the classifier keeps a trailing cursor
  sample history (WM_MOUSEMOVE + the 15 ms modal timer) and measures the
  speed over the last ~100 ms at release. Releasing while the cursor is
  still moving fast (>= 0.5 px/ms) is a THROW -> maximize, no matter how
  long it dwelled at the edge first; a HOLD now requires the cursor
  parked in the zone >= 400 ms AND a slow release.
- The release position is queried FRESH (GetCursorPos inside
  WM_EXITSIZEMOVE) instead of trusting the last mouse sample.
- An edge touch within the last 150 ms also counts as a top gesture (the
  cursor can release slightly below the edge or cross into a monitor
  above on multi-monitor setups).

Second user follow-up: "still happens". Rather than guess a third
time, the classifier is now instrumented AND made race-proof:

- **Diagnostic line per drag release** (stderr, visible with
  --debug-view): `[WIN32] snap: syscmd=0x... zone=? recent=?
  dwell=..ms speed=..px/ms -> maximize|top-half|ignore` plus consumption
  lines (`throw -> maximize`, `hold -> top half`, `maximize lost to a
  late system snap - re-applying`). A still-failing throw is now
  diagnosable from the log alone.
- **De-gated the WM_SYSCOMMAND dependency**: if no WM_SYSCOMMAND precedes
  WM_ENTERSIZEMOVE (a missing SC_MOVE would have silently disabled the
  whole classifier - consistent with throws still landing half), the
  drag defaults to a MOVE. Resizes still see SC_SIZE first and keep their
  precise gate.
- **Late-snap retry**: after a throw's MaximizeWindow(), the main loop
  re-checks at +250 ms (2 retries) and re-maximizes if the system applied
  a half/quarter layout AFTER our call (IsWindowMaximized check).
- Recent-edge-touch window widened 150 -> 300 ms.

Validation: build clean; gui_testgen_rf, gui_cvbs_baseband,
gui_waveform_overlay all OK. If a throw STILL half-snaps, run with
`misrc_gui --debug-view`, reproduce, and read the `[WIN32] snap:` lines:
  - `-> ignore` on a real throw = classifier missed the gesture (raise
    GUI_WIN32_SNAP_RECENT_MS / widen the zone).
  - `-> top-half` on a real throw = speed threshold too low (raise
    GUI_WIN32_SNAP_THROW_PX_MS).
  - `-> maximize` yet still half = the retry log tells whether a late
    system snap overrode it.

Third follow-up: "debug view did nothing?" — the user's terminal showed no
output from `--debug-view` (the GUI exe's console attach does not surface
stderr in every terminal). The classifier summary is now ALSO shown in the
IN-APP STATUS BAR after every drag release (`Snap: zone=? recent=?
dwell=..ms speed=.. -> verdict`), via
`gui_win32_modal_take_drag_summary()` — no console needed. The user just
throws the window and reads the bottom bar.

### Not yet done

- PARKED (user, 2026-11-03, "note this issue for now"): throws at the top
  edge SOMETIMES still land in half mode despite the velocity classifier,
  the SYSCOMMAND de-gate and the late-snap retry. Next step: read the
  in-app STATUS BAR diagnostic after a bad throw (Snap: zone/recent/dwell/
  speed -> verdict) and tune the matching threshold. Rounds 12-13 stay
  uncommitted until the user re-confirms or lifts the park.
- User GUI click-through re-confirmation of rounds 12-13 (then commit).
- CVBS right/top padding diagnosis (needs screenshot).
- The stale-repo edits in `C:\Users\Harry\misrc_gui_dev` are uncommitted
  and NOT part of this work — decide whether to revert them there.

## Round 14 — --gen-system / --gen-format flags + the GUI-automation hard rule

User request: "add flags and make it a hard project dev rule all GUI
elements must have a GUI autoload trigger flag for automated testing.."

Changes:

1. **CLI flags** (misrc_gui.c): `--gen-system <pal|ntsc|secam|mesecam|0-3>`
   and `--gen-format <vhs|svhs|betamax|video8|hi8|0-4>` (also s-vhs/beta/
   8mm aliases; case-insensitive; numeric accepted). Parsed in the first-pass
   arg loop (GUI flags - never route to the capture CLI), applied AFTER the
   settings load and BEFORE the startup default-system push, so flag >
   --config > default. Fail fast: unknown value prints the accepted list
   and exits 1 (verified: exit 1 + message). Valid runs log `[AUTO]`
   lines (verified: `--gen-system mesecam --gen-format betamax` prints
   both and launches). Usage text updated. Note: a session that saves
   settings persists the overridden values.
2. **Hard rule doc** `dev/gui_automation_flags_rules.md` (+ dev/README.md
   index): every user-interactable GUI element ships with a CLI flag
   and/or --config key in the SAME change; fail fast; [AUTO] logging;
   observable effect; flag > config > default; panel-local runtime state
   exempt until it gains a persisted key. Includes the inventory of
   current triggers and the known-gaps backlog (panel view/split,
   Signal/Mode/Scale/Trig dropdowns, decoder-mode cycle, fullscreen flag,
   M-Client remote table lacks the gen keys).

Validation: build clean; gui_testgen_rf, gui_cvbs_baseband,
gui_waveform_overlay all OK; --help lists both flags; invalid-value and
valid-launch paths verified with Start-Process + redirected streams (the
GUI-subsystem exe's exit code does not read back through pwsh's `&` —
use Start-Process for scripted checks).

### Test Gen autoload/config state (user question 2026-11-03)

"did new CLI args get added for the autoload config for test gen modes
etc?" - NO new CLI args. What exists:
- `--config <file>` autoloads `video_system` (0-3, incl. MESECAM) and
  `gen_tape_format` (0-4); the round-10 startup push honors them
  (composite decoders map MESECAM->SECAM, RF preview starts MESECAM).
- Both keys persist in the default settings file on changes/exit.
- Unattended gen launch: `misrc_gui --config <json> --select-device sim
  --auto-capture` (the device is named "[Simulated] Test Signal").
- Not exposed: dedicated CLI flags (--gen-system/--gen-format) and the
  M-Client remote settings table. Offered to add flags on request.
