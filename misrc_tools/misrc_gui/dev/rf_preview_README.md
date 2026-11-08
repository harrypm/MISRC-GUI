# Tape RF preview

The RF Preview panel provides PAL/NTSC/SECAM VHS SP (native or MESECAM), PAL/NTSC S-VHS,
NTSC Betamax, ordinary NTSC SuperBeta and NTSC-J ED Beta video-RF preview. It weaves two original fields into alternating
rows, without deinterlacing, field duplication or motion interpolation.
Output is RGBA at 768x576 PAL or 640x480 NTSC, displayed at 4:3. Nominal
output rates are 25 and 29.97 fps.

## Operation

1. Open a Video panel for the desired RF channel.
2. Cycle Decoder to **RF Preview** and choose **Format: VHS** or
   **Format: Betamax**. In VHS mode, **System** cycles through PAL, NTSC,
   SECAM and MESECAM. Choose **System: SECAM** for native frequency-divided
   recording or **System: MESECAM** for heterodyne recording.
   Cycle **Preset** between **VHS** (default) and **S-VHS**. S-VHS supports
   manual PAL/NTSC selection. Selecting SECAM or MESECAM restores the VHS
   preset. Selecting S-VHS from either system switches to PAL. Betamax currently
   supports NTSC only. Cycle **Preset** through **Betamax** (default),
   **Betamax Hi-Fi**, **SuperBeta** and **ED Beta** to select the video recording profile.
   **Audio Mon** independently controls Hi-Fi listening for Betamax Hi-Fi,
   SuperBeta and ED Beta. Changing a preset does not enable monitoring; muting keeps the
   video profile and lock.
3. Connect a capture device or select FLAC playback files and connect.
4. The shared Budget button cycles 0.50, 0.75, 1.00, 1.50, 2.00 or 3.00
   logical cores and is saved with the existing application settings. Its
   default scales with the machine's logical processor count.

Only RF Preview uses this RF pipeline. CVBS and Tape Decode retain their existing
processing paths. The system and tape speed are not detected automatically.
The input must contain video RF. Beta Hi-Fi needs the four audio carriers on
that same captured RF channel; a separate audio-only RF feed does not produce
a picture. Linear tape audio is outside this preview. See
[NTSC Betamax details](betamax_preview_README.md) and
[ordinary NTSC SuperBeta details](superbeta_preview_README.md) for profile,
parameter and audio limits.

The OSD reports physical input rate, H/V/color lock, decoded/displayed fps,
DSP time, worker age, vertical candidates, accepted/rejected fields, queue
loss, pressure skips and worker CPU. Display fps counts unique frames drawn.
Worker age starts at preview submission; it excludes device buffering, the
upstream capture FIFO and final UI presentation. These counters are separate
from recording drops.

Active frames are 768x576 for PAL/SECAM/MESECAM or 640x480 for NTSC. Full
frames retain 768x626 or 640x526 samples, including horizontal and vertical
blanking. Full display pixels are wider because its horizontal window spans
the entire line. The UI corrects that pixel aspect using the actual delivered
frame size, preserving 4:3 inside the active picture even while modes switch.
The complete Full raster therefore has a display aspect about 1.5100 for
PAL-family profiles or 1.4562 for NTSC-family profiles. Window resizing only
fits and centers that geometry; it does not change the decoded sample count.
See [MESECAM details](mesecam_vhs_preview_README.md) for signal and validation
limits distinct from [native SECAM](secam_vhs_preview_README.md).

## Sample rates and continuity

The decoder processes RF internally at 20 MSPS. Ordinary VHS, Betamax and
SuperBeta use these conversion paths:

| Physical input | Conversion |
| --- | --- |
| 20 MHz | No rate conversion |
| 40 MHz | Stateful halfband FIR, 2:1 |
| 80 MHz | Two stateful halfband FIR stages, 4:1 |
| Other 16..200 MHz | libsoxr HQ, when available |

Without libsoxr, these ordinary profiles support only 20/40/80 MHz.
S-VHS and ED Beta require at least 20 MSPS input. Native 20 MSPS bypasses
rate conversion; higher rates, including 40/80 MSPS, use their dedicated
libsoxr converter with a 9.5 MHz passband and 10 MHz stopband. The S-VHS
picture/sync path then works at 5 MSPS. Capture RF retains its original
physical rate. Unsupported rates fail explicitly. Rate support does not
establish usable signal bandwidth or picture quality for every device.

Every input block carries its physical rate, absolute sample position,
epoch and discontinuity flag. Rate changes, seeks, looping, channel mapping
changes, missing input and dropped preview blocks reset resampler/decoder
history. Hardware writes publish sparse metadata at absolute FIFO positions;
preview rejects ambiguous boundaries while recording keeps the acquired RF.
Unavailable preview metadata disables preview for that block and preserves
recording channel presence and mapping using available capture information.

FLAC uses actual decoded sample counts; final-block padding is not decoded as
RF. Its physical rate is retained independently of whether length is known.
Same-rate pairs share the paired reader and FIFO. A MIXED-RATE pair (both
files loaded, different resolved rates) runs the player's INDEPENDENT mode
(2026-11-03, user request: "Player should run files independently"): each
file feeds its own per-channel RF FIFO at its own true clock, the preview
workers are fed per channel with honest rate/position metadata, and both
decoders run time-correctly. Link mode: Synced (default) keeps one shared
transport; Separate gives each file its own Play/Pause + Loop next to its
file input row. A channel reaching its end without loop pauses only that
channel. Recording stays impossible during playback, so the archival paths
never see the independent streams.

## Processing and ownership

`capture/playback -> raw FIFO + metadata -> extraction -> bounded preview
queue -> rate normalization -> RF decoder -> newest RGBA -> UI`

- Extraction submits every block independently of the waveform/display stride.
  It never waits for decoder, renderer, frame mutex, CPU permit or queue space.
- Visible RF Preview panels subscribe to a worker keyed by channel and complete
  tape/system/Hi-Fi profile. Matching panels share decoding and one input submission. Workers are
  created lazily and released after the last subscriber and in-flight copy.
- Each worker has 256 slots of 65536 int16 samples, about 32 MiB. Queued input
  duration and age are capped at 200 ms. Overrun drops stale preview work and
  reacquires from the next accepted block.
- Workers own resampler, filter, sync and color state. The UI owns textures and
  its frame copy; capture does not access Raylib textures.
- Beta Hi-Fi audio has a separate consumer and fixed 32-slot queue of 32768
  float RF samples per slot (about 4 MiB, 52.4 ms of RF). Enqueue never waits.
  Audio overload invalidates audio history without resetting video. Both
  consumers retain the same 20 MSPS sample positions and stream generations.
  Only the selected Hi-Fi monitor source decodes audio while Audio Mon and
  capture are active; muting retains the Hi-Fi video profile.

The streaming decoder performs quadrature FM demodulation, luma deemphasis,
independent color-under IQ recovery, horizontal/equalization/vertical sync,
measured line resampling, burst phase/gain correction and PAL/NTSC conversion.
Native SECAM uses separate FM chroma discrimination, causal LF deemphasis,
alternating D'R/D'B identification and same-field line memory instead of the
PAL/NTSC burst/phase path. Its luma calibration uses validated H-sync tips and
porches, preserving fades without normalizing each picture line. See
[native SECAM VHS](secam_vhs_preview_README.md) for recording-type and
identification limits.
Y and chroma share the line map. Field parity and cadence must agree before
publishing a woven frame. Missing chroma falls back to grayscale.

After 40 lines without H sync, acquisition discards learned thresholds, pulse
history and unfinished fields. Continuous input can then recover automatically
when tape playback returns, without reconnecting or changing PAL/NTSC. Sample
positions, DSP filters, diagnostic totals and the capture session stay intact.

Luma uses fixed profile levels with bounded, slow timing correction rather
than noisy per-line black/gain normalization. NTSC VHS and PAL/NTSC S-VHS
display deemphasis is independent of the sync plane; Beta synchronization
uses its matched shelf. S-VHS also stabilizes burst phase/gain and gates
neighboring color pairs at luma or chroma boundaries. Chroma averaging uses
neighbors from the same field and retains the original luma rows.

The input-rate separation, field assembly and color-under concepts were
informed by vhs-decode-dotnet. The halfband coefficients originate from its
`VHSDecode.Core/Rf/HalfRateSampleLoader.cs`; source attribution is retained in
`gui_rf_resampler.c`. This preview does not implement the complete offline
.NET TBC/color pipeline or promise output equivalence. VHS LP/EP,
PAL Betamax, dropout repair and advanced comb filtering are not implemented.
The S-VHS preview omits its additional linear/nonlinear sub-deemphasis and
full RF equalization.

## Capture protection

All video and Hi-Fi consumers share a soft CPU-time budget, with a
machine-scaled default and FIFO arbitration. The concurrent processing limit
is the ceiling of the configured budget, capped at eight; one core or less
retains a single active permit. Budget waits occur only inside consumers.
Windows uses thread CPU time and requests below-normal
priority; unavailable thread accounting falls back to processing wall time.

This allowance covers preview processing, not the entire GUI or an OS-enforced
quota. The bounded CPU token balance includes reservations reconciled against
measured cost, with refill retained for in-flight work rather than lost during
long blocks. Idle time cannot accumulate an unbounded processing allowance.
On Windows, the allowance also preserves a bounded accounting quantum per
registered thread counter so delayed CPU-time reports do not charge already
elapsed work a second time through unnecessary pacing waits. Valid zero CPU
deltas stay zero; only an unavailable counter uses work wall time.
A block cannot be preempted; active blocks and coarse per-thread OS CPU
accounting can produce short bursts above the selected rate.

Extraction monitors capture and selected recording buffers. Preview pauses
at 85% fill or a new write drop, or after 150 ms continuously at 60% fill.
Recovery requires fill at or below 35% without new drops for two seconds.
A new session clears old pressure state. Paused workers reject input before
copying and freeze the last complete frame; resume reacquires from fresh RF.

## FLAC startup and timeline

Connect prepares an independent playback context on a joinable background
thread. The UI publishes it and starts playback after preparation succeeds.
Cancel/Space request cancellation; shutdown joins the loader before freeing
application state. Disconnect joins producers/indexing before retiring their
context, so a later connection cannot inherit old decoder or index results.

Usable metadata or a validated persistent cache supplies the length directly.
Otherwise, startup attempts a bounded libFLAC tail probe: metadata reads at
most 1 MiB, tail access/read totals at most 8 MiB, callback reads at most
64 KiB, and an approximately 250 ms cooperative deadline. A filesystem call
already in progress cannot be interrupted by that deadline.

The tail endpoint is provisional and appears with `~` on the timeline. It
allows best-effort seeking while an independent background reader verifies
all frame grammar/integrity from the beginning. Tail failure leaves ordinary
unknown-length playback available. The producer's actual EOF controls looping;
a provisional endpoint does not truncate playback.

Successful verification publishes the total and caches it without changing
position, speed, pause/loop state or epoch. Failure withdraws the provisional
length and seeking while playback can continue. The cache validates file
identity, size, mtime and bounded head/tail fingerprints; it is not a complete
content hash. Full verification is not a PCM/STREAMINFO MD5 check. A successful
seek to valid embedded frames does not establish their stream provenance.

## Build and tests

Install the dependencies described in the repository README. From its root:

```sh
meson setup build misrc_tools --buildtype=release
meson compile -C build
meson test -C build --print-errorlogs --num-processes 1
```

The registered tests cover synthetic PAL/NTSC RF, field weave, sync loss and
recovery, color, resampling, queue/lifecycle/CPU policy, input stamps,
recording isolation under missing metadata, FLAC startup/cancel/seek/cache,
physical-rate tags and timeline state. The timeline helper test is registered
for native, single-command compilers when Python is available. Tests do not
require a capture device or media file.
On Windows, verify static RTL-SDR linkage and startup separately from
hardware capture validation.

For optional media checks, raw input is signed little-endian int16 carrying
the normalized signed 12-bit capture values. A padded 16-bit capture must be
converted to that scale first. Commands below use POSIX executable names;
Windows executables have `.exe`:

```sh
build/gui_rf_preview_file input.s16le 20000000 ntsc output.ppm --realtime
build/gui_rf_preview_file input.s16le 40000000 pal output.ppm --max
build/gui_rf_preview_file input.s16le 40000000 pal-svhs output.ppm --max
build/gui_rf_preview_file input.s16le 40000000 ntsc-beta-hifi output.ppm --max output.wav 6
build/gui_rf_preview_file input.s16le 40000000 ntsc-beta-hifi output.ppm --realtime output.wav 32 1.5
build/gui_rf_preview_record_load input.s16le 20000000 ntsc output-prefix 60 on 1 4 1.0
```

The file probe links the production worker/resampler/core, prints metrics and
saves the last complete frame. Maximum-speed mode drains all submitted input;
realtime mode follows its physical clock. The record-load probe compares
recorded samples and reports drops; `off` supplies its baseline without
preview. These probes bypass the physical capture device and native window.

The longer reconnect integration is built explicitly and needs an existing
output directory plus at least one second of original padded 16-bit, 20 MHz
NTSC raw RF. It writes these samples as FLAC16 and production playback then
reduces them to the capture scale; do not pre-scale this probe's input:

```sh
meson compile -C build gui_rf_playback_reconnect_test
build/gui_rf_playback_reconnect_test padded16-ntsc20.s16le output-dir
```

It re-encodes supplied RF as FLAC and exercises the actual playback FIFO,
extraction, routes, worker/core, frame swap and display-thread lifecycle over
four sessions, including 30 seconds disconnected. It checks both known and
unknown header lengths. Graphics and inactive recording services are stubbed;
this is not physical-device or visible-window validation.

Real media quality, weak-machine performance and sustained hardware recording
still need separate validation. Short headless checks cannot establish those
properties for every capture or computer.
