
idf_component_register(
    SRCS
        "uart_console.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        shell
        console
        esp_driver_uart
        vfs
        log
        freertos
)
