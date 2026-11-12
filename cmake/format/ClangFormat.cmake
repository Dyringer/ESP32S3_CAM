# ClangFormat.cmake — Daniel Dyringer
# Adds style check and format targets using clang-format via run_clang_format.py.
#
# Usage:
#   clang_format_install(my_target SOURCE_PATHS src/ include/)
#
# Targets created:
#   ${TARGET_NAME}_styleCheck  — reports violations
#   ${TARGET_NAME}_styleFormat — fixes violations in-place

function(clang_format_install TARGET_NAME)
    set(ARG_MULTI SOURCE_PATHS)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "${ARG_MULTI}")

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_FOUND)
        message(WARNING "clang_format_install: Python3 not found — skipping format targets")
        return()
    endif()

    set(_script ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/run_clang_format.py)

    add_custom_target(${TARGET_NAME}_styleCheck
        COMMAND ${Python3_EXECUTABLE} ${_script} -r ${ARG_SOURCE_PATHS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Style check: ${TARGET_NAME}"
        VERBATIM
    )

    add_custom_target(${TARGET_NAME}_styleFormat
        COMMAND ${Python3_EXECUTABLE} ${_script} -r --fix ${ARG_SOURCE_PATHS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Style format: ${TARGET_NAME}"
        VERBATIM
    )
endfunction()
