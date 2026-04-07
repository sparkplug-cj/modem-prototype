cmake_minimum_required(VERSION 3.25)

# Must be before find_package(Zephyr) to provide custom board and shield specifications
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeFiles/globals.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../CMakeFiles/shields.cmake)

if(NOT DEFINED $ENV{ZEPHYR_BASE})
    set(ZEPHYR_BASE ${ZEPHYR_ABS_DIR} CACHE PATH "The directory points to Zephyr root" FORCE)
else()
    set(ZEPHYR_BASE $ENV{ZEPHYR_BASE} CACHE PATH "The directory points to Zephyr root" FORCE)
endif()

# Find zephyr and import Zephyr's cmake
find_package(Zephyr REQUIRED HINTS ${ZEPHYR_BASE})

project(control_application)

include(${WORKSPACE_ROOT_CMAKEFILES_DIR}/firmware-compile-options.cmake)
include(${PLATFORM_DIR}/CMakeFiles/build-platform-targets.cmake) # Discover all all targets under platform directory.

target_sources(app PRIVATE
  ${CONTROL_SRC_DIR}/main.c
)

generate_inc_file_for_target(
    app
    ${CMAKE_CURRENT_SOURCE_DIR}/one_month.txt
    one_month_txt.inc
)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Platform units
target_link_libraries(app PRIVATE platform_src_modem_board_obj)
target_link_libraries(app PRIVATE platform_src_modem_at_obj)
target_link_libraries(app PRIVATE platform_src_modem_shell_obj)
target_link_libraries(app PRIVATE platform_src_modem_net_obj)

mark_zephyr_system_includes(app)
target_link_libraries(app PRIVATE zephyr_interface)
add_dependencies(app zephyr_generated_headers)
