set(BOARD_REVISION_OPTIONS "a4")

if(NOT DEFINED BOARD_REVISION)
  message(FATAL_ERROR "No board revision specified. Accepted revisions: ${BOARD_REVISION_OPTIONS}")
else()
  if(NOT BOARD_REVISION IN_LIST BOARD_REVISION_OPTIONS)
    message(FATAL_ERROR "${BOARD_REVISION} is not a valid revision for Control. Accepted revisions: ${BOARD_REVISION_OPTIONS}")
  endif()
endif()
