# SPDX-License-Identifier: MIT
#
# Stock SP-1 units are flashed with the Solderless web updater
# (https://solderless.engineering) using build/zephyr/zephyr.bin.
# The runners below are only for units with SWD access.

board_runner_args(jlink "--device=nRF52840_xxAA" "--speed=4000")
board_runner_args(pyocd "--target=nrf52840" "--frequency=4000000")
include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
