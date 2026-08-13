# MISRC Android Support — Implementation Plan

Branch: `android-support` (display name "Android Support")
Base: `refs/heads/main` @ `08881be` ("FX3 ADC integration + HSDAOH/MISRC regression fix + CI post-build guards (#17)")
Target: basic, launchable **Android 11+ (API 30) `.apk`** for **`arm64-v8a` (aarch64-linux-android)**, built from the existing ARM64 MISRC GUI code.

> Status: **implemented and locally verified (2026-08-13)** on the `android-support` branch. SDK + NDK r25c installed, 7 cross deps built, `libmisrc_gui.so` cross-compiled (ARM aarch64, `ANativeActivity_onCreate` exported, zero host lib leaks), and a debug-signed/zipaligned APK produced and verified (`apksigner verify` v3 TRUE, `sdkVersion:'30'`, `targetSdkVersion:'34'`, `package: dev.misrc.gui`). CI job + packaging-mirror guard added. NOT yet validated on a physical device (GUI render / USB capture are separate later milestones). See `misrc_tools/misrc_gui/dev/dev_notes_README.md` 2026-08-13 entry for full details.
>
> Original status (superseded): **plan only**. No Android SDK/NDK was installed on the dev host, so the APK could not be built or verified locally in that session.

## Goal and scope

Produce a minimal but real Android build of `misrc_gui` that:
- Launches as a NativeActivity app and shows the existing raylib + Clay GUI.
- Targets `arm64-v8a`, `minSdkVersion=30` (Android 11), `targetSdkVersion=34`.
- Cross-compiles the current GUI C sources unchanged where possible, using NDK `aarch64-linux-android-clang`.
- Ships as a signed, zipaligned `.apk` (debug-signed for the first release; release-signing keys supplied later).

**In scope for the "basic" first release:**
- GUI renders on Android via raylib's `rcore_android.c` (EGL + ANativeWindow).
- Simulated device capture path (`misrc_tools/misrc_gui/input/gui_simulated.c`) works as a no-hardware demo.
- Settings load/save to app-private storage.
- `--smoke-test` equivalent runs under NativeActivity for CI validation.

**Explicitly out of scope for the basic release (to be revisited):**
- Real hsdaoh/MS2130 USB capture on Android (USB Host permission model + libusb Android backend — see Risks).
- CXADC/V4L2, FX3, DdD backends (no V4L2/libusb on Android without a backend).
- Audio monitoring playback (miniaudio OpenSL ES/AAudio wiring).
- Google Play release signing + store listing.

## Current state (verified by inspecting the codebase)

- Build system: Meson (`misrc_tools/meson.build`) is the primary build; `CMakeLists.txt` is an alternate GUI-only path. CI uses Meson.
- GUI entry: standard `int main(int argc, char **argv)` at `misrc_tools/misrc_gui/core/misrc_gui.c:322`. macOS elevation (`osascript`/`posix_spawn`) is `#if defined(__APPLE__)`; Windows console attach is `#if defined(_WIN32)`. Neither affects Android.
- raylib: CI clones raylib **5.5** (`.github/workflows/build.yml:202,530,734,906`). Vendored source copy at `.deps/src-appimage-local/raylib` includes `src/platforms/rcore_android.c` (NativeActivity + EGL + ANativeWindow) — Android is a supported raylib platform.
- hsdaoh: `third_party/hsdaoh` is CMake-based; `hsdaoh.pc` declares `Libs.private: -lusb-1.0` — hsdaoh depends on **libusb-1.0**.
- Optional deps and their gates in `misrc_tools/meson.build`:
  - libFLAC **>= 1.5.0** hard-required for multithreaded encode (`meson.build:177-179`); `LIBFLAC_ENABLED`.
  - FFTW3f **required** for `misrc_gui` (`meson.build:324-326`); `LIBFFTW_ENABLED`.
  - libsoxr optional; `LIBSOXR_ENABLED`.
  - libusb-1.0 enables FX3 + DdD (`meson.build:232-269`); `ENABLE_FX3`/`ENABLE_DDD`.
  - ALSA (CXADC audio) is `LIBASOUND_ENABLED`; only enabled when `host_system == 'linux'` (`meson.build:198-210`).
- Platform-specific sources selected by `host_system`:
  - Linux: `misrc_capture/simple_capture/simple_capture_v4l2.c` + `-lX11 -lGL -lpthread -ldl -lrt` (`meson.build:298,407-408`).
  - Windows: `simple_capture_mediafoundation.c` + MediaFoundation libs.
  - macOS: `simple_capture_avfoundation.m` + Cocoa/AVFoundation frameworks.
  - **No Android variant exists.** V4L2 and X11 are unavailable on Android; raylib uses ANativeWindow instead of X11.
- NASM `extract.asm` is x86_64-only (`meson.build:81`, `373-375`) and is already skipped for arm64 — no change needed for Android arm64.
- Threading layer `misrc_tools/common/threading.h` wraps pthreads (`pthread_create`), available in the NDK.
- No existing Android/NDK references in the project sources (grep found only raylib-internal `__ANDROID__` paths inside `.deps/src-appimage-local/raylib`).

## Target architecture

- **ABI:** `arm64-v8a` (`aarch64-linux-android24` toolchain; `minSdkVersion=30`).
- **Windowing:** raylib `rcore_android.c` → EGL 1.5 + OpenGL ES 3.0 + `ANativeWindow`. No X11/GLX.
- **Entry point:** NativeActivity. raylib's android backend supplies the `ANativeActivity onCreate` entry and invokes the app's `main` via its native_app_glue wrapper; the existing `main()` in `misrc_gui.c:322` is reused as-is if raylib's wrapper maps `main`→`android_main`, otherwise `main` is renamed/gated to `int android_main(int argc, char *argv[])` under `#if defined(__ANDROID__)`. **Must be validated against raylib 5.5 `rcore_android.c` before finalizing** (see Risks).
- **Packaging:** APK via `aapt2` + `d8` + `zipalign` + `apksigner` (no Gradle required if using a plain NDK + SDK build-tools flow), or a Gradle wrapper project for CI convenience.

## Work breakdown

### 1. NDK cross-toolchain + Meson cross-file
- Install Android NDK (r25c or newer) and SDK `build-tools` + `platforms;android-30`.
- Add `android/aarch64-linux-android.ini` Meson cross-file pointing at:
  - `c = '$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang'`
  - `ar`, `strip`, `nm` from the NDK LLVM toolchain.
  - `sysroot` = NDK sysroot.
  - `[built-in options]` c_args/ld_args for `-DANDROID -D__ANDROID_API__=30 -fPIC -pthread` and `-landroid -lEGL -lGLESv3 -llog`.

### 2. Cross-compile dependencies for `arm64-v8a` into `.deps/install-android-arm64`
Mirror the existing `.deps/install` vendored-dep pattern (see `meson.build:126-150`) but with an Android prefix:
- **libFLAC 1.5.0**: CMake cross-build with the NDK toolchain file. Must produce `flac.pc` reporting `1.5.0` (the `meson.build:177-179` hard gate will fail configure otherwise).
- **FFTW3f**: CMake cross-build (`-DBUILD_SHARED_LIBS=OFF`, disable MPI/openmp).
- **libsoxr**: CMake cross-build.
- **libusb-1.0**: CMake cross-build with the **Android backend** enabled (`-DPLATFORM=android` or the libusb android os events backend). **High-risk** — see Risks.
- **hsdaoh**: CMake cross-build against the Android libusb. May need patching if hsdaoh uses Linux-only `libusb_set_option`/udev paths; `INSTALL_UDEV_RULES=OFF` already used in CI (`build.yml:222,535,739,913`).
- **raylib 5.5**: CMake cross-build with `-DPLATFORM=ANDROID -DANDROID_ARCH=arm64 -DANDROID_API_VERSION=30 -DANDROID_NDK=...`. Produces `libraylib.a` built on `rcore_android.c`.

Record exact versions + SHAs in `android/deps-versions.txt` so the Android artifact is reproducible and can be byte-compared like the existing CI mirror rules.

### 3. Meson: add an Android host branch
Edit `misrc_tools/meson.build`:
- Add `android` handling next to the `linux`/`windows`/`darwin` blocks (`meson.build:272-304` and `399-413`):
  - Do **not** add `simple_capture_v4l2.c` (no V4L2 on Android). Provide a stub `simple_capture/simple_capture_android.c` exposing the `sc_*` symbols used by `common/device_enum.c` and `misrc_gui/input/gui_cxadc.c` (grep `ingest_`/`sc_get_*`) so linking succeeds with CXADC disabled.
  - Replace `-lX11 -lGL -lGL` with `-lEGL -lGLESv3 -landroid -llog -lao` (OpenSL ES for audio later).
  - Keep `LIBASOUND_ENABLED=0` (ALSA is linux-only; Android is not `host_system == 'linux'` under the cross-file — set `host_machine.system()` to `'android'`).
  - Disable FX3/DdD unless the Android libusb backend proves usable; gate with a new `android_usb_enabled` flag.
- Add `android` to the `host_cpu_family == 'aarch64'` NASM guard exclusion (NASM must not run on Android arm64; `meson.build:81,373-375` already skips non-x86_64, so this is a no-op but should be asserted).
- GUI `win_subsystem`: leave as console (irrelevant on Android; NativeActivity is set via manifest, not linker subsystem).

### 4. Code: Android platform shims
- **Entry point** (`misrc_gui.c:322`): if raylib 5.5 `rcore_android.c` does not call `main` directly, add:
  ```c
  #if defined(__ANDROID__)
  int android_main(int argc, char *argv[]) { return main(argc, argv); }
  #endif
  ```
  (Final form to be confirmed by reading `rcore_android.c`.)
- **Window flags** (`misrc_gui.c:379-388`): `FLAG_WINDOW_RESIZABLE` is a no-op on Android; `FLAG_MSAA_4X_HINT` + `FLAG_VSYNC_HINT` are valid. Default 1425x720 is ignored — raylib uses the full ANativeWindow. No code change expected, but verify no `SetWindowSize` is called before `InitWindow` on Android.
- **Output path** (`gui_settings.c`, `gui_record.c`): desktop defaults use `~/Desktop`-style paths. On Android, default `output_path` to the app's external files dir passed in from Java (`getExternalFilesDir(null)`), or `/sdcard/Android/data/dev.misrc.gui/files/` as a static fallback. Gate under `#if defined(__ANDROID__)`.
- **Metadata temp-file gate**: the recently-added `s_record_embed_metadata_requested` gate in `gui_record.c` is desktop-only behavior and is unaffected; no Android change.
- **POSIX APIs**: `clock_gettime`/`nanosleep`/`pthread`/`aligned_alloc` are all available in NDK API 30 with `-D_POSIX_C_SOURCE=200809L` (already set in `meson.build:16,78`). No change expected.
- **Disabled backends**: ensure `gui_app_enumerate_devices` on Android only lists the simulated device (and, if USB works, hsdaoh). `gui_capture.c` connect path for hsdaoh must either work via Android libusb or be gated off with a clear "USB capture not supported in this build" status message.

### 5. APK packaging
Add `android/` scaffolding:
- `AndroidManifest.xml`: `minSdkVersion=30`, `targetSdkVersion=34`, `android:label="MISRC"`, `android:hasCode="true"` (small Java stub for NativeActivity + USB permission intent), activity `android:name="com.raylib.GameActivity"` or a custom `NativeActivity` with `android.app.lib_name="misrc_gui"`.
- `java/.../MainActivity.java` (or use raylib's supplied `game_activity`): forwards `getExternalFilesDir` to native, handles USB device attach intents (`ACTION_USB_DEVICE_ATTACHED`) for later hsdaoh support.
- `res/` + `assets/` (icon from `assets/Icons/MISRC_Icon.png`).
- Build script `android/build-apk.sh`:
  1. `aapt2 link --manifest AndroidManifest.xml -I platforms/android-30/android.jar -o base.apk`
  2. `d8` dex the Java stub → `classes.dex`
  3. zip `lib/arm64-v8a/libmisrc_gui.so` + `classes.dex` + `res/` into the APK
  4. `zipalign -p -f 4 base.apk aligned.apk`
  5. `apksigner sign --ks debug.keystore aligned.apk` (debug keystore generated via `keytool`)
- `android/debug.keystore` generation step (do **not** commit the keystore; generate in CI or document `keytool -genkey`).

### 6. CI (preferred build path)
Add a job to `.github/workflows/build.yml` modeled on `linux-appimage` (`build.yml:75-430`):
- Runner: `ubuntu-22.04` (x86_64 host cross-compiling to arm64).
- Steps: install NDK r25c + SDK build-tools + `platforms;android-30`; cross-build deps (step 2) into `.deps/install-android-arm64`; `meson setup --cross-file android/aarch64-linux-android.ini build-android misrc_tools`; `meson compile -C build-android misrc_gui`; run the APK packaging script; upload `android_MISRC_<version>_arm64.apk`.
- Add an Android packaging-mirror check to `misrc_tools/test/ci_guard_tests.py` consistent with the existing CI mirror rules (assert the APK's `libmisrc_gui.so` links only vendored deps, not system libs).

## Risks and unknowns (must be validated, not assumed)

1. **hsdaoh/libusb on Android (highest risk).** hsdaoh requires libusb-1.0. libusb's Android backend historically needs either a rooted device (raw `/dev/bus/usb` access) or a Java USB Host bridge via JNI. **Unverified** whether the current vendored libusb builds cleanly with `-DPLATFORM=android` against NDK API 30, or whether hsdaoh's `hsdaoh_open()` works without root. Mitigation for the basic release: ship **without** hsdaoh/FX3/DdD capture (simulated-only) and document USB capture as a follow-up task.
2. **raylib 5.5 Android entry contract.** Whether `rcore_android.c` calls the app's `main` directly or expects `android_main` needs to be read from `.deps/src-appimage-local/raylib/src/platforms/rcore_android.c` before finalizing the entry shim. Incorrect entry = silent native crash on launch.
3. **OpenGL ES vs desktop GL.** raylib on Android uses GLES 3. The GUI/visualization shaders (phosphor, FFT, oscilloscope) must be GLES-compatible. `gui_phosphor_rt.c`, `gui_fft.c`, `gui_oscilloscope.c` may use desktop GL calls. **Unverified** — needs a GLES compat audit.
4. **Clay + raylib renderer.** `misrc_tools/misrc_gui/ui/clay_renderer_raylib.c` uses raylib draw calls; should be backend-agnostic, but text rendering via `LoadFontFromMemory` (`misrc_gui.c:401,411`) must work under GLES. Unverified.
5. **No local NDK.** Cannot build/verify on this host in this session. First verification is CI (or local after NDK install). Any claim of "it builds/works" before a CI green run is **invalid** per the existing CI mirror rules.
6. **Meson `host_system` for Android.** Meson may report the NDK target as `linux` unless the cross-file forces `system = 'android'`. If misreported, the Linux branch (`meson.build:297-299`) would try to add V4L2 + `-lX11` and fail. The cross-file must set the machine triplet to `android`/`aarch64`.

## Verification (honest, no results claimed yet)

- **Cannot verify locally** without NDK + SDK. Do not report a local build as evidence.
- Verification plan once CI exists:
  1. CI job compiles `misrc_gui` for `arm64-v8a` and produces a signed APK.
  2. `apksigner verify` passes.
  3. `aapt2 dump badging` shows `sdkVersion:'30'` and `nativeCode: 'arm64-v8a'`.
  4. Install on an Android 11+ arm64 device (or emulator) and confirm the GUI window renders and the simulated device path can be selected.
  5. Real-device USB capture test is a **separate, later** milestone (depends on risk #1).

## Pickup checklist (for the next session)

1. Read `.deps/src-appimage-local/raylib/src/platforms/rcore_android.c` to confirm the entry-point contract (risk #2) and the exact CMake flags raylib 5.5 expects for `PLATFORM=ANDROID`.
2. Install NDK r25c + SDK build-tools 34 + `platforms;android-30` locally, or stand up the CI job.
3. Cross-build the six deps (libFLAC 1.5.0, FFTW3f, libsoxr, libusb, hsdaoh, raylib) into `.deps/install-android-arm64` and record versions/SHAs.
4. Add the `android` branch to `misrc_tools/meson.build` + the `simple_capture_android.c` stub.
5. Add the entry shim + storage-path gate in `misrc_gui.c` / `gui_settings.c`.
6. Add `android/` APK scaffolding (manifest, Java stub, `build-apk.sh`).
7. Add the CI job + packaging-mirror guard test.
8. First green CI APK → real-device smoke test.

## Files to be added/modified (planned, not yet changed)

- New: `android/ANDROID_SUPPORT_PLAN.md` (this file)
- New: `android/aarch64-linux-android.ini` (Meson cross-file)
- New: `android/AndroidManifest.xml`
- New: `android/java/dev/misrc/gui/MainActivity.java`
- New: `android/build-apk.sh`
- New: `android/deps-versions.txt`
- New: `misrc_tools/misrc_capture/simple_capture/simple_capture_android.c` (stub)
- Modified: `misrc_tools/meson.build` (android host branch)
- Modified: `misrc_tools/misrc_gui/core/misrc_gui.c` (entry shim, android storage path)
- Modified: `misrc_tools/misrc_gui/core/gui_settings.c` (android default output path)
- Modified: `.github/workflows/build.yml` (android-apk job)
- Modified: `misrc_tools/test/ci_guard_tests.py` (android packaging-mirror guard)
