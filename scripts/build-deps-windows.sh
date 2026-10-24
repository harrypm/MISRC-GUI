#!/usr/bin/env bash
# Build vendored deps into .deps/install for a local Windows (MSYS2 UCRT64) build.
# Mirrors the deps-build block of the .github/workflows/build.yml `windows-exe` job
# so local == CI for hsdaoh + libuvc. raylib/fftw3f/flac/soxr/libusb are reused
# from the pacman UCRT64 install (identical versions to CI's MSYS2 setup) to
# avoid redundant recompiles.
#
# Idempotent: skips work whose stamp matches the current inputs, so repeated runs
# or new terminal sessions reuse .deps/install until an input changes.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DEPS_PREFIX="${DEPS_PREFIX:-$REPO_ROOT/.deps/install}"
HSDAOH_SOURCE_DIR="${HSDAOH_SOURCE_DIR:-$REPO_ROOT/third_party/hsdaoh}"
# Pinned to the latest libuvc release tag (v0.0.7) so the deps cache key is
# reproducible. CI's windows-exe job clones `main` (drifting); step 4 of the
# plan updates CI to this same tag so local == CI. Bump here + in CI together.
LIBUVC_REF="${LIBUVC_REF:-v0.0.7}"
RAYLIB_TAG="${RAYLIB_TAG:-5.5}"

# MSYS2 UCRT64 must be on PATH (caller ensures this, but enforce for safety).
# MINGW64 is deprecated by MSYS2; UCRT64 is the CI toolchain since run
# 37035393223's migration, so it is preferred. mingw64 stays as a fallback.
if [[ -z "${MINGW_PREFIX:-}" ]]; then
  if [[ -d /ucrt64 ]]; then MINGW_PREFIX=/ucrt64
  elif [[ -d /mingw64 ]]; then MINGW_PREFIX=/mingw64
  else echo "[build-deps-windows] ERROR: no MSYS2 ucrt64/mingw64 found" >&2; exit 1; fi
fi
export PATH="$MINGW_PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig:$MINGW_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
# Force mingw-w64 GCC so CMake uses the Windows-GNU platform (which finds
# windres for the RC language). Without this, CMake can pick a Clang from the
# inherited Windows PATH and fail with "No CMAKE_RC_COMPILER could be found."
export CC=gcc
export CXX=g++
export RC=windres
CMAKE_TOOLCHAIN_ARGS=(-DCMAKE_C_COMPILER=gcc -DCMAKE_RC_COMPILER=windres)

log()  { printf '[build-deps-windows] %s\n' "$*"; }
fail() { printf '[build-deps-windows] ERROR: %s\n' "$*" >&2; exit 1; }

mkdir -p "$DEPS_PREFIX/lib/pkgconfig" "$DEPS_PREFIX/include" "$REPO_ROOT/.deps"

# --- Stamp: rebuild only when inputs change ---------------------------------
stamp_file="$DEPS_PREFIX/.build-stamp"
compute_stamp() {
  {
    echo "libuvc_ref=$LIBUVC_REF"
    echo "raylib_tag=$RAYLIB_TAG"
    # Toolchain identity: switching MINGW64 <-> UCRT64 changes the CRT the
    # static libs are built against, so the stamp must bust on a switch
    # (mirrors the CI cache key naming the ucrt64 toolchain).
    echo "msystem_prefix=${MINGW_PREFIX:-unknown}"
    # hsdaoh source tree (the thing that actually matters for MISRC builds).
    if [[ -d "$HSDAOH_SOURCE_DIR/.git" ]]; then
      git -C "$HSDAOH_SOURCE_DIR" rev-parse HEAD 2>/dev/null || true
    fi
    find "$HSDAOH_SOURCE_DIR" -type f \( -name '*.c' -o -name '*.h' -o -name 'CMakeLists.txt' -o -name '*.in' \) \
      -exec sha256sum {} + 2>/dev/null | sort
    # pacman dep versions we rely on (raylib/fftw3f/flac/libusb/soxr).
    for m in raylib fftw3f flac libusb-1.0 soxr; do
      printf '%s=%s\n' "$m" "$(pkg-config --modversion "$m" 2>/dev/null || echo missing)"
    done
  } | sha256sum | awk '{print $1}'
}

if [[ -f "$stamp_file" && -f "$DEPS_PREFIX/lib/pkgconfig/hsdaoh.pc" ]]; then
  prev="$(cat "$stamp_file" 2>/dev/null || echo none)"
  cur="$(compute_stamp)"
  if [[ "$prev" == "$cur" ]]; then
    log "deps already built (stamp matches); reusing $DEPS_PREFIX"
    exit 0
  fi
  log "inputs changed since last build; rebuilding deps."
else
  log "no stamped deps install found; building deps."
fi

require_tools() {
  local missing=0
  for t in "$@"; do command -v "$t" >/dev/null 2>&1 || { echo "[build-deps-windows] Missing: $t" >&2; missing=1; }; done
  [[ $missing -eq 0 ]] || fail "Install missing MSYS2 UCRT64 tools and retry."
}
require_tools cmake ninja gcc pkg-config git sed

# --- libuvc (static) --------------------------------------------------------
if [[ ! -d "$REPO_ROOT/.deps/libuvc/.git" ]]; then
  rm -rf "$REPO_ROOT/.deps/libuvc"
  log "cloning libuvc (ref=$LIBUVC_REF)"
  clone_args=(clone --depth 1)
  if [[ "$LIBUVC_REF" != main && -n "$LIBUVC_REF" ]]; then
    clone_args+=(--branch "$LIBUVC_REF")
  fi
  clone_args+=(https://github.com/libuvc/libuvc.git "$REPO_ROOT/.deps/libuvc")
  git "${clone_args[@]}" || git clone https://github.com/libuvc/libuvc.git "$REPO_ROOT/.deps/libuvc"
fi
# libuvc's FindLibUSB.cmake special-cases MINGW in a way that breaks the static
# build on MSYS2; relax it to MSVC-only (same patch as CI windows-exe job).
sed -i 's/^if (MSVC OR MINGW)$/if (MSVC)/' "$REPO_ROOT/.deps/libuvc/cmake/FindLibUSB.cmake"
cmake -S "$REPO_ROOT/.deps/libuvc" -B "$REPO_ROOT/.deps/libuvc/build" -G Ninja \
  "${CMAKE_TOOLCHAIN_ARGS[@]}" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_BUILD_TARGET=Static \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_EXAMPLE=OFF \
  -DBUILD_TEST=OFF \
  -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX"
cmake --build "$REPO_ROOT/.deps/libuvc/build" --parallel
cmake --install "$REPO_ROOT/.deps/libuvc/build"

# --- hsdaoh (static, vendored) ----------------------------------------------
rm -rf "$REPO_ROOT/.deps/hsdaoh"
cp -R "$HSDAOH_SOURCE_DIR" "$REPO_ROOT/.deps/hsdaoh"
cmake -S "$REPO_ROOT/.deps/hsdaoh" -B "$REPO_ROOT/.deps/hsdaoh/build" -G Ninja \
  "${CMAKE_TOOLCHAIN_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" \
  -DINSTALL_UDEV_RULES=OFF
cmake --build "$REPO_ROOT/.deps/hsdaoh/build" --target hsdaoh_static --parallel
mkdir -p "$DEPS_PREFIX/include" "$DEPS_PREFIX/lib" "$DEPS_PREFIX/lib/pkgconfig"
for hdr in hsdaoh.h hsdaoh_export.h hsdaoh_raw.h hsdaoh_crc.h; do
  [[ -f "$REPO_ROOT/.deps/hsdaoh/include/$hdr" ]] && cp "$REPO_ROOT/.deps/hsdaoh/include/$hdr" "$DEPS_PREFIX/include/"
done
if   [[ -f "$REPO_ROOT/.deps/hsdaoh/build/src/libhsdaoh.a" ]];          then cp "$REPO_ROOT/.deps/hsdaoh/build/src/libhsdaoh.a" "$DEPS_PREFIX/lib/libhsdaoh.a"
elif [[ -f "$REPO_ROOT/.deps/hsdaoh/build/src/libhsdaoh_static.a" ]];    then cp "$REPO_ROOT/.deps/hsdaoh/build/src/libhsdaoh_static.a" "$DEPS_PREFIX/lib/libhsdaoh.a"
elif [[ -f "$REPO_ROOT/.deps/hsdaoh/build/src/hsdaoh_static.lib" ]];     then cp "$REPO_ROOT/.deps/hsdaoh/build/src/hsdaoh_static.lib" "$DEPS_PREFIX/lib/libhsdaoh.a"
else fail "hsdaoh static library not found in expected build output paths"; fi
cp "$REPO_ROOT/.deps/hsdaoh/build/libhsdaoh.pc" "$DEPS_PREFIX/lib/pkgconfig/libhsdaoh.pc"
# hsdaoh.pc Libs must pull libuvc + libusb (hsdaoh hard-requires libuvc).
sed -i 's/^Libs: .*/Libs: -L${libdir} -lhsdaoh -luvc -lusb-1.0/' "$DEPS_PREFIX/lib/pkgconfig/libhsdaoh.pc"
for PCDIR in "$DEPS_PREFIX/lib/pkgconfig" "$DEPS_PREFIX/lib64/pkgconfig"; do
  [[ -f "$PCDIR/libhsdaoh.pc" && ! -f "$PCDIR/hsdaoh.pc" ]] && cp "$PCDIR/libhsdaoh.pc" "$PCDIR/hsdaoh.pc"
done

# --- raylib (static, built from source with internal GLFW) ------------------
# Built vendored (NOT reused from pacman) because the pacman libraylib.a was
# built with USE_EXTERNAL_GLFW=ON and references external glfw symbols, so a
# fully -static link fails with undefined __imp_glfw* references. Building from
# source with raylib's default USE_EXTERNAL_GLFW=OFF compiles GLFW into
# libraylib.a (self-contained) -- exactly what CI's windows-exe job does, so
# local == CI. The installed raylib.pc lands in .deps/install/lib/pkgconfig
# (first on PKG_CONFIG_PATH) so Meson picks it over the pacman one.
if [[ ! -d "$REPO_ROOT/.deps/raylib/.git" ]]; then
  rm -rf "$REPO_ROOT/.deps/raylib"
  log "cloning raylib (tag=$RAYLIB_TAG)"
  git clone --depth 1 --branch "$RAYLIB_TAG" https://github.com/raysan5/raylib.git "$REPO_ROOT/.deps/raylib"
else
  git -C "$REPO_ROOT/.deps/raylib" fetch --depth 1 origin "refs/tags/$RAYLIB_TAG:refs/tags/$RAYLIB_TAG" || true
  git -C "$REPO_ROOT/.deps/raylib" checkout --detach "$RAYLIB_TAG"
fi
cmake -S "$REPO_ROOT/.deps/raylib" -B "$REPO_ROOT/.deps/raylib/build" -G Ninja \
  "${CMAKE_TOOLCHAIN_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_GAMES=OFF \
  -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX"
cmake --build "$REPO_ROOT/.deps/raylib/build" --parallel
cmake --install "$REPO_ROOT/.deps/raylib/build"
log "raylib: $(pkg-config --modversion raylib) (built vendored with internal GLFW)"

# --- sanity -----------------------------------------------------------------
pkg-config --exists hsdaoh || fail "hsdaoh.pc not resolvable after build"
pkg-config --exists fftw3f || fail "fftw3f not found; install mingw-w64-ucrt-x86_64-fftw"
pkg-config --exists libzstd || fail "libzstd not found; install mingw-w64-ucrt-x86_64-zstd"
log "hsdaoh: $(pkg-config --modversion hsdaoh)"
log "fftw3f: $(pkg-config --modversion fftw3f)"

compute_stamp > "$stamp_file"
log "OK: deps installed to $DEPS_PREFIX"
