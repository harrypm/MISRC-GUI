# NTSC-J ED Beta preview

Choose **Decoder: RF Preview**, **Tape: Betamax**, then **Preset: ED Beta**.
This selects a separate ED video modulation profile. **Audio Mon** controls
its Beta Hi-Fi listening independently, using the device-driven callback.
Capture RF is preserved. Active preview is woven 640x480 at nominal 29.97 fps;
Full preview is 640x526 with the existing NTSC pixel geometry and display ratio.

## Signal path

ED uses a 20 MSPS video front end before mixing/filtering to the existing
5 MSPS picture/sync rings. Sources from 20 to 200 MSPS are accepted. Native
20 MSPS input bypasses rate conversion. Higher rates use a dedicated soxr
converter with a 9.5 MHz passband and 10 MHz stopband; this preserves the
9.3 MHz white carrier better than the legacy 40-to-20 MSPS halfband.
The converter uses 16-bit quality and a maximum 4096-sample DFT block.

The luma I/Q sections run at 20/20/5/5 MSPS: two elliptic sections at
20 MSPS (1.75 MHz pass edge, 0.2 dB ripple, 40 dB stop attenuation), followed
by two Butterworth sections at 5 MSPS (2 MHz cutoff). This suppresses the
negative-frequency RF image before FM discrimination while keeping the
retained experimental preview response. The chroma sections run at
20/20/5/5 MSPS. Coefficients and delay compensation use each section's
actual rate. SSE2 shares the luma and chroma filter lanes; scalar builds
perform the equivalent filters. This reduced-rate preview loses upper FM
sidebands and is a deliberate quality/performance compromise.

Beta Hi-Fi retains its separate legacy 20 MSPS converter and response.
Both converters consume the same source blocks, maintain independent output
positions and reset at source discontinuities, even though their output
rates match. With Audio Mon off, ED skips the audio-only converter.
Re-enabling audio clears its buffered tail and anchors its clock to the
current source block; video continues without reset. Switching between ED
and another preset reconfigures the converter response at unchanged rates.

Frame callback timestamps remain on the shared 20 MSPS media clock.
Ordinary VHS, Betamax and SuperBeta keep their existing resampler paths.
Capture RF is preserved at its original source rate; the 20 MSPS limit
applies only to the preview video path. Upsampling cannot recover RF
sidebands missing from a source capture.

## Parameters and limits

Sony's EDV-9500 circuit-theory manual specifies 6.8 MHz sync tip and 9.3 MHz
100-percent white. With this preview's -40..100 IRE mapping, the deviation is
17857.142857 Hz/IRE and blanking is 7.514285714 MHz. NTSC-J uses zero setup.
The preview mixes at 8.05 MHz. Color-under is 350*fH/8, or 688374.125874 Hz.
Existing Beta line/head color recovery and burst ACC are used. Validated
H-sync tip/porch measurements calibrate display levels after field lock.

The ED luma discriminator folds each quadrant to a ratio in [0,1] with
one division. An endpoint-constrained odd degree-17 minimax atan polynomial
replaces the rotation and long Taylor dependency chain. Its measured
maximum error is below 7e-9 radians (0.006 Hz at 5 MSPS), retaining the
existing 2e-8-radian test limit. Matching atan(1)=pi/4 keeps the octant
boundary continuous. Zero vectors and nonfinite inputs retain the prior
atan2f fallback and signed-zero behavior. Physical FM endpoints and the
working picture rate are unchanged.

The causal RF high-pass (4.2 MHz, four poles), RF low-pass (9.5 MHz), the luma
I/Q filters above and color I/Q low-pass (350 kHz) are preview choices.
The 5 MSPS working picture does not retain ED Beta's full analog resolution.
This initial ED preview uses flat luminance de-emphasis. Sony's separate
third-order LCR and diode expansion, phase correction, offline TBC, comb and
dropout repair are not reproduced. Accordingly this is a monitoring preset,
not a full-fidelity ED archival decoder.

The ED deck shares the Beta Hi-Fi four-carrier system. The existing audio
carrier/cadence detector must acquire the actual RF before audio becomes
nonzero; selecting ED does not claim a recording contains Hi-Fi audio.

## Primary references

[Sony EDV-9500 circuit-theory Operation Manual](https://archive.org/details/Sony-EDV-9500-Operation-Manual):
sections 1-2-5/1-2-6, pages 1-16..1-20 (ED emphasis and FM endpoints),
1-3-4, page 1-25 (RF bandwidth), 1-3-6, page 1-29 (ED de-emphasis),
1-4-1..1-4-5, pages 1-32..1-34 (color conversion), and chapter 4
(Beta Hi-Fi recording/playback).
