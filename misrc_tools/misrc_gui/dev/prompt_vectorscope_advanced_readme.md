# Prompt log: Advanced (composite measurement) vectorscope + beam renderer

Branch `vectorscope` (worktree `MISRC-GUI-web`), on top of 24900d7.
Date: 2026-11-17 / 18. Status: implemented, tested on the Test Gen (PAL,
NTSC, SECAM) and checked in the GUI. harrypm confirmed PAL Advanced shows
both phases as intended (2026-11-17). The follow-up (SECAM Advanced, a single
Standard PAL burst, the NTSC live-switch fix) is described at the end.
Committed and pushed to main on 2026-11-18 at harrypm's request: "the core
stuff is all here for what I need now, so commit and push to main the
additions". That approval covers the controls as shipped
(`gui_ux_change_approval_rules.md`). NTSC Advanced still needs adjustments
(see Open).

## Input

> also we need to port the advanced vectorscope mode over aswell

With feedback from Stephen Neal (Discord, 2026-11-17) on the existing
scope: a PAL composite scope shows two bursts but only one content phase,
where a real PAL composite vectorscope shows both (V-switched, mirrored)
sets of bars. And: "is there an intensity control to avoid everything
looking the same brightness? You want bright points and dimmer lines between
them ... the intensity should reflect the amount of content of that value".

Follow-up (harrypm): "looks good just advanced mode needs both phases".
Asked which case was meant. NTSC has no V-switch, so decode-orc and
tbc-analyse plot one phase there and draw the lower-case set only as a
reference. Answer: PAL Advanced as shown (both sets filled) is right; NTSC
stays as decode-orc does it.

Source: tbc-tools tbc-analyse Advanced vectorscope tab (ported there from
decode-orc's composite measurement scope): `composite_vectorscope.{h,cpp}`
(acquisition, flywheel, readouts) and `vectorscopedialog.cpp`
(`getCompositeTraceImage`: hit + transit counts, beam spot, dwell anchor,
graticule).

## What was ported

| decode-orc / tbc-analyse | MISRC-GUI Vector panel |
|---|---|
| Composite carrier acquisition: one burst-locked reference, NO V-switch correction | `Mode: Advanced`. PAL bars plot on both mirrored target sets, bursts at 135 / 225 deg |
| Reference: +-32-line straight-line fit of the un-swung burst angle | Streaming: alpha-beta tracker on the un-swung burst angle (~20-line time constant), holds through burst-less lines |
| Slow burst-referenced gain | Burst-level average, 0.05 per line |
| Sample window Active / Whole line / Burst | `Lines: Active / Whole / Burst` (default Whole) |
| Field range | `Field: Active / Whole` (VBI lines included) |
| Readouts: burst level (IRE, % nominal), phase jitter, V split, chroma / burst | Same, per field, bottom-left. Burst IRE is referenced to the line's own sync height (PAL 300 mV = 42.86 IRE, NTSC 40 IRE), from the un-combed band-pass |
| Graticule: upper case = +V targets, lower case mirrored = -V; drawn for NTSC too as a reference | Same |
| Renderer: hits + Bresenham transits (both ends), Gaussian beam spot sigma = canvas / 410, dwell anchor = min(8 x lines / stride x spot0^2, half-dwell point), transit weight 0.04 x gain x stride / (lines x spot0^2), additive over the graticule, gain 1-10 (default 3) | Same law in both modes. `Intensity` button + mouse wheel. Points plot uses it; Density keeps `(level / max)^(1 / gain)` |
| Colour: decode-orc green / colourised / Blend (field 1 yellow, field 2 cyan) | `Colour: Mono / Multi / Blend` |
| `sample_stride` under a sample ceiling | Stride 2 when Trace is on (the transits join the samples), 1 when off |

Standard mode keeps the decoder's view (per-line burst lock, V-switch
undone, one target set) on the new renderer.

## Controls (UX sign-off needed)

Overlay buttons, right-aligned, in this order: System, Mode, Lines and Field
(Advanced only), Graticule, Plot, Colour, Trace, Intensity. The mouse wheel
over the panel steps Intensity 1-10. Graticule labels now sit just outside
their zones, and every label is drawn after all zones, so the mirrored set's
zones do not cover them at small panel sizes. NTSC's single burst label goes
below its marker (outward at 180 deg it sat on Yl).

Launch flag (`gui_automation_flags_rules.md`, same change):
`--vector mode=standard|advanced,lines=active|whole|burst|porch,field=active|whole,graticule=75|100|both|off,plot=points|density,colour=mono|multi|blend,trace=on|off,intensity=1..10,system=auto|pal|ntsc|secam`.
This closes the "vectorscope System/Graticule/Plot/Colour" known gap.

## CPU (harrypm rule: never increase CPU load)

Bench: the real `gui_cvbs.c` + panel source compiled into one harness with
the release flags (`-O3 -funroll-loops -ftree-vectorize -ffast-math -flto`).
It feeds Test Gen frames at 40 MS/s through `vs_process` (decode thread:
feed + decoder + accumulate). After every 64 k-sample chunk it runs
`vs_take_fields` + `vs_build_image` (render thread), so every field is
taken, as the 60 fps render loop does. The baseline is HEAD 24900d7's
sources extracted with `git archive`. Three interleaved runs; values are
medians.

PAL bars, decode us/line | render us/field:

| | HEAD 24900d7 | Standard, trace on | Standard, trace off | Advanced |
|---|---|---|---|---|
| Decode | 25.2 | 22.9 (-9%) | 22.9 (-9%) | 24.2 (-4%) |
| Render | 620 | 550 (-11%) | 310 (-50%) | 870 |
| Total CPU per second of video | 425 ms | 385 ms | 373 ms | 421 ms |

NTSC: decode 21.8 -> 19.9 (Standard), 20.3 (Advanced); render 668 -> 466
(Standard), 696 (Advanced).

Options (Standard, PAL, render): Multi 670, Blend 810, Density 590, Blend +
Density 1250 us. Blend spreads each field separately. Every Standard
combination stays at or under HEAD's total.

The first port cost +5.4 us/line decode and 2-3 ms/field render. The
savings that paid for the new features:

- **Decoder plot pass:** split into contiguous vectorisable passes:
  - the Hann lowpass of the carrier products;
  - rotation, gain and grid cell, with the line's box;
  - a scalar loop for the hit / transit counts.
  
  It previously ran the 9-tap lowpass per point with the plot call inside,
  so nothing vectorised.
- **decode-orc's stride 2** with the trace on.
- **Advanced extras:**
  - the chroma / burst sum is folded into the plot pass (every 2nd plotted
    point), not run as a second lowpass pass;
  - the raw burst level uses a rotation recurrence, not `cos` / `sin` per
    sample.
- **Field bounding box:** computed by the decoder. Grids are cleared only
  inside it, and the renderer uses it, so there is no 1M-cell scan.
- **`gui_vectorscope_take_swap`** trades buffers, so the 2 MB copy per
  field is gone.
- **Spreads:** separable and branch-free over zero-padded float buffers,
  limited to the box. Both fields go through one spread (linear) unless
  Blend is on.
- **Dwell anchor:** a one-pass check that the dwell at or above the
  8-a-line level holds half the total. When it does (dense video), decode-orc's
  histogram is skipped; it is computed only for sparse content.
- **Colour pass:** per-row level buffers and branch-free per-option loops,
  writing packed 32-bit pixels. Density uses a 3200-entry LUT
  (exponent + 7 mantissa bits) instead of `powf` per cell.
- **Texture upload:** `UpdateTextureRec` sends only the changed rows.

Bench caveats:

- Building it at `-O2` without `-ffast-math` overstated the costs
  (+40-80%).
- Rendering once per frame instead of per chunk drops every other field,
  so one parity slot only ever holds a warm-up fragment. That looks like a
  field-parity bug but is not one.
- Gaussian noise of 40 codes on the Test Gen frames shrinks the plotted
  box to about a third (in HEAD too), so it is not a usable stress input.

## Tests

`test/gui_vectorscope_test.c`, new `run_advanced(system, rate)`:

- PAL: all 12 targets (B..Yl and the mirrored b..yl) within 0.3 deg / 0.6%.
  Both bursts. Readouts: burst 78.9% of nominal (Test Gen 79%), jitter
  0.47 deg, V split 0.00 deg, chroma / burst 1.43.
- NTSC: targets + burst. Burst 80.0%, jitter 0.12-0.15 deg, chroma / burst
  1.24.
- PAL at +500 ppm source rate: burst 78.9%, jitter 0.49 deg.

The existing Standard runs (bars, -I / +Q, SECAM, transitions straight) pass
at stride 2. Full suite: 59 / 60. `update_cli` fails as before (an
environmental failure, unrelated).

GUI check (Simulated Test Signal, `--panel-view a:vector`): PAL Advanced
(Mono and Blend), Standard PAL and NTSC Advanced screenshots. Bars are bright
points with dim transition lines between them; the readouts match the test.

## Files

- `signal/gui_vectorscope.{h,c}`:
  - modes / windows / readouts;
  - transit grids;
  - cell-index transits with a one-cell fast path;
  - stride;
  - per-field box;
  - dirty-row tracking (acc pair all zero at every field start);
  - `take_swap`, `take_ex`.
- `signal/gui_cvbs.c`:
  - Advanced line path (`cvbs_vscope_ll_advanced`: flywheel, AGC, IRE,
    readouts), run on every line in Advanced;
  - vectorised `cvbs_vscope_ll_plot`;
  - one line counted per line;
  - SECAM stride.
- `visualization/gui_vectorscope_panel.{h,c}`:
  - field slots, beam renderer, Advanced graticule;
  - readouts, controls, `--vector`.
- `core/misrc_gui.c`: `--vector` arg + usage.
- `dev/gui_automation_flags_rules.md`: entry; known gap closed.
- `test/gui_vectorscope_test.c`: Advanced runs.

## Commands

```sh
# MSYS2 UCRT64 (PowerShell: $env:MSYSTEM="UCRT64"; C:\msys64\usr\bin\bash.exe -l script.sh)
export PKG_CONFIG_PATH=<scratch>/fakepc:/c/Users/Harry/MISRC-GUI/.deps/install/lib/pkgconfig:/ucrt64/lib/pkgconfig
ninja -C build-ucrt && (cd build-ucrt && meson test --no-rebuild --print-errorlogs)
./build-ucrt/gui_vectorscope_test.exe
# GUI
build-ucrt/misrc_gui.exe --select-device Simulated --gen-system pal --auto-capture \
    --vector mode=advanced --panel-view a:vector
```

## Follow-up, 2026-11-17 / 18

### Input

> Add advnaced mode for NTSC and SECAM, in standard mode PAL should only
> show one phase of burst

Then a screenshot of NTSC Advanced showing rings, with "Jitter 109.10 deg"
("not sure if it was an outdated build but NTSC was looking like this in
advanced mode").

Questions asked and answers:

- **SECAM Advanced.** decode-orc and tbc-analyse have no SECAM composite
  scope ("Composite acquisition needs PAL, NTSC or PAL-M (SECAM carries FM
  chroma)"). Asked which design to build. harrypm chose "Transmitted,
  unpaired".
- **NTSC Advanced** already existed. harrypm chose "Nothing, just check": make
  sure it still works across live PAL -> NTSC -> SECAM switches.

### Changes

- **Standard PAL burst: one phase.** The burst window is V-switched like the
  picture, so every burst lands at 135 deg. The graticule shows one "burst"
  mark in Standard and both (Burst / burst) in Advanced. Test: `bst135`
  share 3.1% (doubled); nothing at 225 deg (at most 0.2%; the Yl -> Cy
  transition passes within 0.012 of that point, 0.04-0.06%).
- **SECAM Advanced (the transmitted view).**
  - Plot: each line plots only its own colour difference, D'B lines along U
    and D'R lines along V, with no receiver one-line pairing.
  - Reference: the field-tracked rest carrier, not the line's own porch, so
    a line's rest error shows as an offset.
  - Lines: Active / Whole / Porch (the back-porch rest carrier; `lines=porch`
    is accepted as an alias of `burst`).
  - Field: Active / Whole. VBI lines plot the bell-free carrier frequency.
  - Graticule: each bar's D'B value on the U axis and its D'R value on the
    V axis. Labels alternate sides; D'B / D'R axis captions.
  - Readouts: rest-carrier error per component (kHz), line-to-line rest
    jitter (kHz), porch carrier level in mV p-p against the line's sync
    (300 mV) and as a % of the standard, and ident errors (consecutive
    lines with a decisive porch carrying the same component).
  - Standard level: 2M0 = 23% of luminance (161 mV p-p) at f0 = 4.286 MHz
    through the bell, |1 + j16u| / |1 + j1.26u|. That gives D'B 166.7 mV and
    D'R 214.5 mV.
- **NTSC Advanced live-switch fix (root cause of the 109 deg screenshot).**
  - Reproduced offline: one frame of PAL "tail" decoded as NTSC (the
    pipeline after a Test Gen switch) gave jitter 108.7 deg and C/B 1.41
    forever, as in the screenshot.
  - Cause: the scope's line-length measurement accepted the PAL line during
    the tail (916.4 samples at the NTSC rate, 0.8% long, inside its 3%
    gate). It then counted each span with that stale length. The count went
    off by more than 0.1 line within about 12 lines, so every span restarted
    before `CVBS_LL_MIN_LINES` (32) and the stale length stuck.
  - Effect: a carrier 0.8% off, so the bars rotated across each line (rings)
    and from line to line (jitter). Standard mode smeared too.
  - Fix: count lines edge to edge with the PLL's current period;
    span / count stays the precise length.
  - Safety net: the Advanced flywheel re-acquires after 8 consecutive lines
    with more than 60 deg innovation.
  - Result: relocks within 2 frames (1-frame tail) to 6 frames (3-frame
    tail), to 0.15 deg.
- **Stale-mode fields.** After a Mode / Lines / Field click, up to two fields
  latched in the old setting are still in flight. The panel now drops them,
  with their buffers zeroed for the scope. A screenshot taken just after an
  Advanced -> Standard click showed SECAM's unpaired axes under the Standard
  graticule. Offline, Standard SECAM stays paired after every switch.
- **SECAM porch level calibration (checked against hard data).**
  - Ground truth: a direct quadrature fit of the generated Test Gen porch
    (scratchpad `probe_secam_porch.c`) gives D'B 132.3 mV p-p (79.3%) and
    D'R 174.2 mV (81.2%).
  - Problem: the decoder's porch envelope read 118 / 157 mV. Its band
    low-pass (Hamming FIR) is not flat at the carriers' +-78.125 kHz offset
    from the block centre.
  - Fix: the readout divides by the filter's gain there, computed from its
    taps per rebuild, and uses the envelope median.
  - Result: 132 mV (79.3%) / 175-176 mV (81.7%).
- **Run-length counting.** A beam resting in one cell keeps its hit and
  transit counts in registers until it moves; the transit end counts fold
  into the runs, and the box grows at the flush. New test
  `run_trace_counts`: a 200 k-step random walk (stays, steps, jumps,
  off-plot points, breaks) compared with a straight port of decode-orc's
  `accumulateLine` gives 0 hit / 0 transit cells different and an exact box.
- **SECAM point pass.** Shared with PAL / NTSC: the Catmull-Rom weights are
  tabled once per call (they were recomputed per point), the cells are
  computed in one pass, then the shared counting function runs. The stride
  start uses a mask, not two `%` by a runtime value per point.

### CPU (medians of 6 interleaved runs, bars; decode us/line, render us/field)

| | HEAD 24900d7 | Standard | Advanced |
|---|---|---|---|
| PAL decode | 25.63 | 23.02 (-10%) | 24.55 (-4%) |
| PAL render | 620 | 527 (-15%) | 833 |
| SECAM decode | 28.61 | 28.66 (=) | 29.18 (+2%) |
| SECAM render | 613 | 566 (-8%) | 591 |

Total per second of video against HEAD:

- PAL Standard: -10%.
- PAL Advanced: -1.5%.
- SECAM Standard: -0.3%.
- SECAM Advanced (new mode): +1.6%.

The first SECAM build was +6.9% decode until the point pass was shared; the
`%` alone cost about 0.8 us/line.

### Tests added

- `run_trace_counts` (exact equivalence with decode-orc's counts).
- `run_advanced_secam`:
  - axis targets for all 12 values;
  - 0.000% of points off the axes;
  - rest error under 0.05 kHz, jitter 0.16 kHz;
  - porch 79.3% / 81.7% against 79.3 / 81.2 (+-3);
  - no ident errors.
- `run_switch_tail`: PAL -> NTSC with a PAL tail in Advanced. Max jitter
  0.17 deg over the last 8 fields.
- Standard PAL / PAL+500ppm: `bst225` absent.

## Open

- NTSC Advanced "needs some adjustments" (harrypm, 2026-11-18). Not yet
  specified; ask what before changing it. Its readouts and single-phase
  plot follow decode-orc.
- A real PAL capture (MS2130 / MISRC) through Advanced: jitter and V split
  on real timebase error, and the Whole-line sync-edge leak on lines without
  comb history (it shows as excursions beyond the bars and widens the render
  box).
