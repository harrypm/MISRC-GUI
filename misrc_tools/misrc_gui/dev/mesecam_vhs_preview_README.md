# MESECAM VHS SP RF preview

Select **Video -> Decoder: RF Preview -> Tape: VHS -> System: SECAM**, then
set **MESECAM: On**. System cycles only PAL, NTSC and SECAM. The MESECAM
switch defaults to **Off**, selecting native SECAM; its remembered value
only affects SECAM VHS and cannot alter PAL/NTSC or Betamax. The two recording
methods remain separate internal profiles because they have different
carrier frequencies, FM deviations and component references. Betamax remains
NTSC-only. This profile covers MESECAM VHS SP, not LP/EP qualification.

## Signal and identification

MESECAM translates studio SECAM below a nominal 5,060,571.875 Hz converter.
Dr and Db rests land at 654,321.875 and 810,571.875 Hz. The spectrum reverses
but retains the full 280/230 kHz studio deviations; it is not the native
divide-by-four method. The preview demodulates the tape FM directly, with
the correct inversion, legal 304,321.875..1,160,571.875 Hz rails, causal LF
inverse and a 250 kHz decoded-C low-pass. A 600 kHz complex sideband filter
and measured registration trim are preview engineering choices.

The buffered 5 MSPS complex samples also pass through a forward/backward
200 kHz RF high-pass before discrimination. It rejects low-frequency ingress
without the nonlinear phase delay of an equally steep causal high-pass,
which would damage legitimate strongly modulated FM sidebands. Padded chunks
read only samples already received; this does not add a raw-input block-size
dependency. Native SECAM and the PAL/NTSC/Beta paths retain their own filters.
On SSE2 targets, two independent chunks occupy the four I/Q lanes; each
keeps its own padding, state, valid length and reverse-pass origin. Other
targets use the scalar path. The FM phase shortcut retains an atan2 fallback;
its worst error is below 0.35 Hz at the 5 MSPS working rate.
MESECAM luminance uses the same bounded shortcut; other profiles retain
their original demodulation. MESECAM's finite Y/C magnitudes use double
intermediates, with the original CRT fallback for non-finite inputs.

Carrier quality is measured locally before the LF inverse. Envelope loss
and impossible phase excursions mark unreliable samples; short bounded gaps
are interpolated before LF recovery, while missing carrier pixels remain
gray. The envelope threshold uses trusted H or bottle references together
with the current line's carrier level, preserving real amplitude fades and
low-saturation colour. Line identity memory is separate from current carrier
validity. Pixel masks preserve healthy regions of partially damaged lines.

Native SECAM retains its validated bottles-only identification. MESECAM can
identify from either valid VBI bottles or quiet H references at both known
rest carriers. H identification reads a separate pre-LF discriminator ring,
requires stable alternating references from both components, and needs four
agreeing fields. No active-picture median or expected colour-bar palette
establishes identity. Reference expiry, gaps and resets invalidate colour.
Noisy or carrier-free rows remain grayscale. Same-field, like-component
filtering and centered missing-component interpolation do not blend fields
or cross the VBI/picture boundary.

The supplied tape exposed a widened first equalizer: a physical 2.42 us
pulse can cross the preview sync chain's 3.5 us classification boundary.
The shared PAL-family sync recovery accepts it only with a measured preceding clear H,
the remaining four half-line equalizers and the broad train proving the
missing fifth slot. Its true half/integer phase still comes from that clear
H. The false H is removed from the render-anchor ring. Missing equalizers,
repeated parity and wrong field cadence remain rejected; there is no forced
field alternation or global pulse-width relaxation.
The recovery is independent of chroma selection: native SECAM and PAL
receivers must still publish the MESECAM input's compatible 625/50 luma.
MESECAM additionally requires a complete witnessed five-EQ train; its
missing-pulse negative contracts do not relax the older profiles' policy.
Published H-sync event counts remain monotonic when the false anchor is
removed, preserving the worker's cumulative-counter delta contract.

The recording-method distinction follows the [JVC technical guide, section
3.1.5](https://decode-orc.github.io/analogue-video-specifications/vhs/JVC-Video-Technical-Guide-VTG82063/section-3/#315-me-secam-vhs-system).

## Geometry

PAL timing is retained: Active 768x576 and Full 768x626, 25 frames/second
nominal, weaving the original two fields. Full's display pixel-aspect
correction is 64/52, so its active picture stays 4:3. The Full outer raster
is about 1.5100:1. Actual texture dimensions select the UI correction, which
also fixes PAL/native SECAM and the corresponding NTSC/Beta Full geometry.

## Verification and limits

The independent physical fixture creates studio RGB/LF pre-correction and
FM, then a separate converter oscillator produces the difference signal.
It tests bars, field identity, head phases, gain/clock/jitter/noise, 85 kHz
chroma, gray/black/white, missing/weak chroma, bottles-only and H-only ID,
three cold solid colours without references, three identity-expiry cases,
Full geometry, gaps and malformed sync. Both field phases exercise measured
first-EQ widening; genuinely missing EQ, repeated parity and incorrect
cadence remain negative cases. The AM bell envelope in the fixture tests
envelope immunity, not restoration of a complete complex studio HF network.
Advanced HF phase equalization, offline comb/repair and a conversion-LO
servo are outside this reduced-bandwidth preview.

Real-file input: `vhs_sp_mesecam_colorbars_10cxadc3.flac`, SHA-256
`1f9e8fe06404fe7ed175697b22a9f109a5b526083f22decfa5f076a209dbf544`.
It contains 179,830,784 mono signed16 samples. The 17,898 Hz header follows
the application's existing kHz convention: 17,898,000 physical samples/s,
10.047535 seconds. Independent H/field/carrier measurements verify the rate
scale, not a precision ADC crystal calibration. The production worker uses
libsoxr to normalize that rate to 20 MSPS.

At the initial implementation, the source-paced fixture produced 501 accepted fields / 250 frames
at 24.988 fps with a 1.5-core budget, zero rejects, queue drops or colour-loss
events. This is independent worker throughput; the user's actual GUI
screenshot reported about 21-22 fps despite lock and zero drops. Decode and
presentation speed are separate observations. The user also confirmed the
corrected Full display ratio. No hardware capture or sustained-recording
qualification is claimed.

Original Python vhs-decode 177b505e MESECAM output restores the correct
studio carrier pair and supplies a six-frame full-decode reference. Its PNG
rendering uses the installed local SECAM backend whose exact source revision
is not traceable. Our .NET 2.14.0+ff03b9bd currently has inconsistent MESECAM
converter/table/metadata rates and outputs shifted chroma carriers. Its
uncorrected colour is therefore not the ground truth for this implementation.
Raw TBC, source/clock evidence, commands and the explicitly calibrated .NET
analysis image remain under `.ci-artifacts/mesecam-vhs-preview/analysis`.
No other repository's source was changed.

As a cross-system check, the original Python decoder also ran this MESECAM
file with the deliberately incorrect native `SECAM` choice and chroma left
enabled. Its first eight output Y fields were byte-identical to the correct
MESECAM decode, while colour identification failed. This establishes the
sample's compatible luma path; it is not proof of valid wrong-system colour.

The final production worker also consumed this entire sample separately as
PAL, native SECAM and MESECAM: each published 501 accepted fields / 250
frames with zero field rejects or queue drops. Native SECAM remained
colour-unlocked and displayed Y; MESECAM locked colour. The independent
cross-profile fixture checks actual physical field identity and Y levels
for both first-EQ phases, rather than only counting callbacks.

The final Windows regression suite passed 43/44 tests, including native
SECAM, MESECAM, existing RF preview, routes and geometry. The only failure
is the separately reproduced upstream Windows update_cli directory-creation
failure. All 46 static/post-build guards passed. These tests do not establish
hardware capture, long-duration playback or GUI presentation throughput.

## Carrier interference and switch-off regression

The second supplied input is
`vhs_sp_mesecam_undeviated-chroma-carriers_chroma-carriers-switchoff_10cxadc3.flac`,
SHA-256 `901c8e0ae0b4ecdcd10f7496adccf3f2a1f4361aa87c9f8da63a7eb3c178f2ce`.
Its 536,870,912 signed16 samples span 29.996140 seconds at 17.898 MSPS.
Both inputs were decoded continuously from sample zero with the production
resampler and decoder. The switching file produced 1,499 fields / 749 frames;
the colour bars produced 501 fields / 250 frames. Neither rejected a field,
and every published frame end matched the pre-refinement 95f7972 baseline.

At 2.8-3.2 seconds, the unchanged image diagnostic samples every second row
and column inside the active raster. Its U/V RMS fell from 12.37/11.37 to
4.89/1.78 in 8-bit units; the fraction with RGB spread above 20 fell from
8.80% to 1.65%. These are sampled output-image diagnostics, not RF SNR or
proof of perfect restoration. Some thin coloured streaks remain. A matched
pre/post-discriminator trace located the error before the limiter: local
phase corruption already had a biased frequency estimate. The limiter
reduced that bias but could not remove it; LF recovery retained it.

The source carriers disappear at approximately 12.38-12.39 seconds. The
refined output is exactly gray in every pixel of BOTH fields from the first
complete post-switch frame ending 12.44124665 seconds until the end of the
file. The preceding mixed weave may retain its healthy older field. This
check scans the entire RGBA raster, including edges; C-lock OFF alone is
insufficient evidence. In particular, a retained line-ID flywheel may only
authorize pixels when fresh identification or adequate current usable
carrier coverage still exists. Fresh references can preserve healthy parts
of a damaged line even when whole-field colour coverage is insufficient.

The supplied colour bars remained locked throughout their complete decode.
The eight central bar RGB medians changed by at most 5/255 from 95f7972
(mean absolute change 0.875/255); white neutrality and all six hue-dominance
checks passed. Independent physical RF contracts additionally cover strong
150 kHz FM near the studio limiter, 8% saturation, weak modulated carriers,
active-only 6 dB fades, low-frequency RF ingress, phase/AM/click damage,
whole/partial-line and whole/partial-field losses, noisy sudden switch-off,
bottle-only identification without quiet H, restoration and physical Y.
Fixed versus ragged input chunks produce identical frames and status in
both Active and Full modes, including the scalar IQ path.

The final expanded test took 51.79 seconds in the local x64 Meson suite;
its budget is 180 seconds for slower and non-SSE CI runners. This is
timeout headroom, not a relaxed image oracle or an ARM throughput result.
The additional IQ rings use 4 MiB per decoder, with a 240 KiB pixel mask.
The local GCC static stack audit estimates about 62 KiB for the decoder
call chain; this is not a runtime stack high-water measurement.
Detailed raw inputs, failed prototypes and final independent evidence stay
outside git under `.ci-artifacts/mesecam-optimization-20261023`.

The final source-paced production worker also consumed both COMPLETE files,
sequentially after the known heavy tests had stopped, at a 1.5-core budget.
Colour bars delivered all 501 fields / 250 frames at a reported 24.994 fps;
the switching file delivered all 1,499 fields / 749 frames at 24.983 fps.
Both had zero rejected/dropped input blocks, zero field rejects and only
the initial worker reset. These are headless worker results, not GUI, FFT,
hardware capture or sustained-recording measurements. Intermediate versions
that passed image tests but dropped realtime input were rejected.
