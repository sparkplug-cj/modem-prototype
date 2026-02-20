include_guard(GLOBAL)

cmake_path(SET WORKSPACE_ROOT_DIR NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/..)

cmake_path(SET CONTROL_DIR NORMALIZE ${WORKSPACE_ROOT_DIR}/control)
cmake_path(SET CONTROL_SRC_DIR NORMALIZE ${CONTROL_DIR}/src)

cmake_path(SET PLATFORM_DIR NORMALIZE ${WORKSPACE_ROOT_DIR}/platform)
cmake_path(SET PLATFORM_SRC_DIR NORMALIZE ${PLATFORM_DIR}/src)
cmake_path(SET PLATFORM_DTS_DIR ${PLATFORM_DIR})

cmake_path(SET THIRDPARTY_DIR NORMALIZE ${WORKSPACE_ROOT_DIR}/thirdparty)

cmake_path(SET ZEPHYR_ABS_DIR NORMALIZE ${WORKSPACE_ROOT_DIR}/thirdparty/zephyrproject/zephyr)
cmake_path(SET ZEPHYR_INC_DIR NORMALIZE ${ZEPHYR_ABS_DIR}/include)

cmake_path(SET TARGETS_DIR ${WORKSPACE_ROOT_DIR}/targets)

# Provide locations to Zephyr for our out-of-tree custom board definitions.
list(APPEND BOARD_ROOT ${TARGETS_DIR})
list(APPEND DTS_ROOT ${TARGETS_DIR} ${PLATFORM_DTS_DIR})
list(APPEND SOC_ROOT ${TARGETS_DIR})

find_program(CCACHE_FOUND ccache)

if(CCACHE_FOUND)
    set(CMAKE_C_COMPILER_LAUNCHER ccache)
    set(CMAKE_CXX_COMPILER_LAUNCHER ccache)
endif(CCACHE_FOUND)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})

function(mark_zephyr_system_includes target)
  get_target_property(ZEPHYR_INC_DIRS zephyr_interface INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(${target} SYSTEM PRIVATE ${ZEPHYR_INC_DIRS})
endfunction()
