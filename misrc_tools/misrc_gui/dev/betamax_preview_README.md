# NTSC Betamax preview

Select **Decoder: RF Preview**, **Tape: Betamax**, then cycle **Preset** through
**Betamax**, **Betamax Hi-Fi**, **SuperBeta** and **ED Beta**. New panels default to Betamax.
These are manual video recording presets: Betamax Hi-Fi selects the higher
video FM carriers and matching RF filters; SuperBeta selects its own video
calibration. See the [SuperBeta notes](superbeta_preview_README.md) for details.

**Audio Mon** independently enables or mutes Hi-Fi listening for Betamax Hi-Fi,
SuperBeta and ED Beta. Changing the video preset or Tape does not turn monitoring on.
Muting skips audio DSP while retaining the video profile and lock. Ordinary
Betamax does not run Hi-Fi audio DSP, even if the RF contains audio carriers.
There is no automatic recording-format detection.

Only one visible Hi-Fi worker supplies the speaker stream. Priority follows
A-left, A-right, B-left, B-right panel order. Matching channel/profile panels
share a worker. This source replaces ordinary device PCM/Demod monitoring and
stays silent on loss of lock. Removing all visible Hi-Fi profiles restores
ordinary monitoring. Capture recording retains its original RF samples.
Disconnect, seek, signaled input gaps, profile changes and preview queue
overruns invalidate queued PCM and flush the device stream at the next
main-thread pump. Late callbacks while capture is stopped are rejected;
restarting with the same worker also flushes before accepting new PCM.

## Video

Both profiles use 688374.125874126 Hz color-under and Betamax's 0/180 degree
track/line rotation. The ordinary profile uses 3.6/4.8 MHz sync/white FM;
the Hi-Fi profile uses 3.8/5.2 MHz and stronger rejection of its audio carriers.
Beta synchronization uses the matched Beta de-emphasis plane. Video is woven
640x480 at nominal 29.97 fps; it is not deinterlaced.

Beta sync-level acquisition can also recover a picture from Hi-Fi-recorded RF
with the **Betamax** preset. The ordinary profile retains its own RF filtering and
fixed black/IRE calibration, so that picture can be noisy and differ from the
matching Hi-Fi profile. Acquisition does not detect or switch recording formats;
the preset still selects the video profile. **Audio Mon** controls listening.

After a 50 ms startup grace, or 50 ms without a newly accepted field, the
Beta-only fallback can estimate sync-tip and porch levels from the existing
5 MSPS ring. Attempts are limited to once per 50 ms; regularly advancing valid
fields bypass this statistical search. Candidate levels must pass pulse-width
and line-cadence checks before they are used. A successful estimate clears old
partial fields and vertical-sync history, then waits for future complete,
alternating fields through the normal V-sync checks. Ring samples used to
estimate levels are never replayed as decoded frames.

The format parameters and algorithm references are the local
`vhs-decode-dotnet` implementation, including
`Formats/format-params.snapshot.json` and `Decode/VhsChromaDecoder.cs`.
This remains a reduced-bandwidth streaming preview. It omits the offline
decoder's nonlinear sub-deemphasis, RF equalization, full TBC/comb/repair and
export pipeline. PAL Beta and linear audio are not supported. ED Beta has a
separate [20 MSPS video preview](ed_beta_preview_README.md).
Ordinary NTSC SuperBeta has separate profiles; see the
[SuperBeta preview notes](superbeta_preview_README.md) for their parameters
and validation limits.

## Hi-Fi audio

Audio consumes the same contiguous, rate-normalized 20 MSPS RF as video.
It runs in a separate consumer, with a fixed 52.4 ms input queue and its own
filter history. Video never waits for audio queue space. Queue overload resets
only audio, and stream generations suppress work from an earlier position.
Video and audio share the aggregate CPU budget; settings above one core permit
bounded parallel work. Turning Audio Mon off skips Hi-Fi DSP while retaining
the correct Hi-Fi video carriers and filtering. Unselected Hi-Fi panels also
skip audio DSP. Enabling the monitor again starts fresh calibration.

The audio device pulls Hi-Fi PCM directly from its bounded 85 ms queue, using
its requested frame count. GUI frame timing does not drive Hi-Fi refills, and
a device buffer larger than the requested size does not add periodic silence.
Device creation, source changes and flushing remain on the main thread.

Four carriers at 87.75, 97.25, 106.75 and 116.25 times the NTSC line frequency
are demodulated, calibrated against the captured head cadence, switched by
track and resampled to 48 kHz stereo. Initial calibration takes about 300 ms;
unlocked or unusable RF produces silence. A calibration refresh replaces the
learned head midpoint and carrier references only after its complete window
passes validation. A weak or transitional refresh preserves an existing valid
lock; independent live head-cadence and carrier-loss checks still mute bad RF.
Brief losses have bounded repair;
longer losses mute. EQ, stereo-linked expansion and high-frequency dynamics
follow the latest local .NET Beta Hi-Fi implementation's default settings.

The reference is the uncommitted implementation present on 2026-10-20 in
`VHSDecode.Core/HiFi/BetaHiFiRfDecoder.cs`, `BetaHiFiSignalDsp.cs` and
`BetaHiFiAudioProcessor.cs`, with `docs/ntsc-betamax-hifi.md`:

| Reference file | SHA-256 |
| --- | --- |
| BetaHiFiRfDecoder.cs | fb0270a37130cf6e4f3b8c8e2c020868f1fc9e29092b17f2de5677f14c1f2cba |
| BetaHiFiSignalDsp.cs | 76c2c3f59137c0040b7f909b720bf76fa7291bde35ec7e58b3ef7f793f6dbe87 |
| BetaHiFiAudioProcessor.cs | 03f63e24d693181b2522747f6e609106ef2d0d23efbd7dc8b167628d4a1c9c71 |

This C implementation adapts offline processing to causal, fixed-memory
streaming filters and capture-local calibration. It does not promise bitwise
or perceptual equivalence to the offline decoder. No whole-file normalization
or synthesized noise is applied.

The long IQ FIRs select an AVX2 kernel at runtime when the CPU/OS supports it,
otherwise retaining SSE2 or portable scalar processing. AVX2 is isolated from
the baseline executable and does not require a global AVX2 build target.
Its accumulation grouping preserves the SSE2 PCM output. No FIR taps,
coefficients, sample rates or sound-processing settings are reduced for speed.

The playback producer never waits for the audio device. Its FIFO holds at most
4096 stereo frames, keeping the newest audio on overflow. The device callback
pulls the frame count it needs; the requested buffer size is only a hint.
This bounds the software playback backlog but does not
establish exact audio/video synchronization or device latency. Preview CPU
overruns can reset both decoders and cause short audio interruptions. A lower
shared CPU Budget can therefore prevent continuous sound. The GUI already
offers 0.50/0.75/1.00/1.50/2.00/3.00 core budgets, with a machine-scaled default.
The optional final file-probe argument selects that same budget explicitly;
its realtime default is 1.00, independently of the GUI's machine default.

## Validation and limits

The automated suite covers ordinary Beta/Hi-Fi/SuperBeta video, color, weave,
Full/Active geometry, sync reacquisition, initial/reacquired Hi-Fi lock at head
switches, dropouts, nonfinite input, block splitting, route ownership, mute,
capture restart, sidecar seeks and monitor-source changes. File probes can
save the same worker-published RGBA frame and 48 kHz PCM used by preview:

```sh
build/gui_rf_preview_file input.s16le 40000000 ntsc-beta-hifi output.ppm --max output.wav 6
build/gui_rf_preview_file input.s16le 40000000 ntsc-superbeta-hifi output.ppm --realtime output.wav 32 1.5
build/gui_rf_preview_file input.s16le 40000000 ntsc-superbeta output.ppm --max --full 6
```

Budget is a shared CPU allowance, not a frame-rate guarantee. File-based
checks cover real NTSC Beta Hi-Fi and SuperBeta Hi-Fi RF, including Budget
1.5 and 2. Ordinary Beta has synthetic coverage. Physical-device capture,
speaker listening, precise A/V synchronization and sustained operation are
separate validation steps; file probes do not establish those properties.

The video path is RF worker RGBA -> copied renderer RGBA -> raylib RGBA
texture -> DrawTexturePro. The probe reads the worker's published RGBA;
rendering retains byte-wise RGBA and does not repack channels. Headless
results do not measure the final GPU display or audio device latency.
