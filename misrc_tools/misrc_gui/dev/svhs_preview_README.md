# PAL/NTSC S-VHS RF preview

Choose **Decoder: RF Preview**, **Format: VHS**, then **Preset: S-VHS**.
The VHS preset defaults to **VHS** and is remembered independently of the
Betamax presets. Select **System: PAL** or **System: NTSC** manually; the
preview does not detect the recording system or tape speed automatically.
Selecting **System: SECAM** or **System: MESECAM** restores **Preset: VHS**.
Selecting S-VHS from either system switches to PAL.

S-VHS provides video preview only. VHS/S-VHS Hi-Fi audio decoding and linear
tape audio monitoring are outside this preset. Selecting it does not establish
whether the captured recording contains audio.

## Signal path and rates

Sources from 20 to 200 MSPS feed a 20 MSPS video front end. Native 20 MSPS
input bypasses conversion. Higher rates, including 40 and 80 MSPS, use the
same wide-passband libsoxr converter as ED Beta: 9.5 MHz passband, 10 MHz
stopband, 16-bit quality, and a maximum 4096-sample DFT block. This converter
retains the S-VHS upper FM band more evenly than the ordinary VHS halfband
path. Higher-rate input requires libsoxr; rates below 20 MSPS are explicitly
rejected for this preset.

After RF filtering and mixing, S-VHS uses the existing VHS 5 MSPS picture,
sync and color-under processing. Luma I/Q uses two Butterworth sections with
a 1.75 MHz cutoff before reduction. This is a reduced-bandwidth preview and
does not retain S-VHS's full analog resolution or all FM sidebands. The worker
skips the unused narrow-band Beta audio converter. Switching between VHS and
S-VHS reconfigures the converter response even when the input rate is unchanged.
Discontinuities clear resampler and decoder history before reacquisition.

Before color-under mixing, a causal 20 kHz DC tracker removes the residual
ADC offset from the S-VHS chroma input. Otherwise the mixer translates DC to
the color-under frequency, where a low-pass filter alone cannot reject it
sufficiently and burst AGC amplifies it into false color.
The tracker retains state across input blocks and resets with the decoder
on a gap or profile reset. It affects only S-VHS color processing, uses
constant state, and adds no field buffering. Luma and capture samples retain
their existing paths.

For PAL S-VHS, sufficiently consistent valid bursts establish the actual
field's alternating PAL swing sequence. An isolated contradictory burst
cannot reverse V for the entire picture line. Such a burst is excluded from
phase-reference and phase-step estimation; the line uses the nearest trusted
reference in its existing segment as the 45-degree agreement anchor. If no
coherent trusted estimate is available, color on that line is suppressed.
The sequence comes from measured bursts, not assumed field/head parity; weak
or incoherent whole fields retain the existing gray fallback.

For S-VHS, a local 33-line burst reference stabilizes chroma phase and gain
after removing the recording-track rotation and PAL swinging burst. Weak or
amplitude-outlier references do not contribute. Amplitude has its own robust
reference: rejecting a neighbor's phase cannot discard its healthy amplitude
and fall back to excessive single-line AGC. A 45-degree phase gate rejects
incoherent neighbors. Two coherent eight-line populations can additionally
identify a sustained phase step of at least 24 degrees, above four times the
estimated mean uncertainty, and split the phase window there. Smaller or
unsupported changes retain bounded local smoothing. The original
adjacent-line PAL cancellation remains in place. Only neighboring pairs from
the same field whose Y and U/V agree contribute to further color smoothing,
so equal-luminance color boundaries also stop the average. The original Y
rows and the two original fields remain separate. The decoder owns an
additional 2.81 MiB row workspace; no previous field or frame is reused.

The 5 MSPS S-VHS FM discriminator reuses the existing ED full-quadrant bounded
phase approximation. No RF-rate, luma filter, level calibration or display
headroom is changed by this acceleration. Ordinary VHS/Betamax paths retain
their existing discriminator choices.

S-VHS additionally applies field-local adaptive chroma noise reduction after
the original pair/2H color recovery. A four-line-class template removes the
coherent component of the sync-reference C interval before estimating its
residual power through the same pair/2H weights. The reference is not assumed
to contain zero C, and the result is a robust noise estimate rather than a
measurement of pure ADC noise. The highest power decile is winsorized before
averaging. Small estimates, below an equivalent two-code blue-channel RMS
policy, bypass noise reduction.

Local U/V moments use a five-column horizontal box and a nine-pair vertical
triangle (a five-pair box convolved with itself). The vertical kernel has no
negative spectral lobes. A Wiener estimate suppresses noise in flat color
regions while local chroma variance and a luma-variance guard protect edges.
Only current-field, valid active pixels contribute. Rolling moments and exact
S-VHS median selection limit CPU cost. The approximately 3.8 MiB additional
decoder workspace is overwritten per field; no previous image field/frame
contributes. Original per-pixel Y values, line positions and timestamps are
retained. Strong noise reduction can still suppress real color detail below
the estimated noise floor; it is not lossless color restoration.

Frame callback positions retain the shared 20 MSPS media clock. Active output
weaves the original fields into 768x576 PAL or 640x480 NTSC at nominal 25 or
29.97 frames per second. Full output uses the existing 768x626 or 640x526
raster, with its display pixel aspect corrected by the UI.

Capture RF remains at its original source rate. The conversion to 20 MSPS
applies to preview processing; it does not resample the recorded RF. Upsampling
cannot restore signal bandwidth missing from a capture.

## Profile parameters

The nominal FM endpoints and color-under values follow
[vhs-decode's VHS/S-VHS definitions at commit 177b505e3a433c454794aeb53f469599ddc8a168](https://github.com/oyvindln/vhs-decode/blob/177b505e3a433c454794aeb53f469599ddc8a168/vhsdecode/format_defs/vhs.py).
Both systems record 5.4 MHz sync tip and 7.0 MHz reference white. Their IRE
calibration differs because PAL's sync-to-active-voltage ratio differs from
the NTSC -40..100 IRE mapping.

| Parameter | PAL S-VHS | NTSC S-VHS |
| --- | --- | --- |
| Sync tip | 5.4 MHz | 5.4 MHz |
| Blanking / 0 IRE | 5.88 MHz | 5.857142857 MHz |
| Reference white / 100 IRE | 7.0 MHz | 7.0 MHz |
| FM deviation per IRE | 11200 Hz | 11428.571429 Hz |
| Color-under | 626953 Hz | 629370.629371 Hz |
| Line period | 64 us | 63.555555556 us |
| Preview mixing center | 6.2 MHz | 6.2 MHz |
| RF high-pass / low-pass cutoff | 2.0 / 8.98 MHz | 2.0 / 8.98 MHz |
| Color I/Q low-pass cutoff | 500 kHz | 500 kHz |

Color recovery retains ordinary VHS's line/head phase law and PAL/NTSC
conversion. Both S-VHS profiles use the matched 13.9794 dB, 273755.82 Hz
display-luma shelf with Q=0.462088186. PAL sync retains its existing
100/400 ns zero/pole path and NTSC sync retains its 120/320 ns path.
The shelf has unity DC gain: blanking, white calibration and display headroom
are unchanged. Its isolated group delay is not used as a Y/C timing offset.
The causal filter orders, mixing center and reduced I/Q bandwidths are preview
engineering choices rather than a complete implementation of the tape standard.

## Limits

The upstream S-VHS profile adds a linear sub-de-emphasis network with fitted
zero time constants of 1.305 us and 63.83 ns, pole time constants of 882.5 ns,
88.76 ns and 17.18 ns, plus a 4 dB shelf at 2 MHz. It also enables nonlinear
sub-de-emphasis and RF equalization. This preview omits these additional
S-VHS stages, full offline time-base correction, dropout repair and advanced
comb filtering. Fine detail, transitions, levels and noise can therefore differ
from a complete S-VHS decode. S-VHS ET and individual tape-speed variants have
no separate presets.

H/V/color lock counters describe decoder state. They do not establish clean
picture quality, correct tape identification, real-time throughput or sustained
capture reliability. Real media quality, native-window playback and hardware
recording require separate validation.

## Validation

Windows validation on 2026-10-27 passed all 45 Meson tests and 46 static /
post-build guards. The GUI includes `gui_rtlsdr.c`, `ENABLE_RTLSDR=1` and the
static `librtlsdr_static.a`; system-PATH version and smoke startup checks passed.

Independent physical RF fixtures cover PAL/NTSC S-VHS FM endpoints, color bars,
both recording-head orders, original-field weaving, grayscale without burst,
arbitrary block partitions, 20 MSPS timestamps, discontinuity reacquisition and
returning to ordinary VHS. Worker/route tests cover 16 MSPS rejection, native
20 MSPS, 40/80-to-20 MSPS conversion, converter changes at an unchanged source
rate, shared/independent panels, SECAM behavior and Beta preset isolation.

The supplied `S-VHS-Test-Tape_2022-04-04_22-00-14.lds` was read as packed
10-bit, 40 MSPS RF through the existing LDS playback rules. One-second excerpts
were converted only to the playback reader's signed sample container, retaining
40 MSPS; the production preview worker performed the conversion to 20 MSPS.
Offline probes submitted every RF block without queue rejection or loss:

| Source window | Published frames | Accepted fields | Rejected fields |
| --- | --- | --- | --- |
| 0-1 s | 14 | 29 | 3 |
| 4-5 s | 23 | 48 | 1 |
| 8-9 s | 24 | 49 | 0 |

Each window ended with H/V/color lock and a 768x576 PAL frame. During initial
validation before the color-quality revision, the same LDS file was also
loaded directly in the Windows GUI, where the new S-VHS preset
displayed 40-to-20 MHz processing and locked color video. The decoded sample
retains substantial luma/chroma noise, and its opening has poorer field
acceptance; these checks establish playable color preview, not restored or
full-resolution S-VHS quality. NTSC real-media playback, physical-device
capture and sustained operation remain unvalidated.

On 2026-10-27, the same unmodified 4-5 s RF window exposed DC leakage into
chroma. Rejecting DC in the production preview visibly reduced its large
colored grain while retaining 40-to-20 MSPS conversion, 23 published frames,
48 accepted fields, one cadence rejection and zero rejected/dropped input
blocks. PAL/NTSC physical-RF regressions now exercise positive/negative ADC
offsets and an offset step inside an active field, arbitrary block partitions,
original-field weaving and clean reset after biased input. The remaining
color streaks and luma texture still differ from full offline decoding;
DC rejection alone does not implement its comb, dropout or S-VHS
sub-de-emphasis stages.

The subsequent color-quality revision uses the matched display-luma shelf,
500 kHz color I/Q bandwidth, independent robust burst amplitude/phase
references, phase-step window splits and edge-gated same-field color pairs.
Independent physical RF tests now include equal-luminance vertical and
horizontal color boundaries with different contents in the original fields,
fixed-seed burst-only gain/phase disturbances, 30/60-degree true channel
phase steps, sparse weak/phase-corrupted bursts, continuous burst loss and
reset without burst. Known U/V targets remain unchanged. The local phase
recovery gate is five degrees outside four source lines of a step; the sparse
burst fixture also rejects clipped color amplification. The previous coupled
estimator fails these targeted regressions.

The final production worker published 24 frames from the original 8-9 s
window after 40-to-20 MSPS conversion, accepting 49 fields and rejecting or
dropping no input blocks. Maximum-speed and real-time-paced submission
produced identical final-frame pixels. For seven flat bar interiors, the
RGB-derived within-parity UV row statistic changed from 0.06264 in the
DC-fixed baseline to 0.03087; a representative full-decode frame measured
0.01132. These are bounded display-scatter observations, not RF SNR or
time-aligned PSNR/SSIM. No denoise, scaling or grading was applied after the
preview worker. Residual streaks, isolated bright/dark lines and lower fine-detail
bandwidth remain visible. These source-window checks do not establish
arbitrary-tape quality, sustained throughput or hardware capture reliability.

The v2 revision adds PAL burst-cadence protection and the bounded FM phase
helper. A new independent physical RF regression inverts only isolated PAL
bursts by 180 degrees at normal amplitude while active Y/U/V remains
unchanged. It covers both recording-head orders and both line parities,
checking the corrupted and paired neighboring rows against known color
targets. The preceding revision fails this case; the final revision passes
without relaxing any existing phase, edge, noise or field-weave thresholds.

For the same final 8-9 s production frame, the flat-bar UV row statistic
changes from 0.03087 in the preceding revision to 0.02050; the full-decode
reference remains 0.01132. The final still is written directly from the
worker output with lossless PNG encoding. Real-time-paced and maximum-speed
submission produce identical pixels, also matching the performance probe.

Performance comparison freezes both binaries and original RF clips, then
runs six adjacent AB/BA pairs per one-second source window, with a separate
warm-up and startup control. For the 4-5 s and 8-9 s windows respectively,
paired worker-thread CPU medians decrease by 23.40% and 22.70%, and worker
processing-wall medians decrease by 19.37% and 19.76%. Source samples,
accepted/rejected fields and published frames match, and all measured runs
have zero rejected/dropped input blocks. Every run is retained, including
host-load and scheduling observations. These two static source windows do
not establish sustained hardware-capture performance or a universal speedup.

The chroma-NR revision adds independent clean .008/.025 color-detail fixtures,
including weak equal-Y edges, 8/16-source-line color textures and small colored
glyphs. Clean cases retain the baseline output through the low-noise bypass.
A fixed-seed, independently band-limited I/Q noise signal is also modulated
onto color-under throughout sync, burst and active regions, without injecting
it into the FM oscillator. This explicitly exercises NR-on behavior. Flat
noise, color means, strong-edge width and signed texture response are checked
per original field. Same-seed positive/negative textures measure the response
without confusing a single noisy DFT with real detail transfer.

The final synthetic noise cases reduce flat color scatter by about 32-52%,
with strong color edges widening by about 2.8-5.1% and stable color means.
Texture below that noise level still loses substantial modulation (about
53-88% in the measured cases); clean-detail bypass tests are not evidence of
lossless detail retention while NR is active. Signed-response regressions
reject the earlier rectangular-window negative-lobe case. The selected
triangle passes all measured signed cases, without claiming that any
nonlinear denoiser preserves every possible below-noise texture.

For the actual final 8-9 s production frame, the flat-bar UV row statistic
changes from 0.02050 in v2 to 0.01753, compared with 0.01132 in the full-decode
reference. Source RF remains 40 MSPS and is downsampled to 20 MSPS by the
production worker. Maximum-speed and real-time-paced submission give the
same pixels and zero input block rejection/loss. Independent diagnostic
copies compare all accepted fields in both 4-5 s and 8-9 s windows: raw Y
float values, H-line positions/periods, geometry, timestamps and published
field mapping are bit-identical to v2; final source-field Y blocks are also
compared byte-for-byte. RGB-derived luminance is not this internal-Y oracle.

Six adjacent AB/BA pairs per source window compare frozen v2 and final NR
binaries. Paired worker CPU medians increase by 4.21% / 5.33%, and worker
processing-wall medians by 3.95% / 0.60%, for 4-5 s / 8-9 s respectively.
All runs, warm-up/startup controls and host/scheduling observations are kept;
frames, accepted/rejected fields and zero-loss input counts match. These
bounded file measurements do not establish a general hardware-capture or
sustained throughput guarantee. Earlier slower NR implementations and their
failed performance/texture observations remain in the local validation data.

The 2026-10-28 picture-C revision adds a backward 500 kHz fourth-order
Butterworth filter on the existing 5 MSPS I/Q picture buffer. Source RF is
still downsampled to 20 MSPS by the production worker. Each independent
1024-sample chunk has 128 future samples for settling; paired chunks use
four independent SIMD lanes, with scalar tails and a portable scalar path.
The original I/Q ring stays intact, and the picture buffer reuses scratch
storage belonging to the mutually exclusive MESECAM profile. Its actual
group delay is subtracted from picture-C registration. This improves image
rejection and changes both phase and magnitude; it is not the complete
decoder's asymmetric SuperGaussian response.

Burst, recording-track, PAL cadence and gain/phase reference measurements
retain the original causal I/Q response. Applying the backward picture
filter to those short references lets future active-picture color affect
the reference and can smear weak vertical color boundaries. The existing
independent weak-edge regression detects that failure; separating reference
measurement from picture filtering restores its original bound without
changing thresholds or adding spatial/temporal NR. Picture and quiet-noise
samples use the same new response. The 150/700 color-vector target, Y,
geometry, FM path and all existing NR parameters remain unchanged.

New physical-RF coverage reuses three simultaneous midtone Y/C transitions
under both recording-head orders, including clock/jitter cases, and checks
100/300 kHz equal-Y horizontal chroma against independently prescribed
U/V amplitudes and phases. Original fields remain separate. The existing
7/5-pixel PAL/NTSC peak-registration bounds are retained; gradient centroids
are reported for diagnosis. A newly proposed extra one-output-pixel
centroid bound was not sufficiently calibrated to the 5 MSPS working grid
and is not an acceptance contract. Its failed trial remains in local data.
At 300 kHz, measured amplitude loss versus the preceding revision is about
0.49-0.94%, while absolute phase error decreases by about 6.4 degrees.
The extra filter still attenuates near-cutoff detail: its additional
response at 500 kHz is -3 dB, and some real-frame color edges widen by
one or two display pixels.

For unchanged 4-5 s / 8-9 s RF windows at the same preview color scale,
seven fixed flat-bar interiors show pixel-UV scatter reductions of
7.54% / 14.19%, and within-parity row scatter reductions of 2.45% / 6.49%.
The latter confirms only a modest reduction of horizontal color streaks.
These are display-derived observations, not RF SNR or aligned PSNR.
The separate full-decode reference uses a different color-gain contract,
so its absolute UV scatter must not be treated as an equal-scale SNR oracle.
No saturation reduction or post-render edits were used for this revision.

Six adjacent AB/BA pairs per one-second source window compare frozen
preceding-NR and new binaries. Paired worker CPU medians increase by
2.38% / 1.39%; paired processing-wall medians change by -1.58% / -2.84%.
All runs, startup/warm-up controls and host-load observations are retained;
work counts and zero input rejection/loss match. The final maximum-speed
and paced 8-9 s replay gives identical pixels. Independent diagnostic
copies retain bit-identical Y/H-map fingerprints, source timestamps,
geometry and publication mapping in every accepted field of both windows;
the final four original-field Y blocks also match byte-for-byte.
PAL/NTSC full-field, both-head tests compare whole, mixed and 8192-sample
partitions, bounded prepared-C reads and gap/reacquisition against fresh
decoders. These bounded source/synthetic checks do not qualify sustained
hardware capture or guarantee equal quality on other tapes.
