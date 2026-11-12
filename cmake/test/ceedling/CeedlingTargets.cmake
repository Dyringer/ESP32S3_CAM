# CeedlingTargets.cmake — Daniel Dyringer
# Installs CMake targets for Ceedling-based unit testing.
#
# Requires:
#   - ceedling in PATH  (gem install ceedling)
#   - project.yml.in present in the test source directory
#
# Targets:
#   ${TARGET_NAME}_unitTest         — run all tests
#   ${TARGET_NAME}_unitTestClean    — clobber test build
#
# Env vars honoured at build time:
#   VERBOSE=1            — verbose ceedling output
#   TEST_SELECTOR=<pat>  — run only matching tests

function(ceedling_targets_install TARGET_NAME)
    find_program(CEEDLING ceedling REQUIRED)

    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/project.yml.in")
        message(FATAL_ERROR
            "ceedling_targets_install: project.yml.in not found in\n"
            "  ${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    # Expand CMake variables into the project file (out-of-source)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/project.yml.in"
        "${CMAKE_CURRENT_BINARY_DIR}/project.yml"
        @ONLY
    )

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_runner "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ceedling_run.py")

    # Tests run from the binary dir so ceedling finds project.yml there
    add_custom_target(${TARGET_NAME}_unitTest
        COMMAND "${Python3_EXECUTABLE}" "${_runner}" test
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Ceedling tests: ${TARGET_NAME}"
        VERBATIM
    )

    add_custom_target(${TARGET_NAME}_unitTestClean
        COMMAND "${CEEDLING}" clobber
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Ceedling clean: ${TARGET_NAME}"
        VERBATIM
    )
endfunction()
