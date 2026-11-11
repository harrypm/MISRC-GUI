# 2026-10-27 — Test Gen simulated CVBS: V-sync rolling fix (interlaced half-line model)

Status: PAL SYNC FIX USER-VERIFIED. ROUND 2 (contrast + NTSC/SECAM)
USER-VERIFIED: "massive improvement although the signal on the left and
right sides are a bit broken". ROUND 3 (the solid-white cast strips + the
baseline bump) USER-REJECTED: "broke it more - hard white in the black, the
white is too big". ROUND 4 (the cast strips REMOVED - the reference is
plain full-height bars, the hacktv av_test pattern; the coupling bump
reshaped to peak at the picture's start) USER-VERIFIED ("good", 2026-10-27).
NEXT: SECAM clean-up (the pre-emphasis/ident port from hacktv secam.c).

## Symptom

The simulated device's hacktv-ported CVBS bars signal (Gen mode) rolls
vertically in the Video panel. Horizontal is stable. Amplitude/contrast were
already matched to the real EBU HackDAC reference in the previous session
(+0.7 / -0.3 envelope, 2047x scale in the 12-bit domain).

## Root cause (from the decoder's gates, not theory)

`gui_cvbs.c` `cvbs_vbi_first_eq()` derives FIELD PARITY from the offset of
the first EQ pulse of each VBI train relative to the last H sync:

- half-line offset   -> PAL parity 0 (boundary = first_eq - 0.5 lines)
- integer-line offset -> PAL parity 1 (boundary = first_eq - 1.0 lines)

and requires parity to ALTERNATE between consecutive candidates, plus a
312.5-line cadence (+/-2.0).

The old `gui_testgen.c` classified whole 64us lines (trains at integer line
positions). Measured pulse grid of the old generator:

- field 1 first EQ at frame slot 0, last H of the previous frame at -1.0
  lines -> integer offset -> parity 1
- field 2 first EQ at line 314 (t=313 lines), last H at line 313 (t=312)
  -> integer offset -> parity 1

BOTH fields derived as parity 1 -> every second candidate rejected with
"vertical fields repeat parity" (`reject_parity`), no field ever anchored,
the weave never stabilised -> vertical rolling.

## Fix

Rewrote `misrc_tools/misrc_gui/signal/gui_testgen.c` around a HALF-LINE
(32us) SLOT GRID - the structure a real interlaced signal has:

PAL (1250 slots/frame):
- field 1 train: EQ x5 slots 0-4, broad x5 slots 5-9, EQ x5 slots 10-14
- H grid: ODD slots 15..623 (field 1), 641..1249 (field 2)
- field 2 train: EQ x5 slots 625-629, broad x5 slots 630-634, EQ x5 635-639
- blank slots: 624, 640
NTSC (1050 slots): same shape, 6 pulses per train, H grid on EVEN slots.

The two H grids sit 625 slots (312.5 lines) apart - that IS the interlace.
The slot start samples are spread over the frame (need/1250 +-1), so the
line period is exact at any rate and the old short-last-line stream jump
every frame is gone.

### Decoder-gate arithmetic (verified against the generated buffers)

PAL:
- recent-H gate: f1 first EQ at slot 0 vs last H (prev frame slot 1249) =
  0.5 lines (< 2.1 OK); f2 first EQ at slot 625 vs last H1 (slot 623) =
  1.0 line (OK)
- parity gate: f1 half -> parity 0; f2 integer -> parity 1 (alternating OK)
- cadence gate: field starts -0.5 / 312.0 lines -> 312.5 apart (OK)
NTSC mirrors it (parity 0 = integer offset, boundary 1.0; cadence 262/263).

PAL V-switch and SECAM D'R'/D'B' parity now key off a per-field H-line
index (reset at each field boundary, even index -> +V/+135deg/D'R'), which
reproduces the previously-verified colour phase mapping: field 1's first
ACTIVE line carries +V (old line_1 23 odd -> +1), field 2's first ACTIVE
line -V (old line_1 336 even -> -1), alternating line by line, with the
same-phase H pair across each frame boundary (the real PAL 4-field wrap).

## Verification done

- `meson compile -C build-local`: all 58 targets link clean.
- `meson test -C build-local`: 44/45. The one failure (`update_cli`) is
  PRE-EXISTING and unrelated (updater version-compare semantics from the
  previous session: compare returns 0 for a dev current version, while
  update_cli_test.py:172 expects v9.9.9 to compare newer than dev-...).
  Left untouched deliberately - it contradicts the user-requested "dev
  versions must not show as outdated" change; needs a user decision.
- Structural pulse-grid check (throwaway harness in %TEMP%\testgen_check,
  generated buffers scanned at 40 MSPS): PAL EQ/broad/H slot positions and
  widths as designed (EQ 2.35us @ slots 0-4/10-14/625-629/635-639 at the
  -0.25 FS threshold reading 2.25; broad 27.3; H 4.7 @ odd slots; blank
  slots 624/640 empty; last H at slot 1249). NTSC even H grid + EQ x6 at
  slots 525-530 confirmed. Burst dips to -0.30 FS (3/7 x 0.7) are the
  accepted burst amplitude, not a defect.

## Awaiting user verification

1. GUI: Simulated device + Gen (PAL) - vertical rolling gone, bars stable.
2. Gen NTSC and SECAM cycle - same stability.
3. Colour still correct (burst/V-switch parity preserved by construction,
   but confirm on screen).

## Round 2: the active-area contrast (washed out) + NTSC/SECAM

A/B against the real reference (Desktop\cvbs-pal-ebu_2026.10.19_18.01.52_
Video_RF_12-bit.flac), decoded with the EXACT GUI panel path
(gui_cvbs_test --panel --full on both raws):

1. WASH-OUT (all systems). The decoder's white commit = blank + 90% of the
   filtered luma max (gui_cvbs.c:2758) needs the pattern's luma max ABOVE
   the bar-white plateau. The real EBU card's castellated edge strips sit at
   0.856 composite = 1.22x the 0.70 bar white (measured); the sim's plain
   full-field bars had luma max = the bar white exactly -> the commit landed
   10% below the plateau -> the display range (21,688) narrower than the
   signal span (24,304): the bright bars clamped flat at 255 (the real's
   peak = 212) and the darks lifted. FIX: the EBU castellated edge strips
   (black/white checker, left+right, active/16 wide, 0.854 white) - the
   commit then lands at 26,423 (the real's 26,629) and the white bar maps
   to ~0.92 of display range, no clamping. No DC shift: the calibrated
   -0.30/+0.70 waveform readout is preserved.
2. THE CHAIN CHROMA GAIN 0.75. The real capture's bar chroma and burst
   measure uniformly 0.75x the standard amplitudes (yellow chroma 0.176
   composite vs the standard 0.235; burst 0.221 p-p vs 0.3 - the earlier
   probe's burst number 7,685 confirmed by the aligned dump). The generator
   scales U/V and the burst by HT_CHROMA_GAIN 0.75; the burst fracs are the
   standards (PAL 3/14, NTSC 0.20). The decoded bar amps now match the
   real's (yellow 0.305 vs 0.288, cyan 0.441 vs 0.418, red 0.457 vs 0.445,
   blue 0.327 vs 0.318).
3. SECAM. Two bugs: (a) the FM subcarrier rode the H-sync (the porch branch
   ran for x < hsync): the raw min measured -16,494 (the tip poisoned);
   fixed - the subcarrier is blanked during the sync. (b) The D' polarity
   was inverted vs the decoder's verified demod convention
   (D'B = +1.505*(B-Y) at 230 kHz/unit, D'R = -1.902*(R-Y) at 280 kHz/unit):
   the bars decoded with the swapped axes; fixed (D'B = +3.052*u,
   D'R = -2.168*v). The decode now: 248/0 accepted, the correct vectors
   (yellow 166.4, blue 350.4), fob/for calibrated 4.250/4.406 MHz.
4. NTSC. hacktv's active_left 9.20us put the picture (and the castellation)
   inside the decoder's back-porch window (burst_end..active_start =
   7.8..9.45us): the porch sampled the cast checker (dc +16,132) and the
   VBI detection died (5 candidates in 250 fields). FIX: active_left 9.45
   (the decoder's profile value). The decode: 247/1 (the 1 = the stream-
   start bootstrap), the vectors correct.
5. Edge rise. The EBU reference's blanking->white 10-90 measures 0.19us =
   a raised-cosine parameter of 0.32 (hacktv's 0.20/0.25 spec values are
   ~1.6x faster than the real chain); sync_rise_us = 0.32 all systems.
   (A ringing/emulation attempt was REVERTED: the real edges do not ring,
   and a one-pole coupling emulation shifted the baseline by the content's
   mean, which would have broken the calibrated waveform readout; the sag
   transient variant broke the VBI parity gates - both measured and
   dropped.)

Headless verification after the fixes (all at 40 MSPS, --panel --full):
PAL 248/0 accepted, NTSC 247/1, SECAM 248/0; the bar vectors exact; the
bar amps within ~5% of the real reference; the PPM row/column profile
matches the real's (the cast 47-54 vs 49-56, the white bar 223 vs 212, the
blacks 0/10 both). meson test 44/45 (the one failure = the pre-existing
update_cli version-compare semantic conflict, unrelated).

## Round 3: the castellated sides (user: "sides a bit broken")

The user's screenshot (the Video panel) shows the round-2 checker cast
decoding as a dense black/white vertical striping. Per-line PPM profiles of
the reference decode: the reference card's cast strips are SOLID bar-white
(constant 212 down every row, no checker - measured at the fine row steps);
the checker was my invention.

Also measured on the reference, per line: at each field's VBI the WHOLE
baseline rides up (the porch -1,434 -> +3,598) and decays exponentially
(tau ~10 lines) back to -1,430, the sync step staying intact through the
sag. This baseline bump is what the decoder's white commit (blank + 90% of
the filtered luma max) rides: it is why the reference's committed display
white lands above the bar-white plateau (no clamping). Two earlier attempts
were measured and dropped: (a) an edge-ringing emulation (the real edges do
NOT ring - the blanking->white edge measures a clean 0.19us 10-90 rise, no
overshoot), and (b) a discontinuous per-frame sag (it shifted which EQ
pulses cross the slicing threshold and broke the VBI parity gates, 247
"repeat parity" rejects).

Round-3 changes:
1. The cast strips = SOLID white at the bar-white level (the reference's
   measured structure) for PAL/SECAM.
2. The AC-coupling baseline bump = restored as a CONTINUOUS smooth bump per
   field boundary (rise tau 2.5 lines through the VBI, decay tau 10.2
   lines, peak ~0.155 composite - the reference's measured shape). Smooth
   across frame seams; no DC bias; the -0.30/+0.70 waveform readout is
   preserved.
3. NTSC: the bump at ANY amplitude poisons the NTSC back-porch window
   (burst_end 7.8..active_start 9.45us sits differently than PAL's) and
   kills the VBI gates (measured: accepted drops to 0-27 with cad/par
   rejects at every bump variant). NTSC keeps the user-verified per-line
   checker at 0.854 (its headroom for the white commit) and takes no bump.
   Follow-up: audit the NTSC porch window/interactions with a real NTSC
   reference capture before touching it again.

Headless verification: PAL 248/0, NTSC 247/1, SECAM 248/0 (the levels'
commit: PAL white 22,760 / SECAM 22,878); GUI rebuilt; user verdict:
REJECTED ("hard white in the black, the white is too big" - the solid
white strips = the invented element, too prominent; the bump's lift
visibly brightened the top of the picture).

## Round 4: the cast strips REMOVED (the reference = plain full-height
## bars) + the bump reshaped to peak at the picture's start

Reference PPM re-measurement (definitive): the reference card is PLAIN
FULL-HEIGHT 8 BARS, edge to edge of the active area - no cast strips, no
checker (the hacktv av_test pattern; the "cast-like" values I profiled
earlier were the porch/burst-leakage column, not card content). Both cast
variants (round-2 checker, round-3 solid) were my inventions - removed
entirely.

The coupling bump RESHAPED to the reference's measured behaviour: the bump
RISES across the VBI (tau ~7 lines) and PEAKS at the first picture lines
(+5,024 s16 on the white bar there - the reference's per-line dump: the
first picture line's white = 27,776 vs the steady 22,752 plateau), then
decays (tau ~9.4 lines). Previous shape peaked mid-VBI and had decayed
away by the picture - useless for the white commit.

NTSC now works WITH the bump (248/0): the earlier NTSC bump-failures were
confounded with the cast-strip experiments (the cast's edges landed inside
the NTSC porch window and poisoned the level commit); with plain bars the
bump is clean on all three systems.

Headless verification: PAL 248/0, NTSC 248/0, SECAM 248/0; the commits:
PAL black 348/white 26,189; NTSC black 267/white 24,138; SECAM black
430/white 23,535 (the reference: black -1,364/white 26,629); the row-200
column profile matches within ~7% everywhere (the sim 228/176/182/0/10/55
vs the reference 212/163/169/0/10/55). meson test 44/45 (the one failure =
the pre-existing update_cli version-compare semantic conflict).
GUI rebuilt; awaiting user verification.

## Files touched

## Files touched

- misrc_tools/misrc_gui/signal/gui_testgen.c (rewritten generator)

## Restore point

Pre-change state: git HEAD (main) before this edit - gui_testgen.c was the
only file modified in this session.
