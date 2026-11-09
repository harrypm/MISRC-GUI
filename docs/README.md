# MISRC-GUI documentation

User-facing sub-documents live in this folder (`docs/`). The repository root
keeps only the primary entry documents (`README.md`, `CONTRIBUTING.md`,
`INSTALLATION.md`, `TECHNICAL.md`, `UPDATE.md`, `LICENSE*`); developer notes
and prompt logs live in `misrc_tools/misrc_gui/dev/`. See
`misrc_tools/misrc_gui/dev/docs_folder_rules.md` for the HARD rule that
enforces this layout.

## Contents

| Document | Description |
|---|---|
| [DECODER_MODES.md](DECODER_MODES.md) | Every decoder mode (Video panel decoders, RF Preview tape profiles, CVBS modes, Demod modes, monitor video codecs) and the state of each |
| [MONITOR_MODES.md](MONITOR_MODES.md) | Unified Server / Monitor & Remote network modes, endpoints and Local Save |
| [MONITOR_VIDEO_CODECS.md](MONITOR_VIDEO_CODECS.md) | Monitor video codec negotiation (H.264/AV1/VP9/HEVC) and per-platform build requirements |
| [MONITOR_VIDEO_STAGE1.md](MONITOR_VIDEO_STAGE1.md) | Monitor video stage 1: MVD1 wire framing and RGB-to-I420 conversion |
| [MONITOR_VIDEO_STAGE2.md](MONITOR_VIDEO_STAGE2.md) | Monitor video stage 2: decoded-frame handoff (framebox + sinks) |
| [MACOS-CXADC.md](MACOS-CXADC.md) | macOS integration guide for CXADC capture |
