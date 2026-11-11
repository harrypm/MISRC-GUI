# Prompt readme: v1.2.7 follow-up fixes (drag-and-drop, mode switch UI, preview font, updater)

Date: 2026-10-26 (session: MISRC-GUI main @ ab6d743, tag v1.2.7, after PR #16 merge)

## User inputs (verbatim intent)

1. "Drag and drop cross platorm needs fixing for MacOS/Linux for player mode."
   Clarified: drop does nothing at all (no file loads, no status change).
2. "Mode swich UI needs reverting back and fixng cycel, internal text needs
   fixing to use normal fonts..." + "swiching modes to Monitor & Remote is
   not fluid and instant".
   Clarified: revert to old compact toggle style extended to 4 modes; a
   HOLD-press on the toggle opens a dropdown to select a mode directly; the
   "Waiting for Server preview..." placeholder uses the unpleasing default
   bitmap font - remove that font entirely (use the app's normal Inter font).
3. "Update / Version control fixes required..."
   Clarified: says update available when it shouldn't (wrong tag/version logic).

## Root causes found (verified in code, not assumed)

- Drag-and-drop: the app pipeline (misrc_gui.c IsFileDropped poll ->
  gui_playback_handle_dropped_files) is portable; raylib 5.5 registers GLFW's
  drop callback on all platforms. macOS ALWAYS relaunches the GUI as root
  (gui_macos_relaunch_as_admin_if_needed, misrc_gui.c) and root-owned windows
  never receive Finder drag-and-drop (hard macOS limitation; elevation
  contract is ci_guard-locked). Linux: cause not verifiable from Windows -
  diagnostics added for a live test.
- Mode switch stall: the About-window Mode toggle click handler ran
  gui_net_apply_mode + gui_app_enumerate_devices synchronously on the render
  thread; that path JOINS network threads sitting in up-to-seconds socket
  timeouts (client 2s connect/1s IO; monitor 1s/0.9s; server ~300ms).
- Preview font: gui_panel.c used raw raylib DrawText/MeasureText (raylib's
  built-in default bitmap font) for the M-Client placeholder lines.
- Updater: gui_updater_compare_versions fell back to lexical strcmp when the
  current version was not semver, so dev-YYYY-MM-DD-sha and unknown_version
  builds compared "older" than every v* tag -> permanent false "update
  available". Separately, local Windows builds reported unknown_version
  because meson's vcs_tag could not execute the POSIX git-version.sh under
  native-Windows Python/meson.

## Changes made

- visualization/gui_panel.c: placeholders now render with gui_text_draw /
  gui_text_measure (the app's Inter font); no raylib default-font use left.
- core/gui_updater.{h,c}: new gui_updater_version_is_comparable(); a
  non-comparable current (dev/unknown) is never older than a release tag.
  test/gui_updater_test.c extended for the new semantics.
- ui/gui_ui.c version window: dev/unknown builds show "Dev build (<ver>) -
  latest release <tag>"; no Install row for them. Headless --check-update
  (core/misrc_gui.c) prints current_comparable and update_available: false
  for dev builds.
- misrc_tools/git-version.py: portable Python twin of git-version.sh;
  meson.build vcs_tag uses it (git-version.sh stays for CI shell scripts).
  Local Windows build version.h now resolves (v1.2.7 at a tagged HEAD)
  instead of unknown_version.
- net/gui_net.{h,c}: async mode transitions (gui_net_request_apply_mode +
  gui_net_transition_busy): the stop/start sequence runs on a worker; the
  main-thread pollers/query helpers early-out while it runs; gui_net_stop
  joins it at exit. Client LAN timeouts cut (connect 2000->750ms, IO
  1000->500ms). Deferred device re-enumeration via app->net_reenumerate_pending
  (gui_app.h + main loop).
- net/gui_monitor_transport.{h,c}: gui_monitor_client_stop stages its
  app->settings/devices restores for the main thread
  (gui_monitor_client_take_pending_restore, consumed by gui_net_poll_mirror
  and gui_net_stop). mon_get / mon_get_preview timeouts 1s/0.9s -> 400ms.
- ui/gui_ui.c mode toggle: reverted compact style (width sized to the longest
  label, hint lists the four modes), quick-click cycles, >=450 ms hold opens
  a direct-select menu (item click switches immediately, ESC/backdrop/click
  outside closes, menu-first ESC); all switching goes through the async
  transition (sync apply only as worker-spawn fallback).
- core/misrc_gui.c: positional playback files (misrc_gui <file> [...] /
  Finder "Open With") route through the same drop classifier at launch;
  [PLAYBACK] Drop delivered: N file(s) stderr diagnostic per real drop;
  one-time macOS elevated drop-unavailability hint when Playback is selected
  with the Playback device; usage text documents positional files.
- dev/playback_drag_drop_audio.md: cross-platform entry-points addendum.
- Test build fixes (pre-existing v1.2.7 breaks from PR #16, verified via
  git log -S / diff --stat that they predate this session):
  - meson.build: gui_settings_paths_test links the real
    misrc_gui/net/gui_monitor_settings.c.
  - test/monitor_local_save_stub.c (new): CPU-only stdio-passthrough stub of
    the mls_proxy_* API + local-save state no-ops; linked into
    gui_record_paths, gui_rf_input_stamps, gui_rf_playback_reconnect tests.
  - gui_audio_hifi_monitor_test / gui_audio_soxr_lifecycle_test: one-line
    gui_monitor_server_active stubs; soxr also stubs the remaining proxy
    write-path symbols after #undef-ing gui_audio.c's stdio redirects.

## Commands run + results

- python misrc_tools/test/ci_guard_tests.py --static-only -> 45/45 PASS
- pwsh -File scripts/build-local.ps1 -> guard gate OK, meson build 80/80,
  --smoke-test OK ("build path is connected and local build succeeded")
- meson test -C build-local -> 45/45 OK (was failing to link on 4 targets:
  gui_settings_paths, gui_rf_input_stamps, gui_record_paths,
  gui_audio_soxr_lifecycle - all pre-existing from bdcbb7a)
- Positional-file launch proof (isolated --config under .tmp, real 40 MSPS
  EBU raw capture, 12 s run, process stopped by its own PID):
  stderr shows "[PLAYBACK] Drop: zone=A A=...ebu_1min_first20s_40msps.raw
  B=(keep) audio=(none) (0 rejected, 0 extra)" -> "[GUI] Selected device:
  [Playback] FLAC Files" -> "[PLAYBACK] File A (u8): header rate=40000 Hz
  ... (real_rate=40000000Hz, 40.00 sec)" -> "Playback thread started".

## Follow-up round (2026-10-27 ~01:25-01:41)

User feedback after testing the first build:

1. "text on standby mode for monitor and remote not fixed" - the font fix
   had been REVERTED in the working tree (gui_panel.c was back to raw raylib
   DrawText; every other session edit was still present). Re-applied the
   gui_text_draw/gui_text_measure change with a comment explaining why raw
   DrawText must not be used there. Lesson: verify the built binary's source
   state before reporting a fix done.
2. "remove the extended text from the info pannel for the button" -
   clarified as "remove the extra monitor wavefrom + fft text": the
   "Monitor waveform + FFT (Zstd-1)" info line under the mode row in the
   About window is removed (gui_ui.c). The "click to cycle ... - hold for
   menu" hint next to the Mode toggle was also removed in this round.
3. "mode switch cycle is far far too slow" - two real causes fixed:
   - Uninterruptible sleeps in the net threads made every STOP wait them
     out: client worker backoff (up to 2 s) and 1 s poll sleep are now 50 ms
     interruptible slices; discovery recv timeout 1 s -> 250 ms; LAN client
     connect timeout 750 -> 300 ms; the monitor client skips its preview
     fetch when stopping (halves its join wait).
   - Clicks during a running transition were refused; now the transition
     worker CHASES settings.net_mode (clicks queue, the newest target is
     applied next - gui_net_request_apply_mode(app, prev_mode,
     force_reapply)). Finished workers are joined so thread handles do not
     leak; gui_net_stop joins a finished worker too.

Rebuild notes: the first relink attempt failed because a running test
instance had misrc_gui.exe locked; after it closed, rebuild + full meson
test = 45/45 OK. An 8-second unattended sanity launch was force-killed on
its timer while the user happened to be clicking through modes in it
(01:37:37) - its stderr incidentally proved the new cycling applies modes
back-to-back (server up, then server down + client discovery, no refused
clicks). Final exe relinked 01:41:28 with the info-line removal; guards
45/45 PASS.

## Test Gen panel + hacktv values round (2026-10-27 ~01:49-02:05)

User requests (verbatim intent):
- "For now git clone HackTV and lets copy its values to make some accurate
  VB CVBS on demand"
- "Also add a 'Test Gen' Pannel to the GUI plugin list for a test genarator mode"
- "I want PAL/NTSC/SEACAM modes to be toggleable on demand"

### What was built

1. hacktv cloned to third_party/hacktv (Codeberg, GPL-3.0-or-later, (c)2017-2026
   Philip Heron - same licence as this project, values and formulas freely
   shareable).
2. signal/gui_testgen.c/h (NEW): a time-based composite CVBS test frame
   generator for PAL, NTSC and SECAM with hacktv's exact constants:
   - PAL: sync -0.30 / blank 0 / black 0 / white 0.70, active 51.95us @
     10.40us, hsync 4.70us, eq 2.35us, broad 27.30us, burst 2.25us @ 5.6us
     at 3/7 of the white-blank span at +-135deg (V-switch), fsc 17734475/4
     = 4433618.75 Hz, U 0.493 / V 0.877.
   - NTSC: 525/(30/1.001), active 52.90us @ 9.20us, eq 2.30us, broad 27.10us,
     sync -40/140, black 7.5/140 (setup), white 100/140, burst 2.5us @ 5.3us
     at 4/10 at 180deg, fsc 39375000/11 = 3579545.4545 Hz.
   - SECAM: 625-line PAL geometry; FM chroma: D'B centre 4.25 MHz (272 fH),
     D'R centre 4.40625 MHz (282 fH), deviation clamps hacktv's SECAM encoder
     values; no pre-emphasis or ident (v1 - the decoder calibrates carriers
     itself from the porch).
   - Bars (hacktv av_test.c layout): 75% amplitude colour bars (0xBF) in the
     top ~3/4, then 75% red stripe, gradient, 8-level grey columns.
   - The frame is synthesised as a function of ABSOLUTE TIME per sample (no
     integer-samples-per-line assumption - NTSC at 40 MSPS is 2542.22
     samples/line and a line-buffered generator would drift).
3. visualization/gui_testgen_panel.c (NEW): the "Test Gen" panel view.
   Generates the frame ON DEMAND for the selected system and feeds it through
   the app's OWN CVBS decoder (gui_cvbs_process_buffer) so what you see is
   the test pattern AS DECODED by the production decode chain - not a
   synthetic bitmap. A "System: PAL/NTSC/SECAM" button in the panel overlay
   cycles the system; the frame regenerates and re-decodes on the next render.
4. Panel plumbing: PANEL_VIEW_TESTGEN in gui_app.h's enum, "Test Gen" in
   gui_panel.c's view names + availability (always available - self-
   contained), registration in gui_app_init, meson.build sources.
5. Test build fixes:
   - The gui_stats_layout_harness's LOCAL panel_view_type_name stub had a
     7-entry names array; the new PANEL_VIEW_COUNT (8) made its check loop
     read past the end (access violation, 0xc0000005). Added "Test Gen" to
     the stub's names.
   - test/gui_playback_info_stub.c (NEW): the timeline name budget's
     gui_playback_get_selected_file_info call needed a separate stub TU for
     the three layout harnesses (NOT the audio stub: the playback harnesses
     compile the real gui_playback.c and would collide - multiple definition).

### Not built (later)
- gui_simulated.c channel-A replacement with the hacktv values (the user
  asked for the Test Gen PANEL path first; the generator module is ready for
  the simulated device to adopt).
- Real-data FM for the simulated device channel B (user: "for FM data that
  needs to be based off real data or at least real values" - the plan is
  streaming from a real captured RF file, looping).
- SECAM pre-emphasis / ident lines in the generator (v1 uses plain FM
  chroma; the app's SECAM decoder calibrates its own carrier pair from the
  porch, so ident absence is tolerated).

### Commands run + results
- Full all-target build: exit 0 (after the three fixes above)
- meson test -C build-local --num-processes 4: 45/45 OK

## Not yet verified (needs real user confirmation)

- Real-GUI: mode cycle + hold-press menu behavior; switching to Monitor &
  Remote feels instant (async transition); preview placeholder font (Inter);
  About-window update line on this build.
- Linux drag-and-drop: run the diagnostic build on the Linux machine; if a
  drop attempt produces no "[PLAYBACK] Drop delivered:" stderr line, the
  windowing layer never delivered the event (session/compositor issue);
  a line plus a reject status = app-side. macOS drops cannot work while
  elevated (OS limitation) - use Browse / positional file / Open With.
