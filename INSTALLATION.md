# Building and installing MISRC GUI from source

End users should grab prebuilt binaries from the [releases page](https://github.com/harrypm/MISRC-GUI/releases). This document is for building from source (development, custom toolchains, or platforms without a prebuilt release).

## Prerequisites

### Windows
- [MSYS2](https://www.msys2.org/) with the `UCRT64` environment (MINGW64 is deprecated by MSYS2; CI uses UCRT64) and these packages:
  `mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-fftw mingw-w64-ucrt-x86_64-flac mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libusb mingw-w64-ucrt-x86_64-libsoxr mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-nasm mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf`
- Python 3 (for Meson; the bootstrap script auto-installs user-level `meson` + `ninja` if missing)
- `nasm` (provided by the MSYS2 package above)

### Linux (Debian/Ubuntu)
`cmake git meson ninja-build nasm pkg-config libfftw3-dev libflac-dev libusb-1.0-0-dev libuvc-dev libsoxr-dev libasound2-dev libgl1-mesa-dev libx11-dev libxcursor-dev libxi-dev libxinerama-dev libxrandr-dev`

The UI uses installed fonts for characters missing from its embedded fonts.
For Chinese, Japanese and Korean filenames on Linux, install `fontconfig` and
`fonts-noto-cjk` (or another CJK font). Windows uses its installed system fonts;
install the corresponding language fonts if needed. Font coverage affects only
display, not the UTF-8 paths used to read and write files.

### macOS (Homebrew)
`cmake fftw flac libusb libuvc libsoxr meson nasm ninja pkgconf`

> libFLAC >= 1.5.0 is required for multithreaded FLAC encode. On Ubuntu 22.04 the apt `libflac-dev` is stuck at 1.3.3, so use the prebuilt cache flow or build 1.5.0 from source (CI does this — see `.github/workflows/build.yml`).

## Local build quick start

### Windows (PowerShell)

If local builds fail because `meson` is missing from `PATH`, bootstrap the toolchain first:

```powershell
pwsh -File scripts/build-local.ps1 -BootstrapOnly
```

That command auto-installs user-level `meson` + `ninja` (when missing) and validates the local toolchain connection.

Then run the build:

```powershell
pwsh -File scripts/build-local.ps1
```

### Linux/macOS (bash)

```bash
scripts/build-local.sh
```

Both entry points prepare the source dependencies on first run and reuse them afterward.

The desktop build scripts compile RTL-SDR into the dependency prefix. Release
CI also builds RTL-SDR for its desktop and Android targets. Meson prepares
liba52 when building the RF preview targets. These two dependencies use pinned
upstream archives, verified with SHA-256 before the repository patches are
applied. The first build needs network access or a populated archive cache.
The default cache is `.deps/downloads`. Set `MISRC_DEP_CACHE` to use another
cache directory.

## Vendored deps caching model (local == CI)

hsdaoh, libuvc, and raylib are not available as system packages on all platforms, so they are built from source into `.deps/install` (mirroring the CI `windows-exe` / `linux-appimage` / `macos-app-build` deps blocks). The local dependency scripts store a hash of their inputs in `.deps/install/.build-stamp`. It includes the hsdaoh sources, raylib and libuvc versions, installed dependency versions, and the RTL-SDR source pin and patch. A matching stamp allows the prefix to be reused.

- **Windows**: `scripts/build-local.ps1` auto-invokes `scripts/build-deps-windows.sh` (via MSYS2 UCRT64) on first run or when inputs change. No manual deps step needed.
- **Linux/macOS**: `scripts/build-local.sh` auto-invokes `scripts/build-deps-unix.sh` on first run or when inputs change.
- **CI**: `actions/cache@v4` on `.deps/install` (keyed on `hashFiles(third_party/hsdaoh/**)` + raylib/libuvc versions) skips the rebuild on cache hit.
- **Prebuilt publishing**: `scripts/publish-deps-cache.sh <platform> <arch>` packages `.deps/install` into a tar.xz + sha256 for upload to `harrypm/MISRC-ci-cache` (mirrors the existing libFLAC cache flow), so cold starts can download instead of compiling.

The local==CI contract is enforced by `python misrc_tools/test/ci_guard_tests.py --static-only`.

## Output

- Windows: `build-local/misrc_gui.exe`
- Linux/macOS: `build-local/misrc_gui`

Run `misrc_gui --smoke-test` to verify the binary is functional (same assertion CI uses).
