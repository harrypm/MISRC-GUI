#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if ! pkg-config --exists libavcodec libavutil libswscale; then
  echo "[ffmpeg-test] SKIP: install libavcodec-dev libavutil-dev libswscale-dev"
  exit 0
fi
BIN="${TMPDIR:-/tmp}/misrc-monitor-ffmpeg-test-$$"
trap 'rm -f "$BIN"' EXIT INT TERM
"${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror -DMISRC_HAVE_FFMPEG=1 \
  $(pkg-config --cflags libavcodec libavutil libswscale) \
  "$ROOT/misrc_tools/misrc_gui/net/gui_monitor_video_ffmpeg.c" \
  "$ROOT/misrc_tools/test/gui_monitor_video_ffmpeg_test.c" \
  $(pkg-config --libs libavcodec libavutil libswscale) -o "$BIN"
"$BIN"
