#!/usr/bin/env bash
# Build a debug-signed, zipaligned Android APK from the cross-compiled
# libmisrc_gui.so (+ bundled libhsdaoh.so) + AndroidManifest.xml + icon.
#
# Uses the built-in android.app.NativeActivity (no custom Java/dex needed
# for the basic release — raylib rcore_android.c provides
# ANativeActivity_onCreate which calls the app's main()).
#
# Prerequisites:
#   - libmisrc_gui.so already built (run build-deps-android.sh + meson first)
#   - Android SDK build-tools;34.0.0 + platforms;android-30 installed
#   - ANDROID_HOME set (or ~/Android/Sdk)
#
# Env:
#   BUILD_DIR     - meson build dir (default: build-android)
#   DEPS_PREFIX   - cross deps prefix (for libhsdaoh.so; default: .deps/install-android-arm64)
#   ANDROID_HOME  - SDK root (default: ~/Android/Sdk)
#   KEYSTORE      - debug keystore path (default: android/debug.keystore, generated if missing)
#   OUT_DIR       - output dir (default: .ci-artifacts/android-apk)
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-android}"
DEPS_PREFIX="${DEPS_PREFIX:-$REPO_ROOT/.deps/install-android-arm64}"
KEYSTORE="${KEYSTORE:-$SCRIPT_DIR/debug.keystore}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/.ci-artifacts/android-apk}"

BT="$ANDROID_HOME/build-tools/34.0.0"
AAPT2="$BT/aapt2"
ZIPALIGN="$BT/zipalign"
APKSIGNER="$BT/apksigner"
# Compile Java/dex against the targetSdk jar (android-34) so API 31+/33+ symbols
# like PendingIntent.FLAG_MUTABLE and Context.RECEIVER_NOT_EXPORTED resolve.
# minSdk stays 30 (set in the manifest + aapt2 --min-sdk-version); runtime
# guards (Build.VERSION.SDK_INT >= N) gate the newer-API calls.
ANDROID_JAR="$ANDROID_HOME/platforms/android-34/android.jar"

log() { printf '[android-apk] %s\n' "$*"; }
fail() { printf '[android-apk] ERROR: %s\n' "$*" >&2; exit 1; }

[[ -f "$ANDROID_JAR" ]] || fail "android.jar not found at $ANDROID_JAR (install platforms;android-30)"
[[ -f "$BUILD_DIR/libmisrc_gui.so" ]] || fail "libmisrc_gui.so not found in $BUILD_DIR (run meson compile first)"
[[ -f "$DEPS_PREFIX/lib/libhsdaoh.so" ]] || fail "libhsdaoh.so not found in $DEPS_PREFIX (run build-deps-android.sh)"
[[ -x "$AAPT2" ]] || fail "aapt2 not found at $AAPT2"
[[ -x "$ZIPALIGN" ]] || fail "zipalign not found at $ZIPALIGN"
[[ -x "$APKSIGNER" ]] || fail "apksigner not found at $APKSIGNER"

# Resolve version from git tags (mirrors build-appimage-local.sh).
VERSION="$(git -C "$REPO_ROOT" describe --tags --dirty --match 'v*' --match 'misrc_tools-*' 2>/dev/null || true)"
if [[ -z "$VERSION" ]]; then
  VERSION="dev-$(git -C "$REPO_ROOT" rev-parse --short HEAD)"
fi
case "$VERSION" in misrc_tools-*) VERSION="${VERSION#misrc_tools-}";; esac

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

log "Building MISRC GUI APK version=$VERSION"

# --- 1. Prepare resources (icon) ---
RES_DIR="$WORK/res"
mkdir -p "$RES_DIR/mipmap-xxhdpi" "$RES_DIR/xml"
ICON_SRC="$REPO_ROOT/assets/Icons/MISRC_Icon.png"
[[ -f "$ICON_SRC" ]] || fail "Icon not found: $ICON_SRC"
cp "$ICON_SRC" "$RES_DIR/mipmap-xxhdpi/ic_launcher.png"
# USB device filter: maps MS2130/MS2131 VID/PID so plugging one launches MISRC
# (manifest USB_DEVICE_ATTACHED intent-filter references @xml/device_filter).
FILTER_SRC="$SCRIPT_DIR/res/xml/device_filter.xml"
[[ -f "$FILTER_SRC" ]] || fail "device_filter.xml not found: $FILTER_SRC"
cp "$FILTER_SRC" "$RES_DIR/xml/device_filter.xml"

# --- 2. Compile resources ---
log "compiling resources..."
"$AAPT2" compile --dir "$RES_DIR" -o "$WORK/resources.zip"

# --- 3. Link manifest + resources -> base APK ---
log "linking manifest + resources..."
"$AAPT2" link \
  --manifest "$SCRIPT_DIR/AndroidManifest.xml" \
  -I "$ANDROID_JAR" \
  --min-sdk-version 30 \
  --target-sdk-version 34 \
  -o "$WORK/base.apk" \
  "$WORK/resources.zip"

# --- 3b. Compile Java stub -> classes.dex ---
# A classes.dex is REQUIRED even for a pure-NativeActivity app: Android's
# installer rejects APKs with no dex ("package appears to be invalid"). The
# stub dev.misrc.gui.MainActivity extends android.app.NativeActivity and adds
# no logic; it exists to produce a valid dex and to be the future JNI hook.
log "compiling Java stub -> classes.dex..."
JAVA_SRC="$SCRIPT_DIR/java/dev/misrc/gui/MainActivity.java"
[[ -f "$JAVA_SRC" ]] || fail "MainActivity.java not found at $JAVA_SRC"
CLASSES_DIR="$WORK/classes"
mkdir -p "$CLASSES_DIR"
javac --release 8 -d "$CLASSES_DIR" -classpath "$ANDROID_JAR" "$JAVA_SRC"
mkdir -p "$WORK/dex"
"$BT/d8" --output "$WORK/dex" --lib "$ANDROID_JAR" \
  $(find "$CLASSES_DIR" -name '*.class')
test -f "$WORK/dex/classes.dex"
# Add classes.dex at APK root (required location).
( cd "$WORK/dex" && zip -j0 "$WORK/base.apk" classes.dex )

# --- 4. Add native libraries (libmisrc_gui.so + libhsdaoh.so) ---
log "adding native libs..."
mkdir -p "$WORK/libdir/lib/arm64-v8a"
cp "$BUILD_DIR/libmisrc_gui.so" "$WORK/libdir/lib/arm64-v8a/"
cp "$DEPS_PREFIX/lib/libhsdaoh.so" "$WORK/libdir/lib/arm64-v8a/"
# NOTE: no -j flag — the lib/arm64-v8a/ path prefix MUST be preserved (Android
# only loads .so files from lib/<abi>/ inside the APK). -0 stores uncompressed
# so zipalign can page-align the .so files.
( cd "$WORK/libdir" && zip -0 "$WORK/base.apk" lib/arm64-v8a/*.so )

# --- 5. Zipalign ---
log "zipaligning..."
"$ZIPALIGN" -p -f 4 "$WORK/base.apk" "$WORK/aligned.apk"

# --- 6. Generate debug keystore if missing ---
if [[ ! -f "$KEYSTORE" ]]; then
  log "generating debug keystore at $KEYSTORE"
  keytool -genkeypair \
    -keystore "$KEYSTORE" \
    -storepass android -keypass android \
    -alias androiddebugkey \
    -dname "CN=Android Debug,O=Android,C=US" \
    -keyalg RSA -keysize 2048 -validity 10000
fi

# --- 7. Sign ---
log "signing APK..."
"$APKSIGNER" sign \
  --ks "$KEYSTORE" \
  --ks-pass pass:android \
  --key-pass pass:android \
  --out "$WORK/signed.apk" \
  "$WORK/aligned.apk"

# --- 8. Verify ---
log "verifying signature..."
"$APKSIGNER" verify "$WORK/signed.apk"

# --- 9. Output ---
mkdir -p "$OUT_DIR"
OUTPUT="$OUT_DIR/misrc_gui-${VERSION}-android-arm64.apk"
cp "$WORK/signed.apk" "$OUTPUT"
chmod +x "$OUTPUT"

log "=== APK built: $OUTPUT ==="
ls -la "$OUTPUT"
"$AAPT2" dump badging "$OUTPUT" | grep -iE "sdkVersion|nativeCode|application-label|package" || true
log "done."
