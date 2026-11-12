# SizeReport.cmake — Daniel Dyringer
# Adds a ${TARGET_NAME}_sizeReport target that prints a detailed RAM/ROM breakdown
# using elf-size-analyze.py (https://github.com/jedrzejboczar/elf-size-analyze).
#
# Usage:
#   size_report_install(my_target [ELF_TARGET my_target.elf])
#
# ELF_TARGET defaults to TARGET_NAME (ESP-IDF names its executable "<project>.elf").

function(size_report_install TARGET_NAME)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ELF_TARGET" "")
    if(NOT ARG_ELF_TARGET)
        set(ARG_ELF_TARGET ${TARGET_NAME})
    endif()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_FOUND)
        message(WARNING "size_report_install: Python3 not found — skipping")
        return()
    endif()

    # Derive toolchain triplet (e.g. xtensa-esp32s3-elf-) from the compiler path
    get_filename_component(_compiler_dir  "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_compiler_name "${CMAKE_C_COMPILER}" NAME_WE)
    string(REGEX REPLACE "-gcc$" "-" _triplet "${_compiler_name}")

    add_custom_target(${TARGET_NAME}_sizeReport
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/elf-size-analyze.py"
                -t "${_compiler_dir}/${_triplet}"
                -R -F
                "$<TARGET_FILE:${ARG_ELF_TARGET}>"
        DEPENDS "${ARG_ELF_TARGET}"
        COMMENT "Size report: ${TARGET_NAME}"
        VERBATIM
    )
endfunction()
