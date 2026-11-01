#!/usr/bin/env bash
# Native MISRC GUI dependency preflight. No compilation, installs, or writes.
set -euo pipefail
mode="${1:---all}"
case "$mode" in
  --codec) modules=(openh264) ;;
  --all) modules=(openh264 fftw3f flac libusb-1.0 soxr libuvc libzstd) ;;
  --gui) modules=(openh264 fftw3f flac libusb-1.0 soxr libuvc libzstd hsdaoh raylib) ;;
  *) echo "[build-deps-check] ERROR: Usage: $0 [--codec|--all|--gui]" >&2; exit 2 ;;
esac

pc="${PKG_CONFIG:-pkg-config}"
if ! command -v "$pc" >/dev/null 2>&1; then
  echo "[build-deps-check] ERROR: pkg-config executable not found: $pc" >&2
  exit 1
fi

apt_name() {
  case "$1" in
    openh264) echo libopenh264-dev ;;
    fftw3f) echo libfftw3-dev ;;
    flac) echo libflac-dev ;;
    libusb-1.0) echo libusb-1.0-0-dev ;;
    soxr) echo libsoxr-dev ;;
    libuvc) echo libuvc-dev ;;
    libzstd) echo libzstd-dev ;;
    hsdaoh|raylib) echo "vendored-$1" ;;
    *) echo "$1" ;;
  esac
}

missing=()
apt_packages=()
for module in "${modules[@]}"; do
  if ! "$pc" --exists "$module"; then
    missing+=("$module")
    if [[ "$module" != hsdaoh && "$module" != raylib ]]; then
      apt_packages+=("$(apt_name "$module")")
    fi
  fi
done

if (("${#missing[@]}" > 0)); then
  echo "[build-deps-check] ERROR: Missing pkg-config development packages:" >&2
  for module in "${missing[@]}"; do
    printf '[build-deps-check]   %-12s  -> %s\n' "$module" "$(apt_name "$module")" >&2
  done
  if [[ "$(uname -s)" == Linux* ]] && (("${#apt_packages[@]}" > 0)); then
    printf '[build-deps-check] Ubuntu/Debian: sudo apt install' >&2
    printf ' %s' "${apt_packages[@]}" >&2
    printf '\n' >&2
  fi
  if [[ "$mode" == --gui ]]; then
    echo "[build-deps-check] hsdaoh/raylib: build vendored deps with scripts/build-deps-unix.sh" >&2
  fi
  echo "[build-deps-check] Build aborted before compilation." >&2
  exit 1
fi

if [[ "$mode" != --codec ]] && ! "$pc" --atleast-version=1.5.0 flac; then
  version="$("$pc" --modversion flac 2>/dev/null || echo unknown)"
  echo "[build-deps-check] ERROR: libFLAC $version is too old; need >= 1.5.0." >&2
  echo "[build-deps-check] Ubuntu 22.04: use the FLAC 1.5.0 vendored/prebuilt flow from .github/workflows/build.yml." >&2
  exit 1
fi

echo "[build-deps-check] OK: $mode"
