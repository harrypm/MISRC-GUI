#!/usr/bin/env bash
# Fake pkg-config validates fail-fast behavior without installing packages.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/pkg-config" <<'MOCK'
#!/usr/bin/env bash
case "$1" in
  --exists) [[ " ${MOCK_MISSING:-} " != *" $2 "* ]] ;;
  --atleast-version=1.5.0) [[ "${MOCK_OLD_FLAC:-0}" != 1 && "$2" == flac ]] ;;
  --modversion) [[ "${MOCK_OLD_FLAC:-0}" == 1 ]] && echo 1.3.3 || echo 1.5.0 ;;
  *) exit 2 ;;
esac
MOCK
chmod +x "$TMP/pkg-config"
export PKG_CONFIG="$TMP/pkg-config"
MOCK_MISSING='' MOCK_OLD_FLAC=0 bash "$ROOT/scripts/check-build-deps.sh" --codec >"$TMP/out"
MOCK_MISSING='' MOCK_OLD_FLAC=0 bash "$ROOT/scripts/check-build-deps.sh" --all >"$TMP/out"
MOCK_MISSING='' MOCK_OLD_FLAC=0 bash "$ROOT/scripts/check-build-deps.sh" --gui >"$TMP/out"
if MOCK_MISSING='openh264 fftw3f' bash "$ROOT/scripts/check-build-deps.sh" --all >"$TMP/out" 2>"$TMP/err"; then
  echo 'ERROR: missing libraries passed preflight' >&2; exit 1
fi
grep -q 'libopenh264-dev' "$TMP/err"
grep -q 'libfftw3-dev' "$TMP/err"
if MOCK_MISSING='hsdaoh raylib' bash "$ROOT/scripts/check-build-deps.sh" --gui >"$TMP/out" 2>"$TMP/err"; then
  echo 'ERROR: missing GUI vendor libraries passed preflight' >&2; exit 1
fi
grep -q 'vendored-hsdaoh' "$TMP/err"
grep -q 'vendored-raylib' "$TMP/err"
if MOCK_MISSING='' MOCK_OLD_FLAC=1 bash "$ROOT/scripts/check-build-deps.sh" --all >"$TMP/out" 2>"$TMP/err"; then
  echo 'ERROR: old libFLAC passed preflight' >&2; exit 1
fi
grep -q 'libFLAC 1.3.3 is too old' "$TMP/err"
if bash "$ROOT/scripts/check-build-deps.sh" --invalid >"$TMP/out" 2>"$TMP/err"; then
  echo 'ERROR: unsupported preflight mode passed' >&2; exit 1
fi
echo 'build-deps preflight regression tests: PASS'
