# Prompt log: Waterfall merged into Spectro, readout-direction dropdown

Branch `spectro-direction` (worktree `MISRC-GUI-web`), on top of origin/main
0c8c8e2 (PR #30 merged). Date: 2026-11-18. Status: committed and pushed to main at harrypm's request ("remove dir lable, commit and push").
The suite passes apart from the issues listed below. Waiting on harrypm's
look in the GUI (on this machine the GUI windows stayed white this
morning; see "GUI check") and UX sign-off
(`gui_ux_change_approval_rules.md`). Not committed.

## Input

> Merge waterfall mode into Spectro add a dropdown horisonstally to switch
> readout direction

## What changed

The Waterfall and Spectro views already shared one engine
(`visualization/gui_spectrogram.c`). They differed only in orientation:

- Waterfall: frequency on X, time on Y scrolling down, newest at the top.
- Spectro: time on X scrolling left, newest at the right; frequency on Y.

Changes:

- **One view.** "Spectro" now has a **Dir** dropdown, Horizontal / Vertical.
  - Horizontal is the former Spectro and the default.
  - Vertical is the former Waterfall.
- **Placement.** Dir sits on the same top-right row as the palette dropdown,
  left of it, with no text label (harrypm, 2026-11-18: "remove dir lable");
  the button reads Horizontal / Vertical like the palette button.
- **Switching direction.**
  - The history buffer holds 1024 x 512 pixels either way, so only its
    dimensions swap and the GPU texture is re-created at the new size.
  - The rolling history is cleared and refills.
  - A paused playback analysis window is re-mapped in the new direction
    and re-requested (the palette dropdown's reload path).
- **Dropdown behaviour.** One dropdown is open at a time. Double-click
  chrome-hide hides both.
- **Waterfall leaves the View pickers.**
  - `PANEL_VIEW_WATERFALL` stays in the enum as a legacy id, so the ids
    after it (Spectro, Demod, Vector, WFM) and per-type tables do not move.
  - It is no longer registered or listed.
  - `panel_config_set_left/right_view` map it to Spectro.
- **Launch flags** (hard rule `gui_automation_flags_rules.md`):
  - `--spectro dir=horizontal|vertical,palette=phosphor|heat` sets the
    launch state of new Spectro panels. The palette dropdown had no flag
    before (an unlisted gap) and now has one.
  - `--panel-view a:waterfall` still works: it opens Spectro with Dir
    Vertical and logs `Spectro (Dir Vertical)`.
- **Tests.** `test/gui_stats_layout_harness.c`'s view stub hides the legacy
  id like the app.

Panel views are not saved in settings, so there is nothing to migrate. The
web remote does not name views.

CPU: unchanged. It is the same engine with the same per-frame work; the
direction is a layout choice, and the second dropdown button is two
rectangles and a label.

## Files

- `visualization/gui_spectrogram.{c,h}`: Dir dropdown, direction switch,
  `--spectro`, one vtable.
- `visualization/gui_panel.c`: legacy id hidden and mapped to Spectro.
- `core/gui_app.h`: legacy id documented.
- `core/misrc_gui.c`: `--spectro`, the `waterfall` alias, usage.
- `input/gui_capture.c`: only Spectro is registered.
- `dev/gui_automation_flags_rules.md`: new entry.
- `test/gui_stats_layout_harness.c`: availability stub.

## Verification

- `--spectro dir=vertical,palette=heat` logs
  `[AUTO] --spectro: dir=Vertical palette=Heat`. Bad values (`dir=diagonal`,
  `palette=red`, `foo=1`, empty) print the accepted list and exit 1.
- `--panel-view a:spectro,waterfall` logs
  `[AUTO] --panel-view: channel A Spectro | Spectro (Dir Vertical)`.
- Suite: 58 / 59 (`update_cli` is environmental, as before).
  `gui_capture_watchdog` cannot link on origin/main itself (see below), so
  it is excluded.

## Issues found on the way (not from this change)

- **`gui_capture_watchdog_test` does not link on origin/main 0c8c8e2.**
  PR #30 added `gui_app_start_capture` / `gui_app_stop_capture` to
  `test/gui_playback_audio_stub.c`, which `test/gui_capture_watchdog_test.c`
  also defines (MinGW UCRT64 + LTO: multiple definition). `meson test`
  then refuses to run any test until it is fixed or excluded.
- **GUI check.** Every GUI window stayed white this morning, including a
  pre-PR #30 binary from 2026-11-16 that drew normally before; same
  GTX 1080 Ti / GL 3.3 / driver 582.66 in every log. So it is this
  machine's state, not a code change. The Spectro layout still needs
  harrypm's eyes.

## Commands

```sh
# MSYS2 UCRT64 (PowerShell: $env:MSYSTEM="UCRT64"; C:\msys64\usr\bin\bash.exe -l script.sh)
ninja -k 0 -C build-ucrt                       # -k 0: past the watchdog link failure
meson test --no-rebuild <every test but gui_capture_watchdog>
build-ucrt/misrc_gui.exe --select-device Simulated --auto-capture \
    --panel-view a:spectro,waterfall            # Horizontal | Vertical
build-ucrt/misrc_gui.exe ... --spectro dir=vertical,palette=heat --panel-view a:spectro
```

## Open

- A visual check once the machine's GUI windows draw again (they stayed
  white on 2026-11-18). harrypm approved the change ("remove dir lable,
  commit and push").
- The watchdog test stub clash (upstream PR #30), when wanted.
