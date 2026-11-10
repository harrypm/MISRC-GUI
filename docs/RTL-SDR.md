# RTL-SDR USB receivers

MISRC GUI can capture complex I/Q samples from RTL2832-based USB dongles
recognized by `librtlsdr`. Use a build with RTL-SDR enabled. See the
[installation guide](../INSTALLATION.md) and the [Osmocom RTL-SDR project](https://github.com/osmocom/rtl-sdr).

## Connect and tune

1. Plug in the receiver and select the **[RTL-SDR]** entry in the device
   list. On Windows, follow your dongle's WinUSB setup instructions if it
   needs a driver. See the [RTL-SDR Blog Windows setup guide](https://www.rtl-sdr.com/rtl-SDR-quick-start-guide/).
2. In **Settings**, open the **SDR** section and set **Frequency (Hz)** and
   **Sample rate** for the signal. Both controls offer presets and a custom
   value. Choose **Auto** or **Manual** under **Gain mode**.
3. Select **Connect** and grant USB permission if prompted on Android. If
   you change the frequency or sample rate in **Settings** during capture,
   disconnect and reconnect to apply the change.
4. Open **Demod** to listen to radio signals. Pick WFM, NFM, AM, USB, or
   LSB, then enable **Audio Mon**. See the [Demod modes](DECODER_MODES.md#demod-panel-modes).

## Blog V4 manual VGA reference

On Windows, Linux, and macOS builds with MISRC's patched librtlsdr,
**Gain mode → Manual** exposes VGA indices `0x00`–`0x08` for the
**RTL-SDR Blog V4** with its **Rafael Micro R828D** tuner. The manual default
is `0x08`. Reconnect to apply a changed index.

In **Auto** gain mode, librtlsdr controls the LNA and mixer gain and uses a
fixed VGA index of `0x0B`. The GUI hides the manual VGA control and keeps the
saved manual index for the next time you connect in **Manual**. The separate
**AGC** toggle controls the RTL2832U's digital AGC.

The table is an empirical reference for the single R828D-based Blog V4 unit
we tested. We fed the receiver from a VCR's Hi-Fi RF test point and used
**1.500 MHz** tuning, **1.024 MSPS**, manual tuner gain **0.0 dB**, and **AGC off**.
The values describe the change in captured I/Q signal power relative to the
manual VGA default `0x08`. The `0.0 dB` entry defines the comparison baseline.

| VGA index | Relative signal level (dB) |
|---|---:|
| `0x08` (default) | 0.0 |
| `0x07` | −3.7 |
| `0x06` | −7.1 |
| `0x05` | −10.7 |
| `0x04` | −14.2 |
| `0x03` | −17.7 |
| `0x02` | −21.2 |
| `0x01` | −24.5 |
| `0x00` | −27.8 |

In the repeat sweep, `0x08` reached the USB I/Q code limits. We measured
`0x00` through `0x07` against the unclipped `0x07` capture and used the
`0x07` to `0x08` difference from the earlier unclipped sweep to express the
values against `0x08`. Treat the numbers as approximate relative changes
for that receiver and setup. Other units, tuner chips, and frequencies may
give different results.

## Preview VHS Hi-Fi audio

With a VHS Hi-Fi RF signal at the receiver input, open the **Video** panel
and select **RF Preview**, **Format: VHS**, and **Preset: Hi-Fi**. Choose
the **System** that matches the tape, PAL or NTSC.

Use the corresponding frequency preset, **1.500 MHz (NTSC Hi-Fi)** or
**1.600 MHz (PAL Hi-Fi)**, and keep both audio carriers inside the sampled
band. The Hi-Fi I/Q path accepts **1.024, 1.200, 1.536, 2.048, or 2.400 MSPS**.
Enable **Audio Mon** to listen to the decoded stereo audio. See the
[VHS Hi-Fi decoder reference](DECODER_MODES.md#vhs-hi-fi-audio-decoder).

## Record a single Hi-Fi RF file

**Hi-Fi RF** recording requires a build with RTL-SDR and SoXR support and a
local output folder. It reconstructs one **8 MSPS, signed 16-bit mono RF**
stream from both I and Q. The file contains the FM carriers for later
decoding into stereo audio.

1. Select the RTL-SDR device. In **Settings → SDR**, start with the
   **1.500 MHz** NTSC or **1.600 MHz** PAL frequency preset and
   **1.024 MSPS**. Connect or reconnect to apply the tuning and sample rate.
2. Under **Settings → Capture**, switch the recording mode from **Native I/Q**
   to **Hi-Fi RF**. MISRC uses both I and Q automatically. The `(tag)` field
   beside **16-bit** adds an optional tag to the filename.
3. Set **RF FLAC compression** to **ON** for lossless FLAC on builds with
   libFLAC, or **OFF** for uncompressed little-endian `.s16`. Use **Record**
   to start recording and **Stop Rec** to finalize the file.

**Sample rate** controls the receiver's input rate. MISRC checks the actual
rate and center frequency when recording starts. The usable passband must
include both carrier bands for PAL or NTSC, and the tuned band must lie
strictly between DC and the 4 MHz output Nyquist limit. The file rate stays
at 8 MSPS.

## Play a recorded Hi-Fi RF file

1. Under **Settings → Playback files**, load the `.flac` or `.s16` file into
   **Ch A…**. Clear **Ch B** if a previous file is still loaded.
2. Select **[Playback] FLAC Files** in the device list and click **Connect**.
   This device handles both formats on builds with libFLAC playback support.
3. In the **Video** panel for channel A, choose **RF Preview**, **Format: VHS**,
   **Preset: Hi-Fi**, and the tape's **System** (PAL or NTSC). Enable **Audio Mon**
   to hear the decoded stereo audio.

Keep the generated `_hifi_rf_16-bit_8.000msps.s16` filename ending when renaming
a raw capture so playback can identify its sample rate and sample scaling.
A finalized FLAC stores that information in its metadata. MISRC's RF FLAC
convention stores 8000 in the FLAC header and 8,000,000 samples/s in the
`RF_SAMPLE_RATE` tag.
