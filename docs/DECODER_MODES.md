# MISRC-GUI decoder modes and states

This page lists every decoder mode in the application and the state of each:
where it is selected, what it decodes, and its current implementation/runtime
status. Sources are the shipping code and the per-feature dev notes in
`misrc_tools/misrc_gui/dev/`; nothing here is a roadmap promise.

Related pages: [MONITOR_MODES.md](MONITOR_MODES.md) (network modes — not
decoders), [MONITOR_VIDEO_CODECS.md](MONITOR_VIDEO_CODECS.md),
[MONITOR_VIDEO_STAGE1.md](MONITOR_VIDEO_STAGE1.md),
[MONITOR_VIDEO_STAGE2.md](MONITOR_VIDEO_STAGE2.md), [MACOS-CXADC.md](MACOS-CXADC.md).

## Video panel — Decoder cycle

The **Decoder** button on a Video panel cycles the panel's decoder plugin
(`video_plugin_kind_t` in `signal/gui_cvbs.c`):

| Mode | Label | Input | State |
|---|---|---|---|
| `VIDEO_PLUGIN_CVBS` | CVBS | Baseband composite (PAL/NTSC/SECAM) | Working — in the cycle |
| `VIDEO_PLUGIN_VHS_FM` | RF Preview | Tape video RF (15 profiles, below) | Working — in the cycle |
| `VIDEO_PLUGIN_TAPE_DECODE` | Tape Decode | Baseband composite with tape-tuned PLL/smoothing | **Hidden from the UI** (`VIDEO_TAPE_DECODE_UI_ENABLED 0`) — code stays compiled, unreachable until it works; flip the define to re-expose |

## RF Preview tape profiles

`gui_rf_profile_t` in `signal/gui_rf_preview.h` (15 profiles). Selected on
the Video panel via **Format** (tape family: VHS / Betamax / LaserDisc),
**System** (VHS: PAL / NTSC / SECAM / MESECAM — the old MESECAM toggle is
gone, the system selector owns it; LaserDisc: PAL / NTSC) and **Preset**
(VHS: VHS / S-VHS / Hi-Fi; Betamax: Betamax / Betamax Hi-Fi / SuperBeta /
ED Beta). Profiles 13–14 are audio-only. All video profiles are
**reduced-bandwidth streaming previews**, not full-TBC archival decoders:
nonlinear sub-de-emphasis, RF equalization, dropout repair and comb
filtering are deliberately omitted. All weave the two original fields (no
deinterlacing).

| # | Profile | System | Chroma method | Hi-Fi audio | Geometry (Active/Full) | State |
|---|---|---|---|---|---|---|
| 0 | PAL VHS SP | PAL 625/50 | Burst-gated color-under | — | 768×576 / 768×626 | Working; synthetic-RF regression suite (weave, sync loss/recovery, colour, resampling) |
| 1 | NTSC VHS SP | NTSC 525/60 | Burst-gated color-under | — | 640×480 / 640×526 | Working; same synthetic regression suite |
| 2 | NTSC Betamax | NTSC | 688.374 kHz color-under, 0/180° rotation | — | 640×480 / 640×526 | Working; synthetic coverage (ordinary Beta has no real-file fixture) |
| 3 | NTSC Beta Hi-Fi | NTSC | as Betamax, 3.8/5.2 MHz carriers | Yes (4-carrier) | 640×480 / 640×526 | Working; real-file validated (video + audio, Budget 1.5/2) |
| 4 | NTSC SuperBeta | NTSC | 688 kHz color-under, 4.4/5.6 MHz endpoints | — | 640×480 / 640×526 | Working; real 40 MSPS SuperBeta Hi-Fi capture validated, display levels calibrated from H tips/porches |
| 5 | NTSC SuperBeta Hi-Fi | NTSC | as SuperBeta | Yes (4-carrier) | 640×480 / 640×526 | Working; real-file validated |
| 6 | SECAM VHS SP (native) | SECAM 625/50 | Native divide-by-four FM chroma (1.0625/1.1015625 MHz) | — | 768×576 / 768×626 | Working; real-file validated (499 fields / 249 frames, zero rejects). Recordings without usable VBI bottles stay grayscale by design |
| 7 | MESECAM VHS SP | SECAM 625/50 via PAL converter | Heterodyne FM (Dr 654.3 kHz / Db 810.6 kHz), studio deviation | — | 768×576 / 768×626 | Working; real-file validated (colour bars + carrier switch-off files, per-file counters in the dev note) |
| 8 | NTSC ED Beta | NTSC-J | 688 kHz color-under, 6.8/9.3 MHz carriers | Yes (4-carrier) | 640×480 / 640×526 | Working as a **monitoring preset** — flat de-emphasis, reduced 5 MSPS picture rate; not a full-fidelity ED archival decoder |
| 9 | PAL S-VHS | PAL 625/50 | Color-under + burst phase/gain stabilization | — | 768×576 / 768×626 | Working; synthetic tests (`gui_rf_svhs`, `gui_rf_svhs_color`, `gui_rf_svhs_stripe`) |
| 10 | NTSC S-VHS | NTSC 525/60 | Color-under + burst phase/gain stabilization | — | 640×480 / 640×526 | Working; same S-VHS synthetic tests |
| 11 | PAL LaserDisc | PAL | Composite video FM, chroma at fsc | LD audio chains (below) | 768×576 / 768×626 | Working; synthetic test (`gui_rf_laserdisc`) |
| 12 | NTSC LaserDisc | NTSC | Composite video FM, chroma at fsc | LD audio chains (below) | 640×480 / 640×526 | Working; synthetic test (`gui_rf_laserdisc`) |
| 13 | NTSC VHS Hi-Fi | NTSC | — (audio-only profile) | Yes (VHS Hi-Fi decoder) | — | Working (audio only); route + audio-monitor tests |
| 14 | PAL VHS Hi-Fi | PAL | — (audio-only profile) | Yes (VHS Hi-Fi decoder) | — | Working (audio only); route + audio-monitor tests |

Notes:

- **S-VHS** (9/10): PAL/NTSC only — selecting S-VHS from SECAM/MESECAM
  switches to PAL, and SECAM/MESECAM restore the VHS preset. Requires
  ≥ 20 MSPS input; higher rates use a dedicated wide-passband libsoxr
  converter, with the picture/sync path working at 5 MSPS. The format's
  extra linear/nonlinear sub-deemphasis and full RF equalization are
  omitted.
- **LaserDisc** (11/12): video decoded by `gui_ld_rf_demod` (20 MSPS RF →
  5 MSPS luma IRE + complex chroma at fsc); frames are published
  audio-clock-synced (`gui_rf_preview_worker_take_ld_frame`). Audio chains
  below.
- **VHS Hi-Fi** (13/14): audio-only profiles (`gui_rf_profile_is_audio_only`)
  selected by Preset **Hi-Fi** — no picture is decoded. See the VHS Hi-Fi
  audio decoder below.
- **Beta Hi-Fi audio** (`gui_rf_profile_has_hifi`): profiles 3, 5 and 8 plus
  the audio-only VHS Hi-Fi profiles enable a separate audio decoder. Beta
  uses four carriers at 87.75/97.25/106.75/116.25 × fH with ~300 ms
  calibration to 48 kHz stereo, Audio Mon gated; the GUI's SuperBeta preset
  uses the Hi-Fi-capable worker so Audio Mon works without changing the
  video profile.
- **Input rates**: 20/40/80 MHz native conversion; other 16–200 MHz rates
  via libsoxr. ED Beta and S-VHS use the dedicated wide-passband converter
  before their reduced-rate preview filters.
- **Not implemented**: VHS LP/EP, PAL Betamax, linear (non-Hi-Fi) tape audio,
  dropout repair, offline TBC/export quality, S-VHS sub-deemphasis and full
  RF equalization.
- Profiles 9–14 arrived upstream (PR #24, `rf-preview-pr`); their states
  cite the registered synthetic tests — per-feature dev notes do not exist
  for them yet.

### LaserDisc audio and digital chains

Analog and digital LaserDisc audio run alongside the LD video profile
(48 kHz interleaved stereo, lock flags, unusable RF muted):

- `gui_ld_rf_analog` — analog FM audio (PAL/NTSC system select)
- `gui_ld_efm_frontend` + `gui_ld_rf_digital` — EFM digital audio: run
  lengths 3–11 recovered from equalized 20 MSPS RF, followed by CIRC error
  correction and an EOF drain via `finish`. Choose **Audio: Digital PCM** for
  linear PCM. On PAL and NTSC, **Audio: DTS** decodes the 44.1 kHz DTS core
  carried on the same digital track and outputs 48 kHz stereo. Builds without
  FFmpeg's DCA decoder omit the DTS option and still offer Digital PCM.
  See the [DTS development record](../misrc_tools/misrc_gui/dev/prompt_ld_dts_readme.md)
  for validation details.
- `gui_ld_ac3_rf` → `gui_ld_ac3_frames` → `gui_ld_ac3_codec` — AC-3 RF:
  QPSK symbol demodulation, framing and decode (vendored liba52)

### VHS Hi-Fi audio decoder

`gui_vhs_hifi.h` — PAL/NTSC VHS Hi-Fi RF to 48 kHz interleaved stereo.
Accepts 20 MSPS or the native low rates (8 / 8.192 / 10 MSPS), or direct
complex-baseband I/Q with a center frequency (the RTL-SDR route via
`gui_video_submit_rtlsdr`). The Hi-Fi-only preview profile can resample
other real RF rates within 8–10 MSPS to 20 MSPS on builds with SoXR.
Unusable RF is muted, and discontinuities reset lock and signal history.
See the [RTL-SDR recording and playback guide](RTL-SDR.md#record-a-single-hi-fi-rf-file).

### RF Preview runtime states

`gui_rf_decode_status_t` (shown on the OSD, `gui_rf_decoder_get_status`):

- `h_locked` / `v_locked` / `color_locked` — horizontal, field and colour lock.
- `fields_accepted` vs `fields_rejected`, with mutually exclusive
  `reject_cadence` / `reject_parity` / `reject_line_map` counters and
  `last_reject_reason`.
- `color_losses` counts locked→unlocked transitions (with
  `last_color_loss_reason`), not per-row grayscale.
- After 40 lines without H sync the decoder discards acquisition history and
  reacquires automatically; counters and the capture session are preserved.

## CVBS decoder modes

`cvbs_decoder_t` in `signal/gui_cvbs.h` / `gui_cvbs.c` (used by the CVBS
plugin and the hidden Tape Decode plugin):

| Selector | Values | State |
|---|---|---|
| System (`cvbs_format_t`) | PAL, NTSC, SECAM (auto-detected; manual dropdown persists in `video_system`) | Working. MESECAM is RF-only: a persisted MESECAM pick maps composite/tape decoders to SECAM |
| Decoder mode | CVBS (baseband) / TAPE (tape-tuned luma kernel + PLL) | CVBS working; TAPE compiled but hidden with the Tape Decode plugin |
| Tape family (`cvbs_tape_format_t`) | VHS, S-VHS, Betamax, Video8, Hi8, U-matic | Selectable; biases luma kernel sharpness and PLL lock thresholds (S-VHS/Hi8 sharper, U-matic softer). Baseband decode only — these are not RF demodulation profiles |
| Tape speed (`cvbs_tape_mode_t`) | SP, LP, ELP | Selectable; PLL tuning + smoothing presets |
| Colour (`cvbs_colour_mode_t`) | OFF (grayscale) / AUTO | Working — burst-gated baseband chroma; SECAM uses the FM-chroma port below |
| 50 Ω | Off (default) / On | Software correction for PAL and NTSC CVBS captured with the Domesday Duplicator. Enabling it on SECAM selects PAL. SECAM is available again when correction is off. |
| Levels (`cvbs_level_mode_t`) | AUTO (per-field adaptive) / FIXED (latched after lock) | Working |
| Sync (`cvbs_sync_mode_t`) | AUTO / FIXED (frozen line period after lock) | Working — default is FIXED |
| OSD (`cvbs_osd_mode_t`) | OFF / MINIMAL / STATS | Working |
| Frame view (`cvbs_frame_view_t`) | ACTIVE (720×576/486) / FULL (4fsc width, 625/525 lines) | Working |

SECAM FM chroma (`cvbs_secam_state_t`): ported from the tbc-tools
secamdecoder pipeline; field calibration locks **one field behind**
(last-good-reuse), per-line porch tracking handles wow/flutter. Diagnostics
(carrier pair, ident confidence/source, porch noise MAD, cal/reuse/nominal
field counters) are exposed via `gui_cvbs_get_secam_diag` and the STATS OSD.

Runtime states: software-PLL `locked` flag with good/bad sync streaks,
V-sync field-boundary state (candidates accepted/rejected with cadence and
parity reject counters), adaptive level state, and a self-measured capture
rate (median H-spacing + locked-PLL refinement) that overrides a wrong
declared rate.

## Radio panel modes

The **Radio** panel (formerly **Demod**; `signal/gui_demod.c`, `demod_mode_t` in
`signal/gui_demod.h`) is an SDR-style receiver: a spectrum and waterfall of
the input, a red tuning line with its channel shaded, and the demodulated
audio strip below. Audio is resampled to 48 kHz and plays through
**Audio Mon**.

### Tuning

- Click or drag in the spectrum or waterfall to tune. On an SDR the mouse wheel scrolls the band (Shift + wheel steps the line); on other inputs it steps the line.
- Tuning snaps to the mode's step on the real frequency, as SDR++ does.
- On an SDR the readout is the absolute frequency (centre + offset).
- SDR frequency box (top row): shows the SDR centre in MHz.
  - Click it and type MHz (e.g. `99.1`), then Enter. Values outside 0.5-1766 MHz are refused.
  - **Band** dropdown: each band is its range (FM 87.5-108, Airband 118-137, 2 m 144-146, Marine 156-162, 70 cm 430-440, PMR446 446.0-446.2 MHz). It also sets the mode.
    - A band wider than the live view is shown whole: the SDR hops across it (not saved) in steps of 80 % of the rate; each step averages 3 FFTs and the clean middle is stitched in, with the RTL DC spike bridged.
    - One waterfall row per pass (FM at 3.2 MSPS = 9 steps, about 1 s). Audio is muted meanwhile.
    - Click or drag in the band view to listen and scrub:
      - The whole band stays on screen and the tuning line follows the mouse linearly across it, with audio.
      - The SDR retunes only when the line leaves the clean middle of its live slice (shown as a blue bar under the scale; line a quarter of the rate above the centre). That slice updates live.
      - The wheel steps the line. The position is saved on release.
    - Pick the band again to refresh it whole (hopping, audio muted).
    - Narrow bands (PMR446) just tune.
    - `--radio band=` does the same; `--radio sweep=<lo>-<hi>` shows any range (1-200 MHz).
- SDR rows under the frequency box (live, saved to the settings; the same values as Settings -> SDR):
  - **Gain** stepper: the tuner's own gain list; below the lowest = Auto; from Auto = ~30 dB.
  - **RTL AGC** on/off. Leave it off with a good antenna: it overloads easily (watch the Clip counters).
  - **Rate** dropdown (span): 0.25 / 1.024 / 1.2 / 1.536 / 2.0 / 2.048 / 2.4 / 2.56 / 2.88 / 3.2 MSPS (above 2.56 many RTL-SDRs drop samples; watch Missed). It restarts a running capture, because the rate is fixed per capture (stream timing, recording). Refused while recording; on Android it applies on the next connect.
- **Pan** (SDR, SDR++ style): scroll through the whole tuner range.
  - Drag with the right mouse button anywhere in the spectrum / waterfall, or with the left button on the frequency scale strip.
  - The SDR retunes live while dragging (not saved, <= ~30 / s); the final centre is saved on release.
  - The tuning line keeps its absolute frequency, and the waterfall history shifts with the band.
  - Mouse wheel (anywhere over the spectrum / waterfall) scrolls the band by a tenth of the view, up = higher (saved).
  - Shift + wheel steps the tuning line by the mode's step; Ctrl + wheel zooms.
- With an RTL-SDR the first capture sets CH1 to a single Radio panel (`--rtl-layout off` keeps the saved layout). CH2 then shows the Q half of the I/Q pair.
- Zoom button (1x-32x around the line) or Ctrl + wheel. Shift while clicking, dragging or scrolling tunes in 100 Hz steps.
- Launch flag `--radio <key=value,...>` covers every control:
  - Panel: `tune`, `input`, `range`, `zoom`.
  - Saved settings: `mode`, `bw`, `sql`, `vol`, `out`, `region`.
  - SDR (saved): `freq` (tuned MHz), `band`, `gain`, `agc`, `rate`.
  - Example: `--radio band=fm,freq=99.1,gain=20,agc=off,rate=1.2M`.

### Signal chain (all modes)

1. The tuned channel is mixed to 0 Hz (numerically controlled oscillator).
2. Integrate and dump by D = fs / (4 x BW).
3. 31-tap Hamming FIR at BW / 2, decimating by 2. The demod rate is about 2 x BW.
4. The mode's demodulator (below).
5. Squelch, volume, then libsoxr to 48 kHz.
   - Squelch gates on the channel power (about 20 ms average, 3 dB hysteresis, 5 ms fade).
   - SQL is the threshold in dBFS (-100..0).
   - The channel power shows under the tuning readout.

**BW Auto** picks the bandwidth per mode; the BW dropdown overrides it and really filters.

- Each mode lists its own widths:

  | Mode | BW choices |
  |---|---|
  | WFM | 500k, 400k, 300k, 250k, 200k, 150k (Auto 250k: a stereo + RDS station is ~256-270 kHz wide) |
  | NFM | 50k, 25k, 12.5k, 10k, 8.33k, 6.25k |
  | AM | 20k, 15k, 10k, 9k, 6k, 3k |
  | USB / LSB | 4k, 3.6k, 3k, 2.7k, 2.4k, 1.8k |

- Changing mode (dropdown, Band, `--radio mode=`) resets BW to Auto.
- The button shows what Auto resolves to, e.g. "BW Auto 200k".

### Modes

| Mode | Use | Demodulator | Auto BW | Snap | Verified |
|---|---|---|---|---|---|
| WFM | FM broadcast (88-108 MHz) | Frequency discriminator. 75 kHz deviation = 0.8 full scale. De-emphasis by Region | 250 kHz | 100 kHz | In the app (1 kHz test tone) and an offline model of the chain: level as expected, no measurable distortion |
| NFM | Two-way radio, PMR446, amateur FM repeaters | Same discriminator, deviation 0.4 x BW (5 kHz at 12.5 kHz), no de-emphasis | 12.5 kHz | 2.5 kHz | Not tested on a signal (same path as WFM apart from the gain) |
| AM | MW / LW / SW broadcast, airband voice (118-137 MHz) | Envelope, 30 Hz DC block, AGC (200 ms release) | 9 kHz (10 kHz Americas) | 9 kHz (10 kHz Americas) | In the app (400 Hz tone) and the offline model |
| USB | Amateur voice above 10 MHz, HF marine / aviation, digital modes | The sideband above the line, shifted to audio, AGC | 3 kHz | 100 Hz | Offline model: recovers the tone |
| LSB | Amateur voice below 10 MHz (80 m / 40 m) | Mirror of USB | 3 kHz | 100 Hz | Offline model: rejects the other sideband by about 54 dB |

### Region (Rgn button)

| Region | WFM de-emphasis | AM raster / Auto BW |
|---|---|---|
| EU/UK | 50 us | 9 kHz |
| Americas (also South Korea) | 75 us | 10 kHz |
| Japan | 50 us | 9 kHz |
| Auto (default) | follows the TV system setting: NTSC -> Americas, else EU/UK | |

The Region is saved in the settings (`demod_region`).

### Input (In button)

- **Auto** (default): I/Q on an SDR. Otherwise the panel's own channel as a real signal.
- **I/Q:** channel A = I, channel B = Q (an RTL-SDR, or an I/Q FLAC pair in the player).
- **Real:** the panel's own channel. The spectrum shows 0 to fs/2 in absolute Hz.

**Continuous feed.** The panel takes every capture block, not the display
frames, which are only the latest 64 k samples per drawn frame (about 10 %
of a 40 MSPS stream).

- Copies are made only while a Radio panel is open, and only for the channels it uses. The capture path never waits for the radio.
- A worker thread runs the chain.
- Without a feed (e.g. a remote source) the panel falls back to display frames.

### Real input above fs/2 (undersampling)

With an open input filter, the MISRC (40 MSPS real) also receives signals
above 20 MHz. They fold into 0-20 MHz.

- **Range** button: picks the 20 MHz RF zone the panel shows and tunes, in real RF frequencies.
  - Zone n covers (n-1) x fs/2 to n x fs/2.
  - Even zones are mirrored: the panel undoes it and swaps USB / LSB.
- `--radio tune=95.2M` picks the zone by itself: 80-100 MHz, where 95.2 MHz is sampled at 15.2 MHz.
- Everything in the same fold position is received at once. An analog band filter for the wanted range keeps other zones out.

### Not done / limits

- **WFM is mono.** No 19 kHz pilot / stereo decode, no RDS.
- **No CW mode.** Use USB / LSB tuned slightly off the carrier.
- **Real input:** reception above 20 MHz depends on the analog front end passing it (see undersampling above).
- **Level meters:** while the Radio plays, its audio level shows on the CH3/4 meters (as for Hi-Fi decoding).
- **Switching away:** a Radio panel switched to another view (or hidden) stops within 0.3 s: no audio, no DSP, and the band view returns the SDR to its saved frequency. Switching back resumes.
- **One audio output.** With two Radio panels open, the first one owns the continuous feed and the audio. The other runs from display frames.

## Monitor video codecs (MVD1)

Wire codec IDs (`net/gui_monitor_video_wire.h`): 1 = H.264 Annex B,
2 = HEVC Annex B, 3 = VP9, 4 = AV1. The Monitor & Remote client advertises a
decodable-codec bitmask on `GET /monitor/video?...&codecs=N`; the server picks
a **working** encoder (created and tested on the real frame size), preferring
hardware in the order AV1 > VP9 > HEVC > H.264, then realtime software
(VP9 > HEVC > AV1 > H.264). Clients that omit the mask get H.264 (backward
compatible). A codec change always restarts on a keyframe.

| Codec | Server encoding | Client decoding | State |
|---|---|---|---|
| H.264 | OpenH264 (always built) | OpenH264 | Working — compatibility default |
| AV1 | Linux/macOS: host FFmpeg (SVT-AV1/libaom) + hardware (NVENC/AMF/VideoToolbox/VAAPI/QSV) | GPU decoder when it opens, else FFmpeg software | Working where the FFmpeg build provides it; negotiated, never assumed |
| VP9 | Linux/macOS: host FFmpeg (libvpx) + hardware | as above | Working (same conditions) |
| HEVC | Linux/macOS: host FFmpeg (libx265) + hardware | as above | Working (license terms reviewed per platform) |

On Windows and Android, the pinned static minimal FFmpeg (n7.1.3) provides
**client decoding** for AV1/VP9/HEVC and also includes DCA for LaserDisc DTS.
Those servers still encode H.264 via OpenH264. Implementation history and boundaries:
[MONITOR_VIDEO_STAGE1.md](MONITOR_VIDEO_STAGE1.md) (wire framing + I420),
[MONITOR_VIDEO_STAGE2.md](MONITOR_VIDEO_STAGE2.md) (decoded-frame handoff),
[MONITOR_VIDEO_CODECS.md](MONITOR_VIDEO_CODECS.md) (negotiation + builds).

## Related (not decoder modes)

- **FLAC/RAW playback decode** — file containers feeding the display pipeline;
  playback normalizes every input convention into the 12-bit display
  container (in = out at 8/12/16-bit).
- **Network modes** (`net_mode`: Local/Server/Client/Monitor & Remote) —
  transport roles, documented in [MONITOR_MODES.md](MONITOR_MODES.md).
- **Test Gen** — signal *generator* (VHS/S-VHS/Betamax/Video8/Hi8 families),
  not a decoder.

## Keeping this page accurate

When a decoder mode is added, hidden, re-qualified or its lock/state surface
changes, update this page in the same change. Detail evidence (real-file
fixtures, counters, validation boundaries) stays in the per-feature dev notes
under `misrc_tools/misrc_gui/dev/`; link to them rather than duplicating
their measurements here.
