include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/globals.cmake)

function(enable_host_test_warnings target)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Werror>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Werror>
  )
endfunction()

function(configure_catch2_test_target target)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs SOURCES LIBRARIES INCLUDE_DIRS)
  cmake_parse_arguments(CTT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  add_executable(${target})
  target_sources(${target} PRIVATE ${CTT_SOURCES})

  target_link_libraries(${target}
    PRIVATE
      Catch2::Catch2WithMain
      ${CTT_LIBRARIES}
  )

  target_include_directories(${target}
    PRIVATE
      ${WORKSPACE_ROOT_DIR}
      ${PLATFORM_DIR}
      ${CONTROL_DIR}
      ${THIRDPARTY_DIR}/fff
      ${CTT_INCLUDE_DIRS}
  )

  set_target_properties(${target} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )

  enable_host_test_warnings(${target})
  add_test(NAME ${target} COMMAND ${target})
endfunction()
