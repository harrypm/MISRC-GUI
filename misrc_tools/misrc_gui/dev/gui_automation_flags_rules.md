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
- RTL-SDR recording mode: `rtlsdr_hifi_rf_record` (`false` for Native I/Q,
  `true` for Hi-Fi RF on builds with SoXR).
- RTL-SDR Hi-Fi RF filename tag: `rtlsdr_hifi_rf_tag` (string, empty by
  default, used in Hi-Fi RF mode).
- RTL-SDR Blog V4 manual VGA: `rtlsdr_vga_index` (integer `0`–`8`, default
  `8`). Select the Blog V4 on a desktop build with the patched librtlsdr
  and set `rtlsdr_gain_mode` to `1` (Manual) to apply the index on connect.
- Playback: positional files / `playback_file_a|b` config keys.
- Overwrite confirmation popup: `overwrite_files` config key (documented in
  `--auto-record` usage as the unattended-run requirement).
- Panel views (sidebar Layout/Left/Right/View, incl. the composite
  vectorscope "Vector" view): `--panel-view <a|b>:<left>[,<right>]`
  (one view = single, two = split; repeatable per channel; unknown names
  print the accepted list and exit 1; logs `[AUTO] --panel-view`). [2026-11-14]
- WFM (waveform monitor) panel System / Channel / Range / Units / Phosphor /
  Intensity buttons: `--wfm <key=value,...>` (channel=y|yc|c, range=
  active|full (frame = full; default full since 2026-11-26), units=ire|mv, phosphor=on|off, intensity=0.1..10, system=
  auto|pal|ntsc|secam; launch state of every WFM panel created after it;
  unknown keys/values print the accepted list and exit 1; logs
  `[AUTO] --wfm` and `[AUTO] WFM panel: ...` per panel). [2026-11-16]
- Histogram panel Mode / Bins and the Parade mode's Ch / Area / Range buttons:
  - Flag: `--histogram <key=value,...>`: mode=amplitude|parade, channel=rgb|r|g|b|w (w = Rec.601 luma), area=active|full, range=8|10, bins=64..4096.
  - Launch state of every Histogram panel created after it. Defaults: parade (since 2026-11-27), rgb, active, 8-bit, 4096 bins.
  - Unknown keys/values print the accepted list and exit 1. Logs `[AUTO] --histogram` and `[AUTO] Histogram panel: ...` per panel.
  - The Bins dropdown had no flag before (an unlisted gap). [2026-11-26]
- Radio panel tuning line (click / drag / wheel in its spectrum + waterfall):
  - Flag `--radio <key=value,...>`; every Radio control (harrypm 2026-11-27: "all modes should have CLI load commands"):
    - Panel state:
      - `tune=<Hz>`: tuning line = offset from the I/Q centre, or the RF frequency on a real input. k / M suffixes.
      - `input=auto|iq|real`, `range=1..6`, `zoom=1|2|4|8|16|32`.
    - Settings (saved; applied after the settings load, before any capture opens the dongle):
      - `mode=wfm|nfm|am|usb|lsb`. Without `bw=` it resets BW to Auto.
      - `bw=auto|<Hz>`, `sql=off|<dBFS -100..-1>`, `vol=0..2`, `out=12|34`, `region=auto|eu|us|jp`.
    - SDR:
      - `freq=<tuned MHz|Hz>`: centre = freq - 300 kHz, line at +300 kHz.
      - `band=fm|air|2m|marine|70cm|pmr`, `gain=auto|<dB 0..60>`, `agc=on|off`, `rate=0.25M|1.024M|1.2M|1.536M|2.0M|2.048M|2.4M`.
    - Logs `[AUTO] --radio`, `[AUTO] --radio settings: ...` and `[AUTO] Radio panel: ...`. [2026-11-27]
- Radio panel SDR Band dropdown + MHz frequency box: no new flag (the SDR frequency is the `rtlsdr_freq_hz` config key; the mode is `demod_mode`). RTL-SDR stock layout (CH1 single Radio on the first RTL-SDR capture): `--rtl-layout on|off`. [2026-11-27]
- Player-mode name hints (automatic, no control; off switch for tests):
  - Flag: `--name-hints on|off` (default on; logs `[AUTO] --name-hints` and `[AUTO] name hint: channel X -> ...` when applied). [2026-11-26]
- View rename (2026-11-26): "Demod" is now "Radio" (`--panel-view ...radio`); the old name `demod` still parses.
- Vector (vectorscope) panel System / Mode / Lines / Field / Graticule /
  Plot / Colour / Trace / Intensity buttons (and the scroll-wheel
  intensity): `--vector <key=value,...>` (mode=standard|advanced, lines=
  active|whole|burst|porch (porch = SECAM's burst window), field=
  active|whole, graticule=75|100|both|off, plot=
  points|density, colour=mono|multi|blend, trace=on|off, intensity=1..10,
  system=auto|pal|ntsc|secam; launch state of every Vector panel created
  after it; unknown keys/values print the accepted list and exit 1; logs
  `[AUTO] --vector` and `[AUTO] Vector panel: ...` per panel). Closes the
  vectorscope System/Graticule/Plot/Colour known gap. [2026-11-17]
- Spectro panel direction, palette and (paused) span dropdowns: `--spectro
  <key=value,...>` (dir=horizontal|vertical, palette=phosphor|heat, span=
  1field|1frame|2frames|3frames|5frames|10frames|25frames|1s|3s|10s|30s|60s
  [marks=on|off added 2026-11-25: the paused Marks toggle]
  [span added 2026-11-19; the wheel steps the same list]; launch
  state of every Spectro panel created after it, the palette shared by all;
  unknown keys/values print the accepted list and exit 1; logs
  `[AUTO] --spectro` and `[AUTO] Spectro panel: ...` per panel). The Waterfall
  view is merged into Spectro as Dir Vertical: `--panel-view a:waterfall`
  still opens it (Spectro, Dir Vertical). The palette dropdown had no flag
  before (an unlisted gap). [2026-11-18]
- Playback cut markers button ([ | ] beside Play / Loop on each timeline
  row) and its menu (Set IN / Set OUT / Go to IN / Go to OUT / Clear / Copy
  FLAC-Chop command): `--markers <a|b>:in=<samples>,out=<samples>,
  menu=open|closed` (any subset; marks bind to the first file that channel
  plays; out <= in, other keys or values print the accepted form and exit 1;
  logs `[AUTO] --markers`). Keys I / O set IN / OUT at the playhead. The
  copied command is also logged as `[CUT] ... FLAC-Chop command:`. [2026-11-19]
- Saved per-channel panel setup (settings `panel_view_a/b`, `video_decoder_a/b`):
  restored at launch before `--panel-view`, which still overrides it; logs
  `[AUTO] saved panels: ...`. The Video decoder button has no own flag yet
  (the saved `video_decoder_*` key is its launch state). [2026-11-19]
- Hi-Fi on CH3/4 + HiFi panel (Source / Format / Span buttons):
  - Flag: `--hifi <key=value,...>`.
    - `decode=off|auto|pal|ntsc|beta`: the HiFi panel Decoder button (settings `hifi34_enabled` + `hifi34_format`). Off by default: nothing decodes until picked (2026-11-26).
    - `src=auto|a|b`: settings `hifi34_source`, 0 to 2.
    - `fmt=auto|pal|ntsc|beta`: settings `hifi34_format`, 0 to 3.
    - `span=10|20|50|100|250|500|1000|2000`: HiFi panel span in ms, for panels created after it.
  - Logs `[AUTO] --hifi`. Bad keys or values print the accepted form and exit 1.
  - The panel view is `hifi` (`--panel-view b:hifi,waveform`).
  - The CH3/4 monitor toggle (`audio_monitor_ch34`) is the existing settings key.
  - The decoder logs `[HIFI34] decoding RF <A|B> as <format>`.
  - Added 2026-11-25.
- Pop-out panels (corner pop-out button on each panel; click a popped slot's placeholder to return it):
  - Flag: `--popout <a|b>:<left|right>[,...]` opens those slots in pop-out windows at launch. Logs `[AUTO] --popout` and `[POPOUT] ... popped out / returned`.
  - Open slots and their window geometry persist in settings `popout_windows`.
  - Inside a pop-out: F11 or the corner button toggles fullscreen on its screen.
  - Without pop-out windows (Android / no GLFW) the same button and `--popout` make the panel full screen inside the app. Back, Esc or its corner button returns it.
  - `MISRC_PANEL_FULLSCREEN=1` (diagnostics) selects that mode on desktop.
  - Added 2026-11-26.
- Video panel decoder mode (the Decoder cycle, System, RF Format family):
  - Flag: `--video decoder=cvbs|rf,system=pal|ntsc|secam|mesecam,format=vhs|betamax|laserdisc` (any subset).
  - Applied to every new Video panel and again after a saved setup is restored, so the flag wins.
  - Logs `[AUTO] --video` and `[AUTO] Video panel: <spec>`. [2026-11-27]
- Video panel Frame button (Active / Full, all three decoders):
  - Flag: `--video-frame active|full` (launch state of new Video panels; logs `[AUTO] --video-frame`).
  - Default Full since 2026-11-26; a saved panel keeps its own.
- Video panel View button (Frame / Split / Field, all three decoders):
  `--video-fields frame|split|field` (launch state of new Video panels;
  logs `[AUTO] --video-fields`; anything else prints the accepted list and
  exits 1). [2026-11-19]
- Transport Pause (partial, for paused-view automation): `--auto-pause
  <seconds>` pauses playback once, that long after it starts (logs
  `[AUTO] --auto-pause`). Used to verify the paused Waveform file window
  and its field / line marks. [2026-11-19]
- Per-channel Play/Pause (timeline row buttons, partial): env
  `MISRC_DEBUG_CHANNEL_PAUSE=<a|b>@<start s>:<period s>[x<count>]` toggles
  one channel the way its button does, on a timer (logs `[DEBUG] channel
  A paused|resumed`). Link mode starts Separate when A and B durations
  differ (> 0.1 s), logged `[PLAYBACK] Link mode: SEPARATE`. [2026-11-28]
- Waveform launch state and the paused player-mode Lock button (Line / HS):
  `--wave <key=value,...>` with zoom=<1..128> (samples per pixel),
  trig=off|rise|fall|sync|cvbs|hs (the Trig dropdown; hs = Head SW),
  marks=on|off (the paused Marks toggle: field / line wireframe) and
  lock=line|hs (logs `[AUTO] --wave`; anything else prints the accepted list
  and exits 1). The Waveform zoom had no flag before (an unlisted gap). [2026-11-19]

## Known gaps (backlog under this rule — do not grow this list)

- Waveform Signal/Mode/Scale/Trig dropdowns,
  waveform playback Res/Range render-resolution dropdowns (per-panel runtime
  state: `res_points`/`res_range`, no settings key),
  OSD/Levels/Sync selectors (decoder mode: `--video` since 2026-11-27): per-panel
  runtime state, no launch key today (rule 6 exemption). [Res/Range added
  2026-11-03, playback device only]
- Player Link mode + per-file transport (PlaybackLinkToggle /
  PlaybackPauseBtn/PlaybackLoopBtn next to the file input rows): per-session
  runtime state (atomics, no settings key) — rule 6 exemption. [added
  2026-11-03; a `--config` key would be required the moment these persist]
- Fullscreen (F11) / borderless mode: no launch flag.
- The M-Client remote settings table (`gui_monitor_settings.c`) does not
  include `video_system` / `gen_tape_format` — remote runs cannot set the
  Test Gen state remotely.
