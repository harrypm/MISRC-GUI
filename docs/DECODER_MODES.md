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
| `VIDEO_PLUGIN_VHS_FM` | RF Preview | Tape video RF (9 profiles, below) | Working — in the cycle |
| `VIDEO_PLUGIN_TAPE_DECODE` | Tape Decode | Baseband composite with tape-tuned PLL/smoothing | **Hidden from the UI** (`VIDEO_TAPE_DECODE_UI_ENABLED 0`) — code stays compiled, unreachable until it works; flip the define to re-expose |

## RF Preview tape profiles

`gui_rf_profile_t` in `signal/gui_rf_preview.h`. All are **reduced-bandwidth
streaming previews**, not full-TBC archival decoders: nonlinear
sub-de-emphasis, RF equalization, dropout repair and comb filtering are
deliberately omitted. All weave the two original fields (no deinterlacing).

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

Notes:

- **Hi-Fi audio** (`gui_rf_profile_has_hifi`): profiles 3, 5 and 8 run the
  separate Beta Hi-Fi audio decoder (four carriers at 87.75/97.25/106.75/116.25
  × fH, ~300 ms calibration, 48 kHz stereo, Audio Mon gated). The GUI's
  SuperBeta preset uses the Hi-Fi-capable worker so Audio Mon works without
  changing the video profile.
- **Input rates**: 20/40/80 MHz native conversion; other 16–160 MHz rates via
  libsoxr. ED Beta accepts 20–200 MHz through a dedicated wide-passband
  converter.
- **Not implemented**: VHS LP/EP, S-VHS, PAL Betamax, linear (non-Hi-Fi) tape
  audio, dropout repair, offline TBC/export quality.

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

## Demod panel modes

`demod_mode_t` in `signal/gui_demod.h` — device-agnostic I/Q audio demod
(any I/Q source: RTL-SDR live capture or I/Q FLAC playback):

| Mode | Processing | State |
|---|---|---|
| WFM | Discriminator + 75 µs de-emphasis (broadcast FM) | Working |
| NFM | Discriminator, no de-emphasis | Working |
| AM | Magnitude + DC block + AGC | Working |
| USB | Upper sideband | Working |
| LSB | Lower sideband | Working |

Output is resampled to 48 kHz (libsoxr) and played through the Audio Mon
path. The panel is always available (`gui_demod_available` — self-contained
DSP). Bandwidth/squelch/volume are settings, not lock states.

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

Windows/Android: the pinned static minimal FFmpeg (n7.1.3, decode-only AV1/
VP9/HEVC, no extra DLLs) provides **client decoding**; those servers still
encode H.264 via OpenH264. Implementation history and boundaries:
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
