# Prompt Log — 2026-10-19 — CVBS colour phase (real-capture fit), full-frame hybrid (tbc-tools analyse), FFT wording/chrome scope

Companion to `2026-10-19_cvbs-colour-phase-hybrid-full_README.md` (the full commands,
outputs, A/B measurements, the reconstruction of the hard-crashed 2026-10-18 session,
and the fix derivations) — same folder, moved from the repo root 2026-10-29.

Headless-verified state (not GUI-confirmed): canonical build + smoke test PASS; full
meson suite 34/34 (incl. the new `gui_cvbs_baseband` full-view hybrid contracts and
`gui_fft_label`); the real HackDAC EBU PAL capture decodes with every bar within ±0.7°
of the EBU vectors in BOTH sync modes (AUTO and the GUI-default FIXED), 99/99 fields
accepted / 0 rejected, self-calibrated rate 40.00 MHz; the full-frame hybrid profiled
on the decoded frame matches the tbc-tools-analyse model (porch ~57/255 blanking band,
sync tips at the floor, studio-swing active window, background keeps decoded chroma).

## Pending GUI verification checklist (user)

1. CVBS panel on the live EBU signal (play the
   `Desktop\EBU_PAL_HackDac2_MISRC_V2.5\10sec-40msps\...Video_RF_12-bit.flac` channel A,
   or make a fresh capture):
   - Colours: the EBU bars decode in the correct order and hues (white, yellow, cyan,
     green, magenta, red, blue, black) — no rotation/mirroring, no per-line hue
     flicker (Hanover bars), stable across both Sync modes (Auto/Fixed).
   - Frame: Active — CRT/production levels: black lifted (not crushed at floor),
     whites with headroom, no pumping.
   - Frame: Full — the hybrid look, comparable to tbc-tools analyse: the VBI
     EQ/broad pulse trains readable at CRT contrast BELOW active black, a ~dark-grey
     blanking band, sync tips at/below the band, the burst region tinted with its
     decoded colour, and the active picture area unchanged (correct levels + colour).
   - Colour: Off — the exact grayscale (R=G=B) in both Frame views.
   - The mode survives worker rebinds / DSP resets as before.
2. FFT panel:
   - The Scale button reads the full words "Scale: Linear" / "Scale: Log" and the
     panel starts on Linear (the stock original mode).
   - Double-click chrome-hide hides ONLY the UI (the Scale button + the "FFT"
     corner title). The dB axis labels (left), the frequency labels (bottom), the
     /div (or visible-span) + zoom readouts and the peak-hover marker + readout must
     STAY VISIBLE while hidden; a second double-click restores everything.
   - Clicks where the hidden button was must NOT trigger it; a control click still
     resets the double-click tracker.
3. Sanity: the VHS FM preview and the other panels are unchanged (this session
   touched `gui_cvbs.c/h`, `gui_rf_preview.h/.c`, `gui_fft.c`, `gui_cvbs_test.c` only).

## Known observations (recorded, non-blocking; see the readme)
- The live bars decode ~10–20% under-saturated vs the EBU 75% amplitudes (hue exact)
  and the white bar reads ~83% display — possible follow-up calibration items, noted
  for the next session if the GUI look warrants it.

## Round 2 + 3 outcome (same day, user-confirmed)
- Round 2 (the GUI panel path): `--panel` diag mode reproduced the exact GUI failure
  headlessly; three stacked root causes fixed (the per-line carrier frame's origin
  wobble at the panel's 4x-fsc rate breaking the V-switch on every other line; the
  absolute carrier frame demanding an exact rate; the rate self-calibration
  overriding the panel resampler's exact-by-construction rate -> `rate_trusted` +
  the hybrid carrier frame + sub-sample sync edges). Every EBU bar within +-0.2 deg
  on the exact plugin path, coherence 1.00.
- Round 3 (the render path): the RGB24->RGBA uint32 packing swapped R/B against
  raylib's Color byte order (yellow<->cyan, red<->blue in the GUI while every
  headless dump was correct) - fixed by writing through Color struct fields.
- Hard rules added: `misrc_tools/misrc_gui/dev/gui_output_verification_rules.md`
  + the render conversion extracted to `cvbs_blit_rgb24_to_color` with a byte-layout
  contract in the suite. Rounds 2+3 committed after the user's GUI colour-order
  confirmation; the FFT Linear/Log feature stays uncommitted pending its own check.
- Handed off (user request, next round): the sync/PLL robustness - the picture does
  not lock stably about half the time and sync is easily lost.

## Deliberately not done
- The FFT Linear/Log feature files stay uncommitted pending their own GUI
  confirmation (gui_fft.c, gui_fft_label_harness.c, the meson.build harness hunk).
- Zip restore point (`git archive` snapshot) waits for the user's "fully working"
  confirmation per the user's rule; update this log + create it after confirmation.
