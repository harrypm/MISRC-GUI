# Prompt log — 2026-11-08: restore v1.2.6 per-channel stats panel + HARD UX sign-off rule

## User request (verbatim intent)

> git pull and restore old GUI layout.. add a hard dev guideline no UX changes
> without directly taking to harrypm / Harry Munday
>
> attached is what I am talking about in GUI mode it was fine and clean now its
> been messed with.. text could be a little bigger but needs proper aliasing

Two screenshots were attached: v1.2.6 (approved clean layout: `Dur:/RAW:/FLAC:/
Ratio:` colored Space Mono rows + RST) vs v1.2.7 (cluttered: `● Recording`
dot row, label/value rows, `Compression` readout, `Buffer OK / Details >`
button, Inter-font values that rendered worse).

Follow-up: "make a contributing md which references the rules".

## Inputs

- Working repo: `/home/harry/MISRC-GUI` (main)
- Screenshots: v1.2.6 approved layout vs v1.2.7 rejected layout (per-channel
  stats panel, right sidebar)

## Commands run

- `git fetch origin` / `git --no-pager log --oneline -15` / `git status --short --branch`
  — found main was 123 commits behind origin/main (6caded7..d62faaa).
- `git pull --ff-only origin main` — fast-forwarded to d62faaa
  ("Rename UPDATE to UPDATE.md"); 128 files changed.
- `git --no-pager log/diff v1.2.6..HEAD -- misrc_tools/misrc_gui/ui/gui_ui.c`
  — located the layout change; extracted the v1.2.6 file to
  `/tmp/gui_ui_v126.c` for exact comparison.
- `ninja -C build-local misrc_gui` — meson regenerate FAILED: the pulled
  commits made `opus` (and `openh264`) HARD deps of the GUI
  (`misrc_tools/meson.build:560`, `:585`). Only runtime `libopus0` is
  installed; `libopus-dev`/`libopenh264-dev` are missing and installing them
  needs sudo (password).
- Syntax check of the edited `gui_ui.c` via the existing
  `compile_commands.json` flags (`gcc -fsyntax-only`) — EXIT 0 (only the
  pre-existing `threading.h` cast-function-type warning).

## Changes made

1. `misrc_tools/misrc_gui/ui/gui_ui.c` — restored the v1.2.6 per-channel
   recording stats rows:
   - Back to full-text colored Space Mono rows (`Dur: HH:MM:SS`, `RAW: ...`,
     `FLAC: ...`, `Ratio: x.x`) using `channel_value_color`, fontId 1.
   - Removed the `● Recording/Last rec` dot row, the `RAW:`/`FLAC:` label
     rows, the `Compression` readout row, and the `Buffer OK / Details >`
     button.
   - Kept the M-Client (remote) stats value computation so monitor-client
     mode still shows the server-side recording numbers.
   - Removed the now-dead `gui_record_details_view_t` /
     `s_record_details_view` machinery and its click handler popup.
   - `FONT_SIZE_RECORD_READOUT` 16 -> 18 ("a little bigger"): these rows use
     Space Mono (fontId 1) drawn from the same bilinear-filtered 64px atlas
     (both embedded fonts set `TEXTURE_FILTER_BILINEAR` at load), so the
     larger size stays antialiased.
   - `gui_ui_stats_layout` again reserves the RAW-readout width with
     fontId 1 (Space Mono) as v1.2.6 did (the v1.2.7 code had drifted to
     fontId 0), now at the new 18px readout size.
2. `misrc_tools/misrc_gui/dev/gui_ux_change_approval_rules.md` — NEW HARD
   rule doc: no UX/GUI changes of any kind without direct sign-off from
   harrypm (Harry Munday); applies to humans and AI/agent edits; restore-first
   for UI-adjacent refactors; approval recorded in the session prompt log.
3. `misrc_tools/misrc_gui/dev/README.md` — registered the new HARD rule doc
   in the contents table.
4. `CONTRIBUTING.md` — NEW: HARD RULE 0 (UX sign-off from harrypm) up top,
   references to all other HARD rule docs in `misrc_tools/misrc_gui/dev/`,
   dev-notes/prompt-log conventions, restore-point convention, build/test
   commands, cross-platform expectations, licensing note.
5. `README.md` — added a short `## Contributing` section pointing at
   `CONTRIBUTING.md` and the HARD UX rule.

## Verification status

- `gui_ui.c` syntax check: EXIT 0 with existing flags.
- FULL BUILD BLOCKED (pre-existing, not caused by this change): the pulled
  main now hard-requires `libopus-dev` and `libopenh264-dev` on Linux
  (`sudo apt install libopus-dev libopenh264-dev`), then
  `ninja -C build-local misrc_gui`.
- PENDING real-world confirmation (user rule: GUI changes need confirmation):
  user to run the GUI and confirm the stats panel matches the v1.2.6
  screenshot with slightly larger text.

## Round 2 (2026-11-08 ~04:30): restore point + 24px + README hands-off

- User confirmed the restored layout is fixed, and set the readout size:
  "24 pix".
- `FONT_SIZE_RECORD_READOUT` 18 -> 24 (user-fixed value, harrypm
  2026-11-08). Syntax check EXIT 0. Full build still blocked on the missing
  system packages (`libopus-dev`, `libopenh264-dev`) — needs sudo.
- RESTORE POINT: the user-confirmed good state (v1.2.6 stats rows, 18px) was
  zipped per the restore-point rule to
  `restore_points/2026-11-08_v126-stats-panel-restore-good.zip` (578 MB,
  tree minus .git/build dirs/.deps/restore_points).
- NEW HARD RULE — README hands-off: user mandated the root `README.md` is
  never edited (content/links/formatting/whitespace) without a direct
  instruction from harrypm, because AI agents auto-read contributor docs and
  then alter user-facing docs. Doc:
  `misrc_tools/misrc_gui/dev/readme_hands_off_rules.md`; registered in
  `dev/README.md` and stated in `CONTRIBUTING.md` (the doc clankers
  auto-read).
- README.md reverted: the Contributing section added earlier this session
  was removed, restoring README to its upstream state. Contributing.md and
  the dev/ rules remain the single entry points.

## Round 3 (2026-11-08 ~04:41): readout at 48px

- User: "Dur / raw / flac / ratio make it 3x bigger — the text". Clarified
  baseline: 3x the ORIGINAL v1.2.6 16px readout = 48px (not 3x the interim
  24px). `FONT_SIZE_RECORD_READOUT` 24 -> 48. Syntax check EXIT 0.
- Note: at 48px Space Mono the sidebar reserves ~690px for the max RAW
  readout (the width reservation measures the same define/font); the stats
  panel remains vertically scrollable. Full build still gated on
  `libopus-dev`/`libopenh264-dev` (sudo).

## Round 4 (2026-11-08 ~04:46): self-contained deps + local build OK

- User: "update anything internally that needs updating to ensure local dev
  building pulls all requirements" then "vendor any dependencies required
  ... building should be self contained from the repo as possible" then
  "I don't want things changing without hard human intervention" and
  "make that a dev rule 'all deps must be self-contained'".
- Clarified via questions: commit pinned sources IN-REPO (like
  `third_party/hsdaoh`), keep the harmless preflight prep edits.
- Vendored into `third_party/` (pinned tags, .git stripped, provenance in
  each `VENDOR.md`): `opus` v1.5.2 (17 MB) and `openh264` v2.6.0 (12 MB,
  upstream `res/` 61 MB test-bitstreams excluded — not referenced by the
  meson build).
- `scripts/build-deps-unix.sh` now builds opus (CMake, static) and openh264
  (meson, static) from those committed trees into `.deps/install` — no
  build-time downloads. `check-build-deps.sh` gained a `--system` mode
  (distro-set only) used inside the deps build; the final `--gui` preflight
  covers everything.
- `scripts/build-local.sh`: the early `--codec` gate now applies only to a
  caller-provided `MISRC_DEPS_PREFIX`; a located prefix missing the codecs
  is topped up in place (offline) instead of failing.
- `misrc_tools/meson.build`: when the linked openh264 is the STATIC vendored
  archive (detected via `libopenh264.a` in the .pc libdir), the C++ runtime
  (libstdc++ / libc++) is linked after it — fixes `undefined reference to
  __vmi_class_type_info` at link; shared distro openh264 builds (CI
  libopenh264-dev) stay C-only, so CI linkage is unchanged.
- NEW HARD RULE: `misrc_tools/misrc_gui/dev/self_contained_deps_rules.md`
  — all build deps self-contained from committed pinned third_party trees;
  no build-time downloads; dep changes only via reviewed human-approved
  commits; agents never download/sudo-install deps to unblock a build.
  Registered in `dev/README.md` + `CONTRIBUTING.md` (HARD RULE 2).
- RESULT: `scripts/build-local.sh` green — 45/45 static guards PASS, deps
  built from vendored sources, misrc_gui compiled + linked, smoke test
  PASS (`build-local/misrc_gui`). Note: meson still warns it falls back to
  SYSTEM hsdaoh 0.1.0 (the pre-existing fallback used by this machine's
  builds all along); vendored-hsdaoh adoption is unchanged existing flow.
- PENDING: user to run `build-local/misrc_gui` and confirm the 48px
  Dur/RAW/FLAC/Ratio readout (rule: real-world confirmation for UI).

## Round 5 (2026-11-08 ~04:54): readout dialed to 24px

- User: 48px was far too large — "dial that back down to 1.5x on the text
  size from original". `FONT_SIZE_RECORD_READOUT` 48 -> 24 (1.5x the v1.2.6
  16px baseline; same value as the earlier interim step, now as the settled
  choice). Sidebar reservation scales with the define (~345px max RAW
  readout).
- Rebuild: `scripts/build-local.sh` green (guards + compile + link + smoke
  test). PENDING user visual confirmation in the running GUI.

## Round 6 (2026-11-08 ~04:57): 21px + no dead space — settled

- User: "1.3x should keep the size of everything else" →
  `FONT_SIZE_RECORD_READOUT` 24 -> 21 (1.3x the v1.2.6 16px baseline).
- User: "you dont need to push the dead space" → `gui_ui_stats_layout` no
  longer reserves the UINT64_MAX sample ("RAW: 184467440737.10 GB"); it now
  measures a static realistic bound "FLAC: 99999.99 GB" (~100 TB, the
  longest restored row incl. label) — still static, so live byte counts
  never reflow the sidebar; only beyond ~100 TB per channel could a readout
  overflow (unreachable in practice).
- Rebuild: `scripts/build-local.sh` green (guards + compile + link + smoke
  test). Remaining meson warnings are pre-existing upstream ones (system
  hsdaoh fallback; win_subsystem 0.56 feature vs >=0.55 minimum).
- PENDING: user visual confirmation of the settled 21px look.

## Round 7 (2026-11-08 ~04:57): readout settled at 18px (1.1x)

- User: "1.1x" → `FONT_SIZE_RECORD_READOUT` 21 -> 18 (1.1x the v1.2.6 16px
  baseline). Reservation sample unchanged (realistic ~100 TB bound).
- Rebuild: `scripts/build-local.sh` green (guards + compile + link + smoke
  test). PENDING user visual confirmation.

## Round 8 (2026-11-08 ~05:08): commit + push + v1.2.7 release replaced

- User: "commit, push, nuke the v1.2.7 release, but copy my description over
  dont nuke that" / "I want to replace it".
- Saved the release body (2958 B) to /tmp/v127-release-body.md before
  deletion; `restore_points/` added to .gitignore (578 MB zip must never be
  committed).
- Commit 52317c5 (UI restore + HARD rules + vendored deps, 1396 files).
  First push FAILED CI: the 'shell script Git executable modes' guard
  rejected 14 vendored .sh files at 100644. Fixed with git update-index
  --chmod=+x on all tracked .sh (commit b5a3cf5). Re-pushed: run
  42449788253 GREEN (per ci_guard rule 6 the push was not 'done' until
  verified).
- Deleted release v1.2.7 + tag (--cleanup-tag), re-tagged b5a3cf5 as
  v1.2.7, pushed: release run 42450386059 GREEN, published fresh assets
  (6 platform builds built from the new code).
- Restored the user's original description via `gh release edit
  --notes-file` + title 'MISRC GUI v1.2.7'.
- NOTE (inherent to same-tag replacement): clients already running v1.2.7
  see the same version string, so the auto-updater will not offer them the
  rebuilt assets; only fresh downloads get the new build.

## HARD rules now in force

- `misrc_tools/misrc_gui/dev/gui_ux_change_approval_rules.md` — no UX
  changes without talking to harrypm directly.
- `misrc_tools/misrc_gui/dev/readme_hands_off_rules.md` — README.md is
  hands-off without a direct harrypm instruction.
