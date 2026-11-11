# Monitor Video codec selection (feature/monitor-h264-video-20261101)

The Monitor Client sends the bitmask of codecs it can **decode** in
`GET /monitor/video?...&codecs=N`. The headless server chooses a matching
**working** encoder (not merely a compiled-in name); the MVD1 video frame header
identifies the actual codec for each frame. A different codec always begins
with a fresh decoder and a keyframe. Older clients which omit `codecs` receive
H.264, preserving backward compatibility. H.264 is always built via OpenH264.

The server first probes available hardware encoders in codec order
**AV1 > VP9 > HEVC > H.264**, then software backends in the same order.
It tests encoder creation on the actual stream frame size, and falls back
when a device cannot be opened or encoding fails. No hardware-capability
flag is trusted without creating an encoder. The client decodes using
the available GPU decoder when it opens successfully, otherwise FFmpeg
software decoding. If only OpenH264 is installed the existing stream works
unchanged.

## Build and runtime requirements

For Linux/Debian/Ubuntu, install the optional modern codec development
libraries **before configuring the build**:

```bash
sudo apt install libavcodec-dev libavutil-dev libswscale-dev
pkg-config --modversion libavcodec libavutil libswscale
./scripts/build-local.sh
```

FFmpeg must be built with the relevant AV1 (SVT-AV1/libaom), VP9 (libvpx)
and/or HEVC (libx265) encoders. These are optional and determined at runtime;
not every FFmpeg distribution includes all of them. Supported hardware
encoders include NVENC, AMD AMF, macOS VideoToolbox, VAAPI and Intel QSV,
subject to platform drivers, encoder support and physical GPU availability.
FFmpeg licensing and HEVC patent/license terms depend on the chosen
packages/platform. Do not distribute additional codec binaries without
reviewing their license and redistribution requirements.

Android builds and the existing Windows single-EXE builds negotiate H.264
unless a separately packaged modern-codec backend is provided. The MSYS2
all-codec static FFmpeg distribution does not link as a self-contained EXE
because it pulls in unrelated transitive modules (e.g. JPEG XL, librsvg,
Rust runtime object collisions). Do not silently ship non-system runtime
DLLs to work around the release's one-EXE constraint. Linux builds with
the FFmpeg development libraries enabled negotiate AV1/VP9/HEVC. The
CPU budget and codec selection are independent of upstream CVBS/VHS DSP.

## Tests

```bash
sh scripts/test-monitor-video-stage1.sh
sh scripts/test-monitor-video-ffmpeg.sh
```

The second test compiles the **real** FFmpeg backend and performs VP9/HEVC/AV1
encoder/decoder smoke checks when those codec implementations are installed.
The GitHub Linux CI codec job installs FFmpeg development packages explicitly.

## Minimal Windows and Android FFmpeg decoders

`scripts/build-monitor-ffmpeg-minimal.sh windows|android` cross-builds pinned
FFmpeg n7.1.3 as static libavcodec/libavutil/libswscale. Its native AV1, VP9
and HEVC decoders serve Monitor Video. The same build enables FFmpeg's native
DCA decoder for [LaserDisc DTS](DECODER_MODES.md#laserdisc-audio-and-digital-chains).
Windows uses MSYS2 UCRT64/CLANGARM64. Android uses the
NDK arm64 API 30. Meson `-Dmodern_monitor_codecs=enabled` fails early when
these libraries are not linked. No additional runtime DLLs are required.

LaserDisc DTS is optional and uses FFmpeg 5.1 or newer `libavcodec` and
`libavutil` with DCA enabled. It works independently of `libswscale` and
the `modern_monitor_codecs` option.

This is full **client decoding** support but not software AV1/VP9/HEVC
**encoding** on Windows/Android servers: the lean FFmpeg intentionally omits
libaom/libvpx/libx265; these servers still encode H.264 via OpenH264.
Linux/macOS servers retain native host FFmpeg software/hardware encoders.
