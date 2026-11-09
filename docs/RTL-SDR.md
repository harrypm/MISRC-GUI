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

## Preview VHS Hi-Fi audio

With a VHS Hi-Fi RF signal at the receiver input, open the **Video** panel
and select **RF Preview**, **Format: VHS**, and **Preset: Hi-Fi**. Choose
the **System** that matches the tape, PAL or NTSC.

Use the corresponding frequency preset, **1.500 MHz (NTSC Hi-Fi)** or
**1.600 MHz (PAL Hi-Fi)**, and keep both audio carriers inside the sampled
band. The Hi-Fi I/Q path accepts **1.024, 1.200, 1.536, 2.048, or 2.400 MSPS**.
Enable **Audio Mon** to listen to the decoded stereo audio. See the
[VHS Hi-Fi decoder reference](DECODER_MODES.md#vhs-hi-fi-audio-decoder).
