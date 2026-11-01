#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${TMPDIR:-/tmp}/misrc-monitor-video-stage1-test-$$"
trap 'rm -f "$BIN"' EXIT INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 \
  "$ROOT/misrc_tools/misrc_gui/net/gui_monitor_video_wire.c" \
  "$ROOT/misrc_tools/misrc_gui/net/gui_monitor_video_i420.c" \
  "$ROOT/misrc_tools/test/gui_monitor_video_stage1_test.c" -o "$BIN"
"$BIN"
