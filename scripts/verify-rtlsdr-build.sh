#!/usr/bin/env bash
# Check the linked GUI binary without executing it or opening USB hardware.
set -eu

if [ "$#" -ne 1 ]; then
    printf 'Usage: %s <misrc-gui-binary>\n' "$0" >&2
    exit 2
fi

binary=$1
if [ ! -f "$binary" ] || [ ! -r "$binary" ]; then
    printf 'Cannot read RTL-SDR build artifact %s\n' "$binary" >&2
    exit 2
fi

if ! LC_ALL=C grep -aFq -- '[RTL-SDR] Failed to open device' "$binary"; then
    printf 'RTL-SDR GUI backend marker missing from %s\n' "$binary" >&2
    exit 1
fi

if ! LC_ALL=C grep -aFq -- 'RTL-SDR Blog V4 Detected' "$binary"; then
    printf 'Linked librtlsdr marker missing from %s\n' "$binary" >&2
    exit 1
fi

printf 'RTL-SDR GUI backend and librtlsdr verified in %s\n' "$binary"
