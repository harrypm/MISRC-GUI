# Vendored: openh264

- Source: https://github.com/cisco/openh264
- Pin: tag `v2.6.0` (same pin as `android/build-openh264-android.sh`)
- Vendored: 2026-11-08 (harrypm-approved, HARD rule
  `misrc_tools/misrc_gui/dev/self_contained_deps_rules.md`)
- Why: hard GUI dependency (Monitor Client H.264 video; meson errors without
  it). Vendored in-repo so local/CI builds are self-contained and offline:
  no build-time downloads, no distro version drift.
- Contents: complete upstream source tree at the tag, `.git` stripped, and
  `res/` REMOVED (61 MB of upstream's Makefile-driven unit-test bitstreams;
  nothing under `meson.build` references it — the library build is
  unaffected. Restore it from upstream when running openh264's own tests).
- Used by: `dependency('openh264')` in `misrc_tools/meson.build` (Monitor
  Client video codec, with optional FFmpeg codecs layered on top).
- UPDATING (hard human intervention only): replace this tree with a new
  pinned release in a reviewed, human-approved commit. Update the pin here,
  `scripts/build-deps-unix.sh` (OPENH264_TAG), and
  `android/build-openh264-android.sh` in the same change.
