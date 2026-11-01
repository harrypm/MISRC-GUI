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

Android or Windows builds without FFmpeg development libraries retain H.264
and can still connect to a multi-codec server through capability negotiation.
They will not claim AV1/VP9/HEVC support unless built with the optional
libraries. The CPU budget and codec choice are separate from the upstream
CVBS/VHS decoder algorithms, which are not changed.

## Tests

```bash
sh scripts/test-monitor-video-stage1.sh
sh scripts/test-monitor-video-ffmpeg.sh
```

The second test compiles the **real** FFmpeg backend and performs VP9/HEVC/AV1
encoder/decoder smoke checks when those codec implementations are installed.
The GitHub Linux CI codec job installs FFmpeg development packages explicitly.
