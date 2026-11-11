# DEV NOTE (HARD): No UX/GUI layout changes without direct sign-off from harrypm (Harry Munday)

Status: incident-driven (2026-11-08). Read this before touching ANY user-facing
UI code in this repository — layout, sizing, spacing, fonts, wording, element
placement, interaction flow, colors, or control behavior.

## The incident (why this note exists)

Between v1.2.6 and v1.2.7 the per-channel recording stats panel (right
sidebar) was redesigned without the project owner's involvement:

- The compact colored `Dur: / RAW: / FLAC: / Ratio:` Space Mono rows were
  replaced with a Recording status dot + clock, label/value rows in a
  different font, a `Compression` readout, and a `Buffer OK / Details >`
  button.
- The result was visually noisier, and the larger Inter-drawn values
  rendered worse than the Space Mono readout the user approved.
- The whole change had to be reverted (restoring the v1.2.6 rows) and the
  sidebar width reservation re-aligned with the rendered font.

## HARD RULES

1. **No UX changes without talking to harrypm directly.** Any change that
   alters what the user SEES or CLICKS — adding, removing, moving, or
   resizing elements; changing fonts, colors, labels, or wording; changing
   interaction flow — requires direct, explicit approval from harrypm
   (Harry Munday) BEFORE it is written into a commit, PR, or push. Not
   optional, not default-assumed, not "easier to ask forgiveness".
2. This applies to human contributors, maintainers, and any AI/agent or
   LLM-assisted edits alike. An agent must never "improve" the UI on its
   own initiative.
3. **Restore-first:** when refactoring UI-adjacent logic (networking,
   recording, state), keep the existing visual layout byte-for-byte where
   possible. Logic changes and layout changes are separate changes; never
   bundle a layout redesign into a functional PR.
4. Functional additions may expose NEW controls only in the layout harrypm
   approves; propose first (screenshot/mockup or description), implement
   after sign-off.
5. Width/geometry reservations must match the rendered font and size
   (lock-step, see `gui_uniform_border_rules.md` for the underlying
   content-derived sizing rule).

## Scope

- `misrc_tools/misrc_gui/ui/**` (all Clay layouts, renderers, interactions)
- Toolbar, status bar, sidebars, settings panel, popups, dropdowns, icons
- Window titles, button labels, status messages the user reads
- Any asset that changes what the GUI displays

## Approval record

UX sign-offs are recorded in the prompt log for the session
(`prompt_logs/`, see `dev/README.md` naming rules) — state who approved
the change and what was approved.
