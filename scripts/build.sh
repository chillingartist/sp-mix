#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build one firmware variant: m0, m1 or full (default).
#   scripts/build.sh m1 [extra west build args]
# Output: build-<variant>/zephyr/zephyr.bin (flash with the Solderless updater).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
variant="${1:-full}"
shift || true

"$here/scripts/apply-zephyr-patches.sh"

args=()
if [ "$variant" != "full" ]; then
	conf="$here/conf/$variant.conf"
	if [ ! -f "$conf" ]; then
		echo "unknown variant $variant (m0, m1, full)" >&2
		exit 1
	fi
	args+=("-DEXTRA_CONF_FILE=$conf")
fi

# Which commit the binary is (shown by 'status'); -dirty: uncommitted changes
args+=("-DSP1M_GIT=$(git -C "$here" describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)")

west build -p auto -b sp1 -s "$here" -d "$here/build-$variant" "$@" -- "${args[@]}"
