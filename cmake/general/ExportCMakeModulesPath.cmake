# ExportCMakeModulesPath.cmake — Daniel Dyringer
# Appends the project cmake/ directory to CMAKE_MODULE_PATH so modules can be
# included by short name: include(compiler/CompilerWarningsForC)
# Must be called from the top-level CMakeLists.txt.

if(NOT CMAKE_MODULE_PATH)
    set(CMAKE_MODULE_PATH "")
endif()

if("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
else()
    message(WARNING "ExportCMakeModulesPath called from a subdirectory — module path may be incorrect")
endif()
