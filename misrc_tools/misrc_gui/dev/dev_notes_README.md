# MISRC GUI development notes

Recent capture regressions showed that small callback-gating changes can silently break GUI feeds. Keep the following constraints in mind when touching capture/parser/audio paths:

- Preserve tolerated-frame behavior in MISRC frame mode: only drop frames when `result.error_count > 0 && result.report_errors`.
  Do not reject tolerated CRC-only frames, or GUI RF feed can stall while CLI still works.
- Keep capture heartbeat updates early in the callback (after buffer/null checks), before width/height early returns.
  This prevents false timeout/reconnect loops when callback activity exists.
- After any `capture_handler_init(&s_capture_handler)` during GUI capture start, explicitly restore audio capture state:
  `atomic_store(&s_capture_handler.capture_audio, true);`
  Without this, audio monitor path (`stream1 -> BUF_CAPTURE_AUDIO -> gui_audio`) remains empty.
- Validate RF and monitor audio as separate end-to-end checks after capture-path edits:
  - RF: waveform/scope feed present and stable.
  - Audio monitor: `Audio Mon` audible and `BUF_CAPTURE_AUDIO` no longer pinned at 0%.
- Prefer minimal, isolated fixes in `frame_parser`, `gui_capture`, `gui_extract`, and `gui_audio`; avoid unrelated UI/settings churn during capture debugging.

## 2026-04-16 capture/runtime snapshot

- Timestamp (UTC): `2026-04-16T03:38:18Z`
- OS: `Linux Mint 21.3`
- System: `Linux 5.15.0-173-generic x86_64 GNU/Linux`
- Branch: `heads/misrc_gui_dev`
- Commit: `48054ea`
- Stability note: current version is running stable for 8+ hours.

## 2026-04-19 local AppImage build note

- Local AppImage builds are now reproducible and passing smoke tests using:
  - `./scripts/build-appimage-local.sh`
- Default mode runs in an `ubuntu:22.04` container (`docker`/`podman`) to keep a portable glibc baseline.
- Script output location:
  - `.ci-artifacts/linux-appimage/`
- Verified locally:
  - AppImage artifact builds successfully.
  - `APPIMAGE_EXTRACT_AND_RUN=1 <AppImage> --smoke-test` passes.
  - Direct run `<AppImage> --smoke-test` passes on host.
## 2026-04-22 macOS Apple Silicon capture scheduling fix

- Symptom: on M-series Macs, capture/recording workloads could remain on efficiency cores, causing immediate drops/errors under GUI load.
- Root cause pattern: process-priority/QoS promotion was happening too late (after stream startup), so transport/ingest workers did not reliably inherit elevated scheduling class.
- Changes made:
  - `misrc_tools/common/threading.h`
    - Apple Silicon maps `THRD_PRIORITY_ABOVE+` to `QOS_CLASS_USER_INTERACTIVE`.
    - Added macOS QoS hinting in `proc_set_priority(...)`.
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - Apply `proc_set_priority(PROC_PRIORITY_ABOVE)` before `sc_start_capture(...)` / `hsdaoh_start_stream(...)`.
    - Roll back to `PROC_PRIORITY_NORMAL` on startup failure and on capture stop.
  - `misrc_tools/misrc_gui/output/gui_record.c`
    - Move FLAC-recording priority promotion to before encoder/worker creation.
    - Restore normal priority on FLAC init failure paths.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - Apply `proc_set_priority(PROC_PRIORITY_ABOVE)` before stream startup.
    - Restore normal priority on shutdown/startup-failure exit paths.
- Validation:
  - GUI soak run (`--debug-view`) for ~331.9s showed:
    - `waits=0`, `drops=0`
    - record buffers: A waits/drops `0/0`, B waits/drops `0/0`
    - no capture/dropout instability lines during soak window.

## 2026-04-22 macOS scheduling regression repair (post-rebase)

- Issue: after rebasing to `main`, a conflict-resolution mistake in `misrc_tools/common/threading.h` weakened macOS QoS escalation for capture-critical threads and reduced the effectiveness of Apple Silicon core placement.
- Corrective changes:
  - `misrc_tools/common/threading.h`
    - restored clean separation between `thrd_set_priority(...)` and `proc_set_priority(...)` QoS logic.
    - strengthened macOS QoS calls by adding non-zero relative priority for `ABOVE/HIGH/CRITICAL` levels.
    - added Mach thread precedence (`THREAD_PRECEDENCE_POLICY`) alongside QoS for capture-critical caller threads, avoiding blanket process-wide escalation of unrelated threads.
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - move `proc_set_priority(PROC_PRIORITY_ABOVE)` earlier in HSDAOH startup (before `hsdaoh_open`/`hsdaoh_alloc`/`hsdaoh_open2`) so early transport/open threads inherit elevated scheduling.
    - rollback to `PROC_PRIORITY_NORMAL` on all HSDAOH open/alloc failure exits.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - mirror earlier process-priority elevation for CLI HSDAOH path before `hsdaoh_alloc/open2`, with rollback on failure exits.
    - removed accidental early option-parse priority side effect so elevation only happens at real capture startup intent.

## 2026-04-22 macOS callback-thread scheduling follow-up

- Issue: callback-priority promotion was previously guarded by a single process-wide one-shot flag, so only the first callback thread was guaranteed to be elevated.
- Corrective changes:
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - changed callback promotion to thread-local one-shot (`MISRC_THREAD_LOCAL`) so each callback worker thread self-promotes once to `THRD_PRIORITY_CRITICAL`.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - mirrored the same thread-local callback-promotion behavior in CLI capture callback path.
  - `misrc_tools/common/threading.h`
    - in macOS `proc_set_priority(...)`, return early when `pthread_set_qos_class_self_np(...)` succeeds (after setting caller-thread precedence), avoiding unnecessary fallthrough into process `nice` fallback that can fail with EPERM on non-root runs.
- Runtime check (privileged CLI path):
  - successful `hsdaoh` capture runs with `waits=0`, `rf_drops=0`, `audio_drops=0`.
  - sampled `powermetrics --samplers cpu_power` during capture showed sustained higher P-cluster activity than E-cluster activity.
## 2026-05-03 parser CRC mismatch root-cause note
- Symptom: persistent parser error bursts with stable low counts (`13/14/15`) even when capture backpressure remained clean.
- Debug evidence: per-frame parser diagnostics showed `idle=0` and `total==crc` for all observed bursts, confirming CRC-only mismatches.
- Root cause: MISRC shared parser (`misrc_tools/common/frame_parser.c`) diverged from upstream hsdaoh CRC behavior by masking trailer/stream-id high nibbles before `crc16_ccitt(...)`.
- Upstream reference (`.deps-gha-local/hsdaoh/src/libhsdaoh.c`) computes CRC over raw line bytes without this masking.
- Corrective direction: compute CRC on raw line bytes in shared parser to align verifier input with upstream transport format, then revalidate mismatch rate with `MISRC_DEBUG=1` capture runs.

## 2026-08-13 Android arm64-v8a APK support (android-support branch)

- Target: basic, launchable Android 11+ (API 30) `arm64-v8a` APK via NativeActivity + raylib `rcore_android.c`, cross-compiled with NDK r25c on an x86_64 Linux host. Full plan in `android/ANDROID_SUPPORT_PLAN.md`.
- Toolchain: Android SDK cmdline-tools + build-tools;34.0.0 + platforms;android-30/34 + NDK 25.2.9519653 installed to `~/Android/Sdk`; `ANDROID_HOME`/`ANDROID_NDK_HOME`/`NDK_HOME` persisted to `~/.bashrc`.
- Build flow (all reproducible via scripts under `android/`):
  1. `android/build-deps-android.sh` cross-builds 7 static deps into `.deps/install-android-arm64` (libFLAC 1.5.0, FFTW 3.3.10 fftw3f, libsoxr 0.1.3, libusb 1.0.27, libuvc, vendored hsdaoh, raylib 5.5 PLATFORM=Android GLES 3.0). Versions + SHAs recorded in `android/deps-versions.txt`.
  2. `android/gen-cross-file.sh` emits the Meson cross-file `android/aarch64-linux-android.ini` from `$ANDROID_NDK_HOME` + `$DEPS_PREFIX`; `android/android-pkg-config` is a self-contained cross pkg-config wrapper (DEPS_PREFIX-only, no host .pc leak).
  3. `PKG_CONFIG=android/android-pkg-config meson setup --cross-file android/aarch64-linux-android.ini -Dbuildtype=release build-android misrc_tools` then `meson compile -C build-android misrc_gui` produces `build-android/libmisrc_gui.so`.
  4. `android/build-apk.sh` packages `libmisrc_gui.so` + `libhsdaoh.so` + manifest + icon into a debug-signed, zipaligned APK at `.ci-artifacts/android-apk/`.
- Code changes (see `git show` on the android-support commit for the full diff):
  - `misrc_tools/meson.build`: `android` host branch — GUI built as `shared_library('misrc_gui')` (NativeActivity loads .so), CLI executables gated off, link `-lEGL -lGLESv2 -lGLESv3 -landroid -llog -lOpenSLES -lm -ldl` (no -lpthread/-lX11/-lGL), `-DGRAPHICS_API_OPENGL_ES3` added to cflags.
  - `misrc_tools/misrc_capture/simple_capture/simple_capture.h` + new `simple_capture_android.c`: `__ANDROID__` branch checked BEFORE `__linux__` (NDK clang defines both) + no-device `sc_*` stub (CXADC/V4L2 out of scope for basic release).
  - `misrc_tools/misrc_gui/core/gui_settings.c`: `__ANDROID__` storage paths -> `/sdcard/Android/data/dev.misrc.gui/files/` (no HOME/Desktop on Android).
  - `misrc_tools/common/shm_anon.h`: `__ANDROID__` branch uses bionic's native `memfd_create` (API 30), not the glibc syscall shim.
  - `misrc_tools/common/flac_writer.c`: FLAC thread-affinity block gated to `__linux__ && !__ANDROID__` (bionic lacks `pthread_setaffinity_np`).
  - `.github/workflows/build.yml` + `misrc_tools/test/ci_guard_tests.py`: `android-apk` CI job + Android packaging-mirror guard.
- Entry-point contract (risk #2 resolved by reading `rcore_android.c:269-279`): raylib 5.5 defines `android_main()` which calls the app's standard `main()` — the existing `misrc_gui.c` main() is reused UNCHANGED. Do NOT add an `android_main` shim (would collide with raylib's).
- Bugs found & fixed during the cross-build (all verified against hard data, not assumed):
  - raylib 5.5 `rlgl.h:1908-1909` has inverted ES3/non-ES3 branches in `rlActiveDrawBuffers()`: under `GRAPHICS_API_OPENGL_ES3` it called `glDrawBuffersEXT` (an ES2 extension symbol absent on GLES3) instead of `glDrawBuffers` (GLES3 core). Patched via sed in `build-deps-android.sh` to use `glDrawBuffers`. Upstream raylib bug.
  - raylib CMake install emits a Desktop-style `raylib.pc` with `Requires.private: glfw3` (wrong for PLATFORM=Android — rcore_android.c uses EGL/ANativeWindow, no glfw). With the cross pkg-config wrapper forcing `--static`, this made `pkg-config --static raylib` fail and Meson fell back to CMake, picking up a HOST x86_64 `/usr/local/lib/libraylib.a` (arch-mismatch leak). Fixed by dropping the glfw3 require + setting Android `Libs.private` in the deps script.
  - hsdaoh hard-requires libuvc (`CMakeLists.txt:132` FATAL_ERROR) — libuvc was not in the original plan's 6 deps. Added a libuvc cross-build block.
  - hsdaoh `src/CMakeLists.txt:143` links `hsdaoh_test` with `-lrt`, which does not exist on Android (clock_gettime is in libc, like macOS). sed-patched the build copy to add `ANDROID` to the no-rt branch.
  - Meson 0.61.2 reads machine info from `[host_machine]`, NOT `[properties]` — the cross-file must set `system = 'android'` under `[host_machine]` or `host_system` is misreported as `linux` and the linux branch adds `simple_capture_v4l2.c` (no V4L2 headers on Android -> compile cascade failure).
  - Cross pkg-config wrapper must be self-contained (derive `DEPS_PREFIX` from its own location): Meson invokes the binary without the shell env, so requiring `DEPS_PREFIX` as an env var broke dep resolution.
- Verification (hard data):
  - `build-android/libmisrc_gui.so`: 16.2 MB, ELF 64-bit ARM aarch64 shared object.
  - `llvm-nm -D`: `ANativeActivity_onCreate` exported (T) — NativeActivity entry; `android_main` + `main` both exported.
  - `llvm-readelf -d` NEEDED: libEGL.so, libGLESv2.so, libGLESv3.so, libandroid.so, liblog.so, libm.so, libdl.so, libhsdaoh.so, libc.so — all Android sysroot. Zero host x86_64 lib leaks (`/usr/lib/x86_64`/`/usr/local/lib` grep count = 0).
  - APK `misrc_gui-v1.0.7-3-gaf76387-dirty-android-arm64.apk` (17 MB): `lib/arm64-v8a/libmisrc_gui.so` + `libhsdaoh.so` at correct path, both ARM aarch64; `aapt2 dump badging` -> `sdkVersion:'30'`, `targetSdkVersion:'34'`, `package: name='dev.misrc.gui'`; `apksigner verify` v3 scheme TRUE.
  - All 7 cross deps verified ARM aarch64 via `file` on extracted `.o` objects from each static archive.
  - `python3 misrc_tools/test/ci_guard_tests.py --static-only`: 22/22 PASS (including new Android packaging assertions).
- NOT yet validated (separate milestones, per plan out-of-scope):
  - Real-device install + GUI render + simulated-device selection (needs a physical Android 11+ arm64 device; cannot verify on this host).
  - USB host capture (hsdaoh/FX3/DdD) on Android — risk #1; libusb Android backend needs root or a JNI USB-Host bridge. Connect path is runtime-gated off in the basic release.
- Key constraints to preserve when touching Android build paths:
  - Keep `[host_machine] system = 'android'` in the cross-file; do not move it to `[properties]` (Meson 0.61.x ignores it there and misreports host_system as linux).
  - Keep the `__ANDROID__` branch in `simple_capture.h` BEFORE `__linux__` (NDK defines both).
  - Keep `-DGRAPHICS_API_OPENGL_ES3` in the android cflags — it must match the raylib cross-build's GRAPHICS or `rlActiveDrawBuffers` re-introduces the `glDrawBuffersEXT` link error.
  - Do not add `-lpthread` to android link flags (bionic pthread is in libc; -lpthread fails to link).
  - Keep the raylib.pc + rlgl.h sed-patches in `build-deps-android.sh` (run unconditionally so reruns stay correct).
