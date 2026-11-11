#!/usr/bin/env bash
# Build the pinned RTL-SDR static library into the caller's dependency prefix.
# Optional arguments configure the caller's CMake architecture or cross toolchain.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
: "${DEPS_PREFIX:?Set DEPS_PREFIX to the dependency install prefix}"

fail() {
    printf '[build-rtlsdr] %s\n' "$*" >&2
    exit 1
}

mkdir -p "$DEPS_PREFIX"
export DEPS_PREFIX="$(cd -- "$DEPS_PREFIX" && pwd)"
build_root="${RTLSDR_BUILD_DIR:-${DEPS_PREFIX}-rtlsdr-build}"
source="$build_root/source"
build="$build_root/build"
pkg_config="${PKG_CONFIG:-pkg-config}"
for arg in "$@"; do
    case "$arg" in
        -DPKG_CONFIG_EXECUTABLE=*|-DPKG_CONFIG_EXECUTABLE:FILEPATH=*) pkg_config="${arg#*=}" ;;
    esac
done
pkg_config="$(command -v "$pkg_config")" || fail "pkg-config is required"
export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
"$pkg_config" --exists libusb-1.0 || fail "libusb-1.0 is required in the target pkg-config environment"

python="${PYTHON:-python3}"
if [[ -z "${PYTHON:-}" ]] && ! command -v "$python" >/dev/null; then
    python=python
fi
command -v "$python" >/dev/null || fail "Python 3 is required"
"$python" "$root/scripts/fetch-patched-dependency.py" rtl-sdr "$source"
cmake -S "$source" -B "$build" -G Ninja \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON \
    "-DCMAKE_INSTALL_PREFIX=$DEPS_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    "-DPKG_CONFIG_EXECUTABLE=$pkg_config" "$@"
# Upstream's install target also builds the shared library and CLI utilities.
cmake --build "$build" --parallel --target rtlsdr_static

archive=librtlsdr.a
if [[ -s "$build/src/librtlsdr_static.a" ]]; then
    archive=librtlsdr_static.a
fi
[[ -s "$build/src/$archive" ]] || fail "Static archive missing: $build/src/$archive"
mkdir -p "$DEPS_PREFIX/include" "$DEPS_PREFIX/lib/pkgconfig"
cp "$build/src/$archive" "$DEPS_PREFIX/lib/$archive"
cp "$source/include/rtl-sdr.h" "$source/include/rtl-sdr_export.h" "$DEPS_PREFIX/include/"
# Refer to the installed archive directly and preserve libusb's pkg-config flags.
pc="$DEPS_PREFIX/lib/pkgconfig/librtlsdr.pc"
sed -e 's|^prefix=.*|prefix=${pcfiledir}/../..|' \
    -e 's|^Cflags:.*|Cflags: -I"${includedir}" -Drtlsdr_STATIC|' \
    -e 's|^Libs:.*|Libs: "${libdir}/'"$archive"'"|' \
    -e 's|^Libs.private:.*|Requires: libusb-1.0|' "$build/librtlsdr.pc" > "$pc"
"$pkg_config" --static --cflags --libs "$pc"
[[ -s "$DEPS_PREFIX/lib/$archive" ]] || fail "Installed archive missing: $DEPS_PREFIX/lib/$archive"
