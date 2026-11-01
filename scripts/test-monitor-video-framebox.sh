#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${TMPDIR:-/tmp}/misrc-monitor-video-framebox-$$"
trap 'rm -f "$BIN"' EXIT INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -pthread \
  "$ROOT/misrc_tools/misrc_gui/net/gui_monitor_video_framebox.c" \
  "$ROOT/misrc_tools/test/gui_monitor_video_framebox_test.c" -o "$BIN"
"$BIN"
