# Prompt Log — 2026-11-09: docs/ HARD rule + decoder modes doc

Repo: `/home/harry/MISRC-GUI` (branch `main`, up-to-date with `origin/main`, HEAD `b5a3cf5`, tag v1.2.7)

## User request (input)

1. Make a **hard project rule** that docs/sub-docs go into the `docs/` folder.
2. Make a doc under `docs/` listing **all the decoder modes and states** of them.

## Research performed (commands + findings)

- `git status` / `git log --oneline -5` — clean tree on `main` (only pre-existing third_party
  shell-script mode tweaks + prior prompt log edit, untouched by this session).
- `ls` repo root — no `docs/` folder existed. Loose root sub-docs found:
  `MACOS-CXADC.md`, `MONITOR_MODES.md`, `MONITOR_VIDEO_CODECS.md`,
  `MONITOR_VIDEO_STAGE1.md`, `MONITOR_VIDEO_STAGE2.md`.
- Read `CONTRIBUTING.md` — HARD RULES 0–2 live here; detail docs live in
  `misrc_tools/misrc_gui/dev/`; that folder's `README.md` is the index.
- Grep for inbound references to the five loose docs (outside themselves/build dirs):
  **none found** — safe to move with no link breakage.
- Read `misrc_tools/misrc_gui/dev/README.md` — rule-doc naming and index conventions.
- Read `misrc_tools/test/ci_guard_tests.py` — guard test registration pattern
  (`checks: List[Tuple[str, Callable]]` in `main()`); no existing doc-layout guard.
- Decoder enumeration (verified from source, not assumed):
  - `signal/gui_cvbs.c` (`video_plugin_kind_t`, `VIDEO_TAPE_DECODE_UI_ENABLED 0`),
    `signal/gui_cvbs.h` (formats/modes enums, SECAM state), `signal/gui_cvbs.c`
    mode-name helpers.
  - `signal/gui_rf_preview.h` + `.c` — 9 RF profiles (`gui_rf_profile_t`),
    `gui_rf_decode_status_t` lock/reject state.
  - `signal/gui_demod.h` — WFM/NFM/AM/USB/LSB.
  - `net/gui_monitor_video_wire.h` — MVD1 codec IDs 1–4 (H.264/HEVC/VP9/AV1),
    selection preference order.
  - Dev notes for per-profile validation states: `rf_preview_README.md`,
    `betamax_preview_README.md`, `superbeta_preview_README.md`,
    `ed_beta_preview_README.md`, `secam_vhs_preview_README.md`,
    `mesecam_vhs_preview_README.md`.
- FUTO Notes AI Notebook confirmed at `/home/harry/Documents/futo-notes/AI Notebook/`
  (existing prompt-log copies present; no best-practices.md found on this system).

## Changes made (output)

1. `docs/` folder created; five loose root sub-docs moved into it via `git mv`
   (history preserved).
2. New HARD rule doc: `misrc_tools/misrc_gui/dev/docs_folder_rules.md`.
3. `CONTRIBUTING.md` — added "HARD RULE 3 — Documentation lives in `docs/`"
   and updated the dev-notes section wording.
4. `misrc_tools/misrc_gui/dev/README.md` — index updated with the new rule doc.
5. New `docs/DECODER_MODES.md` — all decoder modes + implementation/runtime states.
6. New `docs/README.md` — docs folder index.
7. `misrc_tools/test/ci_guard_tests.py` — new static guard
   `check_docs_layout_contract` enforcing the root-.md allowlist and `docs/`
   presence; registered in `main()`.
8. Prompt log copied to FUTO Notes AI Notebook.

## Verification

- `git status --short` — the five moves are staged renames
  (`R  <old> -> docs/<new>`); no other tree changes from this session
  (pre-existing third_party script-mode + prior prompt-log edits left untouched).
- `python3 misrc_tools/test/ci_guard_tests.py --static-only` — **46/46 PASS**,
  including the new `docs folder layout contract` guard.
- Negative test: with a temporary `TEMP_NEGATIVE_TEST_DOC.md` in the root, the
  guard fails (`rc: 1`) naming the file and the rule doc; temp file removed.
- No inbound references to the moved docs existed (grep before the move), so
  no links broke; `docs/README.md` now indexes all six docs.
- `README.md` was NOT touched (HARD RULE 1, hands-off).
- Decoder doc content was cross-checked against the source enums/headers
  listed above rather than assumed; per-profile validation states cite the
  per-feature dev notes.

## Not done / boundaries

- No commit or push (not requested). Restore point/zip not created (no
  "fully working/fixed" user confirmation yet — documentation-only change).
- Real-world GUI confirmation not applicable (no user-interactable element
  changed; docs + guard only).
