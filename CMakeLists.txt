# ----------------------------------------------------
# Environment
# ----------------------------------------------------
cmake_minimum_required(VERSION 3.22)

set(
    APP_PROJECT_NAME_BASE
    "esp32s3_cam"
    CACHE STRING
    "Project name - Can be overridden by Makefile"
)

# Global dir definitions
set(TOOLS_PATH      ${CMAKE_SOURCE_DIR}/tools)
set(SOFTWARE_PATH   ${CMAKE_SOURCE_DIR}/software)
set(MODULES_PATH    ${SOFTWARE_PATH}/modules)
set(APP_PATH        ${SOFTWARE_PATH}/application)

# CMake module path (project helpers from cmake/)
include(cmake/general/ExportCMakeModulesPath.cmake)
include(general/PreventInSourceBuilds)

# ----------------------------------------------------
# Board
# ----------------------------------------------------

# Changing the board changes sdkconfig-level options (flash size, PSRAM,
# partition table) — use a fresh build directory when switching.
set(BOARD_DIR "${CMAKE_SOURCE_DIR}/boards/esp32s3_n16r8_dk" CACHE STRING "Active board directory")

# Resolve a relative -DBOARD_DIR against the project root, not components/board.
get_filename_component(BOARD_DIR "${BOARD_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

if(NOT EXISTS "${BOARD_DIR}/bsp.h")
    message(FATAL_ERROR "BOARD_DIR '${BOARD_DIR}' does not look like a board directory (missing bsp.h)")
endif()

# smart_io exists only on boards that define its pin pool. Decided here, not in
# Kconfig: component requirements are resolved before sdkconfig exists.
if(EXISTS "${BOARD_DIR}/bsp_smart_io.h")
    set(BOARD_HAS_SMART_IO 1)
else()
    set(BOARD_HAS_SMART_IO 0)
endif()

message(STATUS "Board smart_io: ${BOARD_HAS_SMART_IO}")

# ----------------------------------------------------
# ESP-IDF configuration
# ----------------------------------------------------

# Target chip is selected during configuration (-DIDF_TARGET=esp32s3).
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    set(IDF_TARGET esp32s3)
endif()

# Build type is mapped to an sdkconfig overlay (ESP-IDF controls optimization
# through Kconfig rather than CMAKE_BUILD_TYPE).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_build_conf ${APP_PATH}/config/release.conf)
else()
    set(_build_conf ${APP_PATH}/config/debug.conf)
endif()

# Configuration overlays (later files override earlier ones)
set(SDKCONFIG_DEFAULTS
    ${APP_PATH}/config/proj.conf
    ${BOARD_DIR}/sdkconfig.board
    ${_build_conf}
)

if(BOARD_HAS_SMART_IO)
    list(INSERT SDKCONFIG_DEFAULTS 1 ${APP_PATH}/config/smart_io.conf)
endif()

# Project configuration output
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)

# Additional ESP component search paths: "main" and project modules
set(EXTRA_COMPONENT_DIRS
    ${APP_PATH}/main
    ${APP_PATH}/config
    ${MODULES_PATH}
)

# Include ESP-IDF, now CMake magic will happen.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(BOARD_HAS_SMART_IO ${BOARD_HAS_SMART_IO})
idf_build_set_property(COMPILE_DEFINITIONS "-DBOARD_HAS_SMART_IO=${BOARD_HAS_SMART_IO}" APPEND)
project(${APP_PROJECT_NAME_BASE})

# ----------------------------------------------------
# Versioning
# ----------------------------------------------------

# version.h is written to the build dir; components that need it add
# ${CMAKE_BINARY_DIR} to their include path and depend on the generator target.
include(versioning/GitVersion)
git_version_create(
    PROJECT_PREFIX ESP32S3_CAM_
    OUTPUT_FILE    ${CMAKE_BINARY_DIR}/version.h
)
add_dependencies(__idf_ota ESP32S3_CAM__gitVersion)

# ----------------------------------------------------
# Static analysis
# ----------------------------------------------------

include(format/ClangFormat)
clang_format_install(
    ${PROJECT_NAME}
    SOURCE_PATHS
        "${APP_PATH}"
        "${MODULES_PATH}"
        "${CMAKE_SOURCE_DIR}/boards"
)

include(sizeReport/SizeReport)
size_report_install(
    ${PROJECT_NAME}
    ELF_TARGET ${PROJECT_NAME}.elf
)

# ----------------------------------------------------
# Install
# ----------------------------------------------------

# Everything needed to flash the board from a host machine (the container
# has no USB access): `esptool.py write_flash @flash_args`.
install(
    FILES
        ${CMAKE_BINARY_DIR}/bootloader/bootloader.elf
        ${CMAKE_BINARY_DIR}/bootloader/bootloader.bin
        ${CMAKE_BINARY_DIR}/partition_table/partition-table.bin
        ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.elf
        ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin
        ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map
        ${CMAKE_BINARY_DIR}/flasher_args.json
        ${CMAKE_BINARY_DIR}/flash_args
    DESTINATION "bin"
)

# ----------------------------------------------------
# Tests (host, Ceedling)
# ----------------------------------------------------

set(APP_PROJECT_NAME ${PROJECT_NAME}_test)
add_subdirectory(${CMAKE_SOURCE_DIR}/test/unit)
