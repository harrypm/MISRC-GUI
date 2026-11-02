#!/usr/bin/env bash
# Minimal static FFmpeg for MISRC Monitor Client (AV1, VP9, HEVC).
# Unlike a distro FFmpeg --static closure this has no unrelated librsvg,
# JPEG-XL, Rust, audio or multimedia codecs, preserving Windows one-EXE
# packaging and the Android libmisrc_gui.so dependency contract.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-}"
case "$MODE" in windows|android) ;; *)
  echo "Usage: $0 windows|android" >&2; exit 2 ;; esac
FFMPEG_TAG=n7.1.3
SRC="$ROOT/.deps/ffmpeg-minimal-src"
if [[ "$MODE" == android ]]; then
  : "${ANDROID_NDK_HOME:?Missing Android NDK}"
  PREFIX="${DEPS_PREFIX:-$ROOT/.deps/install-android-arm64}"
  NDK="$(cd "$ANDROID_NDK_HOME" && pwd)"
  NDK_BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
  TOOL_CC="$NDK_BIN/aarch64-linux-android30-clang"
  TOOL_CXX="$NDK_BIN/aarch64-linux-android30-clang++"
  TARGET_ARGS=(--target-os=android --arch=aarch64 --enable-cross-compile
      "--cc=$TOOL_CC" "--cxx=$TOOL_CXX" "--ar=$NDK_BIN/llvm-ar"
      "--ranlib=$NDK_BIN/llvm-ranlib" "--strip=$NDK_BIN/llvm-strip"
      "--nm=$NDK_BIN/llvm-nm" "--sysroot=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot")
else
  : "${MINGW_PREFIX:?Run under MSYS2 UCRT64 or CLANGARM64}"
  PREFIX="${DEPS_PREFIX:-$ROOT/.deps/install}"
  TARGET_ARGS=(--target-os=mingw32)
  if [[ "${MSYSTEM:-}" == CLANGARM64 ]]; then
    TARGET_ARGS+=(--arch=aarch64 --cc=clang --cxx=clang++ --disable-asm)
  else
    TARGET_ARGS+=(--arch=x86_64 --cc=gcc --cxx=g++ --disable-x86asm)
  fi
fi
mkdir -p "$PREFIX" "$(dirname "$SRC")"
PREFIX="$(cd "$PREFIX" && pwd)"
if [[ -f "$PREFIX/lib/pkgconfig/libavcodec.pc" ]] &&
   PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" pkg-config --atleast-version=7.1 libavcodec &&
   [[ -f "$PREFIX/.misrc-ffmpeg-minimal-$MODE-$FFMPEG_TAG" ]]; then
  echo "[ffmpeg-minimal] cached $MODE static FFmpeg $FFMPEG_TAG"
  exit 0
fi
if [[ ! -f "$SRC/configure" ]]; then
  rm -rf "$SRC"
  git clone --depth 1 --branch "$FFMPEG_TAG" https://github.com/FFmpeg/FFmpeg.git "$SRC"
fi
BUILD="$ROOT/.deps/ffmpeg-minimal-build-$MODE-${MSYSTEM:-ndk}"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" \
  "--prefix=$PREFIX" --libdir="$PREFIX/lib" \
  --enable-static --disable-shared --enable-pic --enable-small \
  --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-avdevice --disable-avfilter --disable-avformat --disable-swresample \
  --disable-postproc --disable-everything \
  --enable-avcodec --enable-avutil --enable-swscale \
  --enable-decoder=av1,hevc,vp9 \
  --enable-parser=av1,hevc,vp9 \
  --disable-debug --disable-stripping \
  "${TARGET_ARGS[@]}"
make -j"${JOBS:-4}"
make install
touch "$PREFIX/.misrc-ffmpeg-minimal-$MODE-$FFMPEG_TAG"
if [[ "$MODE" == android ]]; then
  DEPS_PREFIX="$PREFIX" "$ROOT/android/android-pkg-config" --modversion libavcodec
else
  PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}" pkg-config --modversion libavcodec
fi
echo "[ffmpeg-minimal] Installed lean static AV1/VP9/HEVC decoder libraries: $PREFIX"
