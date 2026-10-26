#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build one firmware variant: m0, m1 or full (default).
#   scripts/build.sh m1 [extra west build args]
# Output: build-<variant>/zephyr/zephyr.bin (flash with the Solderless updater),
# build-<variant>-midi with SP1M_MIDI and ...-low with SP1M_STORAGE=low. Each
# combination has its own directory, so an option cannot stick in a cached build.
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

# SP1M_STORAGE=low keeps the settings in their own region below 0xCE000 instead of
# the default 0xF7000 (see boards/teenage_engineering/sp1/sp1-storage-low.overlay).
case "${SP1M_STORAGE:-default}" in
	default) ;;
	low) args+=("-DEXTRA_DTC_OVERLAY_FILE=$here/boards/teenage_engineering/sp1/sp1-storage-low.overlay")
	     low_suffix=-low ;;
	*) echo "unknown SP1M_STORAGE ${SP1M_STORAGE} (default, low)" >&2; exit 1 ;;
esac

# SP1M_MIDI=1 adds MIDI control over USB (docs/MIDI.md); SP1M_MIDI_CHANNEL=1..16
if [ -n "${SP1M_MIDI:-}" ] && [ "${SP1M_MIDI}" != 0 ]; then
	args+=("-DCONFIG_SP1M_MIDI=y")
	midi_suffix=-midi
	if [ -n "${SP1M_MIDI_CHANNEL:-}" ]; then
		args+=("-DCONFIG_SP1M_MIDI_CHANNEL=${SP1M_MIDI_CHANNEL}")
	fi
fi

# Which commit the binary is (shown by 'status'); -dirty: uncommitted changes.
# SP1M_GIT overrides it, e.g. with the version of a release whose tag does not
# exist yet.
args+=("-DSP1M_GIT=${SP1M_GIT:-$(git -C "$here" describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)}")

west build -p auto -b sp1 -s "$here" -d "$here/build-$variant${midi_suffix:-}${low_suffix:-}" "$@" -- "${args[@]}"
