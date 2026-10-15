# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

# The SP-1 board definition lives in this repository.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1mixer)

file(GLOB core_sources src/core/*.c)
target_sources(app PRIVATE ${core_sources})

# Keep plain copy loops as loops: the toolchain's memcpy/memset are byte
# loops, far slower than the word copies the audio path does itself. No
# fused multiply-add (VMLA/VFMA take 3 cycles on the Cortex-M4F, a VMUL plus
# VADD 2): -std=c99 already implies it, this keeps it under -std=gnu*.
target_compile_options(app PRIVATE -fno-tree-loop-distribute-patterns -ffp-contract=off)

# Commit the binary was built from, for 'status' (scripts/build.sh passes it)
if(NOT SP1M_GIT)
  set(SP1M_GIT unknown)
endif()
target_compile_definitions(app PRIVATE SP1M_GIT="${SP1M_GIT}")

target_sources(app PRIVATE
  src/platform/main.c
  src/platform/board_io.c
  src/platform/safety.c
  src/platform/stall.c
  src/platform/leds.c
  src/platform/inputs.c
  src/platform/control.c
  src/platform/store.c
)
target_sources_ifdef(CONFIG_SP1M_USB app PRIVATE src/platform/usb.c)
target_sources_ifdef(CONFIG_SP1M_CDC app PRIVATE src/platform/console.c)
target_sources_ifdef(CONFIG_SP1M_AUDIO_OUT app PRIVATE
  src/platform/audio.c
  src/platform/codec.c
  src/platform/tstamp.c
)

target_include_directories(app PRIVATE src/core src/platform)
