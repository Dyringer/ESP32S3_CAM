
idf_component_register(
    SRCS
        "wifi_connection.c"
        "sta/wifi_sta.c"
        "ap/wifi_ap.c"
        "shell/wifi_shell.c"
        "provisioning/wifi_provisioning.c"
        "provisioning/wifi_provisioning_http.c"
    EMBED_FILES
        "provisioning/wifi_provisioning.html"
    INCLUDE_DIRS
        "."
        "sta"
        "ap"
        "shell"
        "provisioning"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_server
        shell
        cjson           # cJSON (managed component espressif/cjson in IDF 6.0+)
        lwip            # sockets for DNS task
        log
        esp_timer
        common
)