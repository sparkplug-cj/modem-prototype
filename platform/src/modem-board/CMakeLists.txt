# Application-mode unit: add sources/includes to the app target

target_sources(app PRIVATE modem-board.c)
target_include_directories(app PRIVATE inc)
