#!/usr/bin/env bash
set -euo pipefail

CAPTURE_BIN="${1:?usage: capture_stability_ci.sh <capture_bin> <extract_bin> <artifact_dir>}"
EXTRACT_BIN="${2:?usage: capture_stability_ci.sh <capture_bin> <extract_bin> <artifact_dir>}"
ARTIFACT_DIR="${3:?usage: capture_stability_ci.sh <capture_bin> <extract_bin> <artifact_dir>}"

HELP_LOOPS="${CAPTURE_STABILITY_HELP_LOOPS:-20}"
DEVICES_LOOPS="${CAPTURE_STABILITY_DEVICES_LOOPS:-15}"
OPENFAIL_LOOPS="${CAPTURE_STABILITY_OPENFAIL_LOOPS:-12}"
EXTRACT_LOOPS="${CAPTURE_STABILITY_EXTRACT_LOOPS:-10}"
CAPTURE_TIMEOUT_SECONDS="${CAPTURE_STABILITY_TIMEOUT_SECONDS:-45}"

fail() {
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

require_file() {
  local path="$1"
  [[ -f "$path" ]] || fail "required file does not exist: $path"
}

run_with_timeout() {
  local timeout_s="$1"
  shift
  "$@" &
  local pid=$!
  local elapsed=0
  while kill -0 "$pid" 2>/dev/null; do
    if (( elapsed >= timeout_s )); then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      return 124
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  wait "$pid"
}

check_help_usage() {
  local bin="$1"
  local log="$2"
  local rc
  set +e
  "$bin" -h >"$log" 2>&1
  rc=$?
  set -e
  [[ "$rc" -eq 1 ]] || fail "expected -h to exit 1 for $bin, got $rc"
  grep -qi "Usage" "$log" || fail "missing Usage text in help output for $bin ($log)"
}
files_equal() {
  local left="$1"
  local right="$2"
  if command -v cmp >/dev/null 2>&1; then
    cmp -s "$left" "$right"
    return $?
  fi
  if command -v cksum >/dev/null 2>&1; then
    local left_sum right_sum
    left_sum="$(cksum "$left" | awk '{print $1\":\"$2}')"
    right_sum="$(cksum "$right" | awk '{print $1\":\"$2}')"
    [[ "$left_sum" == "$right_sum" ]]
    return $?
  fi
  if command -v sha256sum >/dev/null 2>&1; then
    local left_sha right_sha
    left_sha="$(sha256sum "$left" | awk '{print $1}')"
    right_sha="$(sha256sum "$right" | awk '{print $1}')"
    [[ "$left_sha" == "$right_sha" ]]
    return $?
  fi
  fail "no supported file comparison utility available (cmp, cksum, or sha256sum)"
}

rm -rf "$ARTIFACT_DIR"
mkdir -p "$ARTIFACT_DIR"

[[ -x "$CAPTURE_BIN" ]] || require_file "$CAPTURE_BIN"
[[ -x "$EXTRACT_BIN" ]] || require_file "$EXTRACT_BIN"

for i in $(seq 1 "$HELP_LOOPS"); do
  check_help_usage "$CAPTURE_BIN" "$ARTIFACT_DIR/capture_help_${i}.log"
  check_help_usage "$EXTRACT_BIN" "$ARTIFACT_DIR/extract_help_${i}.log"
done

for i in $(seq 1 "$DEVICES_LOOPS"); do
  log="$ARTIFACT_DIR/devices_${i}.log"
  set +e
  "$CAPTURE_BIN" --devices >"$log" 2>&1
  rc=$?
  set -e
  [[ "$rc" -eq 1 ]] || fail "expected --devices to exit 1, got $rc (iteration $i)"
  grep -q "Devices that can be used" "$log" || fail "device listing heading missing in $log"
done

for i in $(seq 1 "$OPENFAIL_LOOPS"); do
  rf_out="$ARTIFACT_DIR/openfail_${i}.rf"
  log="$ARTIFACT_DIR/openfail_${i}.log"
  set +e
  "$CAPTURE_BIN" -d 9999 -n 65536 -a "$rf_out" -w >"$log" 2>&1
  rc=$?
  set -e
  [[ "$rc" -ne 0 ]] || fail "expected invalid-device open path to fail (iteration $i)"
  grep -Eq "Failed to (open|allocate).+device" "$log" || fail "expected open failure message missing in $log"
  rm -f "$rf_out"
done

INPUT_FILE="$ARTIFACT_DIR/extract_input.bin"
dd if=/dev/zero of="$INPUT_FILE" bs=4096 count=32 status=none

REF_A="$ARTIFACT_DIR/extract_ref_a.bin"
REF_B="$ARTIFACT_DIR/extract_ref_b.bin"
REF_X="$ARTIFACT_DIR/extract_ref_x.bin"
"$EXTRACT_BIN" -i "$INPUT_FILE" -a "$REF_A" -b "$REF_B" -x "$REF_X" >"$ARTIFACT_DIR/extract_ref.log" 2>&1
[[ -s "$REF_A" && -s "$REF_B" && -s "$REF_X" ]] || fail "reference extract outputs were empty"

for i in $(seq 1 "$EXTRACT_LOOPS"); do
  OUT_A="$ARTIFACT_DIR/extract_a_${i}.bin"
  OUT_B="$ARTIFACT_DIR/extract_b_${i}.bin"
  OUT_X="$ARTIFACT_DIR/extract_x_${i}.bin"
  "$EXTRACT_BIN" -i "$INPUT_FILE" -a "$OUT_A" -b "$OUT_B" -x "$OUT_X" >"$ARTIFACT_DIR/extract_${i}.log" 2>&1
  files_equal "$REF_A" "$OUT_A" || fail "ADC A extract output mismatch at iteration $i"
  files_equal "$REF_B" "$OUT_B" || fail "ADC B extract output mismatch at iteration $i"
  files_equal "$REF_X" "$OUT_X" || fail "AUX extract output mismatch at iteration $i"
  rm -f "$OUT_A" "$OUT_B" "$OUT_X"
done

DEVICE_SCAN_LOG="$ARTIFACT_DIR/devices_timed_scan.log"
set +e
"$CAPTURE_BIN" --devices >"$DEVICE_SCAN_LOG" 2>&1
rc=$?
set -e
[[ "$rc" -eq 1 ]] || fail "expected timed pre-scan --devices to exit 1, got $rc"

timed_capture_result="skipped_no_device"
timed_capture_device="none"
timed_capture_rc="NA"

timed_capture_device="$(sed -nE 's/^[[:space:]]+([0-9]+):.*/\1/p' "$DEVICE_SCAN_LOG" | head -n1 || true)"
if [[ -z "$timed_capture_device" ]]; then
  timed_capture_device="$(sed -nE 's/^[[:space:]]+(.+):[[:space:]].*$/\1/p' "$DEVICE_SCAN_LOG" | grep '://' | head -n1 || true)"
fi

TIMED_CAPTURE_LOG="$ARTIFACT_DIR/timed_capture.log"
TIMED_CAPTURE_OUT="$ARTIFACT_DIR/timed_capture.rf"
if [[ -n "$timed_capture_device" ]]; then
  set +e
  run_with_timeout "$CAPTURE_TIMEOUT_SECONDS" \
    "$CAPTURE_BIN" -d "$timed_capture_device" -n 400000 -a "$TIMED_CAPTURE_OUT" -w >"$TIMED_CAPTURE_LOG" 2>&1
  timed_capture_rc=$?
  set -e
  [[ "$timed_capture_rc" -ne 124 ]] || fail "timed capture exceeded ${CAPTURE_TIMEOUT_SECONDS}s timeout"
  [[ "$timed_capture_rc" -eq 0 ]] || fail "timed capture failed for detected device $timed_capture_device (rc=$timed_capture_rc)"
  [[ -s "$TIMED_CAPTURE_OUT" ]] || fail "timed capture output file was empty"
  timed_capture_result="executed"
else
  printf 'No capture device detected in CI runner; timed capture skipped.\n' >"$TIMED_CAPTURE_LOG"
fi

SUMMARY_FILE="$ARTIFACT_DIR/summary.txt"
{
  printf 'capture_help_loops=%s\n' "$HELP_LOOPS"
  printf 'device_enum_loops=%s\n' "$DEVICES_LOOPS"
  printf 'openfail_loops=%s\n' "$OPENFAIL_LOOPS"
  printf 'extract_loops=%s\n' "$EXTRACT_LOOPS"
  printf 'timed_capture_result=%s\n' "$timed_capture_result"
  printf 'timed_capture_device=%s\n' "$timed_capture_device"
  printf 'timed_capture_rc=%s\n' "$timed_capture_rc"
} >"$SUMMARY_FILE"

cat "$SUMMARY_FILE"
