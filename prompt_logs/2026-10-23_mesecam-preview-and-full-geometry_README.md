# MESECAM recording option, compatible luma and Full display geometry

The user supplied vhs_sp_mesecam_colorbars_10cxadc3.flac, requested MESECAM
VHS preview and a correct Full display ratio, then reported that choosing
native SECAM for the MESECAM file produced no luminance. The final request
keeps only PAL / NTSC / SECAM systems and places a default-Off MESECAM
recording-method switch under SECAM.

## Changes

- Keep native SECAM and MESECAM as separate internal worker profiles, with
  the SECAM-only VHS switch selecting their respective FM paths. Per-panel
  state remains independent; PAL, NTSC and all Betamax variants ignore it.
- Decode MESECAM using its inverted heterodyne carriers and full studio FM
  deviations. Identify components using VBI bottles or stable alternating
  quiet H references; preserve expiry, gap and invalid-colour safeguards.
- Recover the supplied file's widened first equalizer only when a preceding
  clear H, the other four equalizers and the broad train prove the missing
  slot. Share this luma sync recovery across PAL-family receivers. Preserve
  measured field phase, cadence rejection and monotonic published counters.
- Derive the Full display correction from actual delivered raster dimensions.
  PAL-family Full pixels use 64/52 display correction; the active picture
  remains 4:3. The corresponding NTSC/Betamax Full correction is also applied.
  The user manually confirmed the corrected Full display ratio.

## Validation

The complete supplied 10.047535-second file at 17.898 MSPS produced 501
accepted fields / 250 frames independently through PAL, native SECAM and
MESECAM, with zero field rejects or queue drops. Native SECAM retained
luminance with no colour lock; MESECAM locked colour. A source-paced worker
run measured 24.988 fps at a 1.5-core budget. The user's separate GUI snapshot
showed about 21-22 fps; worker throughput is not GUI presentation throughput.

The independent physical MESECAM fixture covers heterodyne FM, identification,
noise, levels, expiry, malformed sync and cross-profile luma/field identity.
The final Windows suite passed 43/44 tests: only the existing upstream
update_cli Windows directory-creation failure remains. All 46 static/post-build
guards passed. Static RTL-SDR support is retained in the Windows build.

Original Python vhs-decode 177b505e produced byte-identical Y for eight fields
when decoding this sample as native SECAM versus MESECAM; wrong-system colour
identification failed as expected. The .NET 2.14.0+ff03b9bd MESECAM converter,
lookup and output-rate inconsistency prevents its uncorrected C from being a
colour reference. No other repository was modified. Detailed evidence and
limitations are in misrc_tools/misrc_gui/dev/mesecam_vhs_preview_README.md and
.ci-artifacts/mesecam-vhs-preview. No hardware capture or sustained recording
qualification is claimed. All saved source and documentation files use CRLF.

## MESECAM interference and carrier-loss follow-up

The user supplied the undeviated-carrier / carrier-switch-off sample and
requested less amplification of its three-second colour streaks, prompt
colour muting after switch-off, regression of both real files, a complete
branch review against current main, and a non-codex branch / upstream PR.

The refined path isolates buffered MESECAM IQ without a causal high-pass's
phase distortion. Local carrier validity is checked before LF recovery,
and retained component identity alone cannot authorize noise pixels.
Fresh references preserve healthy regions in partly damaged lines.
SSE2 chunk packing and bounded ME-only phase/norm shortcuts retain a
scalar path and the original legacy-profile calculations.

The complete real inputs retain identical frame-end timestamps and zero
field rejects. The three-second sampled false-colour fraction falls from
8.80% to 1.65%, with some thin streaks remaining. From the first complete
post-switch frame, every pixel in both fields is exactly gray. Both full
source-paced worker runs deliver all fields at about 25 fps with no input
loss. Independent physical tests protect low saturation, weak/fading and
strongly modulated real colour, local losses, noisy switch-off, reference
variants, recovery and input-chunk invariance. Hardware/GUI qualification
remains outside this evidence. Requested PR imagery is a user attachment,
kept outside the git tree.
