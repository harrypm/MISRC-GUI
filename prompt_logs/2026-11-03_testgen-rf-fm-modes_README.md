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

### Not yet done

- User GUI click-through re-confirmation (bezel contrast, gen row, RF
  follow).
- Commit + restore-point zip only after user confirms it is working.
- The stale-repo edits in `C:\Users\Harry\misrc_gui_dev` are uncommitted
  and NOT part of this work — decide whether to revert them there.
