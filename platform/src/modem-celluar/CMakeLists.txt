set(LIBRARY_NAME platform_src_modem_rc7620)

add_library(${LIBRARY_NAME} INTERFACE)

target_include_directories(${LIBRARY_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

target_link_libraries(${LIBRARY_NAME} INTERFACE zephyr_interface)

set(LIBRARY_NAME_OBJ platform_src_modem_rc7620_obj)

add_library(${LIBRARY_NAME_OBJ} OBJECT)

target_include_directories(${LIBRARY_NAME_OBJ} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/inc
  ${PLATFORM_SRC_DIR}/modem-shell/inc
)

target_sources(${LIBRARY_NAME_OBJ} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/modem-rc7620.c
)

target_link_libraries(${LIBRARY_NAME_OBJ} PUBLIC platform_src_modem_rc7620)
target_link_libraries(${LIBRARY_NAME_OBJ} PRIVATE platform_src_modem_shell)

mark_zephyr_system_includes(${LIBRARY_NAME_OBJ})
add_dependencies(${LIBRARY_NAME_OBJ} zephyr_generated_headers)
