# Prompt Log — Real-time Waterfall & Spectrograph view modes

Start: 2026-08-22
Working dir: `C:\Users\Harry\MISRC-GUI`
Goal: Add two new real-time, FFT-derived 2D color view modes (Waterfall, Spectrograph) to the MISRC GUI panel system.

## User input
- "I want to add real-time waterfall, spectrograph view modes"
- Clarifying answers:
  - Distinction: Waterfall = frequency on X, time scrolling down (newest at top); Spectrograph = time on X scrolling left, frequency on Y (audio/speech convention).
  - Color map: Heatmap matching existing phosphor heatmap (black→blue→cyan→green→yellow→red).

## Research (files inspected)
- `README.md`, `TECHNICAL.md` — project overview + GUI data flow.
- `misrc_tools/misrc_gui/core/gui_app.h` — `panel_view_type_t` enum (lines 23-30), panel config structs.
- `misrc_tools/misrc_gui/visualization/panel_interface.h` — panel vtable contract.
- `misrc_tools/misrc_gui/visualization/panel_registry.c` — registry + `panel_process_all` dispatch.
- `misrc_tools/misrc_gui/visualization/gui_panel.{c,h}` — view names, availability, render dispatch, config helpers.
- `misrc_tools/misrc_gui/visualization/gui_fft.{c,h}` — reusable FFT engine (`gui_fft_process_raw`, `fft_state_t`, swap buffers, vtable example).
- `misrc_tools/misrc_gui/visualization/gui_histogram_panel.c` — simplest vtable example (menus).
- `misrc_tools/misrc_gui/visualization/gui_oscilloscope.c` — waveform vtable + zoom/scroll handlers.
- `misrc_tools/misrc_gui/visualization/gui_phosphor_rt.{c,h}` — heatmap ramp (`intensityToHeatmap` shader, lines 67-88) to replicate as CPU LUT.
- `misrc_tools/misrc_gui/ui/gui_ui.{c,h}` — view dropdown auto-iterates `PANEL_VIEW_COUNT` (lines 4453-4525); color/font constants.
- `misrc_tools/misrc_gui/input/gui_capture.c` — `gui_app_init()` panel registration (lines 991-995); includes list.
- `misrc_tools/misrc_gui/processing/gui_display_thread.c` — `panel_process_all()` called on display thread with raw int16 samples (65536/ch), no GL.
- `misrc_tools/misrc_gui/core/misrc_gui.c` — main render loop (GL on this thread); lazy GL init pattern.
- `misrc_tools/meson.build` — GUI source list (lines 345-390).

## Commands run
- `meson compile -C build-local` -> failed: old `build.dat` (made with older meson) incompatible.
- `meson setup --reconfigure build-local misrc_tools` -> OK (regenerated from scratch; raylib 5.5, fftw3f 3.3.11, gcc 16.1, mingw).
- `meson compile` -> link failed: undefined `__imp_glfw*` from `libraylib.a` (static raylib built against glfw DLL).
- `pkg-config --libs --static raylib` -> `-lraylib -lglfw3 -lgdi32`; `--libs --static glfw3` -> `-lglfw3 -lgdi32`.
- Edited `meson.build`: added `misrc_gui/visualization/gui_spectrogram.c` to sources; on Windows changed gui_ldflags to link the glfw import lib `-l:libglfw3.dll.a` (static raylib references `__imp_glfw*` thunks, so the static `libglfw3.a` does not satisfy them).
- `meson compile` -> EXIT_CODE=0; `build-local/misrc_gui.exe` rebuilt (10.6 MB, 2026-08-22 10:18).
- Cosmetic LUT clamp reformat (silence -Wmisleading-indentation); recompile -> link failed ONLY with `cannot open output file misrc_gui.exe: Permission denied` (exe in use / GUI running), not a code error. The 10:18 exe already contains the full feature; the cosmetic edit is behaviorally identical.

## Output / results
### Files changed
- NEW `misrc_tools/misrc_gui/visualization/gui_spectrogram.h`
- NEW `misrc_tools/misrc_gui/visualization/gui_spectrogram.c` (shared engine: embeds `fft_state_t`, 256-entry heatmap LUT cloned from phosphor `intensityToHeatmap`, CPU RGBA8 history 1024x256, lazy `Texture2D`, two vtables).
- `misrc_tools/misrc_gui/core/gui_app.h` -> added `PANEL_VIEW_WATERFALL`, `PANEL_VIEW_SPECTROGRAPH`.
- `misrc_tools/misrc_gui/visualization/gui_panel.c` -> names + availability + include.
- `misrc_tools/misrc_gui/input/gui_capture.c` -> register both panels in `gui_app_init()` + include.
- `misrc_tools/meson.build` -> added source file; Windows gui_ldflags glfw import-lib fix.

### Build status
- Compiles + links cleanly (EXIT_CODE=0); `misrc_gui.exe` rebuilt with the feature.
- NOT yet visually verified. Per project rules, do not assume it works.

### Pending user verification (real-world GUI confirmation)
- Close any running `misrc_gui.exe`, then run `C:\Users\Harry\MISRC-GUI\build-local\misrc_gui.exe`.
- Start a capture (or simulated device), set a channel's View dropdown to **Waterfall** and to **Spectrograph**.
- Confirm: Waterfall scrolls down with newest at top, freq on X (0..Nyquist labels); Spectrograph scrolls left with newest at right, freq on Y (low at bottom, high at top); colors match the existing FFT/phosphor heatmap; the "now" edge marker is visible.
