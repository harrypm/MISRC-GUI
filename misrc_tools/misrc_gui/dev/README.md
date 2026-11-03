# Dev Notes & Prompt Logs

This directory (`misrc_tools/misrc_gui/dev/`) is the single home for all
developer notes, prompt logs, and lock-step tracking documents for the
MISRC GUI project.

## Rule

**All prompt readmes, prompt logs, bug-tracking notes, and lock-step dev
notes must live in this folder.** Do not leave `PROMPT_*.md` or similar
tracking files in the repository root — the root is for user-facing docs
(`README.md`, `INSTALLATION.md`, `TECHNICAL.md`, `LICENSE*`) only.

### Naming convention

Use lowercase with underscores:

- `prompt_<feature>_readme.md` — prompt log for a feature task
- `prompt_<feature>_bug.md` — prompt log for a bug fix
- `<feature>_lockstep_notes.md` — lock-step tracking for ongoing work
- `dev_notes_README.md` — general dev notes (this folder's index)

### Current contents

| File | Description |
|------|-------------|
| `dev_notes_README.md` | General dev notes and capture-path constraints |
| `prompt_server_client_readme.md` | Server/client networking feature prompt log |
| `prompt_statusbar_readme.md` | Status bar compaction / toolbar scaling prompt log |
| `prompt_waveform_readout_bug.md` | Waveform readout bug prompt log |
| `prompt_raw_readout_cxadc_single_flacwarn_bug.md` | RAW readout fix + CXADC single-card mode + FLAC-off startup warning prompt log |
| `prompt_appimage_taskbar_desktop_icon_readme.md` | AppImage Desktop-icon cleanup + stable WM_CLASS / taskbar identity prompt log |
| `prompt_cxadc_direct_native_raw_stream_readme.md` | CXADC direct native RAW stream (byte-exact `.u8`/`.u16`, converted `.s8`/`.s16`) prompt log |
| `cxadc_win_lockstep_notes.md` | CXADC-Win (Windows driver) lock-step dev notes |
| `gui_automation_flags_rules.md` | HARD rule: every GUI element carries a CLI flag / config key for automated testing (inventory + gaps) |
| `gui_output_verification_rules.md` | HARD rule: headless verification must read what the GUI displays (render-path conversion contracts) |
| `gui_uniform_border_rules.md` | HARD rule: visible Clay containers derive width from content (uniform borders) — AudioLevelBars incident |

### When starting a new prompt

Create the tracking file here, not in the repo root:

```
misrc_tools/misrc_gui/dev/prompt_<feature>_<readme|bug>.md
```
