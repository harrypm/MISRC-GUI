# DEV NOTE: Playback drag-and-drop — per-channel zones + audio (the full capture set)

Status: feature-complete + user-confirmed (2026-10-20, real-GUI: "drag and
drop works"). Read this before touching the drop pipeline (the
`IsFileDropped` poll in misrc_gui.c), the classifier/zone/audio-feed in
gui_playback.c, or the monitor-queue API in gui_audio.c.

Guard: `ci_guard_tests.py` `playback drag-and-drop contract` (drop poll +
wrapper pointer + handler/audio/persistence wiring) — the feature cannot be
silently removed.

## The design (contracts in force)

1. THE ZONES ARE THE CHANNEL PANELS. A drop lands on the channel whose cached
   panel bounds contain the logical drop point (`gui_ui_get_mouse_position`
   — the ONLY `GetMousePosition` call site, per the UI scale integration
   contract); outside the canvases (VU/stats/timeline rows) it takes the
   vertically nearest row. Never add a second `GetMousePosition` call.
2. FILES CLASSIFY BY CONTENT, NOT EXTENSION OR DROP ORDER. The content sniffer
   + resolved header rates decide: an audio-rate file (true rate <= 400 kHz;
   WAV 1-8 ch incl. the 46.875 kHz clockgen PCM1802 capture, or native FLAC)
   becomes the monitor source; any RF container (mono FLAC / Ogg-FLAC /
   raw widths / .lds) becomes the ZONE CHANNEL's file. Only slots that
   received a file this drop change (a B-only drop keeps the loaded A; a set
   drop replaces both). One accepted file selects the Playback device, stops
   whatever is running (capture or playback) and (re)starts — the whole
   capture set loads in one action.
3. THE AUDIO FEED IS POSITION-DRIVEN, NOT WALL-CLOCK. target = playhead
   seconds x audio rate; seeks, loop resets, speed changes and pause all
   follow the RF timeline, and a detected jump (50 ms back / 4 s forward
   bounds) flushes the monitor queue (`gui_audio_reset_playback_queue`) and
   re-anchors (exhausted clears too, so a loop restarts the audio). Speed
   folds into the resample step (48000/(rate x mult)): every speed feeds
   exactly 48 k frames/s — the pitch shifts with the speed (tape semantics;
   slow motion pitched down, 2x/4x pitched up, never gaps, never queue
   overflow); Max skips audio (the timeline runs free).
4. THE MONITOR QUEUE IS SHARED, PRODUCERS EXCLUSIVE. `gui_audio_push_playback`
   feeds the same play_q as `gui_audio_push_demod` (48 k stereo int16,
   drop-oldest); the capture-audio thread / demod render path / playback
   feed never run at once. No auto-enable: the user hears it via Audio Mon
   (the demod-panel contract).
5. AUDIO READS ITS OWN WAY. `gui_playback_parse_wav` grew an `out_channels`
   param (NULL keeps the mono RF gate for the raw readers; the audio slot
   accepts 1-8 ch and feeds CH1/CH2, mono duplicated). FLAC audio uses a
   private decoder + the shared IO layer with NO STREAMINFO patch (audio
   STREAMINFO is honest; patch_total = 0) + `seek_absolute` per span,
   skipping the seek when the next span is contiguous. Ogg-FLAC audio is
   rejected with a specific message (remux for monitor audio is not worth
   it); Ogg-FLAC RF still routes through the session-remux path.
6. HARNESSES COMPILE gui_playback.c AS THEIR OWN TU. They do not link
   gui_audio.c (it needs raylib's AudioStream + soxr) — the monitor-queue
   pair is satisfied by `test/gui_playback_audio_stub.c`, linked into the
   five gui_playback harness targets (resolve / watchdog / seek / start /
   reconnect). A new gui_audio dependency from gui_playback.c must extend
   the stub, not leak raylib into the harnesses.

## Known limitations (accepted, recorded)

- An RF FLAC whose header rate collides with the audio-rate list (e.g. a
  24000 "kHz" header on a 24 MSPS capture) classifies as audio — IDENTICAL to
  the existing `gui_playback_resolve_real_rate_hz` behavior (such a file
  also misplays as RF today); RF Vorbis tags win when present.
- An audio-only drop without RF files: sets the audio file + a status hint;
  playback does not start (the start path needs an RF file).
- A Demod panel view and the dropped-audio feed both push the monitor queue
  (RF demod audio vs capture audio) — use one at a time.
- Audio Mon is the audibility gate: nothing is heard until it is enabled
  (mirrors device-audio monitoring and the Demod panel).

## Verification (2026-10-20)

1. `python misrc_tools/test/ci_guard_tests.py --static-only` — 41/41 PASS
   (incl. the new `playback drag-and-drop contract`).
2. `LIBFLAC_ENABLED=0` translation proof for gui_playback.c — EXIT 0.
3. `pwsh -File scripts/build-local.ps1` — guard gate + compile + link +
   `--smoke-test` OK; `meson test -C build-local` — 34/34.
4. Real GUI (user): drag-and-drop confirmed working.

## Cross-platform entry points (2026-10-26 addendum)

The drop pipeline above is platform-neutral (raylib 5.5 registers GLFW's
drop callback on Win32/Cocoa/X11/Wayland; the classifier is plain stdio +
libFLAC), but the EVENT DELIVERY is not:

- macOS: this app always relaunches itself elevated for MS2130
  hsdaoh/libusb capture (misrc_gui.c `gui_macos_relaunch_as_admin_if_needed`,
  locked by the ci_guard `macos admin elevation contract`). Root-owned
  windows NEVER receive Finder drag-and-drop (the WindowServer refuses
  cross-security-session drag delivery) — the drop event simply never
  arrives, `IsFileDropped()` never fires. That is an OS limitation, not an
  app bug; it cannot be fixed while the GUI runs as root.
- Windows: drag-and-drop confirmed working (2026-10-20 verification above).
- Linux: raylib 5.5 + its bundled GLFW 3.4 support drops on X11 (Xdnd) and
  native Wayland (data-device). If drops do nothing on a Linux desktop,
  check the stderr diagnostic (below) to split the failure: no
  `[PLAYBACK] Drop delivered:` line = the windowing layer never delivered
  the event (session/compositor issue, e.g. XWayland under some compositors);
  line present + a reject status = the handler refused the files (app
  issue — see the reject reasons on stderr).

Cross-platform fallbacks in force (misrc_gui.c):

1. POSITIONAL FILE ARGUMENTS: `misrc_gui <rf-file> [<file2> ...]` routes
   every bare-path argument through the SAME `gui_playback_handle_dropped_files`
   classifier at launch (zone A default; audio-rate file becomes the monitor
   source). Works on every platform and SURVIVES the macOS elevated relaunch
   (argv is forwarded), so Finder "Open With" and terminal launches load
   playback files even where drops cannot land. Bare paths are only treated
   as playback files when no capture CLI flag was seen first (the headless
   CLI routing is unchanged).
2. DROP DIAGNOSTIC: one `[PLAYBACK] Drop delivered: N file(s)` stderr line
   each time the platform delivers a drop (see above for how to read it).
3. macOS ELEVATED HINT: when running as root with the Playback device
   selected, a one-time status line points at Browse / the file argument
   instead of a silent no-op drop.
