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
  ${CORE_SOURCES}
)
target_include_directories(sp1m_tests PRIVATE ${CORE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(sp1m_tests PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing
  -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(sp1m_tests PRIVATE -fsanitize=address,undefined)
target_link_libraries(sp1m_tests PRIVATE m)

enable_testing()
add_test(NAME sp1m_tests COMMAND sp1m_tests)
