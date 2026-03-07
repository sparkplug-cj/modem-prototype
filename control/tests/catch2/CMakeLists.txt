cmake_minimum_required(VERSION 3.25)
project(control_catch2_tests LANGUAGES C CXX)

enable_testing()

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../CMakeFiles/host-test-functions.cmake)

add_subdirectory(${THIRDPARTY_DIR}/catch2 ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/catch2 EXCLUDE_FROM_ALL)

configure_catch2_test_target(control_catch2_smoke
  SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/smoke/test_control_catch2_smoke.cpp
)
