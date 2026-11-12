set(main_requires
    esp_event
    nvs_flash
    shell
    uart_console
    network
    net_diag
    common
    data
    board
    http_panel
    ota
    i2c_bus
    sensors
    tools
    camera
    sysmon
    system
    snapshot_upload
    storage
    app_update
)

idf_build_get_property(board_has_smart_io BOARD_HAS_SMART_IO)

if(board_has_smart_io)
    list(APPEND main_requires smart_io)
endif()

idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS "."
    REQUIRES ${main_requires}
)
