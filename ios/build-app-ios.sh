#!/usr/bin/env bash
# Build, package and (optionally) run the MISRC GUI iOS app.
#
# Usage:
#   ./build-app-ios.sh [slice] [extra args]
#     slice: "sim" (default) or "device"
#
#   ./build-app-ios.sh sim              # build + install + launch on a simulator
#   ./build-app-ios.sh sim --no-launch  # build + install only
#   ./build-app-ios.sh device           # build the device .app (needs SIGN_IDENTITY)
#   SIGN_IDENTITY="Apple Development: ..." PROFILE=~/Downloads/dev.mobileprovision \
#     ./build-app-ios.sh device
#
# Flow (mirrors android/build-apk.sh's no-gradle manual packaging):
#   1. Ensure the slice's cross deps exist (.deps/install-ios*; builds them
#      via build-deps-ios.sh when missing).
#   2. Generate the Meson cross-file (ios/gen-cross-file.sh).
#   3. meson setup + compile misrc_gui (static link of everything).
#   4. Assemble MISRC.app: binary + Info.plist (version-injected) + icons.
#   5. codesign (ad-hoc for simulator; SIGN_IDENTITY for device) and, for
#      device, embed the provisioning profile.
#   6. Simulator slice: boot an iOS 17+ simulator, install, launch, and
#      capture a screenshot to ios/screenshot.png as launch evidence.
#
# REQUIRES: Xcode + accepted license (sudo xcodebuild -license).
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

SLICE="${1:-sim}"
shift || true
EXTRA_ARGS=("$@")

case "$SLICE" in
  sim)
    SDK="iphonesimulator"
    INI="$SCRIPT_DIR/aarch64-ios-sim.ini"
    PREFIX="$REPO_ROOT/.deps/install-ios-sim-arm64"
    BUILD_DIR="$REPO_ROOT/build-ios-sim"
    ;;
  device)
    SDK="iphoneos"
    INI="$SCRIPT_DIR/aarch64-ios.ini"
    PREFIX="$REPO_ROOT/.deps/install-ios-arm64"
    BUILD_DIR="$REPO_ROOT/build-ios"
    ;;
  *)
    echo "Usage: $0 [sim|device] [--no-launch] [--udid <id>]" >&2
    exit 1
    ;;
esac

log() { printf '[ios-app] %s\n' "$*"; }
fail() { printf '[ios-app] ERROR: %s\n' "$*" >&2; exit 1; }

# --- toolchain sanity (license gate) --------------------------------------
xcodebuild -version >/dev/null 2>&1 || fail "xcodebuild unavailable — accept the license first: sudo xcodebuild -license"

NO_LAUNCH=0
UDID=""
parse_args() {
  local a
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --no-launch) NO_LAUNCH=1 ;;
      --udid) shift; UDID="${1:-}" ;;
    esac
    shift
  done
}
parse_args "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

# --- 1. cross deps ---------------------------------------------------------
PC_WRAPPER="$SCRIPT_DIR/.gen/pkg-config-$SDK"
"$SCRIPT_DIR/gen-cross-file.sh" "$SLICE" >/dev/null
if ! "$PC_WRAPPER" --exists raylib >/dev/null 2>&1 || ! "$PC_WRAPPER" --exists sdl2 >/dev/null 2>&1; then
  log "cross deps missing for slice '$SLICE' — running build-deps-ios.sh (this builds ALL deps, takes a while)"
  SLICES="$SLICE" "$SCRIPT_DIR/build-deps-ios.sh"
fi
"$PC_WRAPPER" --exists raylib || fail "raylib.pc missing in $PREFIX after deps build"
"$PC_WRAPPER" --exists sdl2 || fail "sdl2.pc missing in $PREFIX after deps build"

# --- 2-3. meson build ------------------------------------------------------
if [[ -f "$BUILD_DIR/build.ninja" ]]; then
  meson setup "$BUILD_DIR" --reconfigure \
    "$REPO_ROOT/misrc_tools" --cross-file "$INI" >/dev/null
else
  meson setup "$BUILD_DIR" "$REPO_ROOT/misrc_tools" --cross-file "$INI"
fi
meson compile -C "$BUILD_DIR" misrc_gui
BIN="$BUILD_DIR/misrc_gui"
[[ -x "$BIN" ]] || fail "meson did not produce $BIN"

# --- 4. assemble MISRC.app -------------------------------------------------
APP="$BUILD_DIR/MISRC.app"
rm -rf "$APP"
mkdir -p "$APP"

VERSION="$(git -C "$REPO_ROOT" describe --tags --always 2>/dev/null || echo v0.0.0)"
VERSION_CLEAN="${VERSION#v}"
BUILD_NUM="$(git -C "$REPO_ROOT" rev-list --count HEAD 2>/dev/null || date +%Y%m%d)"

cp "$BIN" "$APP/misrc_gui"

BUNDLE_ID="${BUNDLE_ID:-org.misrc.gui}"
sed -e "s|<string>1.0.0</string>|<string>$VERSION_CLEAN</string>|" \
    -e "s|<string>1</string>|<string>$BUILD_NUM</string>|" \
    -e "s|org.misrc.gui|$BUNDLE_ID|" \
    "$SCRIPT_DIR/Info.plist" > "$APP/Info.plist"

# Icons: opaque + centered via ios/make_icons.py. The source art is a
# rounded square with a drop shadow and transparent corners; raw sips
# resizes produced transparent (black on iOS) corners and off-center art
# (see 2026-11-08 icon fix notes in the prompt log).
ICON_SRC="$REPO_ROOT/assets/Icons/MISRC_Icon.png"
if [[ -f "$ICON_SRC" ]]; then
  python3 "$SCRIPT_DIR/make_icons.py" "$ICON_SRC" "$APP"
else
  log "WARNING: $ICON_SRC missing — app will use the default icon"
fi

# --- 5. codesign -----------------------------------------------------------
if [[ "$SLICE" == "sim" ]]; then
  codesign --force --sign - "$APP"
  log "ad-hoc codesigned (simulator slice)"
else
  SIGN_IDENTITY="${SIGN_IDENTITY:-}"
  PROFILE="${PROFILE:-}"
  [[ -n "$SIGN_IDENTITY" ]] || fail "device slice needs SIGN_IDENTITY (e.g. \"Apple Development: me@x, YZ...\"); free personal teams work"
  if [[ -n "$PROFILE" ]]; then
    cp "$PROFILE" "$APP/embedded.mobileprovision"
  fi
  # Entitlements derived from the profile (team + app identifier) so the
  # signature matches what the device expects from a personal-team install.
  TEAM_ID="${TEAM_ID:-}"
  if [[ -z "$TEAM_ID" && -n "$PROFILE" ]]; then
    TEAM_ID="$(security cms -D -i "$PROFILE" 2>/dev/null | plutil -extract TeamIdentifier.0 raw -o - - 2>/dev/null)"
  fi
  if [[ -n "$TEAM_ID" ]]; then
    cat > "$APP/entitlements.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>application-identifier</key>
	<string>$TEAM_ID.$BUNDLE_ID</string>
	<key>com.apple.developer.team-identifier</key>
	<string>$TEAM_ID</string>
	<key>get-task-allow</key>
	<true/>
</dict>
</plist>
EOF
    codesign --force --sign "$SIGN_IDENTITY" --entitlements "$APP/entitlements.plist" --timestamp=none "$APP"
  else
    codesign --force --sign "$SIGN_IDENTITY" --timestamp=none "$APP"
  fi
  log "codesigned with: $SIGN_IDENTITY (team: ${TEAM_ID:-unknown})"
fi

log "built $APP"

# --- 6. simulator run ------------------------------------------------------
if [[ "$SLICE" == "sim" && "$NO_LAUNCH" -ne 1 ]]; then
  # Pick an available iOS 17+ simulator (prefer iPhone).
  if [[ -z "$UDID" ]]; then
    UDID="$(xcrun simctl list devices available --json | python3 -c '
import json, sys
data = json.load(sys.stdin)
best = None
for runtime, devices in data.get("devices", {}).items():
    if "iOS" not in runtime:
        continue
    # Runtime keys look like "com.apple.CoreSimulator.SimRuntime.iOS-26-2":
    # take the major after "iOS-" (the old split("iOS") form produced
    # "-26-2", which int() rejected and silently skipped every runtime).
    try:
        ver = int(runtime.split("iOS-")[-1].split("-")[0].split(".")[0])
    except ValueError:
        continue
    if ver < 17:
        continue
    for d in devices:
        if not d.get("isAvailable"):
            continue
        if "iPhone" in d["name"] and best is None:
            best = d["udid"]
        if best is None:
            best = d["udid"]
print(best or "")
')"
  fi
  [[ -n "$UDID" ]] || fail "no available iOS 17+ simulator found (xcrun simctl list devices available)"
  log "target simulator UDID: $UDID"

  xcrun simctl bootstatus "$UDID" -b >/dev/null 2>&1 || xcrun simctl boot "$UDID" 2>/dev/null || true
  xcrun simctl install "$UDID" "$APP"
  log "installed; launching org.misrc.gui"
  xcrun simctl launch "$UDID" org.misrc.gui
  sleep 5
  xcrun simctl io "$UDID" screenshot "$SCRIPT_DIR/screenshot.png" >/dev/null 2>&1 || true
  [[ -f "$SCRIPT_DIR/screenshot.png" ]] && log "screenshot saved: ios/screenshot.png (launch evidence — verify the GUI is visible)"
fi
