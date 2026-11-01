#!/usr/bin/env bash
# Cross-build static OpenH264 2.6.0 for Android arm64-v8a (API 30).
# Run only after android/gen-cross-file.sh.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME is required}"
DEPS_PREFIX="${DEPS_PREFIX:-$ROOT/.deps/install-android-arm64}"
SRC="$ROOT/.deps/src-android-arm64/openh264-src"
BUILD="$ROOT/.deps/src-android-arm64/openh264-build"
CROSS="$ROOT/android/aarch64-linux-android.ini"
[[ -f "$CROSS" ]] || { echo "Generate the Android cross-file first" >&2; exit 1; }
if DEPS_PREFIX="$DEPS_PREFIX" "$ROOT/android/android-pkg-config" --exists openh264; then
  echo "Android OpenH264 cross dependency already present"
  exit 0
fi
mkdir -p "$(dirname "$SRC")" "$DEPS_PREFIX"
if [[ ! -f "$SRC/meson.build" ]]; then
  rm -rf "$SRC"
  git clone --depth 1 --branch v2.6.0 https://github.com/cisco/openh264.git "$SRC"
fi
meson setup "$BUILD" "$SRC" --cross-file "$CROSS" \
  --prefix "$DEPS_PREFIX" --libdir lib --buildtype release \
  --default-library static
meson compile -C "$BUILD"
meson install -C "$BUILD"
DEPS_PREFIX="$DEPS_PREFIX" "$ROOT/android/android-pkg-config" --modversion openh264
