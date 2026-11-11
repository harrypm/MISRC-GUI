# Native SECAM VHS RF preview

Request: implement real-time SECAM VHS RF preview on a new branch, using the
provided 10-second singing capture. PAL RF preview showed only luma.

Branch: `codex/secam-vhs-preview`, based on upstream main `72b33bd`.
The original Betamax development checkout was preserved.

Implemented native /4 FM chroma, causal LF inverse, 625/50 field weaving,
validated luma calibration and VBI bottle identification. Active picture
color cannot vote on Dr/Db identity. The existing RF System control now
cycles PAL/NTSC/SECAM; Beta and CVBS retain their separate paths.

The GUI output chain is decoder bytewise RGBA -> bounded worker RGBA copy
-> `gui_vhs_fm_swap_buffers` front RGBA -> RGBA8 `LoadTextureFromImage` or
`UpdateTexture` -> `DrawTexturePro`. The file probe reads the same final
worker RGBA consumed by the GUI. The physical RF fixture asserts distinct
RGB channels, line registration, tone response and both original fields;
the native GUI was also exercised on local playback. No RGB packing or
texture-format conversion was introduced.

The full sample produced 499 accepted fields / 249 frames, with zero field
rejects, queue drops or color-loss events. Maximum-speed processing was
about 7.79 seconds; source-paced playback was 24.989 fps at a 1.5-core
budget. These are local-file measurements, not hardware/recording proof.

Independent physical fixtures cover VBI bottles, cold and sustained solid
magenta/red/blue, reference-free gray fallback, weak chroma, clock/gain,
discontinuity, Y/C edges, 85 kHz color modulation and Full-frame geometry.
The pre-existing Windows `update_cli` failure creating its initial updates
directory was reproduced on unmodified main and remains outside this work.

Native H-identification-only recordings without usable VBI bottles remain
grayscale. MESECAM, LP/EP, dropout repair and offline TBC equivalence are
not implemented. No physical SECAM capture device was exercised.

Signal parameters, sample identity and verification boundaries are recorded
in `misrc_tools/misrc_gui/dev/secam_vhs_preview_README.md`.
