# DEV NOTE (HARD): README.md is hands-off — no edits without a direct instruction from harrypm (Harry Munday)

Status: user-mandated project rule (2026-11-08). Read this before committing,
merging, or applying ANY change that touches `README.md` — including edits
that feel cosmetic or "helpful".

## Why this note exists

AI agents and LLM assistants ("clankers") automatically read contributor docs
and then "helpfully" touch user-facing docs they were never asked to touch —
adding sections, links, tone fixes, formatting passes. The root `README.md`
is the project owner's public voice. An agent edit changed it without a
direct instruction during the 2026-11-08 session (a Contributing section was
added on top of an approved UX-restore task; it was reverted and replaced by
this rule).

## HARD RULES

1. `README.md` (repository root) is HANDS-OFF. No content, structure,
   formatting, link, punctuation, or whitespace changes of any kind without
   a direct, explicit instruction from harrypm (Harry Munday) covering that
   specific edit. "General improvement", "while I'm here", consistency
   passes, and drive-by doc polish are all forbidden.
2. This binds human contributors, maintainers, and AI/agent/LLM edits alike.
   Agents: contributing docs, dev notes, and rules files are the ONLY docs
   you may update in the course of normal work.
3. If a change genuinely requires a README update (new user-facing feature
   release notes, screenshots the owner asked for), propose it, get a direct
   yes from harrypm, and record that approval in the session's prompt log.
4. Reverting an unauthorized README change back to the owner's last state is
   permitted and expected.
5. Other root user-facing docs (`INSTALLATION.md`, `TECHNICAL.md`,
   `UPDATE.md`, `MONITOR_*.md`) follow the same principle via the UX
   sign-off rule (`gui_ux_change_approval_rules.md`); README.md is the
   strictest case — direct instruction, always.

## Where contributors ARE pointed at the rules

`CONTRIBUTING.md` (repository root) is the agent/contributor entry point and
may be updated by this rule's own maintenance — it is the doc clankers
auto-read. Keep the README hands-off rule stated there.
