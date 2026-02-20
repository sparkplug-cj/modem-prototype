# Add application specific shield overlay files. This is needed because Zephyr v3.7 does not automatically
# include overlay files for specific shield in the application layer.
#
# This makes assumption of the directory layout and expects shield overlays to be under:
#   <application>/boards/shields/<name>/<name>_<qualifiers>.overlay
#
# Only supports exactly 1 shield, will generate fatal error if not satisfied.
#
if(NOT DEFINED SHIELD)
  message(FATAL_ERROR "Shield not defined.")
endif()

string(REPLACE " " ";" SHIELD_LIST "${SHIELD}")
list(LENGTH SHIELD_LIST SHIELD_LIST_LEN)

if(NOT SHIELD_LIST_LEN EQUAL 1)
  message(FATAL_ERROR "Only 1 shield can be specified.")
endif()

list(GET SHIELD_LIST 0 SHIELD_FIRST)
string(REGEX REPLACE "_.*" "" SHIELD_NAME "${SHIELD_FIRST}")
set(EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/shields/${SHIELD_NAME}/${SHIELD_FIRST}.overlay")

message(STATUS "Setting extra devicetree overlay: ${EXTRA_DTC_OVERLAY_FILE}")
