# HARD rule: documentation lives in `docs/`

Established 2026-11-09 after five topic documents (MACOS-CXADC, MONITOR_MODES,
MONITOR_VIDEO_CODECS, MONITOR_VIDEO_STAGE1/2) had accumulated loose in the
repository root next to the primary entry documents.

## The rule

**Every user-facing sub-document lives in `docs/`.** The repository root keeps
only the primary entry documents that external sites and tooling expect at the
top level:

- `README.md` (itself hands-off — HARD RULE 1)
- `CONTRIBUTING.md`
- `INSTALLATION.md`
- `TECHNICAL.md`
- `UPDATE.md`
- `LICENSE*`

Any other documentation — topic guides, per-feature user docs, integration
guides, mode/state references (e.g. `docs/DECODER_MODES.md`) — goes in `docs/`
in the same change that creates it. `docs/README.md` indexes the folder; add
a row for every new document in the same change.

Where other material goes (unchanged, see this folder's `README.md`):

- Dev notes, prompt logs, lock-step tracking: `misrc_tools/misrc_gui/dev/`
  (in-session prompt logs may be drafted in `prompt_logs/` and are moved into
  the dev folder before committing).
- CI/build rules: `.github/CI_RULES.md` and the workflow files.

## Why it is a HARD rule

- The root is the project's front door; loose topic `.md` files bury the
  entry documents and grow unboundedly (five were found at establishment).
- Agents and contributors auto-read root docs; a predictable layout means the
  right context is found without guessing.
- GitHub and external tooling link `README`/`CONTRIBUTING`/`INSTALLATION`/
  `LICENSE` from the root specifically — those must not move.

## Enforcement

`check_docs_layout_contract` in `misrc_tools/test/ci_guard_tests.py` (run by
`python misrc_tools/test/ci_guard_tests.py --static-only`, the local build
gate and CI) fails the build when:

- any `.md` file appears loose in the repository root outside the allowlist
  above, or
- the `docs/` folder (with its `README.md` index) is missing.

Moving a document into `docs/` is a rename (`git mv`) — check inbound links
(`grep` for the old path) and update `docs/README.md` in the same change.
