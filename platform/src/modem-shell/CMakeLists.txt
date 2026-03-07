set(LIBRARY_NAME_OBJ platform_src_modem_shell_obj)

add_library(${LIBRARY_NAME_OBJ} OBJECT)

target_include_directories(${LIBRARY_NAME_OBJ} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(${LIBRARY_NAME_OBJ} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/modem-shell.c
  ${CMAKE_CURRENT_LIST_DIR}/modem-shell-core.c
)

target_link_libraries(${LIBRARY_NAME_OBJ} INTERFACE zephyr_interface)
target_link_libraries(${LIBRARY_NAME_OBJ} PRIVATE platform_src_modem_board)

mark_zephyr_system_includes(${LIBRARY_NAME_OBJ})
add_dependencies(${LIBRARY_NAME_OBJ} zephyr_generated_headers)
