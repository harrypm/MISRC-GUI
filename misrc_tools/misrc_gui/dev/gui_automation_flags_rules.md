# DEV NOTE (HARD): GUI automation flags — every user-interactable GUI element must be automatable at launch

Status: user-mandated project rule (2026-11-03). Read this before adding ANY
new GUI element (button, dropdown, cycle selector, toggle, overlay control,
mode picker), and before closing any change that adds one.

## Why this note exists

Automated GUI testing (headless launch checks, --auto-capture /
--auto-record runs, CI smoke runs, future screenshot/regression harnesses)
can only reach what a CLI flag or a `--config` settings key can preload.
Until 2026-11-03 the Test Gen's new controls (Gen system cycle, Fmt tape
format) were GUI-only: no flag existed, so an automated run could not put
the GUI into MESECAM/Betamax state without synthetic mouse input. The user
made the general requirement a hard rule: **all GUI elements must have a
GUI autoload trigger flag.**

## HARD RULES

1. Every user-interactable GUI element ships with an automation trigger: a
   CLI flag and/or a `--config` settings key that puts the GUI into that
   element's state at launch, with no synthetic mouse/keyboard input. The
   element and its trigger land in the SAME change — an element without its
   automation trigger is incomplete work, not "polish for later".
2. Flags fail fast: an unknown value prints the exact accepted values and
   exits nonzero (the `--select-device` / `--gen-system` pattern), so an
   automated run never silently continues in the wrong state.
3. Flags log their effect (`[AUTO]` prefix on stderr) so a run's log
   records the state it requested and can assert it.
4. A flag's effect must be observable by a test: a settings key, a
   status-bar line, a log line, or a dumpable buffer. A flag that changes
   nothing observable cannot be verified — see
   `gui_output_verification_rules.md` for what "verified" means.
5. A flag overrides config/default values (flag > --config > default), and
   the application point is after the settings load, before anything reads
   the setting. Note that a session which saves settings persists the
   overridden values.
6. Panel-local runtime state (zoom level, an open dropdown, per-panel
   decoder mode picks) is exempt UNTIL it becomes persisted default state —
   the moment a `gui_settings_t` key exists for it, the key + flag become
   mandatory and the element joins the inventory below.

## Contracts in force (inventory — update in the same change that adds an element)

- Test Gen system (waveform Gen button): `--gen-system
  <pal|ntsc|secam|mesecam|0-3>`; config key `video_system` (0-3). [2026-11-03]
- Test Gen tape format (waveform Fmt button, FM RF panels): `--gen-format
  <vhs|svhs|betamax|video8|hi8|0-4>`; config key `gen_tape_format` (0-4). [2026-11-03]
- Device selection: `--select-device <index-or-name>` (+ persisted pick).
- Capture start: `--auto-capture`; record cycle: `--auto-record <seconds>`.
- Whole settings panel: `--config <file>` — every `gui_settings_t` key
  (capture A/B, FLAC, resample, audio outputs, UI scale, net mode, ...).
- Playback: positional files / `playback_file_a|b` config keys.
- Overwrite confirmation popup: `overwrite_files` config key (documented in
  `--auto-record` usage as the unattended-run requirement).

## Known gaps (backlog under this rule — do not grow this list)

- Panel view/split selection, waveform Signal/Mode/Scale/Trig dropdowns,
  CVBS/RF decoder-mode cycle, OSD/Frame/Levels/Sync selectors: per-panel
  runtime state, no launch key today (rule 6 exemption).
- Fullscreen (F11) / borderless mode: no launch flag.
- The M-Client remote settings table (`gui_monitor_settings.c`) does not
  include `video_system` / `gen_tape_format` — remote runs cannot set the
  Test Gen state remotely.
