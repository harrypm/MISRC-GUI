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

## Follow-up: line stability of all CVBS system modes

> Line stablity of all cvbs system modes
> a critical thing is to not increese the CPU load demand

Measured offline with an analytic composite (exact line timing, raised-cosine
edges, burst + chroma on alternate bars) through the real panel path (soxr to
the fixed decode rate, then the decoder), locating every vertical bar edge per
decoded row at its half-level crossing. Before: every edge sawtoothed ~1 px
p2p on every system (NTSC 0.35-0.4 px line-to-line, PAL 0.45-1.1, SECAM 0.55),
with +0.7 px spikes at the right of the frame every ~6 lines. Two causes, both
in `decode_line_to_pixels`:

- The line wraps on a WHOLE sample, so each line's buffer starts 0..1 sample
  after its true origin and the fraction walks with the non-integer line
  length (908.84 samples NTSC, 1145.6 PAL at the decode rates). The PLL phase
  at decode time minus the buffer count is that offset exactly; the pixel loop
  now places luma by it (sub-sample).
- The pixel scale was the buffer's actual count (908 or 909), so each line was
  stretched by up to 1/909 - proportional to x, hence the right-side spikes.
  The scale is now the nominal line period (one line_us across the frame
  width, the render crop's mapping); the count is only the read bound.

After (line-to-line rms / field p2p, px): NTSC 0.012 / 0.034, PAL 0.006 /
0.012, 625 as SECAM 0.008 / 0.016; +-500 ppm clock the same; 34 dB SNR
(30 codes rms) 0.07 / 0.24; real MS2130 NTSC capture 0.009 / 0.03 (was
0.16 / 0.44). Limits: below ~30 dB SNR isolated lines still jump (a single
misread sync); not a tape-level concern.

Colour edges: every colour transition decoded with alternating hue fringes
and a 2-line zigzag (~1 px PAL, ~0.5 px NTSC; the same on the real MS2130
SMPTE capture) - the luma step inside the 2-cycle demod window correlated
with the carrier at its absolute phase, which turns 180/270 deg per line
against the burst reference. The picture chroma path now runs the
vectorscope's front end (line-delay comb, NTSC 1 line / PAL 2 lines, from
the decoder's sample ring, then the half-cycle bandpass), and luma is the
composite minus the separated chroma (comb Y / notch), which also removed
the alternating dot column the raw box mean left at chroma edges. The real
capture's edges are clean. The Test Gen's own NTSC bars keep a mild 2-line
residual: the generator's integer-sample slots make its lines 2542/2543
samples long, so its subcarrier advances 227.5 +- 0.09 cycles per line
(+-32 deg jitter a 1-line comb cannot cancel); a real signal has none.
Fractional slot timing in gui_testgen is the follow-up.

CPU (us per decoded line, the decoder's own profile counters, baseline = the
committed vectorscope branch built from the same sources, 5 alternating runs,
min / median): NTSC colour 20.9 / 23.5 before, 19.8 / 23.7 after; PAL colour
28.9 / 30.6 before, 24.6 / 33.0 after (single runs scatter +-20%, so: equal).
Gray decode (Colour: Off) 4.5 -> 5.9 (the per-pixel luma interpolation; a
one-pass shift of the filtered line would remove it if wanted). Kept neutral
by skipping the 17-tap luma kernel body on colour lines (their luma is the
window box mean), dropping the window-mean prefix arrays (the bandpass output
is mean-free), fusing the bandpass into one 5-tap pass, chroma on the nearest
whole-sample window (luma alone is placed sub-sample), and keeping the pixel
loop integer. An exact chroma placement (one fractional-delay pass of the
line) measured ~+2 us/line and took the real capture's edge alternation from
1.0 to 0.5 levels; not taken. Note for later: the soxr HQ resampler costs
about twice the whole line decode on this path (1.7-2.4 s per 2 s of video
in the harness) - the real CPU lever here.

- Test: `gui_cvbs_line_test`. Luma-only sources (525/625, exact and +-60
  ppm, 625 as SECAM): every edge straight within 0.05 px line-to-line / 0.15
  px p2p. Sources with burst + chroma on alternate bars (the colour path):
  0.2 / 0.45 px - measured ~0.10 px line-to-line, chroma-to-luma crosstalk
  where a chroma edge coincides with a luma edge (the bandpass passes only
  part of the chroma envelope's sidebands; the leftover in luma alternates
  with the carrier's per-line phase). Exact chroma placement did not change
  it, so it is the chain, not the placement. Measured on complete fields:
  the rows above picture_start_line render through the hybrid VBI mapping
  (plateaus clipped) and read ~0.2 px off - contrast, not timing.
- Ring `h_ring` 2048 -> 8192 samples (two PAL lines for the comb).

Signed-off-by: (pending harrypm - please check Video panel + Vector on the
MS2130 with NTSC SMPTE and a PAL source)

## Follow-up: Test Gen accuracy, measured with the vectorscope (2026-11-16)

User request:

> use the vectorscope to cleanup the NTSC/SECAM and PAL test signal genarator modes to make them more accurate
> add a both mode for the graticules mode for vectorscope

Instruments (scratchpad `vs_measure.c`, kept as the new test below): the
Vector panel's exact chain (`gui_cvbs_feed`, scope-only) per-target
centroid / spread / off-path energy, plus DIRECT measurement of the raw
generator samples (AC bump subtracted): H-sync 50% edges against the exact
half-line slot grid, burst carrier phase per line in the absolute stream
frame, burst envelope timing, SECAM per-bar FM plateau frequency.

Found (old generator, 40 MSPS):

| fault | NTSC | PAL | SECAM |
|---|---|---|---|
| whole-sample line starts (2542/2543-sample lines) | SCH jitter 9.3 deg rms, 16 deg max | exact at 40 MSPS | exact at 40 MSPS |
| edge kernel a HALF-cosine ramp (one-sided) | every sync/luma edge ~3.5 samples (87 ns) early vs chroma, lopsided | same | same |
| burst window `ht_burstwin` | hard step at +0.15 us, 50% width 2.35 us (spec 2.50) | hard step, 2.10 us (spec 2.25) | - |
| burst 50% start after sync 50% | 5.54 us (spec 5.30) | 5.84 us (spec 5.60) | - |
| subcarrier restarted at phase 0 every frame | -169 deg jump every frame | +90 deg every frame | - |
| PAL V-switch from per-field H index | - | broken at both field boundaries | - |
| Simulated loop of 2 frames | - | 180 deg carrier jump every 80 ms | - |
| not visible on the scope | - | - | "FM chroma has no burst phase" |

Changed (`signal/gui_testgen.c`, `.h`, `input/gui_simulated.c`):

- Sub-sample line model: every slot starts at its exact position
  `s * need / slots`; each sample is the area average of the piecewise-
  constant line content (the one edge it may straddle found by bisection).
- PASS 2 edge kernel: symmetric Hann pulse, 10-90% = `sync_rise_us` (now
  0.19 us, the EBU reference's measured edge, stated directly).
- Burst window: raised-cosine edges, 50% points at burst start and
  start + width, 10-90% build-up = `burst_rise_us` (0.30 us, ITU/SMPTE).
- Subcarrier locked to the generated line rate (exactly 119437.5 / 177344.75
  cycles per frame) and continued across `frame_index`; PAL V-switch from
  the frame-continuous line count. New `gui_testgen_loop_frames()` (PAL 4,
  NTSC 2, SECAM 2); the Simulated device loops that many composite frames
  (RF stays 2). Its realloc failure path no longer frees live buffers.
- SECAM chroma pass uses the same fractional line origin (identical output
  at 40 MSPS).
- Generation 7x faster (lrint per sample -> clamp + add-and-truncate,
  rotating carrier, recursive AC-bump exponentials): PAL 349 -> 48 ms per
  frame, NTSC 304 -> 41, SECAM 495 -> 124. A PAL system switch (now 4
  frames) stalls the simulated stream 0.19 s instead of 0.70 s.

After (40 MSPS): SCH jitter NTSC 0.06 deg rms; sync 50% exactly on the slot
grid; burst 5.28 / 5.59 us start, 2.49 / 2.25 us wide; zero carrier or
V-switch breaks across every VBI and the loop seam. Through the Vector panel
chain: bar centroids unchanged (within 0.2 deg / 0.4%), off-path energy NTSC
0.366 -> 0.291%, PAL 0.534 -> 0.422%. Decoded picture bars: NTSC every high
component 212 and every low 30 (old: 212..219 / 22..29 drifting across the
bars); PAL high-component drift 178 -> 202 became 176 -> 190.

Vectorscope SECAM (`signal/gui_cvbs.[ch]`, `visualization/gui_vectorscope_panel.c`):
the SECAM line decode's D'B/D'R (the u01/v01 the picture renders) plot as
U/V on the PAL 75% targets; the SECAM decode now also runs for a scope-only
feed. Its one-line hold rings and de-emphasis IIR state moved from file
scope into `cvbs_secam_state_t` (a filter-rebuild generation clears them):
two SECAM decoders (Video + Vector) handed each other their last line at
every chunk boundary. SECAM Test Gen on the scope: every bar within 2.8 deg
hue and 6% amplitude of the 75% targets x 0.75; the generator's own FM
plateaus are within 5.6 kHz of rest + D x deviation x 0.75, so the residual
is the SECAM demod's.

Graticule: new "Both" mode (75% and 100% targets together, one label per
colour at the outer target).

Tests: new `gui_testgen_cvbs` (direct accuracy contract; fails 14 checks on
the old generator, passes the new one, 0.9 s). `gui_vectorscope` now runs
the scope-only feed and adds SECAM. Suite 58/59 (update_cli environmental).
CPU of the PAL/NTSC decode unchanged (no code path touched).

Not changed, needs a decision:

- `HT_CHROMA_GAIN` 0.75 (the EBU HackDAC reference's measured chroma level)
  stays. PAL/NTSC: invisible after burst referencing (scope and picture),
  visible only as chroma excursion on the waveform. SECAM has no burst, so
  the 0.75 x deviation shows as 25% short bars on the scope and 25% less
  saturation in the picture. Standard SECAM deviation is a one-line change
  (the D'x deviation gain in PASS 1.5).
- PAL picture saturation (decoder, not generator): the picture path still
  uses NTSC's `0.2 / burst` gain and a burst window reaching into the
  envelope ramps (the scope path uses 3/14 and a 2-cycle trim). Decoded PAL
  bars read high components 176..190 across the bars where they should be
  equal.
- Chroma bar transitions are hard-switched (no ~1.3 MHz chroma band-limit);
  invisible on the scope (linear chain), small cross-luma in the picture.

### Against the live EBU PAL / SMPTE NTSC (2026-11-16, later)

User feedback:

> bars are off a bit compared to a real sample and scope readout
> or I should say the vector is? I have live EBU PAL playing

A 2 s raw dump of the live EBU PAL (MS2130, the Video plugin's input; a
temporary `MISRC_CVBS_DUMP` hook, removed again) and the earlier SMPTE NTSC
dump were read with the same instruments as the Test Gen: the Vector chain,
and a waveform-monitor style per-bar readout (luma, chroma against each
line's own sync height, hue against the burst, transition timing through the
scope's own detector).

| | live EBU PAL | live SMPTE NTSC | Test Gen before | Test Gen after |
|---|---|---|---|---|
| bars on the Vector panel | within 0.2 deg / 0.8% | within 0.4 deg | within 0.2 deg / 0.4% | unchanged |
| points off the straight transitions | 0.009% | 0.003% | 0.42% PAL / 0.29% NTSC | 0.000% / 0.002% |
| chroma + burst vs sync | 0.79 | 0.80 | 0.75 | 0.79 / 0.80 |
| bar-to-bar 10-90 (scope detector) | 0.570 us | 0.585 us | 0.43 / 0.49 (instant switch) | 0.569 / 0.583 |
| PAL active start (50% of white) | 10.61 us | - | 10.47 | 10.57 |

What the user saw: the Test Gen switched bar chroma instantly, so every
transition on the Vector panel was a chain of loops (the 2 x fsc image of a
wideband step leaking past the 2-cycle demod lowpass), where the live bars
draw clean straight lines. Changed:

- Band-limited bar chroma: each colour step is an integrated raised-cosine
  (10-90 0.34 us PAL / 0.30 us NTSC, matched on the live captures through
  the scope's detector), rendered from the line's exact steps.
- Chain gain per system: PAL 0.79, NTSC 0.80 (measured live, chroma and
  burst agreeing to 0.3%); SECAM keeps 0.75 (`HT_SECAM_CHROMA_GAIN`).
- PAL/SECAM active line 10.50 / 52.0 us (ITU; live 10.54): hacktv's 10.40
  started the picture inside the decoder's back-porch window (profile 10.5).

Not copied from the live sources (their imperfections): NTSC source hue
-0.2..-0.4 deg on every bar; PAL source transitions drawn as slightly
doubled lines (a small U/V timing mismatch in its encoder); live luma
1-1.5% low and PAL black 0.008 below blank.

Tests: `gui_vectorscope` now also fails if more than 0.05% of points leave
the straight transitions; its SECAM clusters use a 0.04 radius (the 0.08
one caught the FM demod's blue->black overshoot after the 0.1 us bar move).
Suite 58/59 (update_cli environmental). Generation 55 / 47 / 126 ms per
PAL / NTSC / SECAM frame (originally 349 / 304 / 495).

### SECAM on the Vector panel: lines, standard deviation (2026-11-16, later)

User feedback:

> PAL Looks good, NTSC is okay, SECAM is still a bit off vector is not useful dots but no lines

- Dots, no lines: the SECAM demod runs decimated (~2.2 MHz, `g_secam_decim`
  panel samples per point) and the scope plotted only those points, so a
  bar-to-bar transition got one point or none. The scope now traces a
  Catmull-Rom curve through the decimated points at the panel rate (one
  point per panel sample, like PAL/NTSC): 534k -> 4.3M points per 16
  fields, transitions drawn (with SECAM's own pre-emphasis hooks).
- A bit off: the generator's SECAM deviation carried the old 0.75 card
  factor, every bar a quarter inside its target. A capture chain scales the
  subcarrier amplitude, never the FM deviation: standard deviation
  (`HT_SECAM_DEV_GAIN` 1.0), amplitude at the PAL chain gain 0.79. SECAM
  bars now land within 1.6 deg / 3.6% of the standard 75% targets (the SECAM
  demod's own error); the generator's FM plateaus within 10.4 kHz (the
  standard LF pre-emphasis still settling in the bar centre, under 4% of
  the deviation; `gui_testgen_cvbs` limit 15 kHz).

Signed-off-by: harrypm (PAL "looks good", NTSC "okay" on the live A/B; SECAM pending)
