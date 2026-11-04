# Prompt Readme — git pull / stash / rebuild to current

Date: 2026-10-14
Repo: `C:\Users\Harry\MISRC-GUI` (branch `main`, remote `https://github.com/harrypm/MISRC-GUI`)

## Task

`git pull` was blocked by local edits to `misrc_tools/misrc_gui/input/gui_capture.c`.
Goal: stash the local changes (with a note), pull to current `origin/main`, then
re-apply (rebuild) the local changes on top.

## Starting state (verified via `git status`, `git log`, `git stash list`)

- Local HEAD: `c051361` — `fix(net): detect client disconnect on idle /rf stream via select probe`
- `origin/main`: `0cfbc19` — 32 commits ahead (new tags v1.2.1 .. v1.2.5 fetched)
- Modified tracked file: `misrc_tools/misrc_gui/input/gui_capture.c` (+64 lines,
  Windows Media Foundation backend fix for MS2130 hsdaoh video stream)
- Untracked files (left in place, do not block the pull):
  - `misrc_tools/misrc_gui/dev/rp2350_firmware_timing_fix.md`
  - `prompt_readme_rtlsdr_demod.md`
  - `prompt_readme_waterfall_spectrograph.md`
  - `test_capture.raw`, `test_capture_err.txt`, `test_capture_out.txt`
- Existing stashes: none

## Commands run and outputs

### User commands (before this session)

```
git pull
```
Output (failed):
```
From https://github.com/harrypm/MISRC-GUI
   c051361..0cfbc19  main -> origin/main
 * [new tag] v1.2.1 -> v1.2.1
 * [new tag] v1.2.2 -> v1.2.2
 * [new tag] v1.2.3 -> v1.2.3
 * [new tag] v1.2.4 -> v1.2.4
 * [new tag] v1.2.5 -> v1.2.5
Updating c051361..0cfbc19
error: Your local changes to the following files would be overwritten by merge:
        misrc_tools/misrc_gui/input/gui_capture.c
Please commit your changes or stash them before you merge.
Aborting
```

```
git pull stash local with notes and rebuild to current
```
Output (failed — git parsed "stash" as a remote name, not a subcommand):
```
fatal: 'stash' does not appear to be a git repository
fatal: Could not read from remote repository.
```

### This session — step by step

1. Inspect state:
   ```
   git status
   git --no-pager log --oneline -3 main; git --no-pager log --oneline -5 origin/main; git stash list
   git --no-pager diff --stat -- misrc_tools/misrc_gui/input/gui_capture.c
   git --no-pager diff -- misrc_tools/misrc_gui/input/gui_capture.c
   ```
   Confirmed the local change is the +64-line Windows MF backend fix
   (route MS2130 video through Media Foundation/simple_capture, keep hsdaoh
   HID transparent-mode control on MI_04, `goto windows_mf_open_hid` /
   `windows_mf_connected` labels to skip libuvc video start).

2. Stash local edits with a note:
   ```
   git stash push -m "Windows MF backend for MS2130 hsdaoh video stream (gui_capture.c) - local edits pre-pull 2026-10-14"
   ```
   Output:
   ```
   Saved working directory and index state On main: Windows MF backend for MS2130 hsdaoh video stream (gui_capture.c) - local edits pre-pull 2026-10-14
   stash@{0}: On main: Windows MF backend for MS2130 hsdaoh video stream (gui_capture.c) - local edits pre-pull 2026-10-14
   ```

3. Pull to current:
   ```
   git pull
   ```
   Output (success): fast-forwarded `c051361` -> `0cfbc19`, 99 files changed,
   21,740 insertions(+), 2,512 deletions(-). No errors. Branch `main` now
   matches `origin/main` (VHS FM preview, RF color preview, spectrogram
   palettes, playback A/B, capture/recording and test changes included).

4. Re-apply the stashed local edits:
   ```
   git stash pop
   ```
   Output:
   ```
   Auto-merging misrc_tools/misrc_gui/input/gui_capture.c
   ...
   Dropped refs/stash@{0} (05f76702aa99f1841d754d13ca0851638b1a1ebb)
   ```
   Auto-merge succeeded — no conflicts. Stash list is now empty.

## Verification (hard data)

- `git --no-pager diff --stat` after the pop: `gui_capture.c | 64 +++++...`
  (exactly the original +64 lines, matching the pre-stash stat — the stash
  round-trip preserved the change byte-for-byte).
- Markers confirmed present in the working diff: `Media Foundation`,
  `sc_start_capture`, `windows_mf_open_hid`, `windows_mf_connected`,
  MS2130 name-matching block, libuvc fallback messages.
- No conflict markers (`<<<<<<<` / `=======` / `>>>>>>>`) anywhere under
  `misrc_tools/misrc_gui/input/`.
- `git stash list` — empty (stash fully consumed by the successful pop).

## Final state

- `main` = `0cfbc19` = `origin/main` (up to date, all 32 commits + tags v1.2.1-v1.2.5 in).
- Working tree: `gui_capture.c` carries the local Windows MF backend fix on
  top of the new base (uncommitted, as before).
- Untracked files untouched.

## Rebuild for local testing (same session)

Command:
```
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build-local.ps1
```

Result:
- Deps build via MSYS2 UCRT64 ran (self-skip stamp re-evaluated after the
  99-file pull; rebuilt and reinstalled into `.deps/install`). A transient
  `ModuleNotFoundError: No module named 'ninja'` from the initial Python
  probe was non-fatal — the script proceeded with MSYS2's ninja.
- Meson reconfigured `build-local` (existing coredata -> `--wipe` path) and
  compiled/linked: `[66/66] Linking target misrc_gui.exe`.
- Smoke test ran: `build-local\misrc_gui.exe --smoke-test` — passed (the
  script only prints its final OK line when the smoke test exits 0):
  `[build-local.ps1] OK: build path is connected and local build succeeded.`
- `misrc_gui.exe`: 11,380,075 bytes, LastWriteTime 14/10/2026 18:56.

Targeted recompile of the auto-merged file (to capture its own warnings as
hard data, since the full build output was condensed):
```
Remove-Item build-local\misrc_gui.exe.p\misrc_gui_input_gui_capture.c.obj
ninja -C build-local misrc_gui.exe
```
Note: plain `ninja -C build-local misrc_gui` fails with "unknown target" —
this build dir needs the `.exe` suffix on Windows.

`gui_capture.c` compile warnings (all pre-existing, none in the merged
Windows MF region, no errors, exe relinked):
- `C:/msys64/ucrt64/include/stdio.h:384/416` — MinGW header `-Wshadow`
  (vsprintf/vsnprintf), system header, not project code.
- `gui_capture.c:2289` — `-Wcomment`: `/*` inside the upstream Android
  libusb comment (`/dev/bus/usb/*`), benign.
- `gui_capture.c:665` — `-Wunused-function`: `gui_capture_device_is_misrc_clockgen`.
- `lto-wrapper.exe` serial LTRANS note — informational.

## Not done / notes

- Hardware test of the merged Windows MF fix is still pending: run
  `build-local\misrc_gui.exe` with the MS2130 + RP2350 attached and watch
  stderr for `[GUI] Windows: trying Media Foundation (kernel usbvideo)...`
  / `found MS2130 via MF` / `MF backend capture started` vs the libuvc
  fallback messages. Smoke test only validates startup, not capture.
- Nothing was committed or pushed (not requested).
- If the local fix needs to be kept long-term, commit it on a branch so
  future pulls don't need the stash dance.
