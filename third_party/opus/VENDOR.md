# Vendored: opus

- Source: https://github.com/xiph/opus
- Pin: tag `v1.5.2`
- Vendored: 2026-11-08 (harrypm-approved, HARD rule
  `misrc_tools/misrc_gui/dev/self_contained_deps_rules.md`)
- Why: hard GUI dependency (Monitor/Remote Audio Mon streams 48 kHz / 10 ms
  Opus mono). Vendored in-repo so local/CI builds are self-contained and
  offline: no build-time downloads, no distro version drift.
- Contents: complete upstream source tree at the tag, `.git` stripped.
- Used by: `misrc_tools/misrc_gui/net/gui_monitor_audio.c` (link via
  `dependency('opus')` in `misrc_tools/meson.build`).
- UPDATING (hard human intervention only): replace this tree with a new
  pinned release in a reviewed, human-approved commit. Update the pin here
  and in `scripts/build-deps-unix.sh` (OPUS_TAG) in the same change.
