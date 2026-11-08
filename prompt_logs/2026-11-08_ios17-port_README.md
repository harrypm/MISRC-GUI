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

## BLACK-SCREEN ROOT CAUSES (found + fixed 2026-11-08 ~07:45)

Two independent raylib-on-iOS bugs, both desktop-GL assumptions breaking
under iOS/EAGL (both patched idempotently in ios/build-deps-ios.sh):

1. **FBO-0 restore**: under GRAPHICS_API_OPENGL_ES3 rlgl.h also defines
   ES2 + RLGL_RENDER_TEXTURES_HINT, so EndTextureMode→rlDisableFramebuffer
   binds framebuffer 0 — INVALID on iOS (SDL's UIKit view owns the window
   FBO, observed id 1). Everything after the first RT pass drew into the
   void. Fix: capture the real window FBO at SDL_GL_CreateContext and
   restore it at every "return to default" site.
2. **viewRenderbuffer unbind**: rlgl.h rebinds renderbuffer 0 after
   creating an RT's depth RBO. SDL2/UIKit presents via
   `[context presentRenderbuffer:GL_RENDERBUFFER]` and its own comment
   says rebinding viewRenderbuffer is the app's job — binding 0 kills all
   presentation even though the GL framebuffer contains correct content
   (proven by in-app glReadPixels probes: fb had the UI, screen black).
   Fix: capture GL_RENDERBUFFER_BINDING too and restore alongside the FBO.

Supporting findings: Apple clang 17 keeps the macOS triple with only
-isysroot (so __IPHONEOS__ is NEVER defined — patches must use
TargetConditionals' TARGET_OS_IPHONE); raylib windows need
FLAG_FULLSCREEN_MODE on iOS; the macOS osascript elevation path must be
gated off iOS (on the SIMULATOR it spawns host osascript successfully and
exits SDL_main instantly).

Verified state at save point: full GUI (toolbar + Clay UI + panels +
bottom bar) renders on the iPhone 17 Pro simulator (iOS 26.2), confirmed
by pixel analysis (screenshot14.png: 487KB, 67k background px, 8.7k UI px
across y≈600–1900) + luminance map showing the panel grid. Visual
sign-off by Harry still pending (rule: real-world confirmation).

ICON (resolved, user-approved 2026-11-08 ~08:44, commit a76964a): source
is a 500x500 window+stand artwork with transparent corners + drop shadow.
Final treatment (ios/make_icons.py, pure-python PNG codec): complete
original art alpha-flattened over WHITE, scaled to 80% of the icon,
centered — matching the Android APK's legacy-launcher look (it ships the
raw icon and the launcher masks it small on a light bg). Earlier attempts
(crop-to-square letterbox over interior color) read as "mangled" and were
reverted. NOTE: this simulator caches icons across installs — an uninstall
+ reinstall + device reboot is required to see icon changes.

RENDER MODEL (resolved, user-approved 2026-11-08 ~10:41, commits
39b39f8 + e90b785): Android-model layout — Clay layout == the PHYSICAL
letterbox canvas (inset 64pt*dpi from the corner curve), 1:1 blit, no
point-space indirection. iOS UI scale default 200% (fresh-install only,
HUD-tunable). The dpi pre-scale + 150% attempts were reverted (squashed
the layout below the 640x360 minimum). Raylib iOS patches now also fix
the drawable size at init AND in the SIZE_CHANGED handler
(SDL_GL_GetDrawableSize; the stock code renders 1x into a 3x framebuffer).

TOUCH INTERACTION (resolved, user-confirmed "buttons now click"
2026-11-08 ~13:37, commits 7e73cbb + f35aa5c): (1) taps dispatched
IMMEDIATELY on finger lift on iOS — the Android pipeline's 300ms
double-tap-wait was a dead wait on every click (pinch-in already
resets zoom, so double-tap is redundant on iOS); (2) THE device bug:
SDL on iOS hardware intermittently reports a finger at exactly (0,0)
MID-TOUCH (misrc_input.log evidence) — the jump exceeded the drag
threshold so every tap was rejected as a drag (53 REJECT-DRAGs in one
session), and pinch midpoints snapped to (0,0) so zoom never held.
Fix: per-touch-slot last-real-position tracking holds through the
phantom reports. Settings modal: touch-scrollable body + clipped
output-path text (ea7ce1b) + compact spacing (0dc74ef) + 150% default
scale so the whole modal fits like desktop (7acba0e).

BUFFER ALLOCATION on device (resolved, user-confirmed "its now working
on real hardware fine" 2026-11-08 ~14:46, commit 9f5fd18): the iOS
sandbox DENIES POSIX shm (probe: shm_open FAIL errno=1) so the
ringbuffer's memfd double-mirror could never allocate on device
("failed to start extraction" on Test Gen; net-mode apply failed the
same way). Fix path: vm_allocate the backing + a 2x region, then
vm_remap the backing into both halves (copy=FALSE = shared pages).
Two dead ends documented: mach_make_memory_entry_64 works at probe
size (1MB) but FAILS at 512MB on device (rb_init code=3,
misrc_rb.log); plain MAP_SHARED|MAP_ANON works but cannot be aliased.
The vm_remap path is PROVEN on hardware: Test Gen + net cycle + touch
all work on the iPhone 14. rb_init failure instrumentation
(RB_LOG_FAIL -> Documents/misrc_rb.log) still compiled in.

Still open: temporary debug logging to strip (ios_gl_debug_dump,
ios_view_input_log + watcher, RB_LOG_FAIL, rb_probe), network-mode
validation vs a desktop server, CI/docs.

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
