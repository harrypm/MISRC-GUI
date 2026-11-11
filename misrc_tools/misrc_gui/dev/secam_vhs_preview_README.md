# Native SECAM VHS RF preview

Select **Decoder: RF Preview**, **Tape: VHS**, **System: SECAM**, and leave
**MESECAM: Off** (the default) for native recordings. This profile decodes
native frequency-divided SECAM VHS SP, using 625/50 timing
and 768x576 woven RGBA output (768x626 in Full mode). It does not select
the separate SECAM CVBS decoder. For heterodyne recordings, turn
**MESECAM: On** and use the [MESECAM recording profile](mesecam_vhs_preview_README.md).
LP/EP recordings are outside this profile's qualification.

## Signal path

- The luma path retains the PAL VHS SP modulation, with a steeper 1.6 MHz
  high-pass to reject the higher native SECAM chroma band. Validated
  multi-line H tips/porches calibrate black and gain once after acquisition;
  picture extrema never normalize individual rows. Clock acquisition and
  field cadence use the robust line grid rather than the last noisy H pulse.
- Native chroma records the SECAM carrier AND deviation divided by four:
  Db 1,062,500 Hz / 57,500 Hz per D'B and Dr 1,101,562.5 Hz / 70,000 Hz per
  D'R. A stateful IQ band filter feeds a separate FM discriminator. The
  direct frequency path needs no PAL burst, tape phase rotation or cloche.
- Legal frequency rails precede the causal LF inverse
  `(1+s/(2*pi*255000))/(1+s/(2*pi*85000))`. Its state carries across blocks,
  lines and fields. Frequency division reduces deviation, not video time.
  D'B and D'R retain their +1.505 / -1.902 color-difference polarities.
- Missing color components use measured neighboring lines in the same
  field. Luma rows and both original field parities remain distinct.
  The finite-bandwidth Y/C registration trim is 600 ns, measured with
  independent physical RF edges; it is not a tape-standard parameter.
- A 250 kHz fourth-order decoded-C low-pass follows LF recovery; its causal
  group delay is included in chroma registration. Like components two lines
  apart receive a range-weighted spatial filter, and the missing component
  is centered between the preceding and following lines. Neither stage
  mixes fields or crosses the VBI/picture boundary. Invalid carrier rows
  remain grayscale. The robust tape-clock estimate scales FM frequencies;
  raw H-edge jitter still supplies geometry, not colour DC correction.
- Native SECAM render anchors use the measured H trailing edge minus the
  clock-scaled 4.7 us pulse width. This avoids alternate false falling edges
  seen on the fixture without smoothing measured tape timing across lines.
  Pulse classification, original start times and field cadence are retained.
- Identity comes from VBI field-identification bottles. Three late
  frequency windows must reach the known Dr/Db endpoint, with the correct
  opposite ramp, sufficient carrier amplitude, at least four consecutive
  alternating lines and two observations of each component. Four consistent
  fields confirm color. The bounded flywheel survives brief missing
  references; gaps, missing H sync and resets discard identity.

Active picture frequency is never an identification reference: legal solid
magenta can reverse the two medians, and solid red/blue can place both on
the same side. Native H-porch references are unreliable on the tested tape.
Recordings without usable VBI bottles currently remain grayscale rather
than guessing component identity; H-identification-only support is not
established. Missing/weak FM chroma also falls back to grayscale.

The existing bounded worker, resampler, capture-pressure gating and shared
CPU budget remain in charge. No recording or capture callback is changed.
The tested 28.636 MSPS source uses the existing libsoxr path to 20 MSPS.

## References

- [ITU-R BT.470-6](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.470-6-199811-S!!PDF-E.pdf),
  Table 2: SECAM carrier/deviation, pre-correction, blanking and identification.
- [JVC Video Technical Guide VTG82063, section 3.1.4](https://decode-orc.github.io/analogue-video-specifications/vhs/JVC-Video-Technical-Guide-VTG82063/section-3/#314-secam-vhs-system):
  native SECAM VHS division by four, distinct from MESECAM conversion.
- Existing `gui_cvbs.c` SECAM implementation and tbc-tools provide the
  verified color-difference conventions; its baseband path is unchanged.

## Verification and limits

The independent `gui_rf_secam_test.c` modulates physical luma and native
FM chroma, including causal record pre-correction, legal limiter, VBI
identification and alternating fields. It tests final RGBA bar hue,
red/blue ordering, physical Y/C edges, 85 kHz constant-luma chroma,
gain/clock offsets, weak/missing chroma, discontinuity reacquisition,
solid-color identity and Full-frame geometry. The original PAL/NTSC/Beta
QAM fixture remains separate; its geometry coverage is retained.
Additional physical FM cases cover a constant-luma vertical colour ramp,
large colour boundary, distinct field markers, VBI exclusion, a missing
carrier row and +/-0.025 line colour noise. Recovered colour noise RMS is
0.00413 R-Y / 0.00199 B-Y while Y changes by only 0.00033 RMS. The 85 kHz
colour test retains its original amplitude and physical Y/C edge checks.

Real-file fixture: `les coulisses de notre damme-singing_10s-8fsc.flac`,
SHA-256 `55dd5e16b330c28583f818766b5e498110929f03765c0cbe48f3314c6f253899`.
It contains 286,326,180 mono s16 samples, with the low byte zero (8-bit
quantization). The application's existing 28,636 Hz metadata multiplier
gives 28,636,000 physical samples/second and 9.998819 seconds; this is not
an independent ADC clock calibration. Measured H timing supports 625/50.

The production worker probe processed all 4,369 blocks with 499 accepted
fields, 249 frames, zero field rejects, zero queue drops and zero color-loss
events. A maximum-speed run took about 7.79 seconds for the 10-second source
on the development machine. A source-paced run at a 1.5-core budget
reported 24.989 fps with the same 499 fields and no drops or color losses.
This is a bounded local-file measurement, not
hardware-capture or sustained-recording qualification.

The quality update's source-paced run retained 499 fields / 249 frames,
24.987 fps, zero rejects/drops/colour losses at a 1.5-core budget. Its measured
worker CPU was about 8.52 seconds for 9.999 source seconds. Actual production
before/after frames near 3 seconds show substantially lower flat-background
colour row fluctuations; this is a local image diagnostic, not a clean-source
SNR measurement. RF luma noise and some chroma streaks remain.

For the full-decode comparison, .NET 2.14.0+ff03b9bd rejects native `SECAM`;
it can supply an Exact PAL luma-only reference, not native SECAM colour.
Original Python vhs-decode 177b505e does execute native SECAM. Its 30-frame
RF/TBC window rendered with the installed local SECAM tbc backend also shows
colour streaks, with steadier luma/geometry. That backend reports an
untraceable 0.0.0 build with diagnostic probes, so this does not establish an
upstream colour-quality ceiling or prove the remaining defects are on tape.
Exact commands, raw TBC/JSON and provenance are retained under the local
`.ci-artifacts/secam-vhs-quality/{dotnet,python,audit}` directories.

Display chain: decoder bytewise RGBA -> worker frame copy ->
`gui_vhs_fm_swap_buffers` RGBA -> `LoadTextureFromImage` / `UpdateTexture`
with `PIXELFORMAT_UNCOMPRESSED_R8G8B8A8` -> `DrawTexturePro`. The worker
probe reads that final GUI-consumed RGBA; the actual GUI was also checked
with local playback. There is no new RGB packing conversion. The same
final RGBA is covered by distinct-channel and field-preservation assertions.

RF noise, dropout repair, advanced comb filtering and offline TBC quality
are outside this preview. No physical SECAM capture device was exercised.
