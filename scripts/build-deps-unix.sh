#!/usr/bin/env bash
# Build vendored deps into .deps/install for a local Linux or macOS build.
# Mirrors the deps-build blocks of the .github/workflows/build.yml
# `linux-appimage` and `macos-app-build` jobs so local == CI for hsdaoh + raylib.
# fftw3f/flac/libusb-1.0/soxr/libuvc come from the system package manager
# (apt libuvc-dev/libfftw3-dev/libflac-dev/libsoxr-dev/libusb-1.0-0-dev on
# Debian/Ubuntu, `brew install fftw flac libusb libuvc libsoxr` on macOS) -- the
# same versions CI installs -- so only hsdaoh + raylib are built vendored.
#
# Idempotent: skips work whose stamp matches the current inputs, so repeated runs
# or new terminal sessions reuse .deps/install until an input changes.
#
# NOTE: authored by mirroring CI; not executed on a Linux/macOS host in this
# session. Validate on first real Linux/macOS use.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DEPS_PREFIX="${DEPS_PREFIX:-$REPO_ROOT/.deps/install}"
HSDAOH_SOURCE_DIR="${HSDAOH_SOURCE_DIR:-$REPO_ROOT/third_party/hsdaoh}"
RAYLIB_TAG="${RAYLIB_TAG:-5.5}"

log()  { printf '[build-deps-unix] %s\n' "$*"; }
fail() { printf '[build-deps-unix] ERROR: %s\n' "$*" >&2; exit 1; }

OS="$(uname -s)"
case "$OS" in
  Linux*)   HOST_SYSTEM=linux ;;
  Darwin*)  HOST_SYSTEM=darwin ;;
  *) fail "Unsupported OS: $OS" ;;
esac
HOST_ARCH="${MACOS_ARCH:-$(uname -m)}"  # MACOS_ARCH overrides macOS build arch

# Raspberry Pi 5/V3D and other Linux ARM64 targets need the OpenGL 2.1 path.
# Keep OpenGL 3.3 for non-ARM Linux and macOS builds.
RAYLIB_OPENGL_VERSION=3.3
if [[ "$HOST_SYSTEM" == "linux" && ( "$HOST_ARCH" == "aarch64" || "$HOST_ARCH" == "arm64" ) ]]; then
  RAYLIB_OPENGL_VERSION=2.1
fi

mkdir -p "$DEPS_PREFIX/lib/pkgconfig" "$DEPS_PREFIX/include" "$REPO_ROOT/.deps"

require_tools() {
  local missing=0
  for t in "$@"; do command -v "$t" >/dev/null 2>&1 || { echo "[build-deps-unix] Missing: $t" >&2; missing=1; }; done
  [[ $missing -eq 0 ]] || fail "Install missing tools and retry."
}
require_tools cmake pkg-config cc git

# Prefer vendored .pc files (especially FLAC 1.5.0 on Ubuntu 22.04) when
# validating libraries. Check BEFORE invoking CMake or reusing the stamp.
export PKG_CONFIG="${PKG_CONFIG:-pkg-config}"
export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
bash "$SCRIPT_DIR/check-build-deps.sh" --all

# --- Stamp: rebuild only when inputs change ---------------------------------
stamp_file="$DEPS_PREFIX/.build-stamp"
compute_stamp() {
  {
    echo "host=$HOST_SYSTEM arch=$HOST_ARCH"
    echo "raylib_tag=$RAYLIB_TAG"
    echo "raylib_opengl_version=$RAYLIB_OPENGL_VERSION"
    if [[ -d "$HSDAOH_SOURCE_DIR/.git" ]]; then
      git -C "$HSDAOH_SOURCE_DIR" rev-parse HEAD 2>/dev/null || true
    fi
    find "$HSDAOH_SOURCE_DIR" -type f \( -name '*.c' -o -name '*.h' -o -name 'CMakeLists.txt' -o -name '*.in' \) \
      -exec sha256sum {} + 2>/dev/null | sort
    for m in fftw3f flac libusb-1.0 soxr libuvc libzstd openh264; do
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

# Isolated git config so safe.directory tweaks do not touch host user config.
export GIT_CONFIG_GLOBAL="$REPO_ROOT/.deps/gitconfig-unix"
touch "$GIT_CONFIG_GLOBAL"
git config --global --add safe.directory "$REPO_ROOT" 2>/dev/null || true
git config --global --add safe.directory "$HSDAOH_SOURCE_DIR" 2>/dev/null || true
git config --global --add safe.directory "$REPO_ROOT/.deps/raylib" 2>/dev/null || true

HS_CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" -DINSTALL_UDEV_RULES=OFF)
RAY_CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF -DBUILD_GAMES=OFF -DOPENGL_VERSION="$RAYLIB_OPENGL_VERSION" -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX")
if [[ "$HOST_SYSTEM" == "darwin" ]]; then
  BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
  FLAC_PREFIX="$(brew --prefix flac 2>/dev/null || echo "$BREW_PREFIX/opt/flac")"
  # hsdaoh's CMake uses pkg_check_modules(flac); export include dirs so its
  # hsdaoh_file target (which does not inherit flac's -I) can find FLAC/*.h.
  HS_CMAKE_ARGS+=(-DCMAKE_C_FLAGS="-I${FLAC_PREFIX}/include -I${BREW_PREFIX}/include -arch ${HOST_ARCH}"
                  -DCMAKE_OSX_ARCHITECTURES="$HOST_ARCH")
  RAY_CMAKE_ARGS+=(-DCMAKE_OSX_ARCHITECTURES="$HOST_ARCH")
fi

# --- hsdaoh (static, vendored) ----------------------------------------------
rm -rf "$REPO_ROOT/.deps/hsdaoh"
cp -R "$HSDAOH_SOURCE_DIR" "$REPO_ROOT/.deps/hsdaoh"
cmake -S "$REPO_ROOT/.deps/hsdaoh" -B "$REPO_ROOT/.deps/hsdaoh/build" "${HS_CMAKE_ARGS[@]}"
cmake --build "$REPO_ROOT/.deps/hsdaoh/build" --parallel
cmake --install "$REPO_ROOT/.deps/hsdaoh/build"
for PCDIR in "$DEPS_PREFIX/lib/pkgconfig" "$DEPS_PREFIX/lib64/pkgconfig"; do
  [[ -f "$PCDIR/libhsdaoh.pc" && ! -f "$PCDIR/hsdaoh.pc" ]] && cp "$PCDIR/libhsdaoh.pc" "$PCDIR/hsdaoh.pc"
done

# --- raylib (static, internal GLFW) -----------------------------------------
if [[ ! -d "$REPO_ROOT/.deps/raylib/.git" ]]; then
  rm -rf "$REPO_ROOT/.deps/raylib"
  log "cloning raylib (tag=$RAYLIB_TAG)"
  git clone --depth 1 --branch "$RAYLIB_TAG" https://github.com/raysan5/raylib.git "$REPO_ROOT/.deps/raylib"
else
  git -C "$REPO_ROOT/.deps/raylib" fetch --depth 1 origin "refs/tags/$RAYLIB_TAG:refs/tags/$RAYLIB_TAG" || true
  git -C "$REPO_ROOT/.deps/raylib" checkout --detach "$RAYLIB_TAG"
fi
cmake -S "$REPO_ROOT/.deps/raylib" -B "$REPO_ROOT/.deps/raylib/build" "${RAY_CMAKE_ARGS[@]}"
cmake --build "$REPO_ROOT/.deps/raylib/build" --parallel
cmake --install "$REPO_ROOT/.deps/raylib/build"

# --- sanity -----------------------------------------------------------------
export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
bash "$SCRIPT_DIR/check-build-deps.sh" --gui
log "hsdaoh: $(pkg-config --modversion hsdaoh)"
log "raylib: $(pkg-config --modversion raylib) (built vendored with internal GLFW)"
log "fftw3f: $(pkg-config --modversion fftw3f)"

compute_stamp > "$stamp_file"
log "OK: deps installed to $DEPS_PREFIX"
