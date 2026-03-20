cmake_minimum_required(VERSION 3.25)
project(platform_catch2_tests LANGUAGES C CXX)

enable_testing()

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../CMakeFiles/host-test-functions.cmake)

add_subdirectory(${THIRDPARTY_DIR}/catch2 ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/catch2 EXCLUDE_FROM_ALL)

configure_catch2_test_target(platform_catch2_smoke
  SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/smoke/test_platform_catch2_smoke.cpp
)

configure_catch2_test_target(platform_catch2_modem_board
  SOURCES
    ${PLATFORM_SRC_DIR}/modem-board/modem-board-core.c
    ${CMAKE_CURRENT_LIST_DIR}/modem-board/test_modem_board_core.cpp
  INCLUDE_DIRS
    ${PLATFORM_SRC_DIR}/modem-board/inc
    ${PLATFORM_SRC_DIR}/modem-board
)

configure_catch2_test_target(platform_catch2_modem_at
  SOURCES
    ${PLATFORM_SRC_DIR}/modem-at/modem-at.c
    ${CMAKE_CURRENT_LIST_DIR}/modem-at/test_modem_at.cpp
  INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/shims
    ${PLATFORM_SRC_DIR}/modem-at/inc
)

configure_catch2_test_target(platform_catch2_modem_shell
  SOURCES
    ${PLATFORM_SRC_DIR}/modem-shell/modem-shell-core.c
    ${CMAKE_CURRENT_LIST_DIR}/modem-shell/test_modem_shell_core.cpp
  INCLUDE_DIRS
    ${PLATFORM_SRC_DIR}/modem-at/inc
    ${PLATFORM_SRC_DIR}/modem-board/inc
    ${PLATFORM_SRC_DIR}/modem-shell
)
