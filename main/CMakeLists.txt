idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        shell
        uart_console
        network
        common
        board
        http_panel
        ota
        camera
        sysmon
        snapshot_upload
        app_update
)
