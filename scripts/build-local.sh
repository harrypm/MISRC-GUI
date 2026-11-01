#!/usr/bin/env bash
# Lightweight native build + smoke-test for local build testing.
#
# Unlike scripts/build-appimage-local.sh (which rebuilds vendored deps from
# source and packages an AppImage for a portable glibc baseline), this script
# assumes the vendored hsdaoh/raylib deps are already built under a deps
# prefix (.deps/install, .deps/install-appimage-local, or $MISRC_DEPS_PREFIX)
# and just runs a fast meson native build + smoke test.
#
# If no vendored deps are found, run:
#   scripts/build-appimage-local.sh --native
# once first to populate .deps/install-appimage-local, then re-run this script.
#
# Usage:
#   scripts/build-local.sh               # build misrc_gui + smoke test
#   scripts/build-local.sh --all         # also build misrc_capture + misrc_extract and help-check them
#   scripts/build-local.sh --clean       # wipe the build dir before setup
#   BUILD_DIR=build-x scripts/build-local.sh
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-build-local}"
BUILDTYPE="${BUILDTYPE:-release}"
BUILD_ALL=0
CLEAN=0

for arg in "$@"; do
  case "$arg" in
    --all) BUILD_ALL=1 ;;
    --clean) CLEAN=1 ;;
    -h|--help)
      sed -n '2,18p' "$0"
      exit 0
      ;;
    *) printf '[build-local] Unknown argument: %s\n' "$arg" >&2; exit 2 ;;
  esac
done

log()  { printf '[build-local] %s\n' "$*"; }
fail() { printf '[build-local] ERROR: %s\n' "$*" >&2; exit 1; }

require_tools() {
  local missing=0
  for t in "$@"; do
    if ! command -v "$t" >/dev/null 2>&1; then
      printf '[build-local] Missing required tool: %s\n' "$t" >&2
      missing=1
    fi
  done
  [[ "$missing" -eq 0 ]] || fail "Install missing tools and retry."
}

require_tools meson ninja pkg-config cc nasm python3

# CI parity gate: run the same static guard tests CI's preflight runs BEFORE
# any deps/meson work, so a guard break (optional-dep guard consistency,
# workflow contracts, ...) fails the local build first instead of only the
# manually dispatched CI run. Regression e34dce9: libFLAC-guarded struct
# members were referenced outside the guard in gui_playback.c; local builds
# passed and the dispatched CI run failed a day later.
GUARD_SCRIPT="$REPO_ROOT/misrc_tools/test/ci_guard_tests.py"
if [[ ! -f "$GUARD_SCRIPT" ]]; then
  fail "CI guard test script not found: $GUARD_SCRIPT"
fi
log "CI guard tests (static) before build"
python3 "$GUARD_SCRIPT" --static-only || fail "CI guard tests failed (see output above). Fix the reported errors and rerun."

# Force the system pkg-config for native builds. A leaked PKG_CONFIG pointing
# at a cross wrapper (e.g. android/android-pkg-config) only searches a cross
# deps prefix and hides system modules like fftw3f/libusb/alsa/soxr.
# Override with MISRC_PKG_CONFIG if a different native pkg-config is needed.
export PKG_CONFIG="${MISRC_PKG_CONFIG:-pkg-config}"
# Detect the H.264 dependency before building vendored deps or touching a
# pre-existing build directory. Missing OpenH264 must never yield a GUI that
# silently falls back to a black video preview.
bash "$SCRIPT_DIR/check-build-deps.sh" --codec

# Locate a vendored deps prefix that provides hsdaoh.pc.
DEPS_PREFIX=""
for candidate in "${MISRC_DEPS_PREFIX:-}" "$REPO_ROOT/.deps/install" "$REPO_ROOT/.deps/install-appimage-local"; do
  [[ -z "$candidate" ]] && continue
  for pcdir in "$candidate/lib/pkgconfig" "$candidate/lib64/pkgconfig"; do
    if [[ -f "$pcdir/hsdaoh.pc" || -f "$pcdir/libhsdaoh.pc" ]]; then
      DEPS_PREFIX="$candidate"
      break 2
    fi
  done
done

# Ensure vendored deps exist and are fresh. The deps script mirrors the
# linux-appimage/macos CI deps blocks and self-skips via a stamp when inputs
# are unchanged, so new terminals reuse until an input changes (local == CI).
# Skip only when the caller points at a prebuilt prefix via MISRC_DEPS_PREFIX.
if [[ -z "$DEPS_PREFIX" && -z "${MISRC_DEPS_PREFIX:-}" ]]; then
  deps_script="$SCRIPT_DIR/build-deps-unix.sh"
  if [[ ! -f "$deps_script" ]]; then
    fail "Deps build script not found: $deps_script"
  fi
  log "Ensuring local deps via $deps_script (skips instantly if already built)"
  bash "$deps_script"
  # Re-detect the deps prefix after the build.
  for candidate in "$REPO_ROOT/.deps/install" "$REPO_ROOT/.deps/install-appimage-local"; do
    for pcdir in "$candidate/lib/pkgconfig" "$candidate/lib64/pkgconfig"; do
      if [[ -f "$pcdir/hsdaoh.pc" || -f "$pcdir/libhsdaoh.pc" ]]; then
        DEPS_PREFIX="$candidate"; break 2
      fi
    done
  done
  if [[ -z "$DEPS_PREFIX" ]]; then
    fail "Deps build reported success but hsdaoh.pc still not found under .deps/install."
  fi
fi

if [[ -z "$DEPS_PREFIX" ]]; then
  fail "No vendored hsdaoh.pc found under .deps/install or .deps/install-appimage-local, and MISRC_DEPS_PREFIX not set."
fi
log "Using deps prefix: $DEPS_PREFIX"

# Make sure hsdaoh.pc exists alongside libhsdaoh.pc (meson looks up 'hsdaoh').
for pcdir in "$DEPS_PREFIX/lib/pkgconfig" "$DEPS_PREFIX/lib64/pkgconfig"; do
  if [[ -f "$pcdir/libhsdaoh.pc" && ! -f "$pcdir/hsdaoh.pc" ]]; then
    cp "$pcdir/libhsdaoh.pc" "$pcdir/hsdaoh.pc"
  fi
done

export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$DEPS_PREFIX:${CMAKE_PREFIX_PATH:-}"

# Check all native libraries (and the vendored GUI libraries) BEFORE Meson
# setup/compile. The preflight reports every missing package in one pass.
bash "$SCRIPT_DIR/check-build-deps.sh" --gui
log "OpenH264: $("$PKG_CONFIG" --modversion openh264)"

# Handle absolute BUILD_DIR (e.g. leaked BUILD_DIR=.../build-android) as-is;
# otherwise resolve relative to the repo root.
if [[ "$BUILD_DIR" = /* ]]; then
  BUILD_PATH="$BUILD_DIR"
else
  BUILD_PATH="$REPO_ROOT/$BUILD_DIR"
fi

if [[ "$CLEAN" -eq 1 ]]; then
  log "Cleaning $BUILD_PATH"
  rm -rf "$BUILD_PATH"
fi

# Reconfigure safely if the build dir already exists.
if [[ -f "$BUILD_PATH/meson-private/coredata.dat" ]]; then
  meson setup "$BUILD_PATH" "$REPO_ROOT/misrc_tools" --buildtype "$BUILDTYPE" --wipe
else
  meson setup "$BUILD_PATH" "$REPO_ROOT/misrc_tools" --buildtype "$BUILDTYPE"
fi

if [[ "$BUILD_ALL" -eq 1 ]]; then
  meson compile -C "$BUILD_PATH" misrc_capture misrc_extract misrc_gui
else
  meson compile -C "$BUILD_PATH" misrc_gui
fi

log "Smoke test"
"$BUILD_PATH/misrc_gui" --smoke-test

if [[ "$BUILD_ALL" -eq 1 ]]; then
  set +e
  "$BUILD_PATH/misrc_capture" -h >/tmp/misrc_capture_help_local.txt 2>&1; rc_capture=$?
  "$BUILD_PATH/misrc_extract"  -h >/tmp/misrc_extract_help_local.txt  2>&1; rc_extract=$?
  set -e
  [[ "$rc_capture" -eq 1 ]] || fail "misrc_capture -h returned $rc_capture (expected 1)"
  [[ "$rc_extract"  -eq 1 ]] || fail "misrc_extract -h returned $rc_extract (expected 1)"
  grep -qi "Usage" /tmp/misrc_capture_help_local.txt || fail "misrc_capture -h missing Usage"
  grep -qi "Usage" /tmp/misrc_extract_help_local.txt  || fail "misrc_extract -h missing Usage"
  log "misrc_capture + misrc_extract help checks passed"
fi

log "OK: build + smoke test passed -> $BUILD_PATH/misrc_gui"
