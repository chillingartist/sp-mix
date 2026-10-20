#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Apply the SP-Mix patches to the Zephyr tree of this west workspace.
# Safe to run repeatedly: patches that are already applied are skipped.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
zephyr="${ZEPHYR_BASE:-$(cd "$here/.." && pwd)/zephyr}"

if [ ! -d "$zephyr/.git" ]; then
	echo "Zephyr tree not found at $zephyr (set ZEPHYR_BASE)" >&2
	exit 1
fi

for patch in "$here"/zephyr-patches/*.patch; do
	name="$(basename "$patch")"
	if git -C "$zephyr" apply --reverse --check "$patch" 2>/dev/null; then
		echo "already applied: $name"
	elif git -C "$zephyr" apply --check "$patch"; then
		git -C "$zephyr" apply "$patch"
		echo "applied: $name"
	else
		echo "cannot apply $name to $zephyr" >&2
		exit 1
	fi
done
