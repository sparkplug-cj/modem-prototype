cmake_minimum_required(VERSION 3.25)

# Must be before find_package(Zephyr) to provide custom board and shield specifications
include(${CMAKE_CURRENT_SOURCE_DIR}/../CMakeFiles/globals.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../CMakeFiles/shields.cmake)

if(NOT DEFINED ENV{ZEPHYR_BASE})
    set(ZEPHYR_BASE ${ZEPHYR_ABS_DIR} CACHE PATH "The directory points to Zephyr root" FORCE)
else()
    set(ZEPHYR_BASE $ENV{ZEPHYR_BASE} CACHE PATH "The directory points to Zephyr root" FORCE)
endif()

if(DEFINED EXTRA_CONF_FILE)
  set(_extra_conf_file_candidates ${EXTRA_CONF_FILE})
  set(_extra_conf_file_existing)

  foreach(_extra_conf_file IN LISTS _extra_conf_file_candidates)
    if(IS_ABSOLUTE "${_extra_conf_file}")
      set(_extra_conf_file_path "${_extra_conf_file}")
    else()
      set(_extra_conf_file_path "${CMAKE_CURRENT_SOURCE_DIR}/${_extra_conf_file}")
    endif()

    if(EXISTS "${_extra_conf_file_path}")
      list(APPEND _extra_conf_file_existing "${_extra_conf_file}")
    else()
      message(STATUS "Skipping missing optional EXTRA_CONF_FILE entry: ${_extra_conf_file}")
    endif()
  endforeach()

  if(_extra_conf_file_existing)
    set(EXTRA_CONF_FILE "${_extra_conf_file_existing}" CACHE STRING "Existing extra configuration files" FORCE)
  else()
    unset(EXTRA_CONF_FILE CACHE)
    unset(EXTRA_CONF_FILE)
  endif()

  unset(_extra_conf_file_candidates)
  unset(_extra_conf_file_existing)
  unset(_extra_conf_file)
  unset(_extra_conf_file_path)
endif()

# Find zephyr and import Zephyr's cmake
find_package(Zephyr REQUIRED HINTS ${ZEPHYR_BASE})

project(control_application)

include(${CMAKE_CURRENT_SOURCE_DIR}/../CMakeFiles/firmware-compile-options.cmake)
include(${PLATFORM_DIR}/CMakeFiles/build-platform-targets.cmake) # Discover all all targets under platform directory.

target_sources(app PRIVATE
  ${CONTROL_SRC_DIR}/main.c
)

# Platform units
target_link_libraries(app PRIVATE platform_src_modem_board_obj)
target_link_libraries(app PRIVATE platform_src_modem_at_obj)
target_link_libraries(app PRIVATE platform_src_modem_rc7620_obj)
target_link_libraries(app PRIVATE platform_src_modem_link_obj)
target_link_libraries(app PRIVATE platform_src_modem_shell_obj)

mark_zephyr_system_includes(app)
target_link_libraries(app PRIVATE zephyr_interface)
add_dependencies(app zephyr_generated_headers)
