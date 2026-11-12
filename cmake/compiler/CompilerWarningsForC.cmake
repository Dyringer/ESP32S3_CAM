# CompilerWarningsForC.cmake — Daniel Dyringer
# Applies a curated set of C compiler warnings to a target.
#
# Usage:
#   compiler_warnings_install(my_target [WARNINGS_AS_ERRORS])
#   compiler_warnings_get(out_var  [WARNINGS_AS_ERRORS])   # returns the flag list

# Build the curated warning flag list for the active compiler into OUT_VAR.
function(compiler_warnings_get OUT_VAR)
    set(ARG_OPTIONS WARNINGS_AS_ERRORS)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "${ARG_OPTIONS}" "" "")
    _compiler_warnings_flags(_flags ${ARG_WARNINGS_AS_ERRORS})
    set(${OUT_VAR} ${_flags} PARENT_SCOPE)
endfunction()

# Internal: resolve the flag list. AS_ERRORS is a bare TRUE/FALSE.
function(_compiler_warnings_flags OUT_VAR AS_ERRORS)
    set(GCC_WARNINGS
        -Wall
        -Wextra
        -Wshadow
        -Wcast-align
        -Wunused
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
    )

    set(CLANG_WARNINGS
        -Wall
        -Wextra
        -Wshadow
        -Wcast-align
        -Wunused
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )

    if(AS_ERRORS)
        list(APPEND GCC_WARNINGS   -Werror)
        list(APPEND CLANG_WARNINGS -Werror)
    endif()

    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        set(${OUT_VAR} ${GCC_WARNINGS} PARENT_SCOPE)
    elseif(CMAKE_C_COMPILER_ID MATCHES ".*Clang")
        set(${OUT_VAR} ${CLANG_WARNINGS} PARENT_SCOPE)
    else()
        message(AUTHOR_WARNING "No warning flags defined for compiler: ${CMAKE_C_COMPILER_ID}")
        set(${OUT_VAR} "" PARENT_SCOPE)
    endif()
endfunction()

# Apply the curated warnings directly to a target (its own sources only).
function(compiler_warnings_install TARGET_NAME)
    set(ARG_OPTIONS WARNINGS_AS_ERRORS)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "${ARG_OPTIONS}" "" "")
    _compiler_warnings_flags(_flags ${ARG_WARNINGS_AS_ERRORS})
    target_compile_options(${TARGET_NAME} PRIVATE ${_flags})
endfunction()
