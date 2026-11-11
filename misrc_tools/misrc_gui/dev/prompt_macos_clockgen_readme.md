# macOS CXADC ClockGen integration session — 2026-11-11

The user requested a local, usable two-CX-card ClockGen capture implementation
with USB linear audio, preserving independent cxadc-macos use and minimizing
upstream GUI changes. No computer-use tooling, publishing, pushing or PR
submission was authorized for this session. Tests use the application's own
smoke/auto-record interface and real connected hardware. No synthetic samples
are represented as hardware evidence.

Baseline: upstream 526e59f, merged into a dedicated local branch while retaining
the previously tested card-reconnect fix. Baseline build/smoke passed. No root
README or dependency tree/build-dependency scripts were edited. Existing
ClockGen device selection, F8 controls and recording layouts are reused;
functional macOS backend/rate/status changes follow the user's explicit task.
No harrypm UX sign-off is asserted; upstream changes remain for review.

Code details, observed tests and limitations are recorded in docs/MACOS-CXADC.md
and the separate cxadc-macos TESTING.md. Headers originate in cxadc-macos;
GNU GPL-2.0-or-later is compatible with this project's GPL-3.0-or-later.
Private media and user configs are not committed. The DriverKit extension,
PCI/DMA register logic, Windows and Linux hardware configuration are unchanged.
