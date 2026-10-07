idf_component_register(
    SRCS
        "snapshot_upload.c"
        "snapshot_upload_shell.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        camera           # camera_frame_acquire / release
        network          # wifi_connection_get_state
        esp_http_client  # POST upload
        mbedtls          # esp_crt_bundle_attach (https:// URLs)
        esp_timer        # esp_timer_get_time — upload timing, "s ago"
        heap             # heap_caps_malloc (PSRAM frame copy)
        freertos
        log
        nvs_flash        # saved configuration (nvs.h)
        shell            # app_shell_register_cmd
)
