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

target_sources(app PRIVATE ${CONTROL_SRC_DIR}/main.c)

mark_zephyr_system_includes(app)
target_link_libraries(app PRIVATE zephyr_interface)

add_dependencies(app zephyr_generated_headers)
