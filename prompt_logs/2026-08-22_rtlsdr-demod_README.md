# Prompt Log — RTL-SDR support + device-agnostic Demod view/plugin

Start: 2026-08-22
Working dir: `C:\Users\Harry\MISRC-GUI`
Goal: Add RTL-SDR raw I/Q capture + a separate, device-agnostic Demod panel (WFM/NFM/AM/USB/LSB).

## User input / decisions
- "I want RTLSDR support" → confirmed: local USB via librtlsdr; demod modes WFM+NFM+AM+USB+LSB; demod is a view/plugin separate from raw I/Q feed; raw I/Q packed as 8-bit into the existing 12-bit pipeline (I=chA, Q=chB); demod audio → CH1/2 default, CH3/4 for co-monitoring; capture sample rate selectable (known-stable set, default 2.4 MSPS = highest widely-stable), demod output always resampled to fixed 48 k.

## Commands run
- `pacman -S --noconfirm --needed mingw-w64-x86_64-rtl-sdr` (via `C:\msys64\usr\bin\pacman.exe`) → installed librtlsdr 2.0.2 (librtlsdr_static.a, librtlsdr.dll.a, rtl-sdr.h). pkg-config module = `librtlsdr` (NOT `rtl-sdr`).
- `meson setup --reconfigure build-local misrc_tools` → OK, "building with RTL-SDR support".
- `meson compile -C build-local` → first fail: `panel_interface.h: No such file` (gui_demod.h in signal/ used `#include "panel_interface.h"`). Fixed to `#include "../visualization/panel_interface.h"`.
- `meson compile` → second fail: `cannot find -lrtlsdr` (MSYS2 .pc Libs `-lrtlsdr` resolves to DLL import lib; global `-static` rejects). Fixed meson.build: on Windows static builds use `partial_dependency(compile_args: true, includes: true)` for compile flags + link the static archive explicitly by filename: `gui_ldflags += ['-l:librtlsdr_static.a']`.
- `meson compile` → EXIT_CODE=0; `misrc_gui.exe` rebuilt (10.77 MB, 11:09). Launched (PID 37316, responding).

## Files changed / created
- NEW `misrc_tools/misrc_gui/input/gui_rtlsdr.{c,h}` — RTL-SDR backend (gated `#ifdef ENABLE_RTLSDR`): enumerate/open/start/stop/is_running; capture thread reads uint8 I/Q via `rtlsdr_read_sync`, packs `(int8)(u8-128)<<4` into the hsdaoh 32-bit format via `rtl_encode_raw_sample`, feeds `BUF_CAPTURE_RF`; sets `app->sample_rate`; mirrors gui_fx3 lifecycle + gui_playback feed pattern.
- NEW `misrc_tools/misrc_gui/signal/gui_demod.{c,h}` — Demod panel (`PANEL_VIEW_DEMOD`): device-agnostic; pulls I/Q from `app->display_thread->samples` in render() (both channels); chains WFM (discriminator + 75us de-emphasis), NFM, AM (mag + DC-block + AGC), USB/LSB (fs/4 shift + LPF); libsoxr resample to fixed 48 k; squelch + volume; pushes stereo int16 to `gui_audio_push_demod` (monitor queue); scrolling audio waveform; mode dropdown via panel menu system.
- `misrc_tools/meson.build` — `librtlsdr` dependency (partial_dependency + `-l:librtlsdr_static.a` on Windows static); `gui_rtlsdr.c` + `gui_demod.c` sources.
- `misrc_tools/misrc_gui/core/gui_app.h` — `DEVICE_TYPE_RTLSDR` (gated `ENABLE_RTLSDR`), `PANEL_VIEW_DEMOD`, RTL-SDR + demod settings fields in `gui_settings_t`.
- `misrc_tools/misrc_gui/core/gui_settings.c` — defaults (100MHz, 2.4MSPS, auto gain, AGC on; demod WFM, open squelch, unity vol, CH1/2) + save + load for all new fields.
- `misrc_tools/misrc_gui/input/gui_capture.c` — includes; `gui_demod_panel_register()` in init; `[RTL-SDR]` enumeration; start block (mirrors FX3); stop block.
- `misrc_tools/misrc_gui/visualization/gui_panel.c` — "Demod" name + availability + include.
- `misrc_tools/misrc_gui/output/gui_audio.{c,h}` — `gui_audio_push_demod()` public wrapper over the monitor play_q.

## Build status
- Compiles + links clean (EXIT_CODE=0) with `-DENABLE_RTLSDR=1`. `misrc_gui.exe` rebuilt.

## NOT yet verified / known gaps (do not assume working)
- **In-app frequency tuning is NOT implemented.** The settings UI (gui_ui.c) is a large Clay declarative section; adding a full RTL-SDR controls block (freq text field, gain dropdown, sample-rate dropdown + click handlers) is the remaining piece. For now the RTL-SDR runs at the persisted defaults (100 MHz / 2.4 MSPS / auto gain / AGC on); to tune, edit `%APPDATA%\MISRC\misrc_gui_settings.json` (`rtlsdr_freq_hz`, `rtlsdr_sample_rate_hz`, `rtlsdr_gain_mode`, `rtlsdr_gain_tenths_db`, `rtlsdr_agc`, `rtlsdr_offset_corr`) and relaunch.
- Demod audio routing: v1 pushes to the single shared monitor queue (CH1/2). True CH1/2-vs-CH3/4 co-monitoring alongside another device's audio is a follow-up (needs a second queue/mixer).
- Demod runs only while the Demod panel is visible (render-driven); switching away mutes it. Moving demod to the display thread is a follow-up.
- Pending user real-world verification: plug in RTL-SDR, confirm dropdown/capture/views/MSPS, Demod WFM/NFM/AM/USB/LSB via Audio Mon, 8-bit FLAC round-trip.
