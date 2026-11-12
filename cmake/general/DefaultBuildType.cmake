# DefaultBuildType.cmake — Daniel Dyringer
# Sets a default build type if none was specified.
# Defaults to Debug when inside a git repo, Release otherwise.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
        set(_default Debug)
    else()
        set(_default Release)
    endif()

    message(WARNING "No build type specified — defaulting to ${_default}")
    set(CMAKE_BUILD_TYPE "${_default}" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel RelWithDebInfo)
endif()
