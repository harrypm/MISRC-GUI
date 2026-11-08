# Prompt log — 2026-11-08: begin iOS 17-current port

## User request (verbatim)

> git pull and begin making an IOS 17-current port

## Inputs

- Working repo: `/Users/harry/MISRC-GUI` (branch `main`, remote `fork/main`)
- Host: macOS, zsh 5.9, Xcode 26.3 (Build 17C529) installed

## Commands run

- `git -C /Users/harry/MISRC-GUI pull` — fast-forward `9800b27` → `b5a3cf5`,
  1,718 files changed (+578,410 / −7,040). Notable: bundled OpenH264 + Opus
  sources, OpenH264 iOS Xcode projects under
  `third_party/openh264/codec/build/iOS/`, fetched branch
  `capture-server-client-modes`, tags `v1.1.7`…`v1.2.7`.
- `ls -la` repo root — found `android/` port scaffolding + `prompt_logs/`.
- Read: `README.md`, `CMakeLists.txt`, `android/ANDROID_SUPPORT_PLAN.md`,
  `android/build-deps-android.sh`, `android/java/dev/misrc/gui/MainActivity.java`.
- Read `misrc_tools/meson.build` (full pass) — platform branches, dep gates,
  android shared-library target, test gating.
- Verified raylib 5.5 (vendored `.deps/raylib`, tag 5.5):
  - `src/platforms/` has NO iOS backend (android/drm/glfw/sdl/rgfw/web only).
  - `cmake/LibraryConfigurations.cmake:95-97` — `PLATFORM=SDL` →
    `PLATFORM_DESKTOP_SDL` + `find_package(SDL2 REQUIRED)` + links
    `SDL2::SDL2`.
  - `OPENGL_VERSION="ES 3.0"` → `GRAPHICS_API_OPENGL_ES3` (same file, ~107-124).
  - `rcore_desktop_sdl.c`: ES 3.0 context request path (lines ~1847-1851),
    `SDL_TEXTINPUT` → `charPressedQueue` → `GetCharPressed()` (line ~1513),
    `SDL_FINGER*` touch handling (~1578-1590).
- Verified GUI integration points:
  - `gui_ui.c:3230/9302` polls `GetCharPressed()`; `gui_ui.c:53,1083-1088`
    calls `android_set_keyboard_visible()` (Android JNI bridge pattern).
  - `gui_capture.c:54,69` includes `<hsdaoh.h>` / `<hsdaoh_raw.h>`
    unconditionally; vendored headers are libusb-free
    (`third_party/hsdaoh/include/hsdaoh.h` = stddef/stdarg/stdint/stdbool +
    hsdaoh_export.h only; `hsdaoh_raw.h` is a 90-line API header) → an iOS
    stub libhsdaoh is viable with zero gui_capture.c changes.
  - `gui_settings.c` Android path gates (143-146, 228-231, 997-999) to mirror.
  - `gui_updater.c` already `__ANDROID__`-gated (533, 613, 708, 829, 1042).
  - `gui_android_viewport.h` — header-only full-frame pinch-zoom via raylib
    APIs; reusable for iOS touch.
  - `simple_capture_android.c` stub pattern to mirror as `simple_capture_ios.c`.
  - `-Dmain=misrc_capture_main` TU-rename pattern already in meson (line ~728)
    → same trick `-Dmain=SDL_main` for the iOS app entry.
  - `gui_net.c` UDP broadcast discovery → Info.plist needs
    `NSLocalNetworkUsageDescription` on iOS 14+.
- `meson.build` dep map for the GUI: zstd (required), opus (required),
  openh264 (required, vendored v2.6.0 tree), fftw3f (required for GUI),
  soxr (required for Opus audio monitor), hsdaoh (HARD dep for all targets —
  must become optional on iOS), ffmpeg (optional), libusb-driven
  FX3/DdD/RTLSDR auto-off when libusb absent.
- `xcodebuild -version` — Xcode 26.3 present, BUT the Apple SDK license is
  NOT accepted; `xcrun`/`xcodebuild` are blocked pending
  `sudo xcodebuild -license` (user action; not run by the agent).
- No `xcodegen`/`pod`/`xcbeautify` on PATH → manual .app assembly route
  (mirrors android/build-apk.sh no-gradle philosophy).

## Key findings

1. Upstream raylib 5.5 has no iOS backend; the SDL backend
   (`rcore_desktop_sdl.c`) + an iOS-built SDL2 (UIKit/EAGL/ES3) covers
   window, GL ES 3.0 context, touch, text input, and audio (raylib raudio →
   miniaudio → CoreAudio) with no raylib patching required for selection —
   only `PLATFORM=SDL -DOPENGL_VERSION="ES 3.0"`.
2. USB capture (hsdaoh/FX3/DdD/CXADC/RTLSDR) is impossible on iOS (no
   libusb backend; no public IOKit USB). Android's "link-but-gate" approach
   becomes "stub hsdaoh, omit libusb entirely" on iOS.
3. The Android port is the structural template: `android/` scaffolding +
   cross deps prefix + meson cross-file + host_system branch + platform
   bridge file + packaging script.
4. Blocker: Xcode license acceptance must be done by the user before any
   SDK/toolchain invocation works (`sudo xcodebuild -license`).

## Decisions (pending plan approval)

- Rendering: SDL2 2.30.x cross-built for iOS (iphoneos + iphonesimulator
  arm64 slices) + raylib 5.5 `PLATFORM=SDL` `GRAPHICS_API_OPENGL_ES3`.
- Deployment target iOS 17.0; device arm64 + simulator arm64.
- Deps cross prefix: `.deps/install-ios-arm64` (device) and
  `.deps/install-ios-sim-arm64` (simulator), mirroring Android's prefix
  layout and `deps-versions.txt` recording.
- First-release scope mirrors Android's "basic release": GUI renders,
  simulated device + playback paths work, settings persist to app
  Documents, Client/Remote monitor network modes link (network prompt via
  Info.plist), no USB capture.

## User inputs during the session

- Hardware ref for on-device testing: iPhone 13 via USB-3 camera adapter →
  MS2130 (hsdaoh stick) attached to the MISRC. Note: raw RF capture on a
  stock iPhone remains infeasible (no public USB bulk-interface API; the
  OS UVC stack would not preserve the raw pipe) — hardware is for on-device
  GUI/monitor-client testing + a later feasibility probe.
- Xcode account signed in (user statement); `security find-identity` still
  shows 0 codesigning identities — the personal-team cert is generated on
  first Xcode build with a team. Not blocking simulator work (ad-hoc).
- Xcode license was accepted during the session (xcodebuild/xcrun verified
  working: Xcode 26.3, iphoneos/iphonesimulator SDK 26.2, iPhone 17-family
  sims available).

## Execution log (M0–M3)

- Scaffolding written: `ios/gen-cross-file.sh` (per-slice meson cross-file
  + baked pkg-config wrapper), `ios/build-deps-ios.sh`, `ios/Info.plist`,
  `ios/build-app-ios.sh`, `ios/hsdaoh_stub/hsdaoh_stub.c`,
  `misrc_tools/misrc_gui/input/ios_platform.c`,
  `misrc_tools/misrc_capture/simple_capture/simple_capture_ios.c`,
  `ios/sdltest/sdltest.c` (control experiment).
- Code gates: meson.build ios host branch; MISRC_IOS define; touch viewport
  gates extended to iOS in 7 files; gui_settings/gui_ui/gui_updater/misrc_gui
  iOS paths; macOS-only code excluded (gui_cxadc/gui_record/gui_capture blanket
  __APPLE__ && !MISRC_IOS; elevation/osascript path excluded after it proved to
  insta-exit SDL_main on the simulator).
- Deps build fixes (self-correct loop):
  1. Xcode 26 toolchain has no llvm-ar → BSD ar/ranlib/strip from clang dir.
  2. Apple clang rejects `-target=X`/`-isysroot=X` combined forms → split argv.
  3. cmake≥4 vs old deps → -DCMAKE_POLICY_VERSION_MINIMUM=3.5 as -D (toolchain
     file too late).
  4. **openH264 meson installed into /opt/homebrew (meson default prefix!)** —
     polluted the host brew tree incl. Cellar headers. Remediated: removed the
     6 stray files + `brew reinstall openh264` (Cellar + links restored to
     bottle state). Script now passes explicit --prefix.
  5. raylib iOS patches: rlgl.h ES3 → OpenGLES/ES3 headers + _EXT/_OES enum
     aliases (incl. GL_DEPTH_COMPONENT32_OES = raw 0x81A7); raudio.c compiled
     as ObjC (miniaudio includes AVFoundation on iOS).
  6. skip-path unbound var fix (OPENH264_SHA).
- App build fixes: meson warning() `+` concatenator restored;
  _DARWIN_C_SOURCE for iOS (st_mtimespec + pthread QoS); system() gated off
  iOS (clang unavailable-attribute); DEVICE_TYPE_DDD uses gated behind
  ENABLE_DDD (latent upstream bug for any libusb-less build); get_num_cores
  extern/calls gated off iOS (CLI lib not linked there).
- **M3 RESULT: misrc_gui links (76/76) and MISRC.app builds, ad-hoc signs,
  installs and launches on the iPhone 17 Pro simulator (iOS 26.2).**
- Black-screen investigation:
  1. First launches: process alive, screen black, two identical screenshots.
  2. `sample` showed main thread idling in UIApplicationMain — SDL_main never
     on stack → either never called or returned instantly.
  3. Root cause #1: macOS osascript elevation path compiled under plain
     __APPLE__ — on the SIMULATOR posix_spawn('/usr/bin/osascript') SUCCEEDS
     (host user), helper returns 1, main returns 0 instantly. Fixed by gating
     it (and its helper block) to macOS-only. (A stray osascript error line
     later confirmed this path had run.)
  4. Root cause #2 (test procedure): `build-app-ios.sh --no-launch` rebuilds
     but does NOT INSTALL — post-fix launches kept running the OLD binary.
    Install-then-launch showed different content.
  5. Control experiment: minimal SDL2 app (same toolchain/libs/bundle) renders
     full RED — SDL2/UIKit/EAGL path proven good.
  6. Post-fix `sample`: SDL_main RUNNING the frame loop (render_single_panel →
    fft_vtable_render → phosphor_rt_render → glDrawElements every frame),
    RSS 177 MB. App is genuinely alive and drawing.
  7. Pixel analysis of the screenshot: only ONE bright band (y 78–116,
    x 173–1099 ≈ toolbar row) renders; everything below is pure black —
    layout/canvas presentation issue under investigation. User asked to
    visually confirm on the open Simulator window.

## Output artifacts (this session)

- `prompt_logs/2026-11-08_ios17-port_README.md` (this file)
- Plan: "MISRC GUI iOS 17+ Port" (create_plan)
- `ios/` scaffolding + code shims + meson ios branch + build scripts (above)
- `build-ios-sim/MISRC.app` — runnable simulator app
- `ios/screenshot*.png` — launch evidence captures

## Blockers / user actions required

1. `sudo xcodebuild -license` (accept Apple SDK license) — blocks ALL
   builds, SDK queries, simulator use.
2. Real-device testing later needs an Apple ID / dev signing identity in
   Xcode (free personal team is enough for sideload); simulator milestone
   needs nothing beyond (1).
3. Per repo hard rule (2026-11-08 UX log): no UX/layout changes without
   Harry's sign-off — the port must only platform-gate, never alter the
   desktop UX.
