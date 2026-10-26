# SPDX-License-Identifier: MIT
#
# Host unit tests for the portable firmware core (src/core).
#   cmake -S tests/host -B build-host && cmake --build build-host && ctest --test-dir build-host

cmake_minimum_required(VERSION 3.20)
project(sp1m_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core)
file(GLOB CORE_SOURCES ${CORE}/*.c)

add_executable(sp1m_tests
  main.c
  test_ring.c
  test_mixer.c
  test_asrc.c
  test_rate_est.c
  test_buttons.c
  test_fader.c
  test_scene.c
  test_ui.c
  test_cli.c
  test_midi_map.c
  test_ctrl.c
  ${CORE_SOURCES}
)
target_include_directories(sp1m_tests PRIVATE ${CORE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(sp1m_tests PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing
  -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(sp1m_tests PRIVATE -fsanitize=address,undefined)
target_link_libraries(sp1m_tests PRIVATE m)

set(MIDI ${CORE}/midi)
add_library(sp1m_midi STATIC
  ${MIDI}/midi_hr.c
  ${MIDI}/midi_bus.c
  ${MIDI}/midi_assign.c
  ${MIDI}/midi_params.c
  ${MIDI}/usb_rt_parse.c
)
target_include_directories(sp1m_midi PUBLIC ${MIDI})
target_compile_options(sp1m_midi PRIVATE -Wall -Wextra)
target_link_libraries(sp1m_tests PRIVATE sp1m_midi)

# Probe compilation AND linking: a compiler can accept the flag while its
# sanitizer runtime is unavailable. Keep the vendor tests usable in that case.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
check_c_source_compiles("int main(void) { return 0; }" SP1M_MIDI_SANITIZERS)
unset(CMAKE_REQUIRED_FLAGS)
if(SP1M_MIDI_SANITIZERS)
  target_compile_options(sp1m_midi PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(sp1m_midi INTERFACE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME sp1m_tests COMMAND sp1m_tests)

foreach(test midi_hr midi_bus midi_assign midi_params usb_rt_parse)
  add_executable(test_${test} midi/test_${test}.c)
  target_link_libraries(test_${test} PRIVATE sp1m_midi)
  # The unchanged vendor tests use assert() as their test runner, including
  # calls with side effects. Keep assertions enabled in Release builds too.
  target_compile_options(test_${test} PRIVATE -Wall -Wextra -UNDEBUG)
  if(SP1M_MIDI_SANITIZERS)
    target_compile_options(test_${test} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
  add_test(NAME test_${test} COMMAND test_${test})
endforeach()
