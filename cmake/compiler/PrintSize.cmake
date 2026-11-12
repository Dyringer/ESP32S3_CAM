# PrintSize.cmake — Daniel Dyringer
# Adds a post-build command that prints section sizes of the target ELF.

function(print_size_install TARGET_NAME)
    if(NOT CMAKE_SIZE)
        find_program(CMAKE_SIZE
            NAMES xtensa-esp32s3-elf-size size
            HINTS "${TOOLCHAIN_PATH}"
        )
    endif()

    if(NOT CMAKE_SIZE)
        message(WARNING "print_size_install: xtensa-esp32s3-elf-size not found — skipping")
        return()
    endif()

    add_custom_command(
        TARGET  "${TARGET_NAME}" POST_BUILD
        COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:${TARGET_NAME}>"
        COMMENT "ELF size: ${TARGET_NAME}"
        VERBATIM
    )
endfunction()
