# GitVersion.cmake — Daniel Dyringer
# Generates version.h from git state at every build via git_version.py.
# The script skips writing when content is unchanged, so downstream targets
# only rebuild when the version string actually changes.
#
# Usage:
#   include(versioning/GitVersion)
#   git_version_create(
#       PROJECT_PREFIX PICO_CORE_
#       OUTPUT_FILE    ${CMAKE_CURRENT_BINARY_DIR}/version.h
#   )

function(git_version_create)
    set(ARG_SINGLE PROJECT_PREFIX OUTPUT_FILE)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "${ARG_SINGLE}" "")

    if(NOT ARG_OUTPUT_FILE)
        message(FATAL_ERROR "git_version_create: OUTPUT_FILE is required")
    endif()
    if(NOT ARG_PROJECT_PREFIX)
        set(ARG_PROJECT_PREFIX "")
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED QUIET)

    set(_script   "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/git_version.py")
    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/internal/version.h.in")

    # The stamp is a fake output that forces re-evaluation on every build.
    # git_version.py only writes version.h when its content would change,
    # so downstream compilations are not retriggered unnecessarily.
    set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/_${ARG_PROJECT_PREFIX}_version.stamp")

    add_custom_command(
        OUTPUT    "${_stamp}"
        BYPRODUCTS "${ARG_OUTPUT_FILE}"
        COMMAND   "${Python3_EXECUTABLE}"
                  "${_script}"
                  --prefix   "${ARG_PROJECT_PREFIX}"
                  --output   "${ARG_OUTPUT_FILE}"
                  --template "${_template}"
        COMMAND   "${CMAKE_COMMAND}" -E touch "${_stamp}"
        COMMENT   "Version header: ${ARG_PROJECT_PREFIX}"
        VERBATIM
    )

    add_custom_target(${ARG_PROJECT_PREFIX}_gitVersion ALL
        DEPENDS "${_stamp}"
    )
endfunction()
