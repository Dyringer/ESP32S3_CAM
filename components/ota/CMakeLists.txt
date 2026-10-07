idf_component_register(
    SRCS
        "ota.c"
        "ota_shell.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        app_update       # esp_ota_begin/write/end/set_boot_partition/get_next_update_partition
        esp_app_format   # esp_app_get_description, ESP_IMAGE_HEADER_MAGIC
        esp_event        # OTA_EVENTS
        shell            # app_shell_register_cmd
        log
    PRIV_REQUIRES
        esp_timer        # esp_timer_get_time — progress rate cap
)
