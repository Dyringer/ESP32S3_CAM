
cmake_minimum_required(VERSION 3.16)

# Board directory — ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB octal PSRAM).
# Holds bsp.h, sdkconfig.board and partitions.csv. Changing it changes
# sdkconfig-level options (flash size, PSRAM, partition table), so remove a
# stale build/ and sdkconfig first (rm -rf build sdkconfig).
set(BOARD_DIR "${CMAKE_SOURCE_DIR}/boards/esp32s3_n16r8_dk" CACHE STRING "Active board directory")

# Normalize to an absolute path so a relative -DBOARD_DIR=boards/... override
# resolves against the project root, not components/board (its consumer).
get_filename_component(BOARD_DIR "${BOARD_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

if(NOT EXISTS "${BOARD_DIR}/bsp.h")
    message(FATAL_ERROR "BOARD_DIR '${BOARD_DIR}' does not look like a board directory (missing bsp.h)")
endif()

# Layer the board's sdkconfig fragment on top of the shared defaults. bsp.h
# is exposed via the fixed-name "board" component (components/board), whose
# INCLUDE_DIRS points at BOARD_DIR — so consumers just REQUIRES board.
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults;${BOARD_DIR}/sdkconfig.board")
set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_cam)
