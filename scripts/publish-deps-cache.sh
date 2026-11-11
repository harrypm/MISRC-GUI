#!/usr/bin/env bash
# Package .deps/install into a per-platform tar.xz + sha256 ready for upload to
# the harrypm/MISRC-ci-cache GitHub release (the same mechanism already used for
# libFLAC 1.5.0 prebuilts — see .github/workflows/build.yml flac-cache flow).
#
# This lets first builds and CI cold starts DOWNLOAD prebuilt hsdaoh+libuvc+
# raylib instead of compiling them from source, saving ~5min per cold run. Since
# hsdaoh is two fixed versions (upstream + MISRC) with no active churn, these
# archives are stable across releases.
#
# Usage:
#   scripts/publish-deps-cache.sh <platform> <arch>
#     platform: linux | windows | macos
#     arch:     x86_64 | arm64 | aarch64
#
#   Example:
#     scripts/publish-deps-cache.sh linux x86_64
#
# This script ONLY creates the archive + sha256 and prints the upload command.
# It does NOT run `gh release upload` (that is external, public-repo-mutating,
# and needs a GH token). Run the printed command manually after reviewing.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DEPS_PREFIX="${DEPS_PREFIX:-$REPO_ROOT/.deps/install}"

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <platform> <arch>" >&2
  echo "  platform: linux | windows | macos" >&2
  echo "  arch:     x86_64 | arm64 | aarch64" >&2
  exit 2
fi

PLATFORM="$1"
ARCH="$2"

case "$PLATFORM" in
  linux|windows|macos) ;;
  *) echo "ERROR: unknown platform '$PLATFORM' (expected linux|windows|macos)" >&2; exit 2 ;;
esac
case "$ARCH" in
  x86_64|arm64|aarch64) ;;
  *) echo "ERROR: unknown arch '$ARCH' (expected x86_64|arm64|aarch64)" >&2; exit 2 ;;
esac

if [[ ! -d "$DEPS_PREFIX/lib/pkgconfig" ]]; then
  echo "ERROR: .deps/install not found at $DEPS_PREFIX" >&2
  echo "Run scripts/build-deps-windows.sh (Windows) or scripts/build-deps-unix.sh (Linux/macOS) first." >&2
  exit 1
fi

log() { printf '[publish-deps-cache] %s\n' "$*"; }

# Verify the deps are complete before packaging.
PC_DIR="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig"
for pc in hsdaoh raylib; do
  if ! PKG_CONFIG_PATH="$PC_DIR" pkg-config --exists "$pc" 2>/dev/null; then
    echo "ERROR: $pc.pc not resolvable from $DEPS_PREFIX — deps build may be incomplete." >&2
    exit 1
  fi
  log "  $pc: $(PKG_CONFIG_PATH="$PC_DIR" pkg-config --modversion "$pc")"
done

OUT_DIR="${OUT_DIR:-$REPO_ROOT/.ci-artifacts/deps-cache}"
mkdir -p "$OUT_DIR"
ARCHIVE_NAME="deps-${PLATFORM}-${ARCH}.tar.xz"
ARCHIVE_PATH="$OUT_DIR/$ARCHIVE_NAME"
SHA_PATH="$OUT_DIR/${ARCHIVE_NAME}.sha256"

log "Packaging $DEPS_PREFIX -> $ARCHIVE_PATH"
# Exclude the stamp file (.build-stamp) and any .git dirs from the archive —
# the stamp is host-specific and would cause a false cache-hit on a different
# machine. The consumer re-stamps from their own input hash.
tar -C "$REPO_ROOT/.deps" \
  --exclude='install/.build-stamp' \
  --exclude='*/.git' \
  -cJf "$ARCHIVE_PATH" \
  install/

SHA="$(sha256sum "$ARCHIVE_PATH" | awk '{print $1}')"
echo "$SHA  $ARCHIVE_NAME" > "$SHA_PATH"
log "sha256: $SHA"
log "Archive: $ARCHIVE_PATH ($(du -h "$ARCHIVE_PATH" | awk '{print $1}'))"

# The release tag convention mirrors the flac-cache: deps-<platform>-<arch>.
# Create the release once with: gh release create deps-${PLATFORM}-${ARCH} \
#   --repo harrypm/MISRC-ci-cache --notes "Prebuilt hsdaoh+libuvc+raylib for ${PLATFORM} ${ARCH}"
RELEASE_TAG="deps-${PLATFORM}-${ARCH}"
CACHE_REPO="harrypm/MISRC-ci-cache"

log ""
log "To upload, run:"
log "  gh release upload ${RELEASE_TAG} \\"
log "    --repo ${CACHE_REPO} \\"
log "    \"${ARCHIVE_PATH}\" \"${SHA_PATH}\""
log ""
log "To consume in CI, add a step before the deps build:"
log "  curl -fsSL -o /tmp/${ARCHIVE_NAME} \\"
log "    https://github.com/${CACHE_REPO}/releases/download/${RELEASE_TAG}/${ARCHIVE_NAME}"
log "  curl -fsSL -o /tmp/${ARCHIVE_NAME}.sha256 \\"
log "    https://github.com/${CACHE_REPO}/releases/download/${RELEASE_TAG}/${ARCHIVE_NAME}.sha256"
log "  (cd /tmp && sha256sum -c ${ARCHIVE_NAME}.sha256)"
log "  mkdir -p .deps && tar -xf /tmp/${ARCHIVE_NAME} -C .deps --strip-components=0"
log ""
log "OK: archive + sha256 ready at $OUT_DIR"
