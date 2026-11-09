# LaserDisc DTS preview development record

Base `main` commit `fca02dc`.

## Scope

- Add **Audio: DTS** beside **Audio: Digital PCM** in the existing LaserDisc
  audio selector for PAL and NTSC. The choice is panel-local and is not saved
  as a default. `--ld-dts` selects DTS in the `gui_rf_preview_file` test tool.
- Share the 20 MSPS EFM/CIRC path with Digital PCM. Decode the 44.1 kHz DTS
  core to 48 kHz stereo and mute DTS payloads in Digital PCM mode.
- Enable FFmpeg's native `dca` decoder in the existing Windows/Android static
  build at `n7.1.3` and invalidate the previous build cache marker. DTS uses
  the existing optional FFmpeg libraries.

## Verification

- **Windows x64 build.** The single EXE passed `--smoke-test`, PE import and
  static-link checks for DCA and RTL-SDR, and the build guards. The
  `gui_rf_routes` test checks the rendered label and both menu cycles with
  simulated DCA availability and worker routing. The LD worker, PAL/NTSC
  synthetic RF video and AC-3 frame regressions also passed.
- **Synthetic DTS.** A standalone test linked the same Windows FFmpeg build
  and decoded 14-bit and 16-bit streams in both byte orders. A separate
  integration test injected synthetic EFM run lengths after the RF frontend
  and checked CIRC-to-DTS decoding, dropped-frame recovery, timestamps and
  EOF at the 48 kHz stereo output.
- **Real capture regression.** A real NTSC PCM capture replayed through
  40-to-20 MSPS conversion and EFM/CIRC. All 142,608 output frames and their
  timestamps matched `main`.
- **Still unverified.** The synthetic DTS RF waveform did not lock at the EFM
  frontend. End-to-end realtime playback of real PAL or NTSC DTS discs and
  live RTL-SDR capture were not tested.

## Pending maintainer approval

- **GUI** (HARD RULE 0). Harry Munday (`harrypm`) needs to approve the
  **Audio: DTS** label and its place in the PAL and NTSC selector cycles.
- **Build** (HARD RULE 2). Harry needs to review and approve enabling DCA
  and changing the cache marker in `scripts/build-monitor-ffmpeg-minimal.sh`.

Both approvals are pending and are requested in the PR. Record Harry's
decision and its scope here after review. See the
[contribution rules](../../../CONTRIBUTING.md).
