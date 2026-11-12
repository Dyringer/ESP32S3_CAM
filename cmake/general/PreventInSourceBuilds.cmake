# PreventInSourceBuilds.cmake — Daniel Dyringer
# Aborts configuration if the build directory is the same as the source directory.

get_filename_component(_src "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
get_filename_component(_bin "${CMAKE_CURRENT_BINARY_DIR}" REALPATH)

if("${_src}" STREQUAL "${_bin}")
    message(FATAL_ERROR
        "In-source builds are not allowed.\n"
        "Create a separate build directory and run cmake from there.")
endif()
