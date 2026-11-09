# Contributing to MISRC GUI

Thanks for contributing. This project captures capture hardware workflows in a
GUI that is used daily for real preservation work, so changes are gated by a
small set of **HARD rules**. Read this file before writing any code.

## HARD RULE 0 — No UX/GUI changes without direct sign-off from harrypm

**Any change that alters what the user sees or clicks requires direct, explicit
approval from harrypm (Harry Munday) before it is committed, opened as a PR, or
pushed.** This includes:

- Layout, element placement, sizing, spacing, padding
- Fonts, text sizes, colors, icons
- Labels, wording, status messages
- Adding or removing any control (buttons, dropdowns, readouts, panels)
- Interaction flow (what clicking/scrolling/dragging does)

This applies to human contributors, maintainers, and AI/LLM-assisted or
agent-driven edits alike. An agent must never "improve" or "clean up" the UI on
its own initiative. Logic refactors must not silently drag layout changes with
them — keep the rendered GUI unchanged unless the UX change was approved first.

Rationale and the incident behind this rule:
`misrc_tools/misrc_gui/dev/gui_ux_change_approval_rules.md`

To propose a UX change, contact harrypm directly with a description or
screenshot/mockup and wait for approval. Approved UX changes record who
approved them in the session's prompt log (see below).

## HARD RULE 1 — README.md is hands-off

The root `README.md` must not be edited — content, links, formatting, or
whitespace — unless harrypm (Harry Munday) directly instructs that specific
edit. This binds humans and AI/agent edits alike: agents auto-read
contributor docs, and the README is the owner's public voice, not ours. If a
change truly needs a README update, ask first and record the approval in the
session's prompt log. Details:
`misrc_tools/misrc_gui/dev/readme_hands_off_rules.md`

## HARD RULE 2 — All build dependencies must be self-contained

Every hard build dependency lives as a committed, pinned source tree under
`third_party/` (with a `VENDOR.md` provenance note); builds do not download
new dependency code, and adding/updating/re-pinning a dependency or editing
the `scripts/build-*` deps flow requires a reviewed, human-approved commit
from harrypm. Agents never download or sudo-install deps to unblock a build —
report and stop. Details:
`misrc_tools/misrc_gui/dev/self_contained_deps_rules.md`

## HARD RULE 3 — Documentation lives in `docs/`

Every user-facing sub-document lives in `docs/`. The repository root keeps
only the primary entry documents (`README.md`, `CONTRIBUTING.md`,
`INSTALLATION.md`, `TECHNICAL.md`, `UPDATE.md`, `LICENSE*`). Topic guides,
per-feature docs, integration guides and mode/state references go in `docs/`
in the same change that creates them, with a row added to `docs/README.md`.
A static CI guard (`check_docs_layout_contract`) rejects loose root `.md`
files outside the allowlist. Details:
`misrc_tools/misrc_gui/dev/docs_folder_rules.md`

## Other HARD rules (read before the relevant kind of change)

All live in `misrc_tools/misrc_gui/dev/` (index: `misrc_tools/misrc_gui/dev/README.md`):

- `gui_automation_flags_rules.md` — every user-interactable GUI element must
  ship with a CLI flag and/or `--config` key that can reach its state at
  launch, in the same change as the element.
- `gui_output_verification_rules.md` — headless verification must read what
  the GUI actually displays; render-path conversions are code under test.
- `gui_uniform_border_rules.md` — visible Clay containers derive their width
  from content, never fixed constants.
- `gui_signal_default_rules.md` — mode-scoped default configs; device defaults
  never leak across modes and user picks are never overwritten.
- `ci_guard_local_build_gate.md` — CI guard / local build gate procedure,
  including "a push is not done until its GitHub Actions run is green".

## Dev notes and prompt logs

All dev notes, prompt logs and lock-step tracking documents live in
`misrc_tools/misrc_gui/dev/` — never loose `.md` files in the repository root
(the root is for the primary entry documents only; user-facing sub-docs go in
`docs/`, per HARD RULE 3). Session prompt logs use
`prompt_<feature>_<readme|bug>.md` naming; see that folder's `README.md`.

When a change is confirmed working by the user, preserve a restore point
(zip of the tree plus a log note) alongside the code so the state can be
restored later.

## Building and testing

- Linux/macOS: `scripts/build-local.sh` (runs the static CI guards first, then
  meson configure + build + smoke test).
- Windows: `scripts/build-local.ps1`.
- Tests: `meson test -C build-local` (or `build-ci-check`).
- Static guards only: `python misrc_tools/test/ci_guard_tests.py --static-only`.

Run the guards and tests relevant to your change before pushing, and watch the
CI workflow triggered by your push until it is green (details in
`ci_guard_local_build_gate.md`).

## Cross-platform

The GUI targets Linux, Windows, macOS and Android. Treat cross-platform
implications as part of every change: use portable API spellings inside
platform `#if` blocks, and let CI be the compiler of record for platforms you
cannot build locally.

## Licensing

By contributing you agree your work is released under the repository's
licenses (GNU GPL v3+ for the project, with component licenses documented in
the `LICENSE*` files).
