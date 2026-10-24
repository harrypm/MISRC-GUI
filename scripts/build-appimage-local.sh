#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-build-appimage-local}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/.ci-artifacts/linux-appimage}"
DEPS_PREFIX="${DEPS_PREFIX:-$REPO_ROOT/.deps/install-appimage-local}"
DEPS_SRC_DIR="${DEPS_SRC_DIR:-$REPO_ROOT/.deps/src-appimage-local}"
TOOLS_DIR="${TOOLS_DIR:-$REPO_ROOT/.deps/tools-appimage-local}"
WORK_TMP_DIR="${WORK_TMP_DIR:-$REPO_ROOT/.tmp/appimage-local}"
APPIMAGE_BUILD_IMAGE="${APPIMAGE_BUILD_IMAGE:-ubuntu:22.04}"
HSDAOH_SOURCE_DIR="${HSDAOH_SOURCE_DIR:-$REPO_ROOT/third_party/hsdaoh}"
RAYLIB_TAG="5.5"
TARGET_MAX_GLIBC="2.35"

log() {
  printf '[appimage-local] %s\n' "$*"
}

fail() {
  printf '[appimage-local] ERROR: %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Usage:
  scripts/build-appimage-local.sh
  scripts/build-appimage-local.sh --native

Modes:
  (default)     Run in an Ubuntu 22.04 container (docker/podman) for a stable glibc baseline.
  --native      Build directly on the host (requires all toolchain deps preinstalled).
                Native mode enforces a max required GLIBC symbol version of 2.35.

Optional environment variables:
  BUILD_DIR            (default: build-appimage-local)
  OUT_DIR              (default: .ci-artifacts/linux-appimage)
  APPIMAGE_BUILD_IMAGE (default: ubuntu:22.04)
  HSDAOH_SOURCE_DIR    (default: third_party/hsdaoh)
EOF
}

resolve_version() {
  local version
  version="$("$REPO_ROOT/misrc_tools/git-version.sh" 2>/dev/null || true)"
  if [[ -z "$version" ]]; then
    version="dev-$(date -u +%Y-%m-%d)-nogit"
  fi
  printf '%s\n' "$version"
}

resolve_arch() {
  local uname_arch
  uname_arch="$(uname -m)"
  case "$uname_arch" in
    x86_64)
      LINUXDEPLOY_ARCH="x86_64"
      ARTIFACT_SUFFIX="x86"
      ;;
    aarch64|arm64)
      LINUXDEPLOY_ARCH="aarch64"
      ARTIFACT_SUFFIX="arm64"
      ;;
    *)
      fail "Unsupported architecture: $uname_arch (supported: x86_64, aarch64/arm64)"
      ;;
  esac
}

require_tools() {
  local missing=0
  local tools=("$@")
  local tool
  for tool in "${tools[@]}"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      printf '[appimage-local] Missing required tool: %s\n' "$tool" >&2
      missing=1
    fi
  done
  if [[ "$missing" -ne 0 ]]; then
    fail "Install missing tools and retry."
  fi
}

max_glibc_required() {
  local file="$1"
  strings "$file" | grep -o 'GLIBC_[0-9]\+\.[0-9]\+' | sed 's/GLIBC_//' | sort -V | tail -n 1
}

assert_glibc_floor() {
  local file="$1"
  local max_glibc
  max_glibc="$(max_glibc_required "$file" || true)"
  if [[ -z "$max_glibc" ]]; then
    return 0
  fi
  if [[ "$(printf '%s\n' "$max_glibc" "$TARGET_MAX_GLIBC" | sort -V | tail -n 1)" != "$TARGET_MAX_GLIBC" ]]; then
    fail "$file requires GLIBC_$max_glibc (target max is GLIBC_$TARGET_MAX_GLIBC)"
  fi
}

build_native() {
  require_tools git cmake meson ninja pkg-config curl convert zip unzip patchelf strings file
  resolve_arch
  local version
  version="$(resolve_version)"

  log "Building AppImage for arch=$LINUXDEPLOY_ARCH version=$version"

  rm -rf "$REPO_ROOT/$BUILD_DIR" "$DEPS_PREFIX" "$WORK_TMP_DIR"
  mkdir -p "$OUT_DIR" "$DEPS_SRC_DIR" "$DEPS_PREFIX" "$TOOLS_DIR" "$WORK_TMP_DIR"

  # Use an isolated git config so safe.directory tweaks do not touch host user config.
  export GIT_CONFIG_GLOBAL="$WORK_TMP_DIR/gitconfig"
  touch "$GIT_CONFIG_GLOBAL"
  git config --global --add safe.directory "$REPO_ROOT" || true
  git config --global --add safe.directory "$HSDAOH_SOURCE_DIR" || true
  git config --global --add safe.directory "$DEPS_SRC_DIR/raylib" || true

  if [[ ! -f "$HSDAOH_SOURCE_DIR/CMakeLists.txt" ]]; then
    fail "Vendored hsdaoh source not found at $HSDAOH_SOURCE_DIR"
  fi

  if [[ ! -d "$DEPS_SRC_DIR/raylib/.git" ]]; then
    git clone --depth 1 --branch "$RAYLIB_TAG" https://github.com/raysan5/raylib.git "$DEPS_SRC_DIR/raylib"
  fi
  git -C "$DEPS_SRC_DIR/raylib" fetch --depth 1 origin "refs/tags/$RAYLIB_TAG:refs/tags/$RAYLIB_TAG" || true
  git -C "$DEPS_SRC_DIR/raylib" checkout --detach "$RAYLIB_TAG"

  cmake -S "$HSDAOH_SOURCE_DIR" -B "$DEPS_SRC_DIR/hsdaoh/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" \
    -DINSTALL_UDEV_RULES=OFF
  cmake --build "$DEPS_SRC_DIR/hsdaoh/build" --parallel
  cmake --install "$DEPS_SRC_DIR/hsdaoh/build"

  for pcdir in "$DEPS_PREFIX/lib/pkgconfig" "$DEPS_PREFIX/lib64/pkgconfig"; do
    if [[ -f "$pcdir/libhsdaoh.pc" && ! -f "$pcdir/hsdaoh.pc" ]]; then
      cp "$pcdir/libhsdaoh.pc" "$pcdir/hsdaoh.pc"
    fi
  done

  cmake -S "$DEPS_SRC_DIR/raylib" -B "$DEPS_SRC_DIR/raylib/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_GAMES=OFF \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX"
  cmake --build "$DEPS_SRC_DIR/raylib/build" --parallel
  cmake --install "$DEPS_SRC_DIR/raylib/build"

  export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
  export CMAKE_PREFIX_PATH="$DEPS_PREFIX:${CMAKE_PREFIX_PATH:-}"
  if ! pkg-config --exists fftw3f; then
    fail "Missing fftw3f pkg-config module. Install FFTW3 development files (e.g. libfftw3-dev) and retry."
  fi
  pkg-config --modversion fftw3f

  if [[ -f "$REPO_ROOT/$BUILD_DIR/meson-private/coredata.dat" ]]; then
    meson setup "$REPO_ROOT/$BUILD_DIR" "$REPO_ROOT/misrc_tools" --buildtype release --wipe
  else
    meson setup "$REPO_ROOT/$BUILD_DIR" "$REPO_ROOT/misrc_tools" --buildtype release
  fi
  meson compile -C "$REPO_ROOT/$BUILD_DIR" misrc_capture misrc_extract misrc_gui

  "$REPO_ROOT/$BUILD_DIR/misrc_gui" --smoke-test
  set +e
  "$REPO_ROOT/$BUILD_DIR/misrc_capture" -h > /tmp/misrc_capture_help_local.txt 2>&1
  local rc_capture=$?
  "$REPO_ROOT/$BUILD_DIR/misrc_extract" -h > /tmp/misrc_extract_help_local.txt 2>&1
  local rc_extract=$?
  set -e
  [[ "$rc_capture" -eq 1 ]] || fail "misrc_capture -h returned $rc_capture (expected 1)"
  [[ "$rc_extract" -eq 1 ]] || fail "misrc_extract -h returned $rc_extract (expected 1)"
  grep -qi "Usage" /tmp/misrc_capture_help_local.txt
  grep -qi "Usage" /tmp/misrc_extract_help_local.txt

  assert_glibc_floor "$REPO_ROOT/$BUILD_DIR/misrc_gui"
  assert_glibc_floor "$REPO_ROOT/$BUILD_DIR/misrc_capture"
  assert_glibc_floor "$REPO_ROOT/$BUILD_DIR/misrc_extract"

  local appdir="$WORK_TMP_DIR/AppDir"
  local package_dir="$WORK_TMP_DIR/package"
  rm -rf "$appdir" "$package_dir"
  mkdir -p "$appdir/usr/bin" "$package_dir"

  cp "$REPO_ROOT/$BUILD_DIR/misrc_gui" "$appdir/usr/bin/"
  cp "$REPO_ROOT/$BUILD_DIR/misrc_capture" "$appdir/usr/bin/"
  cp "$REPO_ROOT/$BUILD_DIR/misrc_extract" "$appdir/usr/bin/"

  # AppRun and the desktop entry are shared with CI (.github/workflows/build.yml):
  # real repo files, not inline heredocs. AppRun installs a launcher in
  # ~/.local/share/applications only; it never writes ~/Desktop.
  install -m 0755 "$REPO_ROOT/assets/appimage/AppRun" "$appdir/AppRun"
  install -m 0644 "$REPO_ROOT/assets/appimage/misrc_gui.desktop" "$appdir/misrc_gui.desktop"

  local icon_src="$REPO_ROOT/assets/Icons/MISRC_Icon.png"
  [[ -f "$icon_src" ]] || fail "Icon not found: $icon_src"
  convert "$icon_src" -resize 512x512\! "$appdir/misrc.png"
  ln -sf misrc.png "$appdir/.DirIcon"
  mkdir -p "$appdir/usr/share/icons/hicolor/512x512/apps"
  cp "$appdir/misrc.png" "$appdir/usr/share/icons/hicolor/512x512/apps/misrc.png"

  local linuxdeploy_bin="$TOOLS_DIR/linuxdeploy"
  local linuxdeploy_plugin="$TOOLS_DIR/linuxdeploy-plugin-appimage"
  curl -fL "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${LINUXDEPLOY_ARCH}.AppImage" -o "$linuxdeploy_bin"
  curl -fL "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/continuous/linuxdeploy-plugin-appimage-${LINUXDEPLOY_ARCH}.AppImage" -o "$linuxdeploy_plugin"
  chmod +x "$linuxdeploy_bin" "$linuxdeploy_plugin"

  (
    cd "$package_dir"
    ARCH="$LINUXDEPLOY_ARCH" APPIMAGE_EXTRACT_AND_RUN=1 "$linuxdeploy_bin" \
      --appdir "$appdir" \
      -e "$appdir/usr/bin/misrc_gui" \
      -d "$appdir/misrc_gui.desktop" \
      -i "$appdir/misrc.png" \
      --output appimage
  )

  local produced_appimage
  produced_appimage="$(find "$package_dir" -maxdepth 1 -type f -name '*.AppImage' | head -n 1)"
  [[ -n "$produced_appimage" ]] || fail "AppImage was not produced by linuxdeploy."

  local output_name="Linux_MISRC_GUI_${version}_${ARTIFACT_SUFFIX}.AppImage"
  local output_path="$OUT_DIR/$output_name"
  cp "$produced_appimage" "$output_path"
  chmod +x "$output_path"

  APPIMAGE_EXTRACT_AND_RUN=1 "$output_path" --smoke-test

  log "Built AppImage: $output_path"
}

run_in_container() {
  local engine=""
  if command -v docker >/dev/null 2>&1; then
    engine="docker"
  elif command -v podman >/dev/null 2>&1; then
    engine="podman"
  else
    fail "Neither docker nor podman is installed."
  fi

  log "Using container engine: $engine"
  log "Image: $APPIMAGE_BUILD_IMAGE"

  to_container_path() {
    local host_path="$1"
    if [[ "$host_path" == "$REPO_ROOT" ]]; then
      printf '/workspace\n'
      return
    fi
    if [[ "$host_path" == "$REPO_ROOT/"* ]]; then
      printf '/workspace/%s\n' "${host_path#"$REPO_ROOT"/}"
      return
    fi
    printf '%s\n' "$host_path"
  }

  local container_out_dir
  local container_deps_prefix
  local container_deps_src_dir
  local container_tools_dir
  local container_work_tmp_dir
  container_out_dir="$(to_container_path "$OUT_DIR")"
  container_deps_prefix="$(to_container_path "$DEPS_PREFIX")"
  container_deps_src_dir="$(to_container_path "$DEPS_SRC_DIR")"
  container_tools_dir="$(to_container_path "$TOOLS_DIR")"
  container_work_tmp_dir="$(to_container_path "$WORK_TMP_DIR")"

  "$engine" run --rm \
    -e HOST_UID="$(id -u)" \
    -e HOST_GID="$(id -g)" \
    -e BUILD_DIR="$BUILD_DIR" \
    -e OUT_DIR="$container_out_dir" \
    -e DEPS_PREFIX="$container_deps_prefix" \
    -e DEPS_SRC_DIR="$container_deps_src_dir" \
    -e TOOLS_DIR="$container_tools_dir" \
    -e WORK_TMP_DIR="$container_work_tmp_dir" \
    -v "$REPO_ROOT:/workspace" \
    -w /workspace \
    "$APPIMAGE_BUILD_IMAGE" \
    bash /workspace/scripts/build-appimage-local.sh --container-internal
}

run_container_internal() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y \
    ca-certificates \
    cmake \
    curl \
    file \
    git \
    imagemagick \
    libasound2-dev \
    libfftw3-dev \
    libzstd-dev \
    libflac-dev \
    libgl1-mesa-dev \
    libsoxr-dev \
    libusb-1.0-0-dev \
    libuvc-dev \
    libx11-dev \
    libxcursor-dev \
    libxi-dev \
    libxinerama-dev \
    libxrandr-dev \
    meson \
    nasm \
    ninja-build \
    patchelf \
    pkg-config \
    unzip \
    zip

  build_native

  if [[ -n "${HOST_UID:-}" && -n "${HOST_GID:-}" ]]; then
    chown -R "${HOST_UID}:${HOST_GID}" "$OUT_DIR" "$REPO_ROOT/$BUILD_DIR" "$DEPS_PREFIX" "$DEPS_SRC_DIR" "$TOOLS_DIR" "$WORK_TMP_DIR" 2>/dev/null || true
  fi
}

main() {
  local mode="${1:-}"
  case "$mode" in
    --help|-h)
      usage
      ;;
    --native)
      build_native
      ;;
    --container-internal)
      run_container_internal
      ;;
    "")
      run_in_container
      ;;
    *)
      usage
      fail "Unknown argument: $mode"
      ;;
  esac
}

main "${1:-}"
