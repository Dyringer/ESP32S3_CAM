# ----------------------------------------------------
# Unit tests
# ----------------------------------------------------

include(format/ClangFormat)
clang_format_install(
    ${APP_PROJECT_NAME}
    SOURCE_PATHS
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

include(test/ceedling/CeedlingTargets)
ceedling_targets_install(${APP_PROJECT_NAME})
