
idf_component_register(
    SRCS
        "http_panel.c"
        "core/http_panel_server.c"
        "pages/dashboard/dashboard.c"
        "pages/web_console/web_console.c"
        "pages/ota/ota_page.c"
        "pages/camera/camera_page.c"
        "providers/http_panel_providers.c"
    EMBED_FILES
        "pages/dashboard/dashboard.html"
        "pages/web_console/web_console.html"
        "pages/ota/ota_page.html"
        "pages/camera/camera_page.html"
    INCLUDE_DIRS
        "."
        "core"
        "providers"
        "pages/dashboard"
        "pages/web_console"
        "pages/ota"
        "pages/camera"
    REQUIRES
        esp_http_server    # httpd_*, WebSocket
        shell              # app_shell_dispatch
        cjson              # cJSON (managed component espressif/cjson in IDF 6.0+)
        log
        network            # wifi_connection_get_state / get_ssid / forget
        freertos
        esp_event          # OTA_EVENTS subscription
        ota                # OTA_EVENTS, ota_begin/write/finish/abort (providers + ota page)
    PRIV_REQUIRES
        camera             # frames, settings, LED (providers + camera page)
        common             # ARRAY_SIZE
        board              # BOARD_NAME                 (providers only)
        esp_wifi           # esp_wifi_sta_get_ap_info  (providers only)
        esp_netif          # esp_netif_get_ip_info      (providers only)
        esp_timer          # esp_timer_get_time         (providers, dashboard, camera page)
        esp_app_format     # esp_app_get_description    (providers only)
        app_update         # esp_ota_get_running_partition (providers only)
        spi_flash          # esp_flash_get_size         (providers only)
        esp_hw_support     # esp_chip_info, esp_read_mac (providers only)
        sysmon             # sysmon_cpu_load            (providers only)
        heap               # heap_caps_*                (providers only)
)
