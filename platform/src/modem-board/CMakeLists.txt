# modem-board unit (application mode)
# Note: we compile this unit as part of the `app` target so it inherits the
# Zephyr include dirs and our SYSTEM include handling (avoids -Werror issues
# triggered by Zephyr inline helpers under -Winline/-Wpedantic).

target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/modem-board.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/inc)
