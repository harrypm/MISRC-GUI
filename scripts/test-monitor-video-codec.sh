#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
NET="$ROOT/misrc_tools/misrc_gui/net"
CC=${CC:-cc}
if pkg-config --exists openh264 2>/dev/null; then
    CFLAGS="$(pkg-config --cflags openh264)"
    LIBS="$(pkg-config --libs openh264)"
    HAVE=1
else
    CFLAGS=
    LIBS=
    HAVE=0
fi
BIN="${TMPDIR:-/tmp}/misrc-mvd-codec-$$"
trap 'rm -f "$BIN"' EXIT HUP INT TERM
# shellcheck disable=SC2086
$CC -std=c11 -Wall -Wextra -Werror -O2 -DMISRC_HAVE_OPENH264=$HAVE $CFLAGS \
    "$NET/gui_monitor_video_codec.c" "$NET/gui_monitor_video_i420.c" \
    "$ROOT/misrc_tools/test/gui_monitor_video_codec_test.c" -o "$BIN" $LIBS
"$BIN"
