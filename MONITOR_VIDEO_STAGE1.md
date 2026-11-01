# Monitor Client H.264 video stream — stage 1

Base revision targeted: `103efbb934766ac88714c89901d3cd5e9226a414`.
Proposed branch: `feature/monitor-h264-video-20261101`.

## Status

This change only adds the independently testable **transport framing and colour-conversion foundation**. It does **not** enable video output in the current MISRC Monitor Client, build OpenH264, introduce `/monitor/video`, or connect the headless server decoders. No production call sites are modified and existing RF/recording behavior is unaffected.

This is deliberately staged because production integration needs review against the full application and platform build targets. Do not treat this as the completed video feature.

## Wire protocol v1

A persistent `GET /monitor/video?...` response (to be added in a follow-up) will contain repeated `MVD1` frames, with a 40-byte header followed by an H.264 Annex B access unit. The existing `MRD5` waveform/FFT protocol remains unchanged.

| Offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | ASCII `MVD1` |
| 4 | 1 | Version = 1 |
| 5 | 1 | Header length = 40 |
| 6 | 1 | Codec = 1 (H.264 Annex B) |
| 7 | 1 | Bit 0 = IDR/keyframe; bit 1 = codec config present |
| 8 | 4 | Stream ID (big endian) |
| 12 | 4 | Access-unit sequence (big endian, wraps) |
| 16 | 8 | Presentation time in microseconds (big endian) |
| 24 | 2 | Width (even, big endian) |
| 26 | 2 | Height (even, big endian) |
| 28 | 2 | FPS × 100 (big endian) |
| 30 | 2 | Reserved, zero |
| 32 | 4 | Compressed access-unit length (big endian) |
| 36 | 4 | Reserved, zero |

Bounded payload: 4 bytes to 2 MiB. Dimensions: even and ≤ 1920×1080. FPS: 1–60.
Clients must reject unknown versions, unsupported codec or invalid sizes. SPS/PPS shall be transmitted with each IDR; clients must not render P-frames before a valid IDR after connecting or resetting the decoder. All streams are bounded, with the newest-frame-only policy in the application layer.

## Pixel conversion

`mvd_rgb_to_i420()` accepts packed RGB24 and RGBA32 CPU frames (alpha ignored), optionally downsizes via nearest-neighbour sampling, and converts to planar 8-bit YUV420 (limited-range BT.601). Target dimensions must be even and no larger than the source. This is a simple portable baseline, not a final high-quality scaling implementation.

## Required integration work, not yet included

1. Separate headless server-side video decode from UI panels and avoid any blocking work on the capture/recording threads.
2. Add OpenH264 as an optional, license-reviewed build dependency for all supported targets. Convert decoder output through a bounded worker and rate-control encoder (no B frames, regular IDR, SPS/PPS on IDR).
3. Implement `/monitor/video` as a persistent socket on the existing monitor listener; negotiate channel, decoder type, resolution, target bitrate, and FPS with server-side validation. Do not expose monitor HTTP unauthenticated to the internet.
4. Add client network receive, bounded frame queue, H.264 decoder and raylib texture upload; permit Video panel in Monitor Client, preserving client reconnect and stream resets.
5. Add resource caps, multi-client encoder-sharing policy, stats and long-running Server + Monitor Client + recording tests, Android 11/12/14/16 builds.

## Standalone regression test

From the repository root on Linux/macOS/MSYS2:

```sh
sh scripts/test-monitor-video-stage1.sh
```

The test does not require raylib, hardware, OpenH264, or the full application build. It covers big-endian framing, malformed header rejection, TCP one-byte fragmentation, back-to-back packets, capacity limits, RGB conversion, downsampling, RGBA alpha handling and source bounds.
