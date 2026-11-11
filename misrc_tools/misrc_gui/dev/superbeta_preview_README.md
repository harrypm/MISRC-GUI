# Ordinary NTSC SuperBeta preview

Select **Decoder: RF Preview**, then **Tape: Betamax** and **Preset: SuperBeta**.
The preset cycles through Betamax, Betamax Hi-Fi, SuperBeta and ED Beta; a new panel
defaults to Betamax. **Audio Mon** independently enables or mutes Beta Hi-Fi
listening, without changing the SuperBeta video profile or relocking video.
Selecting SuperBeta does not turn Audio Mon on. Recording mode is not detected
automatically.

The GUI uses the Hi-Fi-capable SuperBeta worker. The command-line profiles
`ntsc-superbeta` (video only) and `ntsc-superbeta-hifi` remain available with
identical video parameters. Both use the
existing normalized 20 MSPS RF input and 5 MSPS video working rate. Input
resampling, queue limits, CPU budgets and capture RF recording retain the
existing preview behavior. Video remains a reduced-bandwidth woven 640x480
preview at nominal 29.97 fps.

## Nominal carrier parameters and source evidence

This feature targets ordinary NTSC SuperBeta. Two original manufacturer
publications agree on its 4.4 MHz sync-tip and 5.6 MHz peak-white endpoints:

- [Sencore Tech Tip 135, page 2, Figure 3](https://docs.ampnuts.ru/eevblog.docs/Sencore/Sencore_Tech_Tips/TT135p%20-%20How%20To%20Set%20VCR%20FM%20Modulator%20Frequencies.pdf)
  labels the Super Beta sync-tip and peak-white values as 4.4 and 5.6 MHz.
- [Sony EDV-9500 Operating Instructions, page 73](https://schematicsforfree.com/files/Video/Owners%20Manuals/Edv9500%20Owners%20Manual%20Owners%20Manual.pdf#page=73)
  gives the same 4.4-5.6 MHz Super Beta range and 1.2 MHz deviation in its
  frequency-allocation diagram. The diagram also shows 688 kHz chrominance.
  Sony separately describes 6.0 MHz SHB (Super Hi-Band) for Beta I S and
  6.8-9.3 MHz ED Beta on this page; these are outside this preview's scope.

The preview maps sync tip to -40 IRE and peak white to 100 IRE. Therefore:

| Parameter | Preview value | Basis |
| --- | --- | --- |
| Sync-tip FM | 4,400,000 Hz | Manufacturer endpoint |
| Peak-white FM | 5,600,000 Hz | Manufacturer endpoint |
| Hz per IRE | 1,200,000 / 140 = 8,571.428571 Hz | Derived linear mapping |
| Blanking, 0 IRE | 4,400,000 + 40 x Hz/IRE = 4,742,857.142857 Hz | Derived linear mapping |
| FM mixing center | 5,000,000 Hz | Preview DSP choice |
| Color-under | 688,374.125874126 Hz | Shared .NET Beta software reference; Sony diagram rounds to 688 kHz |
| Track/line chroma rotation | [2, 0] quarter turns | Shared .NET Beta software reference |

The local `vhs-decode-dotnet` reference in
`src/VHSDecode.Core/Formats/format-params.snapshot.json` has a different
NTSC `SUPERBETA` calibration: `ire0 = 5,150,000`, `hz_ire = 10,000`, and
`vsync_ire = -40`. Those values imply 4.75 MHz sync tip and 6.15 MHz peak white,
which do not match the ordinary SuperBeta endpoints above. The recording
variant or empirical calibration represented by that software entry has not
been established from a manufacturer source. It is not identified here as
SHB, and this feature does not change that .NET reference.

These nominal endpoints initialize the RF/sync pipeline; they do not establish
the actual black level and modulation span of every recording or playback
machine. Real captured RF must be checked rather than made brighter by
selecting an unrelated lower-carrier format.

## Lightweight video DSP

Both SuperBeta profiles use the following preview filter choices:

| Stage | Preview choice |
| --- | --- |
| RF high-pass | Six biquad sections at 2.0 MHz |
| RF low-pass | One biquad section at 6.88 MHz |
| Chroma low-pass | Four biquad sections at 350 kHz |
| Beta de-emphasis shelf | 8 dB, 100 kHz, Q = 0.5 |

These are engineering choices for the causal streaming preview, informed by
the existing Beta/.NET implementation. They are not Sony format constants
or a reproduction of a VCR's complete analog response. The matched Beta
sync-acquisition plane, chroma recovery and weave logic are reused. The
preview omits offline RF equalization, nonlinear sub-deemphasis, full TBC,
comb filtering, dropout repair and export processing.

SuperBeta also adds one 5 MSPS working sample (200 ns) to the cached chroma
registration delay when the decoder resets. This is a preview calibration
for these filters, not a Sony format timing constant. It adds no per-sample
filter stage or recurring calibration work.

## Display-level calibration

The first version recovered H/V sync using measured levels but still rendered
with fixed nominal blanking and Hz/IRE. A real SuperBeta Hi-Fi capture exposed
clipped shadow detail with this mismatch. Both SuperBeta profiles now cache
display blanking and Hz/IRE from the existing validated multi-line H-sync tip
and porch measurement at the first geometrically valid field. This uses the
-40/0 IRE interval, not the brightest/darkest picture pixels.

The window covers 192 lines and rejects weak RF, irregular H pulses, unstable
tip/porch distributions and implausible gain. Failed attempts retry at most
once per 50 ms. Healthy fields reuse the cached mapping, so fades and content
changes do not drive brightness pumping. Loss/reacquisition invalidates it;
the next valid field can measure again. Levels and pixels use the same WOW
corrected frequency coordinates. Ordinary VHS and Betamax retain their
previous nominal mapping. No new per-sample filter or resampling stage is
added; normalized RF/video rates stay 20/5 MSPS.

## Optional Hi-Fi audio

SuperBeta uses the existing NTSC Beta Hi-Fi worker, carrier calibration,
head switching, 48 kHz stereo PCM and monitor routing. **Audio Mon** off skips
audio DSP and PCM; turning it on enables listening without changing video
carriers or filtering. Queue overflow, stream gaps, seeks, profile changes
and stopped/restarted capture use the existing audio reset and stale-PCM
suppression rules. With multiple panels, the first visible Hi-Fi-capable
worker in A-left, A-right, B-left, B-right order supplies the monitor. A
SuperBeta recording without Hi-Fi carriers stays silent rather than falling
back to another audio source.

See [the Betamax preview notes](betamax_preview_README.md) for the shared
Hi-Fi signal processing, monitor ownership, queue and playback limitations.
Existing ordinary Betamax and Beta Hi-Fi carrier profiles retain their own
calibration.

## Validation

Real RF validation uses the supplied 40 MSPS SuperBeta Hi-Fi capture, with
the video-only and Hi-Fi-capable command-line profiles. Synthetic coverage
also checks both profiles, NTSC field cadence, chroma, adaptive display levels
and Full/Active geometry. GUI route tests cover the four video presets,
independent Audio Mon control and shared-worker monitor ownership.

All profiles keep the existing preview sample rates and causal filters.
Budget 1.5 and 2 file-based checks exercise the shared video/audio scheduler;
short-run timing differences are not universal frame-rate guarantees. The
software capture-pressure harness verifies recorded samples while preview
is paused under backlog, but uses a file producer that may wait/catch up.
It does not prove physical-device deadlines or sustained capture behavior.

See [the Betamax preview notes](betamax_preview_README.md) for usage, audio
monitor controls, the screenshot and validation boundaries.
