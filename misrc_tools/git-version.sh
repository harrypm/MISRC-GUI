#!/bin/sh
set -eu

V="${MISRC_TOOLS_VERSION_OVERRIDE:-${MISRC_TOOLS_VERSION:-}}"

if [ -z "$V" ]; then
	V=$(git describe --tags --dirty --match 'v*' --match 'misrc_tools-*' 2>/dev/null || true)
fi

if [ -z "$V" ]; then
	SHA=$(git rev-parse --short HEAD 2>/dev/null || echo "nogit")
	DIRTY=""
	if git diff --quiet --ignore-submodules -- 2>/dev/null; then
		:
	else
		DIRTY="-dirty"
	fi
	V="dev-${SHA}${DIRTY}"
fi

case "$V" in
	misrc_tools-*)
		V="${V#misrc_tools-}"
		;;
esac

printf '%s\n' "$V"
