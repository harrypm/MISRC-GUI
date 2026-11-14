# Prompt log: composite vectorscope panel + NTSC SMPTE SD colour bars

Date: 2026-11-14. Branch: `web-monitor-remote` (worktree `MISRC-GUI-web`).

## Request (harrypm)

> Change NTSC signal gen to use SMPTE SD colour bars, then begin work on a
> composite (not component) vectorscope, use the one from tbc-tools for the
> plugin window.

## UX sign-off (gui_ux_change_approval_rules.md)

Approved by harrypm (Harry Munday), 2026-11-14, in the session:

- Placement: the vectorscope is **its own panel view** like the other
  monitoring modes ("Vector" in the Left/Right/View pickers). The
  side-by-side-with-picture variant inside the Video panel is parked for later.
- Options carried over from tbc-analyse: **75% / 100% graticule** (plus Off),
  **density plot**, **multi-colour points**. Field select was not requested.
- Controls: right-aligned overlay cycle buttons in the Video panel's existing
  overlay style (`System`, `Graticule`, `Plot`, `Colour`); double-click hides
  the chrome like every other panel.
- View picker name is "Vector" (not "Vectorscope") so the content-derived
  sidebar dropdown width does not grow past "Histogram".

## What changed

- `signal/gui_testgen.c`: NTSC composite channel draws SMPTE SD bars
  (EG 1-1990 / ECR 1-1978): 75% bars, reverse-blue castellations, -I / 100%
  white / +Q / black / PLUGE 3.5-7.5-11.5 IRE. Bar chroma on the 92.5 IRE
  setup span; -I/+Q 20 IRE amplitude. PAL/SECAM and the tape-RF channel keep
  the 8-bar pattern (`gui_testgen_rf_test` asserts 8 bars).
- `signal/gui_vectorscope.[ch]`: lock-free per-field density accumulator +
  graticule target math.
- `signal/gui_cvbs.[ch]`: optional `decoder->vscope` sink fed from the
  burst-locked per-pixel demod (active picture of colour lines; NULL = no
  cost), field-end publish, and `gui_cvbs_feed_*` (the CVBS plugin's exact
  resampler + decoder chain, reusable outside the Video panel).
- `visualization/gui_vectorscope_panel.[ch]`: `PANEL_VIEW_VECTORSCOPE`.
  Graticule ported from tbc-tools `tbc-analyse/vectorscopedialog.cpp`
  (ticks, target boxes, zones, colours, multi-colour colouriser).
  Follows the Test Gen system like the Video panels.
- `core/misrc_gui.c`: `--panel-view <a|b>:<left>[,<right>]` (automation rule).

## Deviations from tbc-tools (deliberate)

- tbc-analyse draws the NTSC I axis at 147 deg and Q at 57 deg; the standard
  angles (burst at 180 deg) are I = 123 deg, Q = 33 deg. The SMPTE -I/+Q
  patches land on 303 / 33 deg, so the standard angles are used here.

## Verification

- `gui_vectorscope_test` (new, meson `gui_vectorscope`): Test Gen bars through
  the panel's decode chain at 40 MSPS / 12-bit sim levels.
  - NTSC SMPTE: all six 75% bars plus -I and +Q within 0.4 deg / 1.3%.
  - PAL 75%: all six bars within 0.03 deg / 2.4%.
- Full meson suite on Windows UCRT64: 57/58 pass; `update_cli` fails on an
  MSYS2 temp-path comparison unrelated to this change.
- Real GUI (Simulated device, `--gen-system ntsc --panel-view a:video,vector`):
  SMPTE bars in the Video panel; vectorscope clusters inside the 75% boxes,
  -I/+Q on their axes.

## Finding for follow-up (not changed)

The CVBS decoder reads the **PAL burst ~11% low** on the Test Gen signal
(burst/range 0.1426 vs 0.1607 expected; NTSC reads correctly). Its fixed
`gain = 0.2 / burst` constant (PAL's burst is 3/14 of the span, not 0.2)
cancels most of it, so PAL picture/vectorscope chroma lands +2%. The window
alignment against the PAL burst envelope should be looked at together with
the constant; both touch the validated Video colour path.

## Follow-up: Auto system (real MS2130, PAL EBU bars, 2026-11-14)

harrypm played real PAL EBU bars through the MS2130: the scope sat on its
NTSC default (a real source has no Test Gen system to follow), never
burst-locked, and plotted only a centre blob.

- The System button now cycles **Auto** -> PAL -> NTSC -> SECAM -> Auto and
  starts in Auto ("System: Auto (PAL)").
- Auto times the vertical sync: `cvbs_vsync_state_t.last_field_us` is the
  interval between broad-pulse runs (a run counts once a second broad
  follows at half-line cadence), measured independently of the
  decoder's EQ/cadence gates so it works while the decoder is on the
  wrong standard. `gui_vectorscope_field_period_system()` accepts 1-3 field
  periods (20 ms / 16.683 ms); 3 agreeing readings switch PAL <-> NTSC.
  625-line sources stay SECAM when SECAM was picked by the Test Gen follow.
- Measured: PAL on an NTSC decoder reads every field; NTSC on a PAL decoder
  reads ~1 in 8 fields (that decoder slices NTSC sync poorly), enough to
  switch in well under a second. `gui_vectorscope_test` covers both.
- Real GUI: MS2130 PAL source -> "Auto (PAL)", burst-locked trace.

## Follow-up: line-locked chroma (real MS2130, 2026-11-14)

> You need to have proper line-locked cvbs-decoding for the scope to work
> (harrypm, after the real EBU bars drew arcs instead of dots)

The scope fed off the picture path's chroma, which demodulates against a
carrier at the nominal fsc of the DECLARED sample rate. The MS2130's real
clock is off nominal, so the carrier slipped across every line and each bar
drew an arc.

- `signal/gui_cvbs.c` `cvbs_vscope_ll_*`: the scope now has its own
  line-locked chroma path. The carrier oscillator runs at exactly
  283.7516 (PAL) / 227.5 (NTSC) cycles per measured line, the line length
  being (H edge now - H edge N lines ago) / N over 32-640 lines. A single
  line's own edges are only good to ~1 sample (~90 deg of slip over a 4fsc
  line, measured), the span averages that to ~0.01 sample. The oscillator
  is continuous across consecutive lines, so the PAL V-switch keeps the
  picture path's burst-step convention. The capture clock error drops out.
- Burst measured on the flat envelope (one carrier cycle in from each end)
  and scaled to each standard's burst level (PAL 3/14, NTSC 0.2 of
  blanking..white). This fixes the PAL burst under-read for the scope.
- The Video picture path is unchanged (still nominal-rate carrier, 0.2
  burst constant).
- `gui_vectorscope_test`: NTSC within 0.2 deg / 0.5%; PAL within 0.06 deg /
  +2.8%; with a source clock 500 ppm off the declared 40 MSPS, NTSC is
  unchanged and PAL within 0.06 deg / +4.2%.
- Real GUI: MS2130 EBU bars plot a dot inside every 75% box with straight
  transitions.

## Follow-up: straight transitions (real MS2130 PAL bars, 2026-11-14)

> its looking better but points plot is warpy instead of lines from point to point

The line-locked demod correlated the RAW composite over a two-cycle box with
the window mean removed. That rejects flat luma but not a luma STEP inside
the window, so every bar transition bowed off its straight line.

- The scope chroma chain is now a composite vectorscope's: chroma bandpass
  (two cascaded `x[k] - (x[k-d] + x[k+d]) / 2` stages, d = half a carrier
  cycle; zero at DC, luma at 1 MHz ~-37 dB) -> line-locked demod -> Hann
  lowpass over two carrier cycles. All linear-phase and identical for U and
  V, so a transition plots as the straight line between its two vectors.
- The burst goes through the same chain and is read 2 carrier cycles in from
  each end of the profile window (envelope rise + the bandpass reach):
  the level is flat within 0.3% from 2 cycles (+4.5% PAL at 1 cycle).
- `gui_vectorscope_test` tolerances tightened to hue < 1 deg, level < 3%:
  NTSC and PAL bars (exact clock and 500 ppm off) all within 0.4 deg / 1%.
- Real GUI: MS2130 PAL EBU bars - dots centred in the 75% boxes, straight
  transitions in bar order.

## Follow-up: trace size + burst vectors (2026-11-14)

> little a bit too tight but the right direction (-> dot/trace size)
> burst line is also missing / there is a bubble in the middle not a line

- Panel: each hit draws a disc ~2.5 screen px across (full centre, 40% rim),
  sized from the current scope side (a 512-grid cell is often < 1 px, so a
  single lit cell drew a hairline). Rebuilt when the panel resizes.
- Decoder: the burst region (envelope included) is plotted too, through the
  same chain, with the TRANSMITTED V: NTSC burst at 180 deg, PAL bursts at
  135 / 225 deg, each a line from the centre (the envelope rise/fall). The
  picture stays V-switched onto single targets.
- Test: burst tips (0.015 radius - the envelope line would average into a
  wider cluster) NTSC within 0.2 deg / 1.1%, PAL 135/225 within 0.05 deg /
  0.3%.

## Follow-up: burst target marks

> add burst positons

Graticule now marks the burst target(s), whatever the 75% / 100% bar level
(hidden with Graticule: Off): NTSC at 20 IRE / 180 deg, PAL at 3/14 / 135 and
225 deg (both V-switch phases). Each mark is a radial tick plus a cross-tick
and a small ring, labelled "burst". They sit exactly where the plotted burst
vectors end, so a correctly locked, correctly levelled signal puts the burst
tip in the ring.

Signed-off-by: harrypm

## Follow-up: NTSC SMPTE bars on the real MS2130 - line-shifted decode

> Now testing SMPTE Bars with NTSC
> I think the decoder needs to be improved also dispite a pure stable source
> the decoder is all line shifted
> its shaking unstable on the vector all line shifted on the video decoder

Diagnosed offline from a 2 s raw dump of the MS2130 (temporary dump hook,
removed): the source is near-perfect (H edges 0.04-sample rms at their own
50% points) but the decoder's sync slicer crosses a single-pole ~500 kHz
filtered edge (tau 11.8 samples = 0.83 us at 4fsc) at a level that is not
the pulse's midpoint, and the AC-coupling sag after every vertical interval
moves that level: edges jumped ~11 samples line to line and curved 15
samples over each field (Video: jagged verticals; vectorscope: hues smeared
round arcs, comb mixing misaligned lines).

- `cvbs_refine_h_edge`: each classified H sync is re-timed at its own 50%
  point (its tip vs its front porch, lightly smoothed raw samples from a
  2048-sample ring) and fed to the PLL and the vectorscope line clock in the
  existing convention (edge = true start + CVBS_SYNC_EDGE_DELAY_SAMPLES).
  Real dump through the decode chain: line-to-line 6.0 -> 0.05 samples rms,
  field p2p 15.6 -> 0.4. Applies to the Video panel too.
- Vectorscope line-delay comb (NTSC 1 line, PAL 2 lines, fractional delay
  from the measured line length) ahead of the chroma bandpass: luma steps
  cancel on vertically uniform content.
- Auto system: the field period now comes from its own tip-relative slicer
  (`cvbs_field_detect`, broad pulses paired at half-line spacing). On the
  wrong standard the decoder's slicer level lands in the picture (~100k junk
  pulses / 2 s) and starved Auto; now every field measures (31/31 in the
  test for PAL<->NTSC either way, and on the real dump).
- Tests: all vectorscope targets pass; the field-period check now requires
  zero bad readings. Suite 57/58 (update_cli environmental).

Signed-off-by: harrypm
